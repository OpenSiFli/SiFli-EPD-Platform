#include <stdbool.h>
#include <rtthread.h>
#include <rthw.h>
#include "wifi_port.h"

extern rt_err_t __real_rt_thread_suspend(rt_thread_t thread);
extern bool __real_aicwf_sdio_readb_cmd52(uint32_t address, uint8_t *data);

#if defined(WLAN_RADIO_SDIO_GUARD) && defined(RT_USING_FINSH)
#include <finsh.h>
#include "sdio_port.h"
#include "sdio_def.h"
#include "sys_api.h"

#ifdef CONFIG_SDIO_BUS_PWRCTRL
extern struct aic_sdio_dev sdio_dev;
static volatile uint32_t cmd52_reads;

static void wlan_sdio(void)
{
    rt_kprintf("wlan SDIO poll=%u pwrctl=%u ready=%u active=%u cmd52=%u\n",
               (unsigned)aicwifi_sdio_rxdata_mode, (unsigned)aic_pwrctl_enable_get(),
               (unsigned)sdio_dev.pwrctl.init_status, (unsigned)sdio_dev.pwrctl.bus_status,
               (unsigned)cmd52_reads);
}
MSH_CMD_EXPORT(wlan_sdio, Show WiFi SDIO activity and bus power control);
#endif
#endif

static bool owns_mutex(rt_thread_t thread)
{
    struct rt_object_information *info = rt_object_get_information(RT_Object_Class_Mutex);
    rt_list_t *node;
    if (!info) return false;
    for (node = info->object_list.next; node != &info->object_list; node = node->next)
    {
        struct rt_mutex *mutex = rt_list_entry(node, struct rt_mutex, parent.parent.list);
        if (mutex->owner == thread) return true;
    }
    return false;
}

rt_err_t __wrap_rt_thread_suspend(rt_thread_t thread)
{
    rt_thread_t self = rt_thread_self();
    unsigned operation = aicwf_wifi_dev.wifi_drive_op;
    if (!thread || !self || thread == self || self != (rt_thread_t)aicwf_wifi_dev.wifi_task_init_hdl ||
        (operation != 2 && operation != 3 && operation != 4 && operation != 5))
        return __real_rt_thread_suspend(thread);

    rt_tick_t report = rt_tick_get();
    for (;;)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        rt_err_t result;
        if ((thread->stat & RT_THREAD_STAT_MASK) == RT_THREAD_CLOSE)
        {
            rt_hw_interrupt_enable(level);
            return RT_EOK;
        }
        if (!owns_mutex(thread))
        {
            /* An IPC waiter must be detached before it is parked. Otherwise
             * mutex_release can hand ownership to an already stopped task. */
            if ((thread->stat & RT_THREAD_STAT_MASK) == RT_THREAD_SUSPEND)
                rt_thread_resume(thread);
            result = __real_rt_thread_suspend(thread);
            rt_hw_interrupt_enable(level);
            return result;
        }
        rt_hw_interrupt_enable(level);
        /* Never tear down a task while it owns a bus or command mutex. The
         * manager may wait, but other services and the UI remain runnable. */
        if ((rt_tick_t)(rt_tick_get() - report) >= rt_tick_from_millisecond(5000))
        {
            rt_kprintf("wlan: shutdown waiting for mutex owner %.*s\n", RT_NAME_MAX, thread->name);
            report = rt_tick_get();
        }
        rt_thread_mdelay(2);
    }
}

bool __wrap_aicwf_sdio_readb_cmd52(uint32_t address, uint8_t *data)
{
#if defined(WLAN_RADIO_SDIO_GUARD) && defined(RT_USING_FINSH) && defined(CONFIG_SDIO_BUS_PWRCTRL)
    ++cmd52_reads;
#endif
    bool result = __real_aicwf_sdio_readb_cmd52(address, data);
    /* The native RX loop retries immediately when the chip goes offline.
     * Preserve the error while allowing its manager and UI to make progress. */
    if (!result && rt_interrupt_get_nest() == 0 && rt_thread_self() != RT_NULL)
        rt_thread_mdelay(20);
    return result;
}
