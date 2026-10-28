#include "app_recovery.h"
#include "system_reset_guard.h"
#include "storage_file.h"
#include <dfs_posix.h>
#include <rthw.h>
#include <string.h>
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif

#define RECOVERY_PATH STORAGE_FLASH_ROOT "/system/recovery.bin"
#define RECOVERY_MAGIC 0x45505231u
#define RECOVERY_VERSION 1u

#if defined(APP_RECOVERY) && APP_RECOVERY
#define RECOVERY_FORCED true
#else
#define RECOVERY_FORCED false
#endif

static app_recovery_status_t recovery_state = {false, true, false, false, -RT_EBUSY};

void app_recovery_get_status(app_recovery_status_t *status)
{
    if (!status) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *status = recovery_state;
    rt_hw_interrupt_enable(level);
}

rt_err_t app_recovery_init(void)
{
    app_recovery_status_t state;
    /* Storage owns the recursive I/O mutex; it must already be initialized. */
    storage_lock();
    app_recovery_get_status(&state);
    if (!state.initialized)
    {
        uint8_t enabled = 1;
        struct stat st;
        state.load_result = -RT_EIO;
        if (storage_available(STORAGE_FLASH))
        {
            if (stat(RECOVERY_PATH, &st) != 0)
            {
                /* This SDK's DFS reports negative POSIX errno values. */
                if (errno == -ENOENT)
                {
                    enabled = 0;
                    state.load_result = RT_EOK;
                }
            }
            else if (storage_record_load(RECOVERY_PATH, RECOVERY_MAGIC, RECOVERY_VERSION,
                                         &enabled, sizeof(enabled)) && enabled <= 1)
                state.load_result = RT_EOK;
        }
        state.initialized = true;
        state.active = RECOVERY_FORCED || state.load_result != RT_EOK || enabled != 0;
        state.next_boot = state.load_result != RT_EOK || enabled != 0;
        state.next_boot_valid = state.load_result == RT_EOK;
        rt_base_t level = rt_hw_interrupt_disable();
        recovery_state = state;
        rt_hw_interrupt_enable(level);
    }
    storage_unlock();
    return state.load_result;
}

bool app_recovery_active(void)
{
    app_recovery_status_t state;
    app_recovery_get_status(&state);
    return state.active;
}

bool app_recovery_forced(void) { return RECOVERY_FORCED; }

rt_err_t app_recovery_check_dynamic(void)
{
    if (system_reset_blocks_apps()) return APP_RECOVERY_BLOCKED;
    app_recovery_status_t state;
    app_recovery_get_status(&state);
    if (!state.initialized) return -RT_EBUSY;
    return state.active ? APP_RECOVERY_BLOCKED : RT_EOK;
}

rt_err_t app_recovery_set_next_boot(bool enabled)
{
    app_recovery_status_t state;
    app_recovery_get_status(&state);
    /* finsh can run before storage_init; do not touch its mutex in that case. */
    if (!state.initialized) return -RT_EBUSY;
    uint8_t value = enabled ? 1 : 0;
    storage_lock();
    bool saved = storage_record_save(RECOVERY_PATH, RECOVERY_MAGIC, RECOVERY_VERSION,
                                     &value, sizeof(value));
    if (saved)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        recovery_state.next_boot = enabled;
        recovery_state.next_boot_valid = true;
        rt_hw_interrupt_enable(level);
    }
    storage_unlock();
    return saved ? RT_EOK : -RT_EIO;
}

#ifdef RT_USING_FINSH
static int recovery(int argc, char **argv)
{
    rt_err_t result = RT_EOK;
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off")))
    {
        result = app_recovery_set_next_boot(!strcmp(argv[1], "on"));
        if (result != RT_EOK)
            rt_kprintf("[recovery] save failed: %d; boot selection unchanged\n", result);
        else if (app_recovery_forced())
            rt_kprintf("[recovery] saved; forced recovery remains active; use normal firmware to apply\n");
        else
            rt_kprintf("[recovery] saved; restart manually to apply\n");
    }
    else if (argc != 1 && !(argc == 2 && !strcmp(argv[1], "status")))
    {
        rt_kprintf("usage: recovery [status|on|off]\n");
        return -RT_EINVAL;
    }
    app_recovery_status_t state;
    app_recovery_get_status(&state);
    rt_kprintf("[recovery] current=%s next=%s load_result=%d forced=%d\n",
               !state.initialized ? "uninitialized" : state.active ? "recovery" : "normal",
               !state.next_boot_valid ? "unknown" : state.next_boot ? "recovery" : "normal",
               state.load_result, app_recovery_forced());
    return result;
}
MSH_CMD_EXPORT(recovery, Recovery mode for next boot: recovery [status|on|off]);
#endif
