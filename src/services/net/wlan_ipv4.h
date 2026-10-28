#ifndef EPD_WLAN_IPV4_H
#define EPD_WLAN_IPV4_H

#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool dhcp;
    uint32_t address, netmask, gateway, dns[2]; /* IPv4, network byte order. */
} wlan_ipv4_config_t;

typedef struct
{
    wlan_ipv4_config_t config; /* Persisted preference, independent of the lease. */
    bool loaded, present, up, link_up, ready, dhcp, pending;
    uint32_t address, netmask, gateway, dns[2]; /* Last w0 sample, network byte order. */
    rt_err_t error; /* Last load/save/apply error; get() itself only copies memory. */
    rt_err_t dns_error; /* DNS setter/readback diagnostic; does not block DHCP. */
} wlan_ipv4_state_t;

/* Call load/save/apply/reset serially on the WLAN service worker, after storage
 * and network_init(). They must not run on tcpip_thread, the WLAN workqueue,
 * or from an SDK event callback. No new thread is created.
 *
 * load() reads /flash/config/wlan-ipv4.bin without applying it. A missing file
 * selects DHCP. Unavailable storage returns -RT_EBUSY for a later retry.
 * Recovery/stat/read failures on available storage and invalid records return
 * -RT_EIO and require explicit reload/save; accepted preferences are retained.
 * save_and_apply() validates and persists before applying; a failed save keeps
 * the previous preference. A failed apply retains the saved preference for retry.
 * DHCP ignores and clears all supplied address/DNS fields. Static IPv4 accepts
 * /1 through /30, unicast host addresses, an optional same-subnet gateway, and
 * optional unicast DNS servers (zero means an empty slot).
 *
 * apply() waits for preceding SDK WLAN work, then uses NETDEV on tcpip_thread.
 * Call on CONNECT/READY/DISCONNECT and periodically, including while offline.
 * Missing/down w0 is deferred with RT_EOK and pending=true. A stable interface
 * is only sampled; DHCP is not restarted. ready means link + IPv4 address,
 * not Internet reachability. Route selection remains owned by network.c.
 * When switching to DHCP, DNS-clear failure is recorded in dns_error while
 * DHCP starts normally. The next nonzero DHCP address clears that diagnostic;
 * DNS slots remain the actual NETDEV values, including any retained old slot.
 * reset() saves DHCP defaults, with the same application/defer semantics.
 */
rt_err_t wlan_ipv4_load(void);
rt_err_t wlan_ipv4_save_and_apply(const wlan_ipv4_config_t *config);
rt_err_t wlan_ipv4_apply(void);
/* Thread-safe, allocation-free, filesystem-free and NETDEV-free snapshot.
 * RT_EOK returns the snapshot even when state.error reports an earlier failure.
 * Before the first successful load/save, loaded=false and pending=true. */
rt_err_t wlan_ipv4_get(wlan_ipv4_state_t *state);
rt_err_t wlan_ipv4_reset(void);

#endif
