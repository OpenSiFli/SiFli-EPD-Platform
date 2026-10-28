/* SPDX-License-Identifier: Apache-2.0 */
#include "weather_retry.h"
#include <stdio.h>
#include <string.h>

weather_error_t weather_classify_status(int status)
{
    if (status == 429) return WEATHER_ERROR_RATE_LIMIT;
    if (status == 408 || status >= 500 || status == 0) return WEATHER_ERROR_TRANSIENT;
    if (status == 200) return WEATHER_ERROR_NONE;
    return WEATHER_ERROR_REJECTED;
}

static int64_t http_date(const char *text)
{
    char month[4], zone[4], tail;
    int d, y, h, m, s;
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (!text) return -1;
    if (sscanf(text, "%*3s, %2d %3s %4d %2d:%2d:%2d %3s%c", &d, month, &y,
               &h, &m, &s, zone, &tail) == 7)
    {
        if (strcmp(zone, "GMT")) return -1;
    }
    else if (sscanf(text, "%*[^,], %2d-%3s-%2d %2d:%2d:%2d %3s%c", &d, month, &y,
                    &h, &m, &s, zone, &tail) == 7)
    {
        if (strcmp(zone, "GMT")) return -1;
        y += y >= 70 ? 1900 : 2000;
    }
    else if (sscanf(text, "%*3s %3s %2d %2d:%2d:%2d %4d%c", month, &d, &h, &m,
                    &s, &y, &tail) != 6) return -1;
    int mon;
    for (mon = 0; mon < 12 && strcmp(month, months[mon]); ++mon) {}
    if (y < 1970 || y > 9999 || mon == 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return -1;
    static const unsigned month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned leap = mon == 1 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    if ((unsigned)d > month_days[mon] + leap) return -1;
    /* Gregorian civil date to UTC seconds, independent of the local timezone. */
    ++mon;
    y -= mon <= 2;
    int era = y / 400, year = y - era * 400;
    int day = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = (int64_t)era * 146097 + year * 365 + year / 4 - year / 100 + day - 719468;
    return days * 86400 + h * 3600 + m * 60 + s;
}

uint32_t weather_retry_after(const char *value, const char *date, int64_t now)
{
    if (!value || !value[0]) return 60;
    uint64_t seconds = 0;
    const char *p = value;
    while (*p >= '0' && *p <= '9')
    {
        seconds = seconds * 10 + (*p++ - '0');
        if (seconds > UINT32_MAX) return UINT32_MAX;
    }
    if (p != value && !*p) return (uint32_t)seconds;
    int64_t target = http_date(value), server = http_date(date);
    if (target < 0) return 60;
    if (server >= 0) now = server;
    if (now < 1577836800) return 60;
    int64_t delay = target - now;
    return delay <= 0 ? 0 : delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
}

static void advance(weather_retry_t *retry, rt_tick_t now)
{
    rt_tick_t elapsed = now - retry->observed_at;
    retry->remaining = elapsed >= retry->remaining ? 0 : retry->remaining - elapsed;
    retry->observed_at = now;
}

void weather_retry_start(weather_retry_t *retry, rt_tick_t now, bool explicit_request)
{
    advance(retry, now);
    if (retry->error == WEATHER_ERROR_RATE_LIMIT && retry->remaining) return;
    /* A throttled round keeps its budget until the normal refresh period. */
    if (explicit_request && !retry->blocked && retry->error == WEATHER_ERROR_RATE_LIMIT) return;
    if (explicit_request)
    {
        retry->blocked = false;
        retry->attempts = 0;
        if (retry->error != WEATHER_ERROR_RATE_LIMIT) retry->remaining = 0;
    }
    if (!retry->blocked && !retry->pending)
    {
        retry->pending = true;
        retry->attempts = 0;
    }
}

bool weather_retry_due(weather_retry_t *retry, rt_tick_t now)
{
    advance(retry, now);
    return retry->pending && !retry->blocked && !retry->remaining;
}

void weather_retry_finish(weather_retry_t *retry, rt_tick_t now, weather_failure_t failure)
{
    retry->observed_at = now;
    retry->error = failure.error;
    retry->remaining = 0;
    ++retry->attempts;
    retry->blocked = failure.error == WEATHER_ERROR_REJECTED ||
                     failure.error == WEATHER_ERROR_CANCELLED;
    retry->pending = failure.error != WEATHER_ERROR_NONE && !retry->blocked && retry->attempts < 3;
    if (failure.error == WEATHER_ERROR_RATE_LIMIT || failure.retry_after)
    {
        retry->remaining = (uint64_t)failure.retry_after * RT_TICK_PER_SECOND;
        /* Manual refresh must also preserve a server-directed cooldown. */
        retry->error = WEATHER_ERROR_RATE_LIMIT;
    }
    else if (failure.error == WEATHER_ERROR_TRANSIENT)
        retry->remaining = (uint64_t)(5u << (retry->attempts - 1)) * RT_TICK_PER_SECOND;
}
