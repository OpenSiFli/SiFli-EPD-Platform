#include "network_internal.h"
#include "wlan_ipv4.h"
#include <rthw.h>
#include <string.h>

static wlan_ipv4_state_t published = {.config = {.dhcp = true}, .pending = true};

static rt_err_t record_error(rt_err_t error)
{
    rt_base_t level = rt_hw_interrupt_disable();
    published.error = error;
    rt_hw_interrupt_enable(level);
    return error;
}

rt_err_t wlan_ipv4_get(wlan_ipv4_state_t *state)
{
    if (!state) return -RT_EINVAL;
    rt_base_t level = rt_hw_interrupt_disable();
    *state = published;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN) && defined(RT_USING_NETDEV)
#include "network.h"
#include "storage_file.h"
#include <dfs_posix.h>
#include <errno.h>
#include <lwip/ip_addr.h>
#include <lwip/tcpip.h>
#include <netdev.h>
#include <ipc/workqueue.h>
#include <wlan_workqueue.h>

#define IPV4_PATH STORAGE_FLASH_ROOT "/config/wlan-ipv4.bin"
#define IPV4_MAGIC 0x57495034u
#define IPV4_VERSION 1u

/* Fixed-width fields avoid persisting C bool representation or struct padding. */
typedef struct
{
    uint32_t dhcp, address, netmask, gateway, dns[2];
} ipv4_record_t;

static uint32_t link_epoch, applied_epoch, stopped_epoch;
static netdev_callback_fn previous_status_callback;
static struct rt_work apply_work;
static uint32_t config_revision;
static bool apply_initialized, apply_queued;
static bool dhcp_transition;

static uint32_t host_address(uint32_t address)
{
    const unsigned char *bytes = (const unsigned char *)&address;
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
        ((uint32_t)bytes[2] << 8) | bytes[3];
}

static bool unicast(uint32_t address)
{
    unsigned first = (unsigned)(address >> 24);
    return first != 0 && first != 127 && first < 224;
}

static rt_err_t normalize(const wlan_ipv4_config_t *input, wlan_ipv4_config_t *output)
{
    if (!input) return -RT_EINVAL;
    memset(output, 0, sizeof(*output));
    output->dhcp = input->dhcp;
    if (input->dhcp) return RT_EOK;
    uint32_t address = host_address(input->address), mask = host_address(input->netmask);
    uint32_t gateway = host_address(input->gateway), hosts = ~mask;
    if (!mask || hosts < 3 || (hosts & (hosts + 1)) || !unicast(address) ||
        !(address & hosts) || (address & hosts) == hosts) return -RT_EINVAL;
    if (gateway && (!unicast(gateway) || gateway == address ||
        (gateway & mask) != (address & mask) || !(gateway & hosts) ||
        (gateway & hosts) == hosts)) return -RT_EINVAL;
    for (unsigned i = 0; i < 2; ++i)
        if (input->dns[i] && !unicast(host_address(input->dns[i]))) return -RT_EINVAL;
    output->address = input->address;
    output->netmask = input->netmask;
    output->gateway = input->gateway;
    memcpy(output->dns, input->dns, sizeof(output->dns));
    return RT_EOK;
}

static bool equal_config(const wlan_ipv4_config_t *left, const wlan_ipv4_config_t *right)
{
    return left->dhcp == right->dhcp && left->address == right->address &&
        left->netmask == right->netmask && left->gateway == right->gateway &&
        left->dns[0] == right->dns[0] && left->dns[1] == right->dns[1];
}

static void accept_config(const wlan_ipv4_config_t *config)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool changed = !published.loaded || !equal_config(&published.config, config);
    if (changed && config->dhcp && !published.config.dhcp) dhcp_transition = true;
    if (!config->dhcp) dhcp_transition = false;
    if (changed) ++config_revision;
    published.config = *config;
    published.loaded = true;
    published.pending = published.pending || changed;
    published.error = RT_EOK;
    rt_hw_interrupt_enable(level);
}

rt_err_t wlan_ipv4_load(void)
{
    if (!storage_path_available(IPV4_PATH)) return record_error(-RT_EBUSY);
    if (!storage_file_recover(IPV4_PATH)) return record_error(-RT_EIO);
    struct stat info;
    wlan_ipv4_config_t config = {.dhcp = true};
    if (stat(IPV4_PATH, &info) == 0)
    {
        ipv4_record_t record;
        if (!storage_record_load(IPV4_PATH, IPV4_MAGIC, IPV4_VERSION, &record, sizeof(record)) ||
            record.dhcp > 1)
            return record_error(storage_path_available(IPV4_PATH) ? -RT_EIO : -RT_EBUSY);
        wlan_ipv4_config_t input = {.dhcp = record.dhcp != 0, .address = record.address,
            .netmask = record.netmask, .gateway = record.gateway, .dns = {record.dns[0], record.dns[1]}};
        if (normalize(&input, &config) != RT_EOK) return record_error(-RT_EIO);
    }
    else if (errno != ENOENT && errno != -ENOENT) return record_error(-RT_EIO);
    accept_config(&config);
    return RT_EOK;
}

static uint32_t ip4_value(const ip_addr_t *address)
{
    return IP_IS_V4(address) ? ip4_addr_get_u32(ip_2_ip4(address)) : 0;
}

static void sample(struct netdev *device, wlan_ipv4_state_t *state)
{
    state->present = device != RT_NULL;
    state->up = device && netdev_is_up(device);
    state->link_up = device && netdev_is_link_up(device);
    state->dhcp = device && netdev_is_dhcp_enabled(device);
    state->address = device ? ip4_value(&device->ip_addr) : 0;
    state->netmask = device ? ip4_value(&device->netmask) : 0;
    state->gateway = device ? ip4_value(&device->gw) : 0;
    for (unsigned i = 0; i < 2; ++i)
        state->dns[i] = device && i < NETDEV_DNS_SERVERS_NUM ? ip4_value(&device->dns_servers[i]) : 0;
    state->ready = state->up && state->link_up && state->address != 0;
}

static void status_changed(struct netdev *device, enum netdev_cb_type type)
{
    if (type == NETDEV_CB_STATUS_LINK_UP || type == NETDEV_CB_STATUS_LINK_DOWN ||
        type == NETDEV_CB_STATUS_DOWN)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        ++link_epoch;
        rt_hw_interrupt_enable(level);
    }
    if (previous_status_callback) previous_status_callback(device, type);
}

static uint32_t observe_link(struct netdev *device)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (device->status_callback != status_changed)
    {
        previous_status_callback = device->status_callback;
        ++link_epoch;
        netdev_set_status_callback(device, status_changed);
    }
    uint32_t epoch = link_epoch;
    rt_hw_interrupt_enable(level);
    return epoch;
}

static rt_err_t set_address(struct netdev *device, uint32_t value,
                            int (*setter)(struct netdev *, const ip_addr_t *))
{
    ip_addr_t address;
    ip_addr_set_ip4_u32(&address, value);
    return setter(device, &address);
}

static rt_err_t set_dns_slot(struct netdev *device, unsigned slot, uint32_t value)
{
    ip_addr_t address;
    ip_addr_set_ip4_u32(&address, value);
    return netdev_set_dns_server(device, (uint8_t)slot, &address);
}

static rt_err_t set_dns(struct netdev *device, const uint32_t desired[2])
{
#if NETDEV_DNS_SERVERS_NUM < 2
    return -RT_ENOSYS;
#endif
    /* Retry in slot order because older NETDEV setters reject a value already
     * present in another slot. Most replacements converge in two passes. */
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < 2; ++i)
            if (ip4_value(&device->dns_servers[i]) != desired[i])
            {
                rt_err_t error = set_dns_slot(device, i, desired[i]);
                if (error != RT_EOK) return error;
            }
    if (ip4_value(&device->dns_servers[0]) == desired[0] &&
        ip4_value(&device->dns_servers[1]) == desired[1]) return RT_EOK;
    /* A swap of two nonzero servers can use an empty slot as temporary space. */
    if (desired[0] && desired[1] && desired[0] != desired[1] &&
        ip4_value(&device->dns_servers[0]) == desired[1] &&
        ip4_value(&device->dns_servers[1]) == desired[0])
    {
        rt_err_t error = set_dns_slot(device, 0, 0);
        if (error == RT_EOK) error = set_dns_slot(device, 1, desired[1]);
        if (error == RT_EOK) error = set_dns_slot(device, 0, desired[0]);
        if (error != RT_EOK) return error;
    }
    /* Never report success when the SDK silently kept an old DNS server. */
    return ip4_value(&device->dns_servers[0]) == desired[0] &&
        ip4_value(&device->dns_servers[1]) == desired[1] ? RT_EOK : -RT_ENOSYS;
}

static bool static_matches(const wlan_ipv4_state_t *state)
{
    const wlan_ipv4_config_t *config = &state->config;
    return !state->dhcp && state->address == config->address && state->netmask == config->netmask &&
        state->gateway == config->gateway && state->dns[0] == config->dns[0] && state->dns[1] == config->dns[1];
}

static rt_err_t configure(struct netdev *device, wlan_ipv4_state_t *state,
                          uint32_t epoch, bool transition)
{
    const wlan_ipv4_config_t *config = &state->config;
    bool reconnect = epoch != applied_epoch;
    if (config->dhcp)
    {
        if (!transition && state->dhcp)
        {
            if (state->ready) state->dns_error = RT_EOK;
            return RT_EOK;
        }
        rt_err_t error = RT_EOK;
        /* A reconnect can start DHCP while NETDEV still reports disabled. */
        if (!state->dhcp && epoch != stopped_epoch)
            error = netdev_dhcp_enabled(device, RT_TRUE);
        if (error == RT_EOK) error = netdev_dhcp_enabled(device, RT_FALSE);
        const uint32_t empty_dns[2] = {0, 0};
        if (error == RT_EOK) error = set_address(device, 0, netdev_set_ipaddr);
        if (error == RT_EOK) error = set_address(device, 0, netdev_set_gw);
        if (error == RT_EOK) error = set_address(device, 0, netdev_set_netmask);
        if (error == RT_EOK)
        {
            /* Legacy NETDEV may retain a duplicate zero or old DNS slot.
             * DHCP must still run so its lease can replace the old settings. */
            state->dns_error = set_dns(device, empty_dns);
            error = netdev_dhcp_enabled(device, RT_TRUE);
        }
        return error;
    }
    if (!reconnect && static_matches(state))
    {
        state->dns_error = RT_EOK;
        return RT_EOK;
    }
    rt_err_t error = RT_EOK;
    /* wlan_lwip starts DHCP directly after link-up, without updating NETDEV's
     * flag. Force a real stop after a reconnect or lost static address. */
    bool restart_suspected = epoch != stopped_epoch ||
        (!state->pending && state->address != config->address);
    if (!state->dhcp && restart_suspected)
        error = netdev_dhcp_enabled(device, RT_TRUE);
    if (error == RT_EOK) error = netdev_dhcp_enabled(device, RT_FALSE);
    if (error == RT_EOK) stopped_epoch = epoch;
    bool replace_address = state->dhcp || reconnect || state->address != config->address ||
        state->netmask != config->netmask;
    if (error == RT_EOK && replace_address && state->address)
        error = set_address(device, 0, netdev_set_ipaddr);
    if (error == RT_EOK && state->netmask != config->netmask)
        error = set_address(device, config->netmask, netdev_set_netmask);
    if (error == RT_EOK && state->gateway != config->gateway)
        error = set_address(device, config->gateway, netdev_set_gw);
    if (error == RT_EOK) error = state->dns_error = set_dns(device, config->dns);
    if (error == RT_EOK && replace_address) error = set_address(device, config->address, netdev_set_ipaddr);
    return error;
}

static void apply_on_tcpip(void *parameter)
{
    (void)parameter;
    rt_base_t level = rt_hw_interrupt_disable();
    wlan_ipv4_state_t next = published;
    uint32_t revision = config_revision;
    bool transition = dhcp_transition;
    rt_hw_interrupt_enable(level);

    wlan_ipv4_state_t *state = &next;
    rt_err_t result = RT_EOK;
    struct netdev *device = netdev_get_by_name("w0");
    sample(device, state);
    uint32_t epoch = device ? observe_link(device) : 0;
    if (device && state->up && state->link_up)
    {
        result = configure(device, state, epoch, transition);
        sample(device, state);
        if (result == RT_EOK &&
            (state->config.dhcp ? state->dhcp : static_matches(state)))
        {
            applied_epoch = epoch;
            state->pending = false;
        }
        else
        {
            state->pending = true;
            if (result == RT_EOK) result = -RT_EIO;
        }
    }
    else
    {
        state->pending = true;
        if (!device) applied_epoch = stopped_epoch = 0;
    }
    state->error = result;
    level = rt_hw_interrupt_disable();
    bool changed = revision != config_revision;
    if (!changed)
    {
        published = next;
        if (state->config.dhcp && !state->pending) dhcp_transition = false;
    }
    apply_queued = false;
    rt_hw_interrupt_enable(level);
    network_changed();
    if (changed) (void)wlan_ipv4_apply();
}

static void after_wlan_jobs(struct rt_work *work, void *parameter)
{
    (void)work;
    (void)parameter;
    err_t error = tcpip_callback_with_block(apply_on_tcpip, NULL, 0);
    if (error != ERR_OK)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        published.error = error == ERR_MEM ? -RT_ENOMEM : -RT_ERROR;
        apply_queued = false;
        rt_hw_interrupt_enable(level);
        network_changed();
    }
}

rt_err_t wlan_ipv4_apply(void)
{
    struct rt_workqueue *queue = rt_wlan_get_workqueue();
    if (!queue) return record_error(-RT_EIO);
    rt_base_t level = rt_hw_interrupt_disable();
    if (!published.loaded || apply_queued)
    {
        rt_err_t result = apply_queued ? RT_EOK : published.error ? published.error : -RT_EBUSY;
        rt_hw_interrupt_enable(level);
        return result;
    }
    if (!apply_initialized)
    {
        rt_work_init(&apply_work, after_wlan_jobs, NULL);
        apply_initialized = true;
    }
    apply_queued = true;
    rt_err_t error = rt_workqueue_dowork(queue, &apply_work);
    if (error != RT_EOK)
    {
        apply_queued = false;
        published.error = error;
    }
    rt_hw_interrupt_enable(level);
    return error;
}

rt_err_t wlan_ipv4_save_and_apply(const wlan_ipv4_config_t *input)
{
    wlan_ipv4_config_t config;
    rt_err_t error = normalize(input, &config);
    if (error != RT_EOK) return record_error(error);
    ipv4_record_t record = {.dhcp = config.dhcp ? 1u : 0u, .address = config.address,
        .netmask = config.netmask, .gateway = config.gateway, .dns = {config.dns[0], config.dns[1]}};
    if (!storage_record_save(IPV4_PATH, IPV4_MAGIC, IPV4_VERSION, &record, sizeof(record)))
        return record_error(-RT_EIO);
    accept_config(&config);
    return wlan_ipv4_apply();
}

rt_err_t wlan_ipv4_reset(void)
{
    const wlan_ipv4_config_t config = {.dhcp = true};
    return wlan_ipv4_save_and_apply(&config);
}

#else
static rt_err_t unsupported(void) { return record_error(-RT_ENOSYS); }
rt_err_t wlan_ipv4_load(void) { return unsupported(); }
rt_err_t wlan_ipv4_save_and_apply(const wlan_ipv4_config_t *config)
{ return config ? unsupported() : -RT_EINVAL; }
rt_err_t wlan_ipv4_apply(void) { return unsupported(); }
rt_err_t wlan_ipv4_reset(void) { return unsupported(); }
#endif
