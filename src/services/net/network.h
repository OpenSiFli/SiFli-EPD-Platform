#ifndef EPD_NETWORK_H
#define EPD_NETWORK_H
#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { NETWORK_AUTO, NETWORK_PAN, NETWORK_WIFI, NETWORK_WIFI_AP } network_source_t;
typedef struct
{
    bool ready;
    network_source_t source;
    uint32_t generation;
    uint32_t address; /* IPv4, network byte order. */
} network_snapshot_t;

typedef struct
{
    bool registered, present, up, link_up, ready, dns_valid;
    char name[3]; /* SDK's two-character Ethernet device name. */
    uint8_t mac[6];
    uint16_t mtu;
    uint32_t address, netmask, gateway, dns[2]; /* IPv4 network byte order. */
} network_interface_snapshot_t;

/* Thread context. Snapshots are copied into caller-owned storage.
 * The default route uses ready WLAN first, then an already-connected PAN. */
void network_snapshot(network_snapshot_t *out);
bool network_ready(void);
/* Last sample of the requested source, even without a lease or default-route
 * selection. RT_EOK means a snapshot was returned, not that it is connected.
 * ready describes IP/link readiness, not Internet reachability. dns_valid
 * requires a ready interface and at least one nonzero IPv4 DNS server.
 * Local-only interfaces do not inherit DNS from the Internet connection. */
rt_err_t network_interface_snapshot(network_source_t source, network_interface_snapshot_t *out);
/* No application callback executes on the network thread. The event must remain
 * alive until unsubscribe returns; that return drains in-flight event sends. */
uint32_t network_subscribe(rt_event_t event, uint32_t bits);
void network_unsubscribe(uint32_t subscription);
#endif
