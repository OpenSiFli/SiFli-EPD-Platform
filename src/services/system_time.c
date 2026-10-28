#include "reminder_internal.h"
/* SPDX-License-Identifier: Apache-2.0 */
#include "system_time_internal.h"
#include "system_time.h"
#include "storage_file.h"
#include "net/network_time.h"
#include <dfs_posix.h>
#include <rtdevice.h>
#include <rtm.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define TIME_MAGIC 0x54494d45u
#define TIME_VERSION 1u
#define TIME_DEFAULTS {480, true, true}

static struct rt_mutex time_lock;
static bool lock_ready, initialized;
static system_time_settings_t settings = TIME_DEFAULTS;
static uint32_t ntp_generation = 1;

static bool take_lock(void)
{
    rt_enter_critical();
    if (!lock_ready)
        lock_ready = rt_mutex_init(&time_lock, "sys_time", RT_IPC_FLAG_PRIO) == RT_EOK;
    rt_exit_critical();
    return lock_ready && rt_mutex_take(&time_lock, RT_WAITING_FOREVER) == RT_EOK;
}

static void cancel_ntp(void)
{
    if (++ntp_generation == 0) ++ntp_generation;
}

/* storage_record_load has a boolean result. Read its fixed record envelope here
 * so a failed read is distinguishable from a deleted or malformed record. */
static system_time_reload_result_t read_settings(system_time_settings_t *out)
{
    uint32_t header[4];
    uint8_t payload[4];
    uint8_t record[sizeof(header) + sizeof(payload)];
    size_t used = 0;
    struct stat st;
    system_time_reload_result_t result = SYSTEM_TIME_RELOAD_IO_ERROR;
    storage_lock();
    if (!storage_path_available(SYSTEM_TIME_SETTINGS_PATH) ||
        !storage_file_recover(SYSTEM_TIME_SETTINGS_PATH)) goto done;
    int fd = open(SYSTEM_TIME_SETTINGS_PATH, O_RDONLY);
    if (fd < 0)
    {
        if (errno == ENOENT || errno == -ENOENT) result = SYSTEM_TIME_RELOAD_DEFAULTED;
        goto done;
    }
    if (fstat(fd, &st) != 0) goto close_file;
    if (st.st_size != (off_t)sizeof(record))
    {
        result = SYSTEM_TIME_RELOAD_DEFAULTED;
        goto close_file;
    }
    while (used < sizeof(record))
    {
        int n = read(fd, record + used, sizeof(record) - used);
        if (n < 0) goto close_file;
        if (!n) { result = SYSTEM_TIME_RELOAD_DEFAULTED; goto close_file; }
        used += (size_t)n;
    }
    memcpy(header, record, sizeof(header));
    memcpy(payload, record + sizeof(header), sizeof(payload));
    result = SYSTEM_TIME_RELOAD_DEFAULTED;
    if (header[0] != TIME_MAGIC || header[1] != TIME_VERSION ||
        header[2] != sizeof(payload) || header[3] != storage_crc32(payload, sizeof(payload)) ||
        payload[2] > 1 || payload[3] > 1) goto close_file;
    int offset = payload[0] | ((unsigned)payload[1] << 8);
    if (offset >= 32768) offset -= 65536;
    if (offset < -720 || offset > 840) goto close_file;
    *out = (system_time_settings_t){(int16_t)offset, payload[2] != 0, payload[3] != 0};
    result = SYSTEM_TIME_RELOAD_LOADED;
close_file:
    if (close(fd) != 0) result = SYSTEM_TIME_RELOAD_IO_ERROR;
done:
    storage_unlock();
    return result;
}

static system_time_reload_result_t reload_locked(void)
{
    system_time_settings_t candidate = TIME_DEFAULTS;
    system_time_reload_result_t result = read_settings(&candidate);
    if (result != SYSTEM_TIME_RELOAD_IO_ERROR)
    {
        if (result == SYSTEM_TIME_RELOAD_DEFAULTED)
            candidate = (system_time_settings_t)TIME_DEFAULTS;
        settings = candidate;
        initialized = true;
        cancel_ntp();
    }
    return result;
}

rt_err_t system_time_init(void)
{
    if (!take_lock()) return -RT_ERROR;
    bool initialized_now = !initialized && reload_locked() != SYSTEM_TIME_RELOAD_IO_ERROR;
    bool ok = initialized;
    rt_mutex_release(&time_lock);
    if (initialized_now) network_time_settings_changed();
    return ok ? RT_EOK : -RT_EIO;
}

system_time_reload_result_t system_time_reload(void)
{
    if (!take_lock()) return SYSTEM_TIME_RELOAD_IO_ERROR;
    system_time_reload_result_t result = reload_locked();
    rt_mutex_release(&time_lock);
    if (result != SYSTEM_TIME_RELOAD_IO_ERROR) network_time_settings_changed();
    return result;
}

void system_time_get_settings(system_time_settings_t *out)
{
    if (!out) return;
    *out = (system_time_settings_t)TIME_DEFAULTS;
    system_time_init();
    if (!take_lock()) return;
    *out = settings;
    rt_mutex_release(&time_lock);
}

static bool save_locked(const system_time_settings_t *candidate)
{
    uint16_t offset = (uint16_t)candidate->timezone_minutes;
    uint8_t payload[4] = {(uint8_t)offset, (uint8_t)(offset >> 8),
                          candidate->automatic, candidate->hour24};
    if (!storage_record_save(SYSTEM_TIME_SETTINGS_PATH, TIME_MAGIC, TIME_VERSION,
                             payload, sizeof(payload))) return false;
    if (settings.automatic != candidate->automatic) cancel_ntp();
    settings = *candidate;
    return true;
}

enum setting_field { AUTOMATIC, TIMEZONE, HOUR24 };
static bool set_field(enum setting_field field, int value)
{
    if (system_time_init() != RT_EOK || !take_lock()) return false;
    system_time_settings_t candidate = settings;
    if (field == AUTOMATIC) candidate.automatic = value != 0;
    else if (field == TIMEZONE) candidate.timezone_minutes = (int16_t)value;
    else candidate.hour24 = value != 0;
    bool ok = save_locked(&candidate);
    rt_mutex_release(&time_lock);
    if (ok && field == AUTOMATIC) network_time_settings_changed();
    if (ok && field == TIMEZONE) reminder_time_changed();
    return ok;
}

bool system_time_set_automatic(bool enabled) { return set_field(AUTOMATIC, enabled); }
bool system_time_set_timezone(int minutes)
{
    return minutes >= -720 && minutes <= 840 && set_field(TIMEZONE, minutes);
}
bool system_time_set_hour24(bool enabled) { return set_field(HOUR24, enabled); }

int64_t system_time_get_utc(void)
{
    if (!take_lock()) return 0;
    int64_t utc = (int64_t)time(NULL) - SYSTEM_TIME_RTC_OFFSET;
    rt_mutex_release(&time_lock);
    return utc;
}

bool system_time_utc_valid(int64_t utc)
{
    return utc >= SYSTEM_TIME_MIN_UTC && utc < SYSTEM_TIME_MAX_UTC;
}

bool system_time_valid(void) { return system_time_utc_valid(system_time_get_utc()); }

static bool local_calendar(int64_t utc, int offset, struct tm *out)
{
    if (!out || !system_time_utc_valid(utc)) return false;
    int64_t seconds = utc + offset * INT64_C(60);
    time_t local = (time_t)seconds;
    return (int64_t)local == seconds && gmtime_r(&local, out) != NULL;
}

bool system_time_local_from_utc(int64_t utc, struct tm *out)
{
    system_time_settings_t current;
    system_time_get_settings(&current);
    return local_calendar(utc, current.timezone_minutes, out);
}

bool system_time_get_local(struct tm *out)
{
    return system_time_local_from_utc(system_time_get_utc(), out);
}

int32_t system_time_local_day(int64_t utc)
{
    if (!system_time_utc_valid(utc)) return INT32_MIN;
    system_time_settings_t current;
    system_time_get_settings(&current);
    return (int32_t)((utc + current.timezone_minutes * INT64_C(60)) / 86400);
}

bool system_time_format(int64_t utc, const char *locale, bool with_date,
                        char *out, size_t size)
{
    if (!out || !size) return false;
    out[0] = 0;
    system_time_settings_t current;
    struct tm calendar;
    system_time_get_settings(&current);
    bool valid = local_calendar(utc, current.timezone_minutes, &calendar);
    char clock[32], text[48];
    if (!valid) snprintf(text, sizeof(text), "%s", with_date ? "---- -- --  --:--" : "--:--");
    else
    {
        if (current.hour24)
            snprintf(clock, sizeof(clock), "%02d:%02d", calendar.tm_hour, calendar.tm_min);
        else
        {
            bool chinese = locale && !strncmp(locale, "zh", 2);
            const char *period = calendar.tm_hour < 12 ? (chinese ? "上午" : "AM") :
                                                       (chinese ? "下午" : "PM");
            int hour = calendar.tm_hour % 12;
            snprintf(clock, sizeof(clock), "%s %02d:%02d", period, hour ? hour : 12, calendar.tm_min);
        }
        if (with_date)
            snprintf(text, sizeof(text), "%04d-%02d-%02d %s", calendar.tm_year + 1900,
                     calendar.tm_mon + 1, calendar.tm_mday, clock);
        else snprintf(text, sizeof(text), "%s", clock);
    }
    if (strlen(text) >= size) return false;
    memcpy(out, text, strlen(text) + 1);
    return valid;
}

static bool leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static bool calendar_seconds(const struct tm *calendar, int64_t *seconds)
{
    static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (!calendar || calendar->tm_year < 123 || calendar->tm_year > 200 ||
        calendar->tm_mon < 0 || calendar->tm_mon > 11 || calendar->tm_mday < 1 ||
        calendar->tm_hour < 0 || calendar->tm_hour > 23 ||
        calendar->tm_min < 0 || calendar->tm_min > 59 ||
        calendar->tm_sec < 0 || calendar->tm_sec > 59) return false;
    int year = calendar->tm_year + 1900;
    if (calendar->tm_mday > days_per_month[calendar->tm_mon] +
        (calendar->tm_mon == 1 && leap_year(year))) return false;
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) days += leap_year(y) ? 366 : 365;
    for (int month = 0; month < calendar->tm_mon; ++month)
        days += days_per_month[month] + (month == 1 && leap_year(year));
    days += calendar->tm_mday - 1;
    *seconds = days * 86400 + calendar->tm_hour * 3600 + calendar->tm_min * 60 + calendar->tm_sec;
    return true;
}

static bool write_utc(int64_t utc)
{
    if (!system_time_utc_valid(utc)) return false;
    int64_t seconds = utc + SYSTEM_TIME_RTC_OFFSET;
    time_t rtc_seconds = (time_t)seconds;
    if ((int64_t)rtc_seconds != seconds) return false;
    rt_device_t rtc = rt_device_find("rtc");
    return rtc && rt_device_control(rtc, RT_DEVICE_CTRL_RTC_SET_TIME, &rtc_seconds) == RT_EOK;
}

bool system_time_set_local(const struct tm *calendar)
{
    int64_t local;
    if (!calendar_seconds(calendar, &local) || system_time_init() != RT_EOK || !take_lock()) return false;
    bool ok = !settings.automatic && write_utc(local - settings.timezone_minutes * INT64_C(60));
    rt_mutex_release(&time_lock);
    if (ok) reminder_time_changed();
    return ok;
}

uint32_t system_time_ntp_token(void)
{
    if (system_time_init() != RT_EOK || !take_lock()) return 0;
    uint32_t token = settings.automatic ? ntp_generation : 0;
    rt_mutex_release(&time_lock);
    return token;
}

bool system_time_ntp_current(uint32_t token)
{
    if (!token || !take_lock()) return false;
    bool current = initialized && settings.automatic && token == ntp_generation;
    rt_mutex_release(&time_lock);
    return current;
}

bool system_time_apply_ntp(uint32_t token, int64_t utc)
{
    if (!token || !take_lock()) return false;
    bool ok = initialized && settings.automatic && token == ntp_generation && write_utc(utc);
    rt_mutex_release(&time_lock);
    if (ok) reminder_time_changed();
    return ok;
}

RTM_EXPORT(system_time_get_utc);
RTM_EXPORT(system_time_utc_valid);
RTM_EXPORT(system_time_valid);
RTM_EXPORT(system_time_get_local);
RTM_EXPORT(system_time_local_from_utc);
RTM_EXPORT(system_time_local_day);
RTM_EXPORT(system_time_format);
RTM_EXPORT(system_time_get_settings);
