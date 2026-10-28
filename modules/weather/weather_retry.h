/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WEATHER_RETRY_H
#define WEATHER_RETRY_H
#include "weather.h"

#define WEATHER_LOOKUP WEATHER_PART_COUNT
#define WEATHER_RETRY_COUNT (WEATHER_PART_COUNT + 1)
#define WEATHER_UPDATE_INTERVAL_MS (30 * 60 * 1000)

typedef struct
{
    weather_error_t error;
    uint32_t retry_after;
} weather_failure_t;

typedef struct
{
    bool pending, blocked;
    unsigned attempts;
    rt_tick_t observed_at;
    uint64_t remaining;
    weather_error_t error;
} weather_retry_t;

weather_error_t weather_classify_status(int status);
uint32_t weather_retry_after(const char *value, const char *date, int64_t now);
void weather_retry_start(weather_retry_t *retry, rt_tick_t now, bool explicit_request);
bool weather_retry_due(weather_retry_t *retry, rt_tick_t now);
void weather_retry_finish(weather_retry_t *retry, rt_tick_t now, weather_failure_t failure);
#endif
