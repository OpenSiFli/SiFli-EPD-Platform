#ifndef NETWORK_RESET_H
#define NETWORK_RESET_H

#include <rtthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    NETWORK_RESET_IDLE,
    NETWORK_RESET_WAIT_STACK,
    NETWORK_RESET_DISCONNECTING,
    NETWORK_RESET_CLEARING,
    NETWORK_RESET_SUCCEEDED,
    NETWORK_RESET_FAILED,
} network_reset_state_t;

typedef enum
{
    NETWORK_RESET_NOT_CONFIGURED,
    NETWORK_RESET_PENDING,
    NETWORK_RESET_CONFIRMED,
    NETWORK_RESET_UNCONFIRMED,
    NETWORK_RESET_COMPONENT_FAILED,
} network_reset_component_t;

typedef enum
{
    NETWORK_RESET_ERROR_NONE,
    NETWORK_RESET_ERROR_TIMEOUT,
    NETWORK_RESET_ERROR_CLASSIC_UNCONFIRMED,
    NETWORK_RESET_ERROR_CLASSIC_STORAGE,
    NETWORK_RESET_ERROR_BLE_STORAGE,
    NETWORK_RESET_ERROR_BLE_PENDING,
    NETWORK_RESET_ERROR_WLAN_UNSUPPORTED,
    NETWORK_RESET_ERROR_SDK_UNSUPPORTED,
    NETWORK_RESET_ERROR_CONTROL,
    NETWORK_RESET_ERROR_WLAN_STORAGE,
} network_reset_error_t;

typedef struct
{
    network_reset_state_t state;
    network_reset_error_t error;
    network_reset_component_t classic_bonds;
    network_reset_component_t classic_targets;
    network_reset_component_t ble_bonds;
    network_reset_component_t wlan_credentials;
    bool paused;
    unsigned classic_store_status;
    unsigned ble_store_status;
} network_reset_status_t;

/* Pause new pairing and automatic reconnect without deleting credentials or
 * starting a reset deadline. May precede btpan_init on the boot thread.
 * Repeated holds are idempotent and do not replace an active reset's status.
 * A worker wakeup error is returned with the policy gate still held. */
rt_err_t network_reset_hold(void);

/* Call from thread context. RT_EOK means accepted, not completed. Concurrent
 * resets return -RT_EBUSY. Both wireless reset and factory reset use this API.
 * May precede btpan_init() on the boot thread: the pause survives PAN init.
 * Initialization itself must be serialized with these pre-init calls.
 * The operation has a 30-second deadline including readiness and verification.
 * get_status() also enforces the deadline if btpan_init() never runs.
 * Preferences are retained; no UI or settings store is accessed.
 * Success/failure both retain the pause until explicit resume. Only SUCCEEDED
 * permits a caller to commit a persistent reset transaction as complete.
 *
 * Classic erase is synchronous in the SDK library; success additionally waits
 * for its flash-backed paired-record query to report zero and verifies the
 * persisted CM reconnect table. BLE deletion must report persistence success.
 * With RT_USING_WIFI and RT_WLAN_DEVICE_STA_NAME, the WLAN worker persists
 * an empty SDK credential cache and the default global auto-connect policy.
 * WIFI without that adapter fails WLAN_UNSUPPORTED; without WIFI it reports
 * NOT_CONFIGURED. WLAN persistence failure prevents successful completion.
 */
rt_err_t network_reset_start(void);

/* Thread-safe snapshot; NULL is ignored. Poll approximately every 100 ms.
 * Hardware commands and storage work run in the existing PAN worker. */
void network_reset_get_status(network_reset_status_t *status);

/* Release a hold or terminal reset's application policy gate using current PAN/BT
 * preferences. This does not restore deleted credentials or old peer targets.
 * -RT_EBUSY while running or a Classic verification reply is outstanding;
 * -RT_ENOSYS if asynchronous BLE persistence cannot be correlated. A failed
 * transaction must remain pending even if the caller explicitly resumes.
 * Repeated calls after a successful release return RT_EOK. */
rt_err_t network_reset_resume(void);

#ifdef __cplusplus
}
#endif
#endif
