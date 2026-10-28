#ifndef EPD_WLAN_AP_NETWORK_H
#define EPD_WLAN_AP_NETWORK_H

#include <rtthread.h>

/* Call serially on the WLAN service worker, after the RT AP start/stop call
 * returns, never from an event callback. Calls wait for preceding WLAN work
 * and finish on tcpip_thread. w0 is STA; w1 is the local-only AP netdev.
 *
 * start: configure DHCPD_SERVER_IP/24 (default 192.168.169.1), gateway zero,
 *        and invoke the SDK DHCP server. Repeated successful starts keep leases.
 * stop:  stop DHCP, disable the AP netdev and clear its IPv4 configuration,
 *        including a server started automatically by the SDK. Idempotent.
 * check: while managed, validate link/address and detect newly overlapping STA
 *        subnets. Call after STA address changes and periodically while AP is on.
 *
 * -RT_EBUSY means STA/AP subnet overlap (or an invalid calling thread).
 * -RT_EIO means missing/down AP or address state mismatch; -RT_EINVAL means
 * an invalid IPv4 configuration. Failures during start/check clear the AP
 * network where possible; the caller must stop the AP radio and preserve STA.
 * Missing NETDEV/raw DHCP support returns -RT_ENOSYS for start/check. Stop
 * succeeds without an AP netdev, or in builds without Wi-Fi/DHCP server;
 * unsupported builds that still contain the server return -RT_ENOSYS.
 *
 * RT_EOK confirms address configuration and completion of the DHCP API call.
 * The SDK's void dhcpd_start/stop APIs cannot report server allocation/bind
 * failures or prove that a client can obtain a lease. No DHCP status is inferred.
 */
rt_err_t wlan_ap_network_start(void);
rt_err_t wlan_ap_network_stop(void);
rt_err_t wlan_ap_network_check(void);

#endif
