#ifndef EPD_NETWORK_INTERNAL_H
#define EPD_NETWORK_INTERNAL_H

#include "network.h"

rt_err_t network_init(void);

void network_changed(void);
/* Adapter registration: SDK two-character lwIP device name, e.g. b0 or w0.
 * NETWORK_WIFI_AP is always local-only and cannot be selected as upstream. */
rt_err_t network_register_interface(network_source_t source, const char *name);
/* Register a local-only interface (e.g. SoftAP) for address queries without
 * making it eligible as the system's upstream/default route. */
rt_err_t network_register_local_interface(network_source_t source, const char *name);

/* Supply per-interface DNS in IPv4 network byte order. With NETDEV, the next
 * tcpip-thread sample forwards values to that interface's SDK setter, retaining
 * its duplicate-value handling; snapshots report the stored result. NETDEV DHCP
 * updates DNS directly and needs no call here. Without NETDEV, zero clears the
 * corresponding cached DNS slot. */
void network_set_dns(network_source_t source, uint32_t primary, uint32_t secondary);

#endif
