#ifndef APP_RECOVERY_H
#define APP_RECOVERY_H

#include <rtthread.h>
#include <stdbool.h>
#include <errno.h>

#define APP_RECOVERY_BLOCKED (-EACCES)

typedef struct
{
    bool initialized;
    bool active;
    bool next_boot; /* Persisted selection for ordinary firmware. */
    bool next_boot_valid;
    rt_err_t load_result;
} app_recovery_status_t;

/* Call after storage_init, before any dynamic application code. Idempotent;
 * ordinary firmware defaults to normal without a record. Read errors and
 * APP_RECOVERY=1 keep dynamic execution disabled for the entire boot. */
rt_err_t app_recovery_init(void);
void app_recovery_get_status(app_recovery_status_t *status);
/* True before initialization as well: uninitialized policy never permits code. */
bool app_recovery_active(void);
/* APP_RECOVERY=1 forces this firmware into recovery regardless of the record. */
bool app_recovery_forced(void);
/* RT_EOK, APP_RECOVERY_BLOCKED, or -RT_EBUSY before initialization. */
rt_err_t app_recovery_check_dynamic(void);
/* Persist only. The current boot never changes mode or unloads running code.
 * Returns -RT_EBUSY before initialization, -RT_EIO on storage failure. */
rt_err_t app_recovery_set_next_boot(bool enabled);

#endif
