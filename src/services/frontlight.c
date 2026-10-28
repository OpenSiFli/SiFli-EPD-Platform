/* SPDX-License-Identifier: Apache-2.0 */
#include "frontlight.h"
#include "storage_file.h"
#include <rtdevice.h>
#include "drv_lcd.h"
#include <dfs_posix.h>
#include <errno.h>
#include <string.h>

#define FRONTLIGHT_MAGIC 0x464c4954u
#define FRONTLIGHT_VERSION 2u
#define FRONTLIGHT_DEFAULT_PERCENT 15u

static frontlight_snapshot_t state;
static bool baseline_lost, rollback_pending, process_scheduled;
static rt_tick_t last_access;
static bool light_window, light_start_pending;
static rt_tick_t light_started;

#define FRONTLIGHT_WINDOW_MS 3000
#define FRONTLIGHT_RETRY_MS 100

static void cancel_window(void)
{
    light_window = light_start_pending = false;
}

static bool window_expired(void)
{
    return light_window && !light_start_pending &&
        (rt_tick_t)(rt_tick_get() - light_started) >=
        (rt_tick_t)rt_tick_from_millisecond(FRONTLIGHT_WINDOW_MS);
}

static uint8_t quantize(uint8_t percent)
{
    if (percent > 100) percent = 100;
    return percent;
}

static uint8_t write_percent(uint8_t percent)
{
    return percent;
}

static bool valid_percent(uint8_t percent)
{
    return percent <= 100 && quantize(percent) == percent;
}

static void update_target(void)
{
    if (!state.preference_valid || !state.committed_percent || state.suspended || window_expired())
        cancel_window();
    state.desired_percent = state.suspended ? 0 : state.locked ?
        (light_window ? state.committed_percent : 0) :
        (state.previewing ? state.preview_percent : state.committed_percent);
}

static bool device_read(rt_device_t device, uint8_t *percent)
{
    return rt_device_read(device, 0, percent, 1) == 1 && valid_percent(*percent);
}

/* An opened device is always closed before returning; the standalone lcdlight
 * device remains available to the SDK's display/PM paths between UI frames. */
static bool access_output(bool apply)
{
    last_access = rt_tick_get();
    process_scheduled = true;
    rt_device_t device = rt_device_find("lcdlight");
    state.available = device != RT_NULL;
    state.output_valid = false;
    state.output_pending = true;
    state.hardware_status = FRONTLIGHT_HW_NO_DEVICE;
    if (!device) return false;
    state.hardware_status = FRONTLIGHT_HW_OPEN_ERROR;
    if (rt_device_open(device, RT_DEVICE_OFLAG_RDWR) != RT_EOK) return false;

    uint8_t actual = 0;
    bool read_ok = device_read(device, &actual);
    state.hardware_status = read_ok ? FRONTLIGHT_HW_READY : FRONTLIGHT_HW_READ_ERROR;
    bool mode_ok = true;
    frontlight_mode_t actual_mode = FRONTLIGHT_MIXED;
    mode_ok = rt_device_control(device, FRONTLIGHT_CTRL_GET_MODE, &actual_mode) == RT_EOK &&
              (unsigned)actual_mode < FRONTLIGHT_MODE_COUNT;
    state.mode_available = mode_ok;
    if (apply && mode_ok && actual_mode != state.mode)
        mode_ok = rt_device_control(device, FRONTLIGHT_CTRL_SET_MODE, &state.mode) == RT_EOK;
    if (apply && mode_ok && (!read_ok || actual != state.desired_percent))
    {
        uint8_t command = write_percent(state.desired_percent);
        if (rt_device_write(device, 0, &command, 1) != 1)
            state.hardware_status = FRONTLIGHT_HW_WRITE_ERROR;
        else if (!device_read(device, &actual))
            state.hardware_status = FRONTLIGHT_HW_READ_ERROR;
        else
            state.hardware_status = actual == state.desired_percent ?
                FRONTLIGHT_HW_READY : FRONTLIGHT_HW_MISMATCH;
    }
    if (!mode_ok) state.hardware_status = FRONTLIGHT_HW_WRITE_ERROR;
    if (rt_device_close(device) != RT_EOK)
        state.hardware_status = FRONTLIGHT_HW_CLOSE_ERROR;
    if (state.hardware_status != FRONTLIGHT_HW_READY) return false;
    bool pending = false;
    if (apply)
    {
        rt_device_t lcd = rt_device_find("lcd");
        uint8_t recorded = 0;
        bool busy = true;
        /* The LCD callback opens the standalone lcdlight device itself.
         * Synchronize only after closing it. During a scan, keep the verified
         * output and its light deadline; process() retries the LCD record at
         * the normal cadence. Pending synchronization still prevents sleep. */
        if (!lcd ||
            rt_device_control(lcd, RTGRAPHIC_CTRL_GET_BUSY, &busy) != RT_EOK ||
            (!busy &&
             (rt_device_control(lcd, RTGRAPHIC_CTRL_GET_BRIGHTNESS, &recorded) != RT_EOK ||
              (recorded != actual &&
               rt_device_control(lcd, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &actual) != RT_EOK))))
        {
            state.hardware_status = FRONTLIGHT_HW_WRITE_ERROR;
            return false;
        }
        pending = busy;
    }
    state.output_percent = actual;
    state.output_valid = true;
    state.output_pending = pending;
    if (apply && light_window && light_start_pending && actual)
    {
        light_started = rt_tick_get();
        light_start_pending = false;
    }
    return true;
}

/* storage_record_load returns only bool. Decode the same record envelope to
 * distinguish missing/invalid records from I/O errors, as system_time does. */
static frontlight_store_status_t read_preference(uint8_t *percent, frontlight_mode_t *mode)
{
    uint32_t header[4];
    uint8_t payload[4];
    uint8_t record[sizeof(header) + sizeof(payload)];
    size_t used = 0;
    struct stat st;
    frontlight_store_status_t result = FRONTLIGHT_STORE_IO_ERROR;
    if (!storage_path_available(FRONTLIGHT_SETTINGS_PATH) ||
        !storage_file_recover(FRONTLIGHT_SETTINGS_PATH)) return result;
    int fd = open(FRONTLIGHT_SETTINGS_PATH, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT || errno == -ENOENT ? FRONTLIGHT_STORE_MISSING : result;
    if (fstat(fd, &st) != 0) goto done;
    if (!S_ISREG(st.st_mode) || st.st_size != (int64_t)sizeof(record))
    {
        result = FRONTLIGHT_STORE_INVALID;
        goto done;
    }
    while (used < sizeof(record))
    {
        int n = read(fd, record + used, sizeof(record) - used);
        /* A short/failed transfer is not evidence that a preference is absent. */
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    memcpy(header, record, sizeof(header));
    memcpy(payload, record + sizeof(header), sizeof(payload));
    result = FRONTLIGHT_STORE_INVALID;
    if (header[0] == FRONTLIGHT_MAGIC && (header[1] == 1u || header[1] == FRONTLIGHT_VERSION) &&
        header[2] == sizeof(payload) && header[3] == storage_crc32(payload, sizeof(payload)) &&
        valid_percent(payload[0]) && !payload[2] && !payload[3] &&
        (header[1] == 1u ? payload[1] == 0 : payload[1] < FRONTLIGHT_MODE_COUNT))
    {
        *percent = payload[0];
        *mode = (frontlight_mode_t)payload[1];
        result = FRONTLIGHT_STORE_READY;
    }
done:
    if (close(fd) != 0 || !storage_path_available(FRONTLIGHT_SETTINGS_PATH))
        result = FRONTLIGHT_STORE_IO_ERROR;
    return result;
}

static bool save_preference(uint8_t percent)
{
    uint8_t payload[4] = {percent, (uint8_t)state.mode, 0, 0};
    bool ok = storage_record_save(FRONTLIGHT_SETTINGS_PATH, FRONTLIGHT_MAGIC,
                                  FRONTLIGHT_VERSION, payload, sizeof(payload));
    state.store_status = ok ? FRONTLIGHT_STORE_READY : FRONTLIGHT_STORE_SAVE_ERROR;
    return ok;
}

static void adopt_output(void)
{
    state.committed_percent = state.output_percent;
    state.preview_percent = state.output_percent;
    state.preference_valid = true;
    update_target();
}

static void adopt_startup_output(void)
{
    /* Cold boot holds the frontlight off while its preference is unknown.
     * A later missing record must still select the first-use default. */
    if (state.output_percent == 0)
    {
        baseline_lost = true;
        return;
    }
    adopt_output();
}

void frontlight_init(void)
{
    if (state.initialized) return;
    state.initialized = true;
    frontlight_reload();
}

frontlight_store_status_t frontlight_reload(void)
{
    if (!state.initialized) return FRONTLIGHT_STORE_UNINITIALIZED;
    state.previewing = false;
    uint8_t percent = 0;
    frontlight_mode_t mode = FRONTLIGHT_MIXED;
    storage_lock();
    state.store_status = read_preference(&percent, &mode);
    if (state.store_status == FRONTLIGHT_STORE_READY)
    {
        state.committed_percent = percent;
        state.mode = mode;
        state.preference_valid = true;
    }
    else if (state.store_status == FRONTLIGHT_STORE_MISSING && !state.preference_valid)
    {
        state.committed_percent = quantize(FRONTLIGHT_DEFAULT_PERCENT);
        state.mode = FRONTLIGHT_MIXED;
        state.preference_valid = true;
    }
    state.preview_percent = state.committed_percent;
    update_target();
    bool output_ok = false;
    if (!state.preference_valid && !baseline_lost)
    {
        output_ok = access_output(false);
        if (output_ok) adopt_startup_output();
    }
    if (state.preference_valid || state.locked || state.suspended)
    {
        /* Once an unknown initial value is forced off, zero cannot be used as
         * evidence of the user's preference. A later valid reload can recover. */
        if (!state.preference_valid) baseline_lost = true;
        output_ok = access_output(true);
    }
    if (state.store_status == FRONTLIGHT_STORE_MISSING && state.preference_valid && output_ok)
        save_preference(state.committed_percent);
    storage_unlock();
    return state.store_status;
}

void frontlight_get(frontlight_snapshot_t *out)
{
    if (out) *out = state;
}

bool frontlight_preview(uint8_t percent)
{
    if (!state.initialized || !state.preference_valid || state.locked || state.suspended) return false;
    state.previewing = true;
    state.preview_percent = quantize(percent);
    update_target();
    if (access_output(true)) return true;
    frontlight_hardware_status_t failure = state.hardware_status;
    frontlight_cancel_preview();
    /* Keep the failed operation visible even when rollback succeeded. */
    if (state.output_valid) state.hardware_status = failure;
    return false;
}

void frontlight_cancel_preview(void)
{
    state.previewing = false;
    state.preview_percent = state.committed_percent;
    update_target();
    if (state.initialized && (state.preference_valid || state.locked || state.suspended || rollback_pending))
        if (access_output(true) && !state.locked) rollback_pending = false;
}

bool frontlight_commit(uint8_t percent)
{
    if (!state.initialized || state.suspended) return false;
    bool known_preference = state.preference_valid;
    if (!known_preference)
    {
        /* An explicit choice can recover from a lost startup baseline. The
         * current readable output is a rollback value for this transaction,
         * and becomes a preference only if the new choice is saved. */
        if (!access_output(false)) return false;
        adopt_output();
    }
    uint8_t candidate = quantize(percent), stored = 0;
    if (state.locked)
    {
        state.previewing = false;
        state.preview_percent = candidate;
        update_target();
        /* A saved preference does not open or renew the temporary light
         * window. A dark lock screen verifies zero while saving its choice. */
        if (light_window) state.desired_percent = candidate;
        if (!access_output(true))
        {
            frontlight_hardware_status_t failure = state.hardware_status;
            frontlight_cancel_preview();
            if (state.output_valid) state.hardware_status = failure;
            goto failed;
        }
    }
    else
    {
        if (!frontlight_preview(percent)) goto failed;
        candidate = state.output_percent;
    }
    frontlight_mode_t stored_mode = FRONTLIGHT_MIXED;
    storage_lock();
    state.store_status = read_preference(&stored, &stored_mode);
    bool ok = state.store_status != FRONTLIGHT_STORE_IO_ERROR;
    if (ok && !(state.store_status == FRONTLIGHT_STORE_READY && stored == candidate && stored_mode == state.mode))
        ok = save_preference(candidate);
    storage_unlock();
    if (!ok)
    {
        frontlight_cancel_preview();
        goto failed;
    }
    state.committed_percent = candidate;
    state.preview_percent = candidate;
    state.previewing = false;
    baseline_lost = false;
    rollback_pending = false;
    update_target();
    return true;
failed:
    if (!known_preference)
    {
        state.preference_valid = false;
        baseline_lost = true;
        rollback_pending = state.output_pending;
    }
    return false;
}

bool frontlight_set_mode(frontlight_mode_t mode)
{
    if (!state.initialized || !state.preference_valid || !state.mode_available ||
        state.suspended || (unsigned)mode >= FRONTLIGHT_MODE_COUNT) return false;
    if (mode == state.mode) return true;
    frontlight_mode_t previous = state.mode;
    state.mode = mode;
    if (frontlight_commit(state.committed_percent)) return true;
    frontlight_hardware_status_t failure = state.hardware_status;
    state.mode = previous;
    frontlight_cancel_preview();
    if (state.output_valid) state.hardware_status = failure;
    return false;
}

void frontlight_set_locked(bool locked)
{
    if (state.locked == locked) return;
    bool was_lit = false;
    rt_tick_t entered = rt_tick_get();
    if (locked && state.initialized && !state.suspended)
    {
        if (access_output(false))
        {
            was_lit = state.output_percent != 0;
            if (!state.preference_valid && !baseline_lost) adopt_startup_output();
        }
        else if (!state.preference_valid) baseline_lost = true;
    }
    cancel_window();
    state.locked = locked;
    if (locked && was_lit && state.preference_valid && state.committed_percent)
    {
        light_window = true;
        light_started = entered;
    }
    frontlight_cancel_preview();
}

void frontlight_set_suspended(bool suspended)
{
    if (state.suspended == suspended) return;
    state.suspended = suspended;
    cancel_window();
    frontlight_cancel_preview();
}

void frontlight_wake(void)
{
    if (!state.initialized || !state.locked || state.suspended ||
        !state.preference_valid || !state.committed_percent) return;
    state.previewing = false;
    state.preview_percent = state.committed_percent;
    light_window = light_start_pending = true;
    update_target();
    access_output(true);
}

static uint32_t remaining_ms(rt_tick_t started, int interval_ms)
{
    rt_tick_t interval = rt_tick_from_millisecond(interval_ms);
    rt_tick_t elapsed = rt_tick_get() - started;
    if (elapsed >= interval) return 0;
    return (uint32_t)(((uint64_t)(interval - elapsed) * 1000u + RT_TICK_PER_SECOND - 1u) /
                      RT_TICK_PER_SECOND);
}

uint32_t frontlight_next_delay(uint32_t default_ms)
{
    if (!state.initialized) return default_ms;
    if (light_window && !light_start_pending)
    {
        uint32_t delay = remaining_ms(light_started, FRONTLIGHT_WINDOW_MS);
        if (delay < default_ms) default_ms = delay;
    }
    if (state.output_pending || light_start_pending)
    {
        uint32_t delay = process_scheduled ? remaining_ms(last_access, FRONTLIGHT_RETRY_MS) : 0;
        if (delay < default_ms) default_ms = delay;
    }
    return default_ms;
}

void frontlight_process(void)
{
    if (!state.initialized) return;
    if (!window_expired() && process_scheduled && remaining_ms(last_access, FRONTLIGHT_RETRY_MS)) return;
    update_target();
    if (state.preference_valid || state.locked || state.suspended || rollback_pending)
    {
        if (!state.preference_valid) baseline_lost = true;
        if (access_output(true) && !state.locked) rollback_pending = false;
    }
    else if (!baseline_lost && access_output(false))
        adopt_startup_output();
}
