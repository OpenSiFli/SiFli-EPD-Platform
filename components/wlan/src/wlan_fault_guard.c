#include "wlan_radio_power.h"
#include "wifi_port.h"
#include "sys_api.h"

#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
#include <ipc/workqueue.h>

extern void __real_aicwf_driver_unexcepted(unsigned int event);
extern void wifi_driver_action(unsigned int action);
extern int __real_aicwf_rtos_semaphore_wait(rtos_semaphore semaphore, int timeout);
extern rtos_semaphore fhost_wpa_exit_sema;
extern void eloop_terminate(void);

static rt_tick_t recovery_window;
static unsigned recovery_attempts;
static volatile unsigned pending_fault;
static bool waiting_for_wpa_exit;
static bool recovery_work_ready;
static struct rt_work recovery_work;

static void recovery_worker(struct rt_work *work, void *data)
{
    (void)work;
    (void)data;
    wlan_adapter_try_recovery();
}

static int recovery_init(void)
{
    rt_work_init(&recovery_work, recovery_worker, RT_NULL);
    rt_enter_critical();
    recovery_work_ready = true;
    rt_exit_critical();
    return RT_EOK;
}
INIT_PREV_EXPORT(recovery_init);

void wlan_fault_kick(void)
{
    if (!pending_fault) return;
    rt_enter_critical();
    if (pending_fault && recovery_work_ready && rt_workqueue_sysq())
        (void)rt_work_submit(&recovery_work, 0);
    rt_exit_critical();
}

void wlan_fault_submit_recovery(void)
{
    rt_enter_critical();
    const volatile struct aic_wifi_dev *dev = &aicwf_wifi_dev;
    if (!pending_fault || !dev->wifi_task_init_hdl || !dev->wifi_task_sem ||
        dev->wifi_drive_op != 0 ||
        aicwf_rtos_semaphore_get_count(dev->wifi_task_sem) != 0)
    {
        rt_exit_critical();
        return;
    }

    unsigned event = pending_fault;
    pending_fault = 0;
    rt_tick_t now = rt_tick_get();
    if (!recovery_attempts || (rt_tick_t)(now - recovery_window) >= rt_tick_from_millisecond(60000))
    {
        recovery_window = now;
        recovery_attempts = 0;
    }
    bool restart = wlan_radio_power_allowed() && dev->g_wifi_init &&
                   !wifi_chip_status_get() && recovery_attempts < 2;
    if (restart)
    {
        ++recovery_attempts;
        wifi_driver_action(3);
    }
    unsigned attempts = recovery_attempts;
    rt_exit_critical();
    rt_kprintf("wlan: transport recovery=%u event=0x%x attempt=%u\n", restart, event, attempts);
}

int __wrap_aicwf_rtos_semaphore_wait(rtos_semaphore semaphore, int timeout)
{
    if (!semaphore || semaphore != fhost_wpa_exit_sema || timeout >= 0)
        return __real_aicwf_rtos_semaphore_wait(semaphore, timeout);

    /* The vendor has already submitted TERMINATE and released its end
     * semaphore here. Let WPA finish its own deinit and signal completion. */
    rt_enter_critical();
    waiting_for_wpa_exit = true;
    if (!wifi_chip_status_get()) eloop_terminate();
    rt_exit_critical();
    int result = __real_aicwf_rtos_semaphore_wait(semaphore, timeout);
    rt_enter_critical();
    waiting_for_wpa_exit = false;
    rt_exit_critical();
    return result;
}

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
    pending_fault = event;
    unsigned operation = aicwf_wifi_dev.wifi_drive_op;
    if (waiting_for_wpa_exit) eloop_terminate();
    rt_exit_critical();
    wlan_fault_kick();
    rt_kprintf("wlan: transport fault=0x%x operation=%u\n", event, operation);
}
#endif
