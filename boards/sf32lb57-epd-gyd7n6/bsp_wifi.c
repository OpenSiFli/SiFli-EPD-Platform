/* SPDX-License-Identifier: Apache-2.0 */
#include "rtconfig.h"

#if defined(SOC_BF0_HCPU) && defined(AIC_WLAN_AIC8800D40N) && \
    defined(AIC8800_USE_RT_WLAN) && !defined(CFG_BOOTLOADER)

#include <rtthread.h>

extern void aic_wifi_demo_test(void);

static void epd_wifi_start_thread(void *parameter)
{
    (void)parameter;
    /* The SDK entry waits for SDIO, loads firmware and binds the STA device. */
    aic_wifi_demo_test();
}

static int epd_wifi_start(void)
{
    rt_thread_t thread;
    rt_err_t result;

    thread = rt_thread_create("epd_wifi", epd_wifi_start_thread, RT_NULL,
                              4096, 20, 10);
    if (thread == RT_NULL)
    {
        rt_kprintf("D40-N: cannot create startup thread\n");
        return -RT_ENOMEM;
    }

    result = rt_thread_startup(thread);
    if (result != RT_EOK)
    {
        rt_kprintf("D40-N: cannot start startup thread (%d)\n", result);
        rt_thread_delete(thread);
    }
    return result;
}
INIT_APP_EXPORT(epd_wifi_start);

#endif
