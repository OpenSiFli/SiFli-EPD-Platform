#ifndef EPD_WLAN_SERVICE_H
#define EPD_WLAN_SERVICE_H

#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "wlan_ipv4.h"

#define WLAN_SERVICE_SSID_BYTES 32
#define WLAN_SERVICE_PASSWORD_BYTES 64

typedef struct
{
    char ssid[WLAN_SERVICE_SSID_BYTES + 1];
    uint8_t ssid_length;
    uint8_t bssid[6];
    uint32_t security;
} wlan_network_id_t;

typedef struct
{
    wlan_network_id_t id;
    int16_t rssi, channel;
    bool secure, available, saved, connected, ready;
} wlan_network_t;

typedef enum
{
    WLAN_OP_NONE, WLAN_OP_ENABLE, WLAN_OP_SCAN, WLAN_OP_CONNECT,
    WLAN_OP_DISCONNECT, WLAN_OP_FORGET, WLAN_OP_AUTOCONNECT,
    WLAN_OP_POWERSAVE, WLAN_OP_IPV4, WLAN_OP_HOTSPOT_START,
    WLAN_OP_HOTSPOT_STOP, WLAN_OP_HOTSPOT_DEAUTH, WLAN_OP_SUSPEND,
    WLAN_OP_P2P
} wlan_operation_t;

typedef struct
{
    bool available, cancel_scan, cancel_connect, directed_scan, bssid_connect;
    bool hotspot, hotspot_hidden, hotspot_deauth, hotspot_clients, concurrent;
    bool powersave, ipv4, suspend, p2p;
    uint32_t sta_security, ap_security; /* WLAN_AUTH_* from the RT adapter. */
} wlan_capabilities_t;

typedef struct
{
    bool active, hidden, concurrent;
    char ssid[WLAN_SERVICE_SSID_BYTES + 1];
    unsigned channel, clients; /* Requested AP channel, not a live RF measurement. */
    uint32_t security;
} wlan_hotspot_t;

typedef struct
{
    bool available, enabled, automatic, busy, connected, ready, truncated;
    bool scanning, suspended;
    wlan_operation_t operation;
    int powersave_level;
    rt_err_t powersave_error, rssi_error;
    uint32_t revision;
    rt_err_t error;
    unsigned count;
    wlan_network_t current;
    wlan_network_t *networks;
    wlan_hotspot_t hotspot;
} wlan_snapshot_t;

/* Start once on the boot thread after storage and network_init(). Repeated
 * calls only ensure the service exists; they do not change its preference. */
rt_err_t wlan_service_start(bool enabled);
/* Commands are accepted asynchronously. Completion/error is in the snapshot;
 * SDK calls and filesystem work never execute on the UI caller. */
/* Enabling retries failed configuration loads without clearing credentials.
 * Successful auto-connect/IPv4 saves recover only their own configuration. */
rt_err_t wlan_service_enable(bool enabled);
rt_err_t wlan_service_scan(void);
rt_err_t wlan_service_scan_network(const wlan_network_id_t *id);
/* Cancellation is accepted without waiting on the SDK's management lock.
 * busy remains set until the underlying operation has returned. Cancelling a
 * connection also pauses automatic reconnect until an explicit connect,
 * auto-connect preference change or WLAN enable operation. */
rt_err_t wlan_service_cancel_scan(void);
rt_err_t wlan_service_cancel_connect(void);
rt_err_t wlan_service_capabilities(wlan_capabilities_t *out);
/* NULL reuses saved credentials. An empty string selects no password. */
/* Connect/forget retry unread credentials; persistent load failures require
 * file repair or explicit reset before credential writes are allowed. */
rt_err_t wlan_service_connect(const wlan_network_id_t *id, const char *password);
rt_err_t wlan_service_connect_ssid(const char *ssid, const char *password);
rt_err_t wlan_service_disconnect(void);
rt_err_t wlan_service_forget(const wlan_network_id_t *id);
rt_err_t wlan_service_set_autoconnect(bool enabled);
rt_err_t wlan_service_radio_state(bool *enabled, bool *connected);
/* Returns an owned copy, containing no credentials. Free even an empty copy. */
rt_err_t wlan_service_snapshot(wlan_snapshot_t *out);
void wlan_service_snapshot_free(wlan_snapshot_t *out);
bool wlan_network_id_equal(const wlan_network_id_t *a, const wlan_network_id_t *b);
/* STA only: 0 disables module low power; 1 enables clock gating. */
rt_err_t wlan_service_set_powersave(int level);
rt_err_t wlan_service_get_powersave(int *level);
rt_err_t wlan_service_set_ipv4(const wlan_ipv4_config_t *config);
rt_err_t wlan_service_get_ipv4(wlan_ipv4_state_t *state);
rt_err_t wlan_service_hotspot_start(const char *ssid, const char *password, unsigned channel);
rt_err_t wlan_service_hotspot_start_ex(const char *ssid, const char *password,
                                      unsigned channel, bool hidden);
rt_err_t wlan_service_hotspot_start_security(const char *ssid, const char *password,
                                            unsigned channel, bool hidden, uint32_t security);
rt_err_t wlan_service_hotspot_stop(void);
rt_err_t wlan_service_concurrent_start(const char *ssid, const char *password,
                                      unsigned channel, bool hidden, uint32_t security);
rt_err_t wlan_service_concurrent_stop(void);
rt_err_t wlan_service_hotspot_deauth(const uint8_t mac[6]);
rt_err_t wlan_service_hotspot_clients(uint8_t (*macs)[6], unsigned capacity, unsigned *count);

/* RT_EOK accepts asynchronous handoff. suspend_result() reports EBUSY only
 * while pending; a completed busy/failing hardware check aborts this sleep.
 * suspend() rejects an active AP or queued/running AP start with -RT_EBUSY,
 * without cancelling it or changing its epoch. The AP owner must explicitly
 * finish stopping the AP before shutdown can request this handoff. */
rt_err_t wlan_service_suspend(void);
rt_err_t wlan_service_suspend_result(void);
void wlan_service_resume(void);
/* Reset notifications are safe before startup and only wake the worker. */
rt_err_t wlan_service_reset_hold(void);
void wlan_service_resume_for_reset(void);
rt_err_t wlan_service_reset_begin(void);
rt_err_t wlan_service_reset_result(void);
/* Only the WLAN worker's reset persistence is exempt from the reset write gate. */
bool wlan_service_reset_write_allowed(void);

#endif
