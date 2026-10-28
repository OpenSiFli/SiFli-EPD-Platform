/* SPDX-License-Identifier: Apache-2.0 */
#include "system_info.h"
#include "net/bt_pan.h"
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#include <stdio.h>
#include <string.h>
#ifdef EPD_USE_LVGL
#include "epd_app_profile.h"
#endif
#define INFO_STRINGIFY_(value) #value
#define INFO_STRINGIFY(value) INFO_STRINGIFY_(value)
#ifdef RT_USING_WIFI
#include <wlan_mgnt.h>
#endif

static rt_tick_t previous_tick;
static uint64_t elapsed_ticks;

static uint64_t uptime_minutes(void)
{
    rt_enter_critical();
    rt_tick_t now = rt_tick_get();
    elapsed_ticks += (rt_tick_t)(now - previous_tick);
    previous_tick = now;
    uint64_t minutes = elapsed_ticks / ((uint64_t)RT_TICK_PER_SECOND * 60);
    rt_exit_critical();
    return minutes;
}

void system_info_process(void)
{
    (void)uptime_minutes();
}

static void read_identity(system_info_t *out)
{
#ifdef EPD_UI_VERSION
    snprintf(out->ui_version, sizeof(out->ui_version), "%s", EPD_UI_VERSION);
#elif defined(EPD_APP_BUILD_ID)
    snprintf(out->ui_version, sizeof(out->ui_version), "build %.12s", EPD_APP_BUILD_ID);
#endif
    strcpy(out->product, "EPD DevKit");
    strcpy(out->model, "sf32lb57-epd-gyd7n6");
#ifdef SIFLI_VERSION
    unsigned version = (unsigned)SIFLI_VERSION;
    snprintf(out->sdk, sizeof(out->sdk), "%u.%u.%u",
             version >> 24, (version >> 16) & 255u, version & 65535u);
#endif
#ifdef SIFLI_BUILD
    /* The SDK emits 00000000 when its Git revision cannot be obtained. */
    const char *build = INFO_STRINGIFY(SIFLI_BUILD);
    if (build[0] && strcmp(build, "00000000") != 0)
    {
        size_t used = strlen(out->sdk);
        snprintf(out->sdk + used, sizeof(out->sdk) - used,
                 "%s%.8s", used ? " " : "", build);
    }
#endif
    const char *date = __DATE__;
    const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (unsigned month = 0; month < 12; ++month)
        if (memcmp(date, months + month * 3, 3) == 0)
        {
            memcpy(out->build_date, date + 7, 4);
            out->build_date[4] = '-';
            out->build_date[5] = (char)('0' + (month + 1) / 10);
            out->build_date[6] = (char)('0' + (month + 1) % 10);
            out->build_date[7] = '-';
            out->build_date[8] = date[4] == ' ' ? '0' : date[4];
            out->build_date[9] = date[5];
            break;
        }
}

/* Called with the lwIP core held, or on tcpip_thread when core locking is off. */
static void read_addresses(system_info_t *out)
{
    const struct netif *netif = netif_default;
    if (!netif || !netif_is_up(netif) || !netif_is_link_up(netif)) return;
#if LWIP_IPV4
    if (!ip4_addr_isany_val(*netif_ip4_addr(netif)) &&
        ip4addr_ntoa_r(netif_ip4_addr(netif), out->addresses[out->address_count],
                      sizeof(out->addresses[0]))) ++out->address_count;
#endif
#if LWIP_IPV6
    for (unsigned i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i)
        if (ip6_addr_isvalid(netif_ip6_addr_state(netif, i)) &&
            !ip6_addr_isany(netif_ip6_addr(netif, i)) &&
            ip6addr_ntoa_r(netif_ip6_addr(netif, i), out->addresses[out->address_count],
                          sizeof(out->addresses[0]))) ++out->address_count;
#endif
}

#if !LWIP_TCPIP_CORE_LOCKING
struct address_request
{
    system_info_t *out;
    struct rt_semaphore done;
};

static void read_addresses_callback(void *parameter)
{
    struct address_request *request = parameter;
    read_addresses(request->out);
    rt_sem_release(&request->done);
}
#endif

static bool read_network(system_info_t *out)
{
#if LWIP_TCPIP_CORE_LOCKING
    if (!sys_mutex_valid(&lock_tcpip_core)) return true;
    LOCK_TCPIP_CORE();
    read_addresses(out);
    UNLOCK_TCPIP_CORE();
    return true;
#else
    /* tcpip_init creates its mailbox before publishing the RT-Thread thread. */
    rt_thread_t tcpip = rt_thread_find(TCPIP_THREAD_NAME);
    if (!tcpip) return true;
    if (tcpip == rt_thread_self()) { read_addresses(out); return true; }
    struct address_request request = {0};
    request.out = out;
    if (rt_sem_init(&request.done, "sys_info", 0, RT_IPC_FLAG_FIFO) != RT_EOK) return false;
    bool ok = tcpip_callback(read_addresses_callback, &request) == ERR_OK;
    /* Keep the request alive until the queued callback has finished. */
    if (ok)
        while (rt_sem_take(&request.done, RT_WAITING_FOREVER) != RT_EOK) {}
    rt_sem_detach(&request.done);
    return ok;
#endif
}

bool system_info_read(system_info_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->uptime_minutes = uptime_minutes();
    read_identity(out);
    bool ok = read_network(out);
#ifdef RT_USING_WIFI
    uint8_t mac[6] = {0};
    if (rt_wlan_get_mac(mac) == RT_EOK && !(mac[0] & 1u) &&
        (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
        snprintf(out->mac, sizeof(out->mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
    if (btpan_is_ready()) btpan_get_local_addr(out->bt, sizeof(out->bt));
    if (strcmp(out->bt, "00:00:00:00:00:00") == 0 ||
        strcmp(out->bt, "FF:FF:FF:FF:FF:FF") == 0) out->bt[0] = '\0';
    return ok;
}
