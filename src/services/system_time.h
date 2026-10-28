/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_SYSTEM_TIME_H
#define EPD_SYSTEM_TIME_H

#include <rtthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct
{
    int16_t timezone_minutes; /* Fixed display offset, -720 .. +840; no DST. */
    bool automatic;
    bool hour24;
} system_time_settings_t;

/* Thread context; returned settings and calendar values are copied. */
void system_time_get_settings(system_time_settings_t *out);
int64_t system_time_get_utc(void);
bool system_time_utc_valid(int64_t utc);
bool system_time_valid(void);
bool system_time_local_from_utc(int64_t utc, struct tm *out);
bool system_time_get_local(struct tm *out);
int32_t system_time_local_day(int64_t utc); /* INT32_MIN for invalid UTC. */
/* locale accepts the platform's zh_cn/en_us codes; other codes use AM/PM.
 * Invalid UTC produces the placeholder and returns false. No partial UTF-8
 * string is returned if the buffer is too small. */
bool system_time_format(int64_t utc, const char *locale, bool with_date,
                        char *out, size_t size);

#endif
