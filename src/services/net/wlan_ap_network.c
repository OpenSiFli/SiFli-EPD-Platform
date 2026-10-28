#include "network_internal.h"
#include "wlan_ap_network.h"

#if defined(RT_USING_WIFI) && defined(RT_USING_NETDEV) && defined(LWIP_USING_DHCPD) && \
    defined(RT_USING_LWIP) && !defined(RT_USING_LWIP141)
#include "network.h"
#include <lwip/ip_addr.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <netdev.h>
#include <dhcp_server.h>
#include <wlan_workqueue.h>

#ifndef DHCPD_SERVER_IP
#define DHCPD_SERVER_IP "192.168.169.1"
#endif

enum ap_operation { AP_START, AP_STOP, AP_CHECK };
struct ap_request
{
    struct rt_semaphore done, tcpip_done;
    enum ap_operation operation;
    rt_err_t result;
};

static bool managed;

static uint32_t ipv4(const ip_addr_t *address)
{
    return IP_IS_V4(address) ? ip4_addr_get_u32(ip_2_ip4(address)) : 0;
}

static uint32_t host_address(uint32_t address)
{
    const unsigned char *bytes = (const unsigned char *)&address;
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
        ((uint32_t)bytes[2] << 8) | bytes[3];
}

/* Addresses and masks are in network byte order. Zero/invalid STA masks
 * cannot establish that two configured subnets are disjoint. */
static rt_err_t subnet_check(uint32_t ap_address, uint32_t sta_address, uint32_t sta_mask)
{
    uint32_t mask = host_address(sta_mask), hosts = ~mask;
    if (!mask || (hosts & (hosts + 1u))) return -RT_EINVAL;
    uint32_t common = mask & 0xffffff00u;
    return (host_address(ap_address) & common) == (host_address(sta_address) & common)
        ? -RT_EBUSY : RT_EOK;
}

static rt_err_t conflict(struct netdev *sta, uint32_t ap_address)
{
    if (!sta || !netdev_is_up(sta) || !netdev_is_link_up(sta) || !ipv4(&sta->ip_addr))
        return RT_EOK;
    return subnet_check(ap_address, ipv4(&sta->ip_addr), ipv4(&sta->netmask));
}

/* NETDEV cannot clear its default pointer. Keep a non-AP object for SAL and
 * clear only the lwIP route if AP was selected. network.c chooses upstreams. */
static rt_err_t exclude_default(struct netdev *ap, struct netdev *sta)
{
    if (!ap) return RT_EOK;
    struct netif *previous = netif_default;
    rt_err_t result = RT_EOK;
    if (netdev_default == ap)
    {
        if (sta && sta != ap) netdev_set_default(sta);
        else result = -RT_ENOSYS;
        netif_set_default(previous == ap->user_data ? RT_NULL : previous);
    }
    else if (netif_default == ap->user_data)
        netif_set_default(RT_NULL);
    return result;
}

static rt_err_t set_value(struct netdev *ap, uint32_t value,
                          int (*setter)(struct netdev *, const ip_addr_t *))
{
    ip_addr_t address;
    ip_addr_set_ip4_u32(&address, value);
    return setter(ap, &address);
}

static void retain_error(rt_err_t *first, rt_err_t next)
{
    if (*first == RT_EOK) *first = next;
}

static rt_err_t disable_client(struct netdev *ap)
{
    return netdev_is_dhcp_enabled(ap) ? netdev_dhcp_enabled(ap, RT_FALSE) : RT_EOK;
}

static rt_err_t cleanup(struct netdev *ap)
{
    managed = false;
    if (!ap) return RT_EOK;
    if (!ap->user_data) return -RT_EIO;
    dhcpd_stop(ap->name);
    rt_err_t result = disable_client(ap);
    retain_error(&result, netdev_set_down(ap));
    retain_error(&result, set_value(ap, 0, netdev_set_ipaddr));
    retain_error(&result, set_value(ap, 0, netdev_set_gw));
    retain_error(&result, set_value(ap, 0, netdev_set_netmask));
    if (netdev_is_up(ap) || netdev_is_dhcp_enabled(ap) || ipv4(&ap->ip_addr) ||
        ipv4(&ap->netmask) || ipv4(&ap->gw)) retain_error(&result, -RT_EIO);
    return result;
}

static bool matches(struct netdev *ap, const ip_addr_t *address, const ip_addr_t *mask)
{
    return netdev_is_up(ap) && netdev_is_link_up(ap) && !netdev_is_dhcp_enabled(ap) &&
        ipv4(&ap->ip_addr) == ipv4(address) && ipv4(&ap->netmask) == ipv4(mask) &&
        !ipv4(&ap->gw);
}

static rt_err_t apply(enum ap_operation operation, struct netdev *ap, struct netdev *sta)
{
    if (operation == AP_STOP) return cleanup(ap);
    if (operation == AP_CHECK && !managed) return RT_EOK;
    if (!ap || !ap->user_data || !netdev_is_link_up(ap)) return -RT_EIO;

    ip_addr_t address, mask;
    if (!ipaddr_aton(DHCPD_SERVER_IP, &address) || !IP_IS_V4(&address) ||
        !ipaddr_aton("255.255.255.0", &mask)) return -RT_EINVAL;
    rt_err_t result = conflict(sta, ipv4(&address));
    if (result != RT_EOK) return result;
    if (managed && matches(ap, &address, &mask)) return RT_EOK;
    if (operation == AP_CHECK) return -RT_EIO;

    /* Reclaim the SDK's automatic server or a previous partial configuration. */
    dhcpd_stop(ap->name);
    result = disable_client(ap);
    if (result == RT_EOK) result = set_value(ap, 0, netdev_set_ipaddr);
    if (result == RT_EOK) result = netdev_set_netmask(ap, &mask);
    if (result == RT_EOK) result = set_value(ap, 0, netdev_set_gw);
    if (result == RT_EOK) result = netdev_set_ipaddr(ap, &address);
    if (result == RT_EOK) result = netdev_set_up(ap);
    if (result != RT_EOK) return result;
    dhcpd_start(ap->name);
    if (!matches(ap, &address, &mask)) return -RT_EIO;
    managed = true;
    return RT_EOK;
}

static void on_tcpip(void *parameter)
{
    struct ap_request *request = parameter;
    struct netdev *ap = netdev_get_by_name("w1");
    struct netdev *sta = netdev_get_by_name("w0");
    request->result = exclude_default(ap, sta);
    /* Stop must still reclaim DHCP/address state if route repair fails. */
    if (request->result == RT_EOK || request->operation == AP_STOP)
        retain_error(&request->result, apply(request->operation, ap, sta));
    if (request->result != RT_EOK && request->operation != AP_STOP)
        (void)cleanup(ap);
    retain_error(&request->result, exclude_default(ap, sta));
    network_changed();
    rt_sem_release(&request->tcpip_done);
}

static void after_wlan_jobs(void *parameter)
{
    struct ap_request *request = parameter;
    err_t error = tcpip_callback(on_tcpip, request);
    if (error == ERR_OK)
    {
        /* Hold the WLAN queue until raw DHCP operations finish, so a later SDK
         * link job cannot enter dhcpd_start concurrently with this callback. */
        while (rt_sem_take(&request->tcpip_done, RT_WAITING_FOREVER) != RT_EOK) {}
    }
    else request->result = error == ERR_MEM ? -RT_ENOMEM : -RT_ERROR;
    rt_sem_release(&request->done);
}

static rt_err_t execute(enum ap_operation operation)
{
    struct rt_workqueue *queue = rt_wlan_get_workqueue();
    if (!queue) return -RT_ENOSYS;
    if (rt_interrupt_get_nest() || rt_thread_self() == queue->work_thread ||
        rt_thread_self() == rt_thread_find(TCPIP_THREAD_NAME)) return -RT_EBUSY;
    struct ap_request request = {0};
    request.operation = operation;
    rt_err_t error = rt_sem_init(&request.done, "ap_done", 0, RT_IPC_FLAG_FIFO);
    if (error != RT_EOK) return error;
    error = rt_sem_init(&request.tcpip_done, "ap_ip", 0, RT_IPC_FLAG_FIFO);
    if (error == RT_EOK)
    {
        error = rt_wlan_workqueue_dowork(after_wlan_jobs, &request);
        if (error == RT_EOK)
        {
            while (rt_sem_take(&request.done, RT_WAITING_FOREVER) != RT_EOK) {}
            error = request.result;
        }
        rt_sem_detach(&request.tcpip_done);
    }
    rt_sem_detach(&request.done);
    return error;
}

rt_err_t wlan_ap_network_start(void) { return execute(AP_START); }
rt_err_t wlan_ap_network_stop(void) { return execute(AP_STOP); }
rt_err_t wlan_ap_network_check(void) { return execute(AP_CHECK); }

#else
rt_err_t wlan_ap_network_start(void) { return -RT_ENOSYS; }
#if defined(RT_USING_WIFI) && defined(LWIP_USING_DHCPD)
rt_err_t wlan_ap_network_stop(void) { return -RT_ENOSYS; }
#else
rt_err_t wlan_ap_network_stop(void) { return RT_EOK; }
#endif
rt_err_t wlan_ap_network_check(void) { return -RT_ENOSYS; }
#endif
