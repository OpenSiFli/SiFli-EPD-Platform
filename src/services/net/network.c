#include "network_internal.h"
#include "network.h"
#include "epd_memory.h"
#include "bt_pan.h"
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <lwip/dns.h>
#ifdef RT_USING_NETDEV
#include <netdev.h>
#endif
#include <rtm.h>
#include <string.h>

typedef struct network_subscription
{
    struct network_subscription *next;
    rt_event_t event;
    uint32_t bits, token;
} network_subscription_t;
static struct rt_event wakeup;
static struct rt_mutex lock;
static struct rt_semaphore sampled;
static rt_thread_t worker;
static network_snapshot_t state, candidate;
static char interfaces[NETWORK_WIFI_AP + 1][NETIF_NAMESIZE] = {"", "b0", "", ""};
static bool routable[NETWORK_WIFI_AP + 1] = {false, true, true, false};
static network_interface_snapshot_t interface_state[NETWORK_WIFI_AP + 1];
static network_subscription_t *subscribers;
static uint32_t next_token;
static ip_addr_t resolvers[NETWORK_WIFI_AP + 1][DNS_MAX_SERVERS];
static bool dns_valid[NETWORK_WIFI_AP + 1];
#ifdef RT_USING_NETDEV
static uint32_t requested_dns[NETWORK_WIFI_AP + 1][2];
static bool dns_pending[NETWORK_WIFI_AP + 1];
#endif
static uint32_t epochs[NETWORK_WIFI_AP + 1], sampled_epoch, active_epoch;
NETIF_DECLARE_EXT_CALLBACK(netif_listener);

void network_changed(void)
{
    if (worker) rt_event_send(&wakeup, 1);
}

#ifdef RT_USING_NETDEV
static void netdev_dns_changed(struct netdev *device, enum netdev_cb_type type)
{
    (void)device;
    if (type == NETDEV_CB_ADDR_DNS_SERVER) network_changed();
}

/* Called on tcpip_thread with lock held. */
static void sample_netdev_dns(network_source_t source, struct netdev *device)
{
    bool valid = false, changed = false;
    if (device && routable[source])
    {
        if (!device->addr_callback) netdev_set_addr_callback(device, netdev_dns_changed);
        if (dns_pending[source])
        {
            bool applied = true;
            for (unsigned i = 0; i < NETDEV_DNS_SERVERS_NUM; ++i)
            {
                ip_addr_t address;
                ip_addr_set_ip4_u32(&address, i < 2 ? requested_dns[source][i] : 0);
                if (netdev_set_dns_server(device, (uint8_t)i, &address) != RT_EOK)
                    applied = false;
            }
            dns_pending[source] = !applied;
        }
    }
    for (unsigned i = 0; i < DNS_MAX_SERVERS; ++i)
    {
        ip_addr_t address;
        ip_addr_set_ip4_u32(&address, 0);
        if (device && routable[source] && i < NETDEV_DNS_SERVERS_NUM)
            address = device->dns_servers[i];
        if (IP_IS_V4(&address) && !ip4_addr_isany_val(*ip_2_ip4(&address))) valid = true;
        if (!ip_addr_cmp(&resolvers[source][i], &address)) changed = true;
        resolvers[source][i] = address;
    }
    if (changed || dns_valid[source] != valid) ++epochs[source];
    dns_valid[source] = valid;
}
#endif

static void netif_changed(struct netif *netif, netif_nsc_reason_t reason,
                          const netif_ext_callback_args_t *args)
{
    (void)args;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    for (unsigned source = NETWORK_PAN; source <= NETWORK_WIFI_AP; ++source)
    {
        if (!interfaces[source][0] || netif->name[0] != interfaces[source][0] ||
            netif->name[1] != interfaces[source][1]) continue;
        ++epochs[source];
        if (reason & (LWIP_NSC_NETIF_REMOVED | LWIP_NSC_LINK_CHANGED)) dns_valid[source] = false;
#ifdef RT_USING_NETDEV
        if (reason & LWIP_NSC_NETIF_REMOVED) dns_pending[source] = false;
#else
        if (routable[source] && (reason & LWIP_NSC_IPV4_SETTINGS_CHANGED) &&
            !ip4_addr_isany_val(*netif_ip4_addr(netif)))
        {
            /* This SDK processes DHCP DNS options before publishing the lease. */
            for (unsigned i = 0; i < DNS_MAX_SERVERS; ++i) resolvers[source][i] = *dns_getserver(i);
            dns_valid[source] = true;
        }
#endif
    }
    rt_mutex_release(&lock);
    /* Query later on tcpip_thread: DOWN/REMOVE callbacks precede the change. */
    network_changed();
}

static void sample(void *parameter)
{
    if (parameter) netif_add_ext_callback(&netif_listener, netif_changed);
    memset(&candidate, 0, sizeof(candidate));
    sampled_epoch = 0;
    struct netif *chosen = NULL;
#ifdef RT_USING_NETDEV
    struct netdev *netdevices[NETWORK_WIFI_AP + 1] = {NULL};
    struct netdev *chosen_netdev = NULL;
#endif
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    struct netif *devices[NETWORK_WIFI_AP + 1] = {NULL};
    for (unsigned source = NETWORK_PAN; source <= NETWORK_WIFI_AP; ++source)
    {
        network_interface_snapshot_t *info = &interface_state[source];
        memset(info, 0, sizeof(*info));
        if (!interfaces[source][0]) continue;
        info->registered = true;
        memcpy(info->name, interfaces[source], sizeof(info->name));
        struct netif *netif;
        NETIF_FOREACH(netif)
            if (netif->name[0] == interfaces[source][0] && netif->name[1] == interfaces[source][1]) break;
        if (!netif) continue;
        devices[source] = netif;
#ifdef RT_USING_NETDEV
        struct netdev *device = netdev_get_by_name(interfaces[source]);
        if (device && device->user_data != netif) device = NULL;
        netdevices[source] = device;
        sample_netdev_dns((network_source_t)source, device);
#endif
        info->present = true;
        info->up = netif_is_up(netif) != 0;
        info->link_up = netif_is_link_up(netif) != 0;
        info->address = ip4_addr_get_u32(netif_ip4_addr(netif));
        info->netmask = ip4_addr_get_u32(netif_ip4_netmask(netif));
        info->gateway = ip4_addr_get_u32(netif_ip4_gw(netif));
        info->mtu = netif->mtu;
        if (netif->hwaddr_len == sizeof(info->mac)) memcpy(info->mac, netif->hwaddr, sizeof(info->mac));
        info->ready = info->up && info->link_up && info->address != 0;
        info->dns_valid = info->ready && dns_valid[source];
        if (info->dns_valid)
            for (unsigned n = 0; n < 2 && n < DNS_MAX_SERVERS; ++n)
                if (IP_IS_V4(&resolvers[source][n]))
                    info->dns[n] = ip4_addr_get_u32(ip_2_ip4(&resolvers[source][n]));
    }
    /* Select an established upstream; route selection never starts a connection. */
    const network_source_t order[] = {NETWORK_WIFI, NETWORK_PAN};
    for (unsigned i = 0; i < 2; ++i)
    {
        network_source_t source = order[i];
        if (!routable[source] || !interfaces[source][0] ||
            (source == NETWORK_PAN && !btpan_is_network_ready())) continue;
        /* SDK device names already contain a digit. Match those two bytes,
         * independently of lwIP's separately allocated netif->num. */
        struct netif *netif = devices[source];
        if (!netif || !netif_is_up(netif) || !netif_is_link_up(netif) ||
            ip4_addr_isany_val(*netif_ip4_addr(netif)) || ip4_addr_isany_val(*netif_ip4_gw(netif))) continue;
#ifdef RT_USING_NETDEV
        if (!netdevices[source]) continue;
        chosen_netdev = netdevices[source];
#endif
        chosen = netif;
        candidate.ready = true;
        candidate.source = source;
        candidate.address = ip4_addr_get_u32(netif_ip4_addr(netif));
        sampled_epoch = epochs[source];
#ifndef RT_USING_NETDEV
        if (dns_valid[source])
            for (unsigned n = 0; n < DNS_MAX_SERVERS; ++n) dns_setserver(n, &resolvers[source][n]);
#endif
        break;
    }
    rt_mutex_release(&lock);
#ifdef RT_USING_NETDEV
    if (chosen_netdev) netdev_set_default(chosen_netdev);
    /* The SDK cannot clear netdev_default and its DNS code dereferences it.
     * With no upstream, retain that object but remove the lwIP default route. */
#endif
    if (netif_default != chosen) netif_set_default(chosen);
    rt_sem_release(&sampled);
}

static void network_worker(void *parameter)
{
    (void)parameter;
    bool first = true;
    for (;;)
    {
        if (tcpip_callback(sample, first ? &state : NULL) == ERR_OK)
        {
            rt_sem_take(&sampled, RT_WAITING_FOREVER);
            first = false;
            rt_mutex_take(&lock, RT_WAITING_FOREVER);
            if (state.ready != candidate.ready || state.source != candidate.source ||
                state.address != candidate.address || active_epoch != sampled_epoch)
            {
                candidate.generation = state.generation + 1;
                state = candidate;
                active_epoch = sampled_epoch;
                for (network_subscription_t *s = subscribers; s; s = s->next)
                    rt_event_send(s->event, s->bits);
            }
            rt_mutex_release(&lock);
        }
        else { rt_thread_mdelay(100); continue; }
        uint32_t events;
        rt_event_recv(&wakeup, 1, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &events);
    }
}

rt_err_t network_init(void)
{
    if (worker) return RT_EOK;
    rt_mutex_init(&lock, "net_sub", RT_IPC_FLAG_PRIO);
    rt_event_init(&wakeup, "net_w", RT_IPC_FLAG_FIFO);
    rt_sem_init(&sampled, "net_q", 0, RT_IPC_FLAG_FIFO);
    worker = epd_thread_create_psram("network", network_worker, NULL, 2048, 22, 10);
    if (worker && rt_thread_startup(worker) == RT_EOK) return RT_EOK;
    if (worker) rt_thread_delete(worker);
    worker = NULL;
    rt_sem_detach(&sampled);
    rt_event_detach(&wakeup);
    rt_mutex_detach(&lock);
    return -RT_ENOMEM;
}

void network_snapshot(network_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!worker) return;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    *out = state;
    rt_mutex_release(&lock);
}
RTM_EXPORT(network_snapshot);
bool network_ready(void) { network_snapshot_t value; network_snapshot(&value); return value.ready; }
RTM_EXPORT(network_ready);

static rt_err_t register_interface(network_source_t source, const char *name, bool upstream)
{
    if (!worker || source < NETWORK_PAN || source > NETWORK_WIFI_AP || !name ||
        (*name && strlen(name) != 2))
        return -RT_EINVAL;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    strcpy(interfaces[source], name);
    routable[source] = upstream && source != NETWORK_WIFI_AP;
    memset(&interface_state[source], 0, sizeof(interface_state[source]));
    dns_valid[source] = false;
#ifdef RT_USING_NETDEV
    dns_pending[source] = false;
#endif
    ++epochs[source];
    rt_mutex_release(&lock);
    network_changed();
    return RT_EOK;
}

rt_err_t network_register_interface(network_source_t source, const char *name)
{ return register_interface(source, name, true); }

rt_err_t network_register_local_interface(network_source_t source, const char *name)
{ return register_interface(source, name, false); }

rt_err_t network_interface_snapshot(network_source_t source, network_interface_snapshot_t *out)
{
    if (!out) return -RT_EINVAL;
    memset(out, 0, sizeof(*out));
    if (source < NETWORK_PAN || source > NETWORK_WIFI_AP) return -RT_EINVAL;
    if (!worker) return -RT_ENOSYS;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    *out = interface_state[source];
    rt_mutex_release(&lock);
    return RT_EOK;
}
RTM_EXPORT(network_interface_snapshot);

void network_set_dns(network_source_t source, uint32_t primary, uint32_t secondary)
{
    if (!worker || source < NETWORK_PAN || source > NETWORK_WIFI) return;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
#ifdef RT_USING_NETDEV
    requested_dns[source][0] = primary;
    requested_dns[source][1] = secondary;
    dns_pending[source] = true;
#else
    for (unsigned i = 0; i < DNS_MAX_SERVERS; ++i)
        ip_addr_set_ip4_u32(&resolvers[source][i], i == 0 ? primary : i == 1 ? secondary : 0);
    dns_valid[source] = true;
#endif
    ++epochs[source];
    rt_mutex_release(&lock);
    network_changed();
}

uint32_t network_subscribe(rt_event_t event, uint32_t bits)
{
    if (!worker || !event || !bits) return 0;
    network_subscription_t *s = rt_malloc(sizeof(*s));
    if (!s) return 0;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    if (next_token == UINT32_MAX)
    {
        rt_mutex_release(&lock);
        rt_free(s);
        return 0;
    }
    uint32_t token = ++next_token;
    *s = (network_subscription_t){subscribers, event, bits, token};
    subscribers = s;
    rt_event_send(event, bits); /* Initial snapshot, including already-online. */
    rt_mutex_release(&lock);
    return token;
}
RTM_EXPORT(network_subscribe);

void network_unsubscribe(uint32_t token)
{
    if (!worker || !token) return;
    rt_mutex_take(&lock, RT_WAITING_FOREVER);
    network_subscription_t **link = &subscribers;
    while (*link && (*link)->token != token) link = &(*link)->next;
    network_subscription_t *removed = *link;
    if (removed) *link = removed->next;
    rt_mutex_release(&lock);
    rt_free(removed);
}
RTM_EXPORT(network_unsubscribe);
