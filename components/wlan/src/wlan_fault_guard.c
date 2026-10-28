#include "wlan_radio_power.h"
#include "wifi_port.h"
#include "sys_api.h"

#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
extern void __real_aicwf_driver_unexcepted(unsigned int event);
extern void wifi_driver_action(unsigned int action);

static rt_tick_t recovery_window;
static unsigned recovery_attempts;

void __wrap_aicwf_driver_unexcepted(unsigned int event)
{
    /* A dead SDIO transport is recoverable; memory/logic assertions (0x20)
     * retain the SDK handler. Preserve one owner for lifecycle transitions. */
    if (event != 0x04 && event != 0x01 && event != 0x02 &&
        event != 0x40 && event != 0x80 && event != 0x100)
    {
        __real_aicwf_driver_unexcepted(event);
        return;
    }
    rt_enter_critical();
    if (!wifi_chip_status_get())
    {
        rt_exit_critical();
        return;
    }
    wifi_chip_status_clr();
    unsigned operation = aicwf_wifi_dev.wifi_drive_op;
    bool restart = aicwf_wifi_dev.wifi_task_init_hdl && aicwf_wifi_dev.wifi_task_sem &&
        operation == 0 && aicwf_rtos_semaphore_get_count(aicwf_wifi_dev.wifi_task_sem) == 0;
    rt_tick_t now = rt_tick_get();
    if (!recovery_attempts || (rt_tick_t)(now - recovery_window) >= rt_tick_from_millisecond(60000))
    {
        recovery_window = now;
        recovery_attempts = 0;
    }
    /* Persistent failure stays offline until explicitly closed/reopened;
     * it must not become an unbounded reset loop. */
    if (restart && recovery_attempts < 2)
    {
        ++recovery_attempts;
        wifi_driver_action(3);
    }
    else restart = false;
    rt_exit_critical();
    rt_kprintf("wlan: transport fault=0x%x operation=%u recovery=%u attempt=%u\n",
        event, operation, restart, recovery_attempts);
}
#endif
