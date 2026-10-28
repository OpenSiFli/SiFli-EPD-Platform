#ifndef WLAN_RADIO_POWER_H
#define WLAN_RADIO_POWER_H

#include <stdbool.h>
#include <rtthread.h>

/* The build must also link the SDIO/DMA wrappers used by radio_power.c. */
#if defined(WLAN_RADIO_SDIO_GUARD) && defined(SF32LB57X) && !defined(CONFIG_OOB)
#define WLAN_RADIO_DEEP_SLEEP_SUPPORTED 1
#else
#define WLAN_RADIO_DEEP_SLEEP_SUPPORTED 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Synchronous host-driver CLOSE/OPEN, for the RT-WLAN adapter's worker only.
 * OPEN restores STA mode. The adapter must serialize other driver operations
 * with this call and publish NONE/STATION only after RT_EOK.
 *
 * Completion means the official host-driver lifecycle has completed. DEEP
 * additionally requires wlan_radio_prepare_deep() to stop the SDIO clock and
 * lower the module power-enable pin after closing the host driver.
 * Before reopening an observed SDIO card, reset the module through the SDK
 * port and restore the same card/function objects with RT SDIO commands.
 * A reselect failure prevents OPEN. The SDK power-pin helpers run only after
 * the host bus has drained; the driver's PD/PU operations are not submitted.
 *
 * A timeout leaves the SDK operation running. A retry waits for that operation
 * before issuing another one. Missing management task: -RT_ENOSYS; inconsistent
 * completion: -RT_EIO; overlapping calls: -RT_EBUSY. No task is created here.
 */
rt_err_t wlan_radio_set_enabled(bool enabled);

/* Last confirmed host-driver state; refreshes when the SDK operation finishes.
 * This value is not an electrical power or RF activity measurement. */
bool wlan_radio_is_enabled(void);

/* Live command readiness, false throughout shutdown, reset and partial init. */
bool wlan_radio_is_ready(void);

/*
 * Worker-only DEEP handoff after a successful CLOSE. Requires real detach,
 * no unfinished/erroring SDIO transfers, no IRQ thread, idle controller and
 * verified IRQ masking, followed by SDIO clock stop and SDK module power-down.
 * The board must provide a directly controlled power pin with low readback.
 * Success blocks further WiFi bus requests until OPEN;
 * OPEN restores the saved NVIC enable state before starting the SDK operation.
 * The adapter must serialize this call, resume and all other driver controls.
 *
 * Build with WLAN_RADIO_SDIO_GUARD and linker options:
 * --wrap=mmcsd_send_cmd --wrap=mmcsd_send_request --wrap=sdio_detach_irq
 * --wrap=HAL_DMA_PollForTransfer
 * Missing observations/unsupported host: -RT_ENOSYS; active resources: -RT_EBUSY;
 * transfer/detach failure: -RT_EIO; lifecycle timeout: -RT_ETIMEOUT.
 * A new OPEN/CLOSE can retry a non-DMA failure. A failed data transfer leaves
 * DMA completion unknown until system reinitialization; this API does not
 * recover or reset DMA. Capability means this check exists, not blanket DEEP.
 */
rt_err_t wlan_radio_prepare_deep(void);

#ifdef __cplusplus
}
#endif

#endif
