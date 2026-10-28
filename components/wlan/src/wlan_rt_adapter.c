#include "wlan_control.h"
#include "wlan_radio_power.h"
#include "epd_memory.h"
#include <rthw.h>
#include <string.h>
#include "wifi_api.h"
#include "wifi_port.h"
#include "netif_api.h"
#include "netif_port.h"
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/etharp.h>
#include <netif/ethernetif.h>
#ifdef RT_USING_NETDEV
#include <netdev.h>
#endif

/* The SDK exports these platform helpers without a standalone public header. */
extern void wifi_set_mode(AIC_WIFI_MODE mode);
extern int wifi_switch_mode(AIC_WIFI_MODE mode);
extern int aic_wifi_get_mode(void);
/* Exported STA status retained by the vendor host library across deinit. */
extern int wlan_connected;
struct mac_addr;
extern int fhost_wpa_disassociate_sta(int vif_index, struct mac_addr *address);
/* fhost_api.h interfaces exported by D40-N; the control link stays opaque. */
struct fhost_cntrl_link;
extern struct fhost_cntrl_link *fhost_cntrl_cfgrwnx_link_open(void);
extern void fhost_cntrl_cfgrwnx_link_close(struct fhost_cntrl_link *link);
extern int fhost_scan(struct fhost_cntrl_link *link, int vif_index, int *frequencies);
extern void __real_aic_wifi_ext_event_register(aic_wifi_event_cb callback);
extern int __real_aicwf_net_if_add(net_if_t *handle, const uint32_t *ip,
                                 const uint32_t *mask, const uint32_t *gateway,
                                 void *vif);

struct adapter_device
{
    struct rt_wlan_device *wlan;
    struct rt_wlan_dev_ops ops;
    rt_err_t (*control)(rt_device_t device, int command, void *argument);
#ifdef RT_USING_DEVICE_OPS
    struct rt_device_ops device_ops;
#endif
};

static struct adapter_device adapters[2];
static struct rt_mutex radio_lock;
static struct rt_mutex tx_lock;
static rt_bool_t radio_lock_ready;
static volatile rt_uint32_t busy_operations;
static volatile rt_uint32_t cancelled_operations;
static rt_thread_t volatile operation_owner;
static rt_uint32_t enabled_roles;
static rt_bool_t suppress_disconnect;
static rt_bool_t ap_started;
static int powersave_level[2] = {-1, -1};
static aic_wifi_event_cb original_event;
static rt_uint8_t clients[AIC_MAX_STA_COUNT][6];
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
static struct aic_ap_cfg recovery_ap;
static bool recovery_ap_valid;
#endif

static int adapter_index(struct rt_wlan_device *wlan)
{
    return wlan == adapters[1].wlan ? 1 : 0;
}

static rt_bool_t mac_is_zero(const rt_uint8_t *mac)
{
    return !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
}

static rt_bool_t mac_is_valid(const rt_uint8_t *mac)
{
    return !mac_is_zero(mac) && !(mac[0] & 1u);
}

static rt_err_t driver_result(int result)
{
    return result == 0 ? RT_EOK : -RT_ERROR;
}

static void indicate(struct rt_wlan_device *wlan, rt_wlan_dev_event_t event)
{
    rt_wlan_dev_indicate_event_handle(wlan, event, RT_NULL);
}

static void begin_operation(rt_uint32_t operation)
{
    rt_base_t level = rt_hw_interrupt_disable();
    cancelled_operations &= ~operation;
    busy_operations |= operation;
    rt_hw_interrupt_enable(level);
}

static void end_operation(rt_uint32_t operation)
{
    rt_base_t level = rt_hw_interrupt_disable();
    busy_operations &= ~operation;
    rt_hw_interrupt_enable(level);
}

static rt_bool_t commit_operation(rt_uint32_t operation)
{
    rt_bool_t accepted;
    rt_base_t level = rt_hw_interrupt_disable();
    accepted = !(cancelled_operations & operation) &&
        (!operation_owner || operation_owner == rt_thread_self());
    if (accepted)
        busy_operations &= ~operation;
    rt_hw_interrupt_enable(level);
    return accepted;
}

static rt_err_t cancel_operation(rt_uint32_t mask)
{
    rt_uint32_t pending;
    rt_base_t level;
    if (mask & ~WLAN_CANCEL_ALL)
        return -RT_EINVAL;
    level = rt_hw_interrupt_disable();
    pending = busy_operations & mask;
    cancelled_operations |= pending;
    rt_hw_interrupt_enable(level);
    return pending ? -RT_EBUSY : RT_EOK;
}

static rt_err_t claim_operations(rt_bool_t claim)
{
    rt_thread_t caller = rt_thread_self();
    rt_base_t level = rt_hw_interrupt_disable();
    if (operation_owner && operation_owner != caller)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    operation_owner = claim ? caller : RT_NULL;
    if (claim) cancelled_operations |= busy_operations;
    rt_hw_interrupt_enable(level);
    if (!claim) return RT_EOK;
    if (rt_mutex_take(&radio_lock, 0) != RT_EOK) return -RT_EBUSY;
    rt_mutex_release(&radio_lock);
    return RT_EOK;
}

static void adapter_sta_status(aic_wifi_mac_status_e status)
{
    if (status == WIFI_STA_STATUS_DISCONNECTED && !suppress_disconnect &&
        adapters[0].wlan && (enabled_roles & WLAN_ROLE_STA))
        indicate(adapters[0].wlan, RT_WLAN_DEV_EVT_DISCONNECT);
}

static rt_wlan_security_t scan_security(unsigned char auth)
{
    switch (auth)
    {
    case AIC_WIFI_AUTH_NONE: return SECURITY_OPEN;
    case AIC_WIFI_AUTH_WEP: return SECURITY_WEP_PSK;
    case AIC_WIFI_AUTH_WPA_PSK: return SECURITY_WPA_AES_PSK;
    case AIC_WIFI_AUTH_WPA2_PSK: return SECURITY_WPA2_AES_PSK;
    case AIC_WIFI_AUTH_WPA3_SAE: return WLAN_SECURITY_WPA3_SAE;
    case AIC_WIFI_AUTH_WPA2_WPA3: return WLAN_SECURITY_WPA2_WPA3_PSK;
    default: return SECURITY_UNKNOWN;
    }
}

static rt_bool_t scan_matches(const struct rt_scan_info *filter,
                              const wifi_ap_info_t *ap)
{
    if (!filter)
        return RT_TRUE;
    if (filter->ssid.len && (filter->ssid.len != ap->ssid_len ||
            memcmp(filter->ssid.val, ap->ssid, ap->ssid_len)))
        return RT_FALSE;
    if (!mac_is_zero(filter->bssid) && memcmp(filter->bssid, ap->bssid, 6))
        return RT_FALSE;
    if (filter->channel_min > 0 && ap->channel < (unsigned)filter->channel_min)
        return RT_FALSE;
    if (filter->channel_max > 0 && ap->channel > (unsigned)filter->channel_max)
        return RT_FALSE;
    return RT_TRUE;
}

static rt_err_t adapter_scan_connected(void)
{
    struct aic_netif_info info;
    struct fhost_cntrl_link *link;
    int result;

    if (aic_netif_get_info(AIC_IF_ROLE_STA, &info) != 0 || info.vif_idx < 0 ||
        !info.net_if || !aic_net_if_vif_ready(info.net_if))
        return -RT_EIO;
    link = fhost_cntrl_cfgrwnx_link_open();
    if (!link)
        return -RT_ERROR;
    /* Scan the associated STA without creating or changing a VIF. */
    result = fhost_scan(link, info.vif_idx, RT_NULL);
    fhost_cntrl_cfgrwnx_link_close(link);
    return result < 0 ? -RT_ERROR : RT_EOK;
}

static rt_err_t adapter_scan(struct rt_wlan_device *wlan, struct rt_scan_info *filter)
{
    rt_err_t result;
    wifi_ap_list_t *scan_results;
    unsigned i;
    char ssid[WIFI_MAX_SSID_LEN + 1] = {0};
    if (adapter_index(wlan) || !(enabled_roles & WLAN_ROLE_STA))
        return -RT_EIO;
    if (filter && (filter->ssid.len > WIFI_MAX_SSID_LEN ||
        (filter->channel_min > 0 && filter->channel_max > 0 &&
         filter->channel_min > filter->channel_max)))
        return -RT_EINVAL;
    begin_operation(WLAN_CANCEL_SCAN);
    scan_results = epd_app_alloc(sizeof(*scan_results), EPD_APP_PSRAM);
    if (!scan_results)
    {
        result = -RT_ENOMEM;
        goto finish;
    }
    if (filter && filter->ssid.len &&
        !memchr(filter->ssid.val, 0, filter->ssid.len))
    {
        memcpy(ssid, filter->ssid.val, filter->ssid.len);
        result = driver_result(aic_wifi_super_scan((uint8_t *)ssid));
    }
    else if (aic_wifi_get_sta_status() == WIFI_STA_STATUS_CONNECTED)
        result = adapter_scan_connected();
    else
        result = driver_result(aic_wifi_scan());
    if (result == RT_EOK && !(cancelled_operations & WLAN_CANCEL_SCAN))
    {
        memset(scan_results, 0, sizeof(*scan_results));
        aic_wifi_getscan(scan_results);
        for (i = 0; i < scan_results->ap_count && i < MAX_AP_COUNT; ++i)
        {
            wifi_ap_info_t *ap = &scan_results->ap_info[i];
            struct rt_wlan_info info;
            struct rt_wlan_buff buffer = {&info, sizeof(info)};
            if (ap->ssid_len > WIFI_MAX_SSID_LEN || !scan_matches(filter, ap))
                continue;
            memset(&info, 0, sizeof(info));
            info.security = scan_security(ap->auth_mode);
            info.band = ap->channel > 14 ? RT_802_11_BAND_5GHZ : RT_802_11_BAND_2_4GHZ;
            info.channel = (rt_int16_t)ap->channel;
            info.rssi = (rt_int8_t)ap->rssi;
            info.hidden = ap->ssid_len == 0;
            info.ssid.len = ap->ssid_len;
            memcpy(info.ssid.val, ap->ssid, ap->ssid_len);
            memcpy(info.bssid, ap->bssid, 6);
            if (!(cancelled_operations & WLAN_CANCEL_SCAN))
                rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_SCAN_REPORT, &buffer);
        }
    }
finish:
    epd_app_free(scan_results);
    if (!commit_operation(WLAN_CANCEL_SCAN))
        result = -RT_EBUSY;
    indicate(wlan, RT_WLAN_DEV_EVT_SCAN_DONE);
    end_operation(WLAN_CANCEL_SCAN);
    return result;
}

static rt_err_t adapter_scan_stop(struct rt_wlan_device *wlan)
{
    (void)wlan;
    return cancel_operation(WLAN_CANCEL_SCAN);
}

static rt_err_t validate_key(rt_wlan_security_t security, const rt_wlan_key_t *key)
{
    if (key->len > RT_WLAN_PASSWORD_MAX_LENGTH || key->len > AIC_MAX_PASSWD_LEN ||
        memchr(key->val, 0, key->len))
        return -RT_EINVAL;
    if (security == SECURITY_OPEN)
        return key->len ? -RT_EINVAL : RT_EOK;
    if (security == SECURITY_UNKNOWN)
        return !key->len || key->len >= AIC_MIN_KEY_LEN ? RT_EOK : -RT_EINVAL;
    if (security == SECURITY_WEP_PSK || security == SECURITY_WEP_SHARED)
        return (key->len == 5 || key->len == 13 || key->len == 10 || key->len == 26) ? RT_EOK : -RT_EINVAL;
    if (security == SECURITY_WPA_AES_PSK || security == SECURITY_WPA_TKIP_PSK ||
        security == SECURITY_WPA2_AES_PSK || security == SECURITY_WPA2_TKIP_PSK ||
        security == SECURITY_WPA2_MIXED_PSK || security == WLAN_SECURITY_WPA3_SAE ||
        security == WLAN_SECURITY_WPA2_WPA3_PSK)
        return key->len >= AIC_MIN_KEY_LEN ? RT_EOK : -RT_EINVAL;
    return -RT_ENOSYS;
}

static void refresh_stack_mac(struct rt_wlan_device *wlan);

static rt_err_t adapter_join(struct rt_wlan_device *wlan, struct rt_sta_info *station)
{
    char ssid[AIC_MAX_SSID_LEN + 1] = {0};
    char key[AIC_MAX_PASSWD_LEN + 1] = {0};
    uint8_t no_bssid[6] = {0};
    rt_err_t result;
    int timeout = 30000;
    if (adapter_index(wlan) || !(enabled_roles & WLAN_ROLE_STA))
        return -RT_EIO;
    if (!station || !station->ssid.len || station->ssid.len > AIC_MAX_SSID_LEN ||
        memchr(station->ssid.val, 0, station->ssid.len) ||
        (!mac_is_zero(station->bssid) && !mac_is_valid(station->bssid)))
        return -RT_EINVAL;
    result = validate_key(station->security, &station->key);
    if (result != RT_EOK)
        return result;
    /* Native stop_sta resets the driver's mode, while RT keeps STATION.
     * As in app_wlan_sta_start(), resynchronise before every connection. */
    if (wifi_switch_mode(WIFI_MODE_STA) != 0)
        return -RT_EBUSY;
    memcpy(ssid, station->ssid.val, station->ssid.len);
    memcpy(key, station->key.val, station->key.len);
    /* wlan_start_sta reserves timeout bit 0 for explicit WEP authentication. */
    if (station->security == SECURITY_WEP_PSK || station->security == SECURITY_WEP_SHARED)
        timeout |= 1;
    begin_operation(WLAN_CANCEL_JOIN);
    /* stop_sta clears this callback; install it for every association. */
    aic_wifi_sta_status_cb_set(adapter_sta_status);
    aci_wifi_set_sta_connect_bssid(station->bssid);
    result = driver_result(aic_wifi_start_sta((uint8_t *)ssid, (uint8_t *)key, timeout));
    aci_wifi_set_sta_connect_bssid(no_bssid);
    memset(key, 0, sizeof(key));
    if (result == RT_EOK && aic_wifi_get_sta_status() != WIFI_STA_STATUS_CONNECTED)
        result = -RT_EIO;
    /* Commit and cancellation have one atomic ordering point. RT callbacks
     * run afterwards with interrupts and scheduling enabled. */
    if (commit_operation(WLAN_CANCEL_JOIN))
    {
        if (result == RT_EOK)
            refresh_stack_mac(wlan);
        indicate(wlan, result == RT_EOK ? RT_WLAN_DEV_EVT_CONNECT : RT_WLAN_DEV_EVT_CONNECT_FAIL);
        /* A passive loss can race the synchronous CONNECT publication. */
        if (result == RT_EOK && aic_wifi_get_sta_status() != WIFI_STA_STATUS_CONNECTED)
        {
            indicate(wlan, RT_WLAN_DEV_EVT_DISCONNECT);
            result = -RT_EIO;
        }
        return result;
    }
    suppress_disconnect = RT_TRUE;
    result = driver_result(aic_wifi_stop_sta());
    suppress_disconnect = RT_FALSE;
    indicate(wlan, RT_WLAN_DEV_EVT_CONNECT_FAIL);
    end_operation(WLAN_CANCEL_JOIN);
    return result == RT_EOK ? -RT_EBUSY : result;
}

static rt_err_t adapter_disconnect(struct rt_wlan_device *wlan)
{
    rt_err_t result;
    if (adapter_index(wlan))
        return -RT_EINVAL;
    /* NONE may follow an explicit disconnect. The native stop entry checks
     * driver mode even when there is no remaining STA connection. */
    if (wifi_switch_mode(WIFI_MODE_STA) != 0)
        return -RT_EBUSY;
    suppress_disconnect = RT_TRUE;
    result = driver_result(aic_wifi_stop_sta());
    suppress_disconnect = RT_FALSE;
    if (result == RT_EOK)
        indicate(wlan, RT_WLAN_DEV_EVT_DISCONNECT);
    return result;
}

static rt_err_t adapter_ap_stop(struct rt_wlan_device *wlan)
{
    rt_err_t result;
    if (!adapter_index(wlan))
        return -RT_EINVAL;
    result = ap_started ? driver_result(aic_wifi_stop_ap()) : RT_EOK;
    if (result == RT_EOK)
    {
        ap_started = RT_FALSE;
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
        memset(&recovery_ap, 0, sizeof(recovery_ap));
        recovery_ap_valid = false;
#endif
        powersave_level[1] = -1;
        memset(clients, 0, sizeof(clients));
        indicate(wlan, RT_WLAN_DEV_EVT_AP_STOP);
    }
    return result;
}

static rt_err_t adapter_mode(struct rt_wlan_device *wlan, rt_wlan_mode_t mode)
{
    int index = adapter_index(wlan);
    rt_uint32_t role = index ? WLAN_ROLE_AP : WLAN_ROLE_STA;
    rt_uint32_t remaining;
    rt_err_t result;
    if (mode == RT_WLAN_NONE)
    {
        if (!(enabled_roles & role))
            return RT_EOK;
        bool ready = wlan_radio_is_ready();
        result = ready ?
                 (index ? adapter_ap_stop(wlan) : adapter_disconnect(wlan)) : RT_EOK;
        if (result != RT_EOK)
            return result;
        remaining = enabled_roles & ~role;
        if (!remaining)
        {
            /* Keep the last mode available to the synchronous deinit path. */
            result = wlan_radio_set_enabled(false);
            if (result != RT_EOK)
                return result;
            powersave_level[0] = powersave_level[1] = -1;
        }
        if (remaining && wlan_radio_is_ready())
            wifi_set_mode(remaining == WLAN_ROLE_STA ? WIFI_MODE_STA : WIFI_MODE_AP);
        enabled_roles = remaining;
        powersave_level[index] = -1;
        if (!ready)
        {
            if (index)
            {
                ap_started = RT_FALSE;
                memset(clients, 0, sizeof(clients));
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
                memset(&recovery_ap, 0, sizeof(recovery_ap));
                recovery_ap_valid = false;
#endif
            }
            indicate(wlan, index ? RT_WLAN_DEV_EVT_AP_STOP : RT_WLAN_DEV_EVT_DISCONNECT);
        }
        return RT_EOK;
    }
    if ((index && mode != RT_WLAN_AP) || (!index && mode != RT_WLAN_STATION))
        return -RT_EINVAL;
    result = wlan_radio_set_enabled(true);
    if (result != RT_EOK)
        return result;
    if (!enabled_roles)
        wifi_set_mode(WIFI_MODE_UNKNOWN);
    if (wifi_switch_mode(index ? WIFI_MODE_AP : WIFI_MODE_STA) != 0)
        return -RT_EBUSY;
    enabled_roles |= role;
    return RT_EOK;
}

static rt_err_t adapter_softap(struct rt_wlan_device *wlan, struct rt_ap_info *ap)
{
    struct aic_ap_cfg config;
    rt_err_t result;
    if (!adapter_index(wlan) || !(enabled_roles & WLAN_ROLE_AP))
        return -RT_EIO;
    if (!ap || !ap->ssid.len || ap->ssid.len > AIC_MAX_SSID_LEN ||
        memchr(ap->ssid.val, 0, ap->ssid.len) || ap->channel <= 0 || ap->channel > 165)
        return -RT_EINVAL;
    result = validate_key(ap->security, &ap->key);
    if (result != RT_EOK)
        return result;
    memset(&config, 0, sizeof(config));
    if (ap->security == SECURITY_OPEN)
        config.sercurity_type = KEY_NONE;
    else if (ap->security == SECURITY_WPA2_AES_PSK)
        config.sercurity_type = KEY_WPA2;
    else if (ap->security == WLAN_SECURITY_WPA3_SAE)
        config.sercurity_type = KEY_WPA3;
    else if (ap->security == WLAN_SECURITY_WPA2_WPA3_PSK)
        config.sercurity_type = KEY_WPA2_WPA3;
    else
        return -RT_ENOSYS;
    config.aic_ap_ssid.length = ap->ssid.len;
    memcpy(config.aic_ap_ssid.array, ap->ssid.val, ap->ssid.len);
    config.aic_ap_passwd.length = ap->key.len;
    memcpy(config.aic_ap_passwd.array, ap->key.val, ap->key.len);
    config.channel = (unsigned char)ap->channel;
    config.band = ap->channel > 14 ? 1 : 0;
    config.hidden_ssid = ap->hidden ? 1 : 0;
    config.bcn_interval = 100;
    config.sta_num = AIC_MAX_STA_COUNT;
    config.dtim = 1;
    /* The host may return success for an already-running AP without applying
     * its new configuration. Validate first, then replace only the AP role. */
    if (ap_started)
    {
        result = adapter_ap_stop(wlan);
        if (result != RT_EOK)
        {
            memset(&config.aic_ap_passwd, 0, sizeof(config.aic_ap_passwd));
            return result;
        }
    }
    result = driver_result(aic_wifi_start_ap(&config));
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
    if (result == RT_EOK)
    {
        recovery_ap = config;
        recovery_ap_valid = true;
    }
#endif
    memset(&config.aic_ap_passwd, 0, sizeof(config.aic_ap_passwd));
    if (result == RT_EOK)
    {
        ap_started = RT_TRUE;
        memset(clients, 0, sizeof(clients));
        refresh_stack_mac(wlan);
        indicate(wlan, RT_WLAN_DEV_EVT_AP_START);
    }
    return result;
}

#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
struct _uap_config_scan_chan;
extern long __real_app_wlan_scan_channels_config(struct _uap_config_scan_chan *parameter);

long __wrap_app_wlan_scan_channels_config(struct _uap_config_scan_chan *parameter)
{
    unsigned operation = aicwf_wifi_dev.wifi_drive_op;
    /* Firmware initialization already restores the supported channel set.
     * The SDK's NULL recovery call replaces it with a 2.4-GHz demo list. */
    if (!parameter && radio_lock_ready && aicwf_wifi_dev.wifi_task_init_hdl &&
        rt_thread_self() == (rt_thread_t)aicwf_wifi_dev.wifi_task_init_hdl &&
        (operation == 3 || operation == 4))
        return 0;
    return __real_app_wlan_scan_channels_config(parameter);
}

extern long __real_app_wlan_bss_config(unsigned long start_stop);

long __wrap_app_wlan_bss_config(unsigned long start_stop)
{
    unsigned operation = aicwf_wifi_dev.wifi_drive_op;
    if (!radio_lock_ready || !aicwf_wifi_dev.wifi_task_init_hdl ||
        rt_thread_self() != (rt_thread_t)aicwf_wifi_dev.wifi_task_init_hdl ||
        (operation != 3 && operation != 4))
        return __real_app_wlan_bss_config(start_stop);
    /* SDK RESET normally starts its demonstration AP. Restore only the
     * successfully configured RT-WLAN hotspot, including its credentials. */
    if (start_stop != 1 || !recovery_ap_valid || !(enabled_roles & WLAN_ROLE_AP))
        return -RT_EIO;
    for (unsigned i = 0; i < AIC_MAX_STA_COUNT; ++i)
    {
        if (!mac_is_zero(clients[i]))
        {
            struct rt_wlan_info info = {0};
            struct rt_wlan_buff buffer = {&info, sizeof(info)};
            memcpy(info.bssid, clients[i], 6);
            rt_wlan_dev_indicate_event_handle(adapters[1].wlan,
                RT_WLAN_DEV_EVT_AP_DISASSOCIATED, &buffer);
        }
    }
    memset(clients, 0, sizeof(clients));
    int result = aic_wifi_start_ap(&recovery_ap);
    ap_started = result == 0;
    indicate(adapters[1].wlan, ap_started ? RT_WLAN_DEV_EVT_AP_START : RT_WLAN_DEV_EVT_AP_STOP);
    return result == 0 ? 0 : -RT_EIO;
}
#endif

static rt_err_t adapter_deauth(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    struct macaddr address;
    int vif;
    if (!adapter_index(wlan) || !ap_started)
        return -RT_EIO;
    if (!mac || !mac_is_valid(mac))
        return -RT_EINVAL;
    vif = aic_wifi_get_ap_vif_idx();
    if (vif < 0)
        return -RT_EIO;
    memcpy(address.array, mac, 6);
    return driver_result(fhost_wpa_disassociate_sta(vif, (struct mac_addr *)&address));
}

static rt_err_t adapter_powersave(struct rt_wlan_device *wlan, int level)
{
    rt_err_t result;
    int index = adapter_index(wlan);
    if (!(enabled_roles & (index ? WLAN_ROLE_AP : WLAN_ROLE_STA)))
        return -RT_EIO;
    if (!index)
    {
#ifdef CONFIG_STATION
        if (level < WLAN_STA_POWERSAVE_MIN_LEVEL || level > WLAN_STA_POWERSAVE_MAX_LEVEL)
            return -RT_EINVAL;
        /* Keep firmware filtering enabled; this is not the FW-log override. */
        result = driver_result(aic_wifi_set_lp_level((uint8_t)level, 0));
#else
        return -RT_ENOSYS;
#endif
    }
    else
    {
#ifdef CONFIG_SOFTAP
        if (level < WLAN_AP_POWERSAVE_MIN_LEVEL || level > WLAN_AP_POWERSAVE_MAX_LEVEL)
            return -RT_EINVAL;
        result = driver_result(aic_wifi_set_ps_lvl(level));
#else
        return -RT_ENOSYS;
#endif
    }
    if (result == RT_EOK)
        powersave_level[index] = level;
    return result;
}

static int adapter_get_powersave(struct rt_wlan_device *wlan)
{
    return powersave_level[adapter_index(wlan)];
}

static int adapter_channel(struct rt_wlan_device *wlan)
{
    struct aic_connected_ap_info_t *ap;
    if (adapter_index(wlan))
        /* aic_ap_get_acs_start_channel reads the cached startup/ACS channel,
         * not a live channel after subsequent channel-switch operations. */
        return ap_started ? -RT_ENOSYS : -RT_EIO;
    if (aic_wifi_get_sta_status() != WIFI_STA_STATUS_CONNECTED)
        return -RT_EIO;
    ap = aic_wifi_get_sta_connected_ap_info();
    return ap ? aic_wifi_phy_freq_to_channel(ap->band, ap->freq) : -RT_EIO;
}

static rt_err_t adapter_set_channel(struct rt_wlan_device *wlan, int channel)
{
    (void)wlan;
    (void)channel;
    /* A running channel change has no public host API. AP start carries it. */
    return -RT_ENOSYS;
}

static net_if_t role_netif(struct rt_wlan_device *wlan)
{
    struct aic_netif_info info;
    if (aic_netif_get_info(adapter_index(wlan) ? AIC_IF_ROLE_AP : AIC_IF_ROLE_STA, &info) != 0)
        return RT_NULL;
    return info.net_if;
}

static rt_err_t adapter_get_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    struct netif *netif = (struct netif *)role_netif(wlan);
    const uint8_t *address = RT_NULL;
    if (!mac)
        return -RT_EINVAL;
    if (netif && netif->state)
        address = aic_fhost_vif_get_mac(netif->state);
    else if (!adapter_index(wlan))
        address = aic_get_mac_address();
    if (!address || !mac_is_valid(address))
        return -RT_EIO;
    memcpy(mac, address, 6);
    return RT_EOK;
}

static rt_err_t adapter_set_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    (void)wlan;
    (void)mac;
    /* The SDK base-address setter does not update an active firmware VIF. */
    return -RT_ENOSYS;
}

static void refresh_netif_mac(struct netif *netif)
{
    struct eth_device *eth = netif->state;
    struct rt_wlan_device *wlan;
    rt_uint8_t mac[6];
    if (!eth)
        return;
    wlan = eth->parent.user_data;
    if (!wlan)
        return;
    if (adapter_get_mac(wlan, mac) != RT_EOK)
        return;
    memcpy(netif->hwaddr, mac, 6);
    netif->hwaddr_len = 6;
#ifdef RT_USING_NETDEV
    {
        char name[3] = {netif->name[0], netif->name[1], 0};
        struct netdev *netdev = netdev_get_by_name(name);
        if (netdev && netdev->user_data == netif)
        {
            rt_base_t level = rt_hw_interrupt_disable();
            memcpy(netdev->hwaddr, mac, 6);
            netdev->hwaddr_len = 6;
            rt_hw_interrupt_enable(level);
        }
    }
#endif
}

static void refresh_stack_mac(struct rt_wlan_device *wlan)
{
    int i;
    for (i = 0; i < 9; ++i)
    {
        char name[3] = {'w', (char)('0' + i), 0};
        rt_device_t device = rt_device_find(name);
        if (device && device->user_data == wlan)
        {
            struct eth_device *eth = (struct eth_device *)device;
            if (eth->netif)
                netifapi_netif_common(eth->netif, refresh_netif_mac, RT_NULL);
            break;
        }
    }
}

static int adapter_send(struct rt_wlan_device *wlan, void *data, int length)
{
    net_if_t netif;
    int result = -RT_EIO;
    if (!data || length <= 0)
        return -RT_EINVAL;
    if (rt_mutex_take(&tx_lock, 0) != RT_EOK)
        return -RT_EBUSY;
    if (!wlan_radio_is_ready())
        goto done;
    netif = role_netif(wlan);
    if (!netif || !aic_net_if_vif_ready(netif))
        goto done;
    result = aicwf_tx_eth_data_process_if(netif, data, (uint32_t)length, RT_NULL);
done:
    rt_mutex_release(&tx_lock);
    return result;
}

extern void __real_aic_wifi_deinit(int mode);

void __wrap_aic_wifi_deinit(int mode)
{
    /* Automatic recovery runs in the SDK manager, independently of RT-WLAN.
     * Let an in-flight join/stop finish before freeing its native context.
     * Normal CLOSE is already initiated under radio_lock and must not retake
     * that lock from the manager while its caller waits for completion. */
    unsigned operation = aicwf_wifi_dev.wifi_drive_op;
    rt_bool_t manager = radio_lock_ready && aicwf_wifi_dev.wifi_task_init_hdl &&
        rt_thread_self() == (rt_thread_t)aicwf_wifi_dev.wifi_task_init_hdl;
    rt_bool_t recovery = manager && (operation == 3 || operation == 4);
    rt_bool_t drain_tx = manager && (operation == 2 || operation == 3 || operation == 4);
    if (recovery)
        rt_mutex_take(&radio_lock, RT_WAITING_FOREVER);
    /* TX remains available during normal JOIN and MAC callbacks. It has its
     * own lock so teardown can drain a native send without blocking JOIN's
     * cross-thread TCP/IP traffic. */
    if (drain_tx)
        rt_mutex_take(&tx_lock, RT_WAITING_FOREVER);
    __real_aic_wifi_deinit(mode);
    /* Forced teardown bypasses wlan_disconnect_sta(), leaving the host's
     * connection flag set. A later start_sta otherwise fails before scanning.
     * Clear it only after all native tasks are gone, under the TX drain lock. */
    if (drain_tx)
        wlan_connected = 0;
    /* Native recovery tears down the association without a leave event.
     * Invalidate RT-WLAN's cached link/IP state so autoconnect can run again. */
    if (recovery && (enabled_roles & WLAN_ROLE_STA))
        indicate(adapters[0].wlan, RT_WLAN_DEV_EVT_DISCONNECT);
    if (drain_tx)
        rt_mutex_release(&tx_lock);
    if (recovery)
        rt_mutex_release(&radio_lock);
}

static void adapter_event(AIC_WIFI_EVENT event, aic_wifi_event_data *data)
{
    struct rt_wlan_info info;
    struct rt_wlan_buff buffer = {&info, sizeof(info)};
    int i, empty = -1, found = -1;
    rt_base_t level;
    if (original_event && !(suppress_disconnect &&
        (event == LEAVE_RESULT_EVENT || event == STA_DISCONNECT_EVENT)))
        original_event(event, data);
    if (!ap_started || !adapters[1].wlan || !data || data->p2p_enabled ||
        (event != ASSOC_IND_EVENT && event != DISASSOC_STA_IND_EVENT))
        return;
    memset(&info, 0, sizeof(info));
    for (i = 0; i < 6; ++i)
        info.bssid[i] = (rt_uint8_t)data->data.auth_deauth_data.reserved[i];
    if (!mac_is_valid(info.bssid))
        return;
    level = rt_hw_interrupt_disable();
    for (i = 0; i < AIC_MAX_STA_COUNT; ++i)
    {
        if (!memcmp(clients[i], info.bssid, 6)) found = i;
        if (mac_is_zero(clients[i])) empty = i;
    }
    if (event == ASSOC_IND_EVENT && found < 0 && empty >= 0)
        memcpy(clients[empty], info.bssid, 6);
    else if (event == DISASSOC_STA_IND_EVENT && found >= 0)
        memset(clients[found], 0, 6);
    else
    {
        rt_hw_interrupt_enable(level);
        return;
    }
    rt_hw_interrupt_enable(level);
    rt_wlan_dev_indicate_event_handle(adapters[1].wlan,
        event == ASSOC_IND_EVENT ? RT_WLAN_DEV_EVT_AP_ASSOCIATED : RT_WLAN_DEV_EVT_AP_DISASSOCIATED,
        &buffer);
}

void __wrap_aic_wifi_ext_event_register(aic_wifi_event_cb callback)
{
    original_event = callback;
    __real_aic_wifi_ext_event_register(adapter_event);
}

/* Reserve the official pool slot and retain VIF metadata in RT-WLAN mode.
 * The RT-WLAN protocol remains the sole owner of the registered lwIP netif. */
int __wrap_aicwf_net_if_add(net_if_t *handle, const uint32_t *ip,
                          const uint32_t *mask, const uint32_t *gateway, void *vif)
{
    int result = __real_aicwf_net_if_add(handle, ip, mask, gateway, vif);
    if (result == 0 && handle && *handle && vif)
    {
        struct netif *netif = (struct netif *)*handle;
        const uint8_t *mac = aic_fhost_vif_get_mac(vif);
        netif->state = vif;
        netif->name[0] = 'a';
        netif->name[1] = 'i';
        netif->num = (uint8_t)aic_fhost_vif_index(vif);
        netif->output = etharp_output;
        netif->hwaddr_len = 6;
        if (mac)
            memcpy(netif->hwaddr, mac, 6);
    }
    return result;
}

static void get_capabilities(struct rt_wlan_device *wlan, struct wlan_control_caps *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->version = 1;
    caps->flags = WLAN_CAP_SCAN_FILTERS | WLAN_CAP_JOIN_BSSID | WLAN_CAP_AP |
                  WLAN_CAP_AP_CLIENTS |
                  WLAN_CAP_CANCEL | WLAN_CAP_AP_HIDDEN | WLAN_CAP_AP_DEAUTH;
    /* The supplied host archive has eight internal socket slots. STA + AP
     * exhausts them when creating the AP EAPOL channel. Do not advertise
     * concurrency until a host archive with sufficient capacity is verified. */
#ifdef CONFIG_STATION
    if (!adapter_index(wlan))
        caps->flags |= WLAN_CAP_POWERSAVE;
#endif
#ifdef CONFIG_SOFTAP
    if (adapter_index(wlan))
        caps->flags |= WLAN_CAP_POWERSAVE;
#endif
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
    caps->flags |= WLAN_CAP_SUSPEND;
#endif
    caps->sta_security = WLAN_AUTH_OPEN | WLAN_AUTH_WEP | WLAN_AUTH_WPA_PSK |
                         WLAN_AUTH_WPA2_PSK | WLAN_AUTH_WPA3_SAE | WLAN_AUTH_WPA2_WPA3;
    caps->ap_security = WLAN_AUTH_OPEN | WLAN_AUTH_WPA2_PSK |
                        WLAN_AUTH_WPA3_SAE | WLAN_AUTH_WPA2_WPA3;
}

static rt_err_t adapter_control(rt_device_t device, int command, void *argument)
{
    struct rt_wlan_device *wlan = (struct rt_wlan_device *)device;
    struct adapter_device *adapter = &adapters[adapter_index(wlan)];
    rt_err_t result;
    if (command == WLAN_CTRL_CANCEL)
        return cancel_operation(argument ? *(rt_uint32_t *)argument : WLAN_CANCEL_ALL);
    if (command == WLAN_CTRL_CLAIM)
        return argument ? claim_operations(*(rt_bool_t *)argument) : -RT_EINVAL;
    if (command == RT_WLAN_CMD_SCAN_STOP)
        return cancel_operation(WLAN_CANCEL_SCAN);
    if (command >= WLAN_CTRL_P2P_FIRST && command <= WLAN_CTRL_P2P_LAST)
        return -RT_ENOSYS;
    if (command == WLAN_CTRL_GET_CAPS)
    {
        if (!argument) return -RT_EINVAL;
        get_capabilities(wlan, argument);
        return RT_EOK;
    }
    if (command == WLAN_CTRL_GET_STATE)
    {
        struct wlan_control_state *state = argument;
        rt_base_t level;
        if (!state) return -RT_EINVAL;
        level = rt_hw_interrupt_disable();
        state->active_roles = enabled_roles;
        state->busy = busy_operations;
        state->cancelled = cancelled_operations;
        rt_hw_interrupt_enable(level);
        state->radio_enabled = wlan_radio_is_ready();
        return RT_EOK;
    }
    if (command == RT_WLAN_CMD_SET_COUNTRY || command == RT_WLAN_CMD_GET_COUNTRY)
        return -RT_ENOSYS;
    if (command == WLAN_CTRL_PREPARE_DEEP && argument)
        return -RT_EINVAL;
    if (command != RT_WLAN_CMD_DISCONNECT && command != RT_WLAN_CMD_AP_STOP &&
        command != RT_WLAN_CMD_SCAN && command != WLAN_CTRL_PREPARE_DEEP && !argument)
        return -RT_EINVAL;
    result = rt_mutex_take(&radio_lock, 0);
    if (result != RT_EOK)
        return -RT_EBUSY;
    if (operation_owner && operation_owner != rt_thread_self() &&
        (command == RT_WLAN_CMD_JOIN || command == RT_WLAN_CMD_SCAN))
        result = -RT_EBUSY;
    else if (command == WLAN_CTRL_PREPARE_DEEP)
    {
#if WLAN_RADIO_DEEP_SLEEP_SUPPORTED
        result = enabled_roles || ap_started || busy_operations ?
                 -RT_EBUSY : wlan_radio_prepare_deep();
#else
        result = -RT_ENOSYS;
#endif
    }
    else if (!wlan_radio_is_ready() && command != RT_WLAN_CMD_MODE &&
        command != RT_WLAN_CMD_GET_MAC)
        result = -RT_EIO;
    else if (command == RT_WLAN_CMD_GET_RSSI)
    {
        result = adapter_index(wlan) || aic_wifi_get_sta_status() != WIFI_STA_STATUS_CONNECTED ? -RT_EIO : RT_EOK;
        if (result == RT_EOK) *(int *)argument = aic_wifi_ap_rssi_get();
    }
    else if (command == RT_WLAN_CMD_GET_CHANNEL)
    {
        int channel = adapter_channel(wlan);
        result = channel > 0 ? RT_EOK : channel < 0 ? (rt_err_t)channel : -RT_EIO;
        if (result == RT_EOK) *(int *)argument = channel;
    }
    else if (command == RT_WLAN_CMD_GET_POWERSAVE && powersave_level[adapter_index(wlan)] < 0)
        result = -RT_ENOSYS;
    else
        result = adapter->control(device, command, argument);
    rt_mutex_release(&radio_lock);
    return result;
}

static int wlan_rt_adapter_init(void)
{
    int i;
    adapters[0].wlan = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    adapters[1].wlan = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_AP_NAME);
    if (!adapters[0].wlan || !adapters[1].wlan)
        return -RT_EIO;
    if (adapters[0].wlan->ops == &adapters[0].ops)
        return RT_EOK;
    if (rt_mutex_init(&radio_lock, "wlan.radio", RT_IPC_FLAG_PRIO) != RT_EOK)
        return -RT_ERROR;
    if (rt_mutex_init(&tx_lock, "wlan.tx", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        rt_mutex_detach(&radio_lock);
        return -RT_ERROR;
    }
    radio_lock_ready = RT_TRUE;
    for (i = 0; i < 2; ++i)
    {
        struct adapter_device *adapter = &adapters[i];
        adapter->ops = *adapter->wlan->ops;
        adapter->ops.wlan_mode = adapter_mode;
        adapter->ops.wlan_scan = adapter_scan;
        adapter->ops.wlan_scan_stop = adapter_scan_stop;
        adapter->ops.wlan_join = adapter_join;
        adapter->ops.wlan_softap = adapter_softap;
        adapter->ops.wlan_disconnect = adapter_disconnect;
        adapter->ops.wlan_ap_stop = adapter_ap_stop;
        adapter->ops.wlan_ap_deauth = adapter_deauth;
        adapter->ops.wlan_set_powersave = adapter_powersave;
        adapter->ops.wlan_get_powersave = adapter_get_powersave;
        adapter->ops.wlan_get_channel = adapter_channel;
        adapter->ops.wlan_set_channel = adapter_set_channel;
        adapter->ops.wlan_get_mac = adapter_get_mac;
        adapter->ops.wlan_set_mac = adapter_set_mac;
        adapter->ops.wlan_send = adapter_send;
        adapter->wlan->ops = &adapter->ops;
#ifdef RT_USING_DEVICE_OPS
        adapter->device_ops = *adapter->wlan->device.ops;
        adapter->control = adapter->device_ops.control;
        adapter->device_ops.control = adapter_control;
        adapter->wlan->device.ops = &adapter->device_ops;
#else
        adapter->control = adapter->wlan->device.control;
        adapter->wlan->device.control = adapter_control;
#endif
    }
    return RT_EOK;
}
INIT_ENV_EXPORT(wlan_rt_adapter_init);

/* Preserve the standard scan's channel constraint at the device boundary. */
rt_err_t __wrap_rt_wlan_dev_scan(struct rt_wlan_device *device, struct rt_wlan_info *info)
{
    struct rt_scan_info filter;
    if (!device)
        return -RT_EIO;
    if (!info)
        return rt_device_control((rt_device_t)device, RT_WLAN_CMD_SCAN, RT_NULL);
    if (info->ssid.len > RT_WLAN_SSID_MAX_LENGTH)
        return -RT_EINVAL;
    memset(&filter, 0, sizeof(filter));
    filter.ssid = info->ssid;
    memcpy(filter.bssid, info->bssid, 6);
    filter.channel_min = info->channel > 0 ? info->channel : -1;
    filter.channel_max = filter.channel_min;
    return rt_device_control((rt_device_t)device, RT_WLAN_CMD_SCAN, &filter);
}
