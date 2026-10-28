#include <rtthread.h>
#include "plat_port.h"

/* The port exports these configuration variables for the host library. RT
 * priorities increase toward idle; match plat_port.h's relative ordering to
 * the actual lwIP thread instead of its FreeRTOS-oriented default base. */
static int wlan_schedule_init(void)
{
    const unsigned base = RT_LWIP_TCPTHREAD_PRIORITY;
    RT_ASSERT(base >= 2 && base + 10 < RT_THREAD_PRIORITY_MAX);
    tcpip_priority = base;
    sdio_oobirq_priority = rwnx_wifi_priority = base - 2;
    sdio_datrx_priority = rwnx_timer_priority = base - 1;
    fhost_cntrl_priority = fhost_rx_priority = base - 1;
    fhost_wpa_priority = fhost_tcpack_priority = base;
    fhost_tx_priority = rwnx_apm_priority = base + 1;
    sdio_pwrclt_priority = base + 3;
    cli_cmd_priority = base + 10;
    /* Native AP setup used 95% of its default 1 KiB command stack. */
    cli_cmd_stack_size = 2048;
    return RT_EOK;
}
INIT_ENV_EXPORT(wlan_schedule_init);
