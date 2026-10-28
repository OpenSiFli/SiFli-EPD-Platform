#ifndef EPD_SYSTEM_TIME_INTERNAL_H
#define EPD_SYSTEM_TIME_INTERNAL_H

#include "system_time.h"

/* RTC seconds retain the firmware's fixed UTC+8 representation. */
#define SYSTEM_TIME_RTC_OFFSET 28800
#define SYSTEM_TIME_MIN_UTC INT64_C(1704067200)
#define SYSTEM_TIME_MAX_UTC INT64_C(4102444800)
#define SYSTEM_TIME_SETTINGS_PATH "/flash/system/time.bin"


typedef enum
{
    SYSTEM_TIME_RELOAD_IO_ERROR = -1,
    SYSTEM_TIME_RELOAD_LOADED,
    SYSTEM_TIME_RELOAD_DEFAULTED
} system_time_reload_result_t;

/* Thread context only; persistence and RTC access are serialized by a mutex. */
rt_err_t system_time_init(void);
/* Missing/invalid records use defaults without writing. I/O errors retain the
 * running settings. Call after storage recovery, before resuming applications. */
system_time_reload_result_t system_time_reload(void);

/* Each setter persists before publishing. Failure leaves settings and pending
 * NTP requests unchanged. Equal values are also saved, including after reload. */
bool system_time_set_automatic(bool enabled);
bool system_time_set_timezone(int minutes);
bool system_time_set_hour24(bool enabled);


/* Calendar fields use struct tm conventions. Only available in manual mode;
 * rejects invalid dates rather than normalizing them. Ignores tm_isdst. */
bool system_time_set_local(const struct tm *calendar);

/* A nonzero token identifies the current automatic-sync session. Successful
 * auto-mode changes/reloads invalidate older tokens, even after off/on. */
uint32_t system_time_ntp_token(void);
bool system_time_ntp_current(uint32_t token);
bool system_time_apply_ntp(uint32_t token, int64_t utc);

#endif
