#include "network_internal.h"
#include "wlan_service.h"
#include "network.h"
#include "wlan_ap_network.h"
#include "network_reset.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "epd_memory.h"
#include "mem_section.h"
#include "ui/ui_app.h"
#include <string.h>
#include <rthw.h>

bool wlan_network_id_equal(const wlan_network_id_t *a, const wlan_network_id_t *b)
{
    return a && b && a->ssid_length <= WLAN_SERVICE_SSID_BYTES &&
        a->ssid_length == b->ssid_length &&
        !memcmp(a->ssid, b->ssid, a->ssid_length) && !memcmp(a->bssid, b->bssid, 6);
}

#if defined(RT_USING_WIFI) && defined(AIC8800_USE_RT_WLAN)
#include <wlan_mgnt.h>
#include <wlan_cfg.h>
#include <wlan_workqueue.h>
#include <wlan_control.h>
#include <lwip/ip_addr.h>
#include <netdev.h>
#include <dfs_posix.h>
#include <errno.h>

#define DBG_TAG "wlan"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define CFG_PATH STORAGE_FLASH_ROOT "/config/wlan.cfg"
#define POLICY_PATH STORAGE_FLASH_ROOT "/config/wlan-auto.bin"
#define POLICY_MAGIC 0x57415554u
#define BAND_PATH STORAGE_FLASH_ROOT "/config/wlan-band.bin"
#define BAND_MAGIC 0x5742414Eu
#define WAKE_CHANGE 1u
#define WAKE_COMMAND 2u
#define WAKE_CFG_IDLE 4u
#define WAKE_STA_CHANGE 8u
#define CFG_MAX_BYTES (16u + RT_WLAN_CFG_INFO_MAX * sizeof(struct rt_wlan_cfg_info))
#define OPEN_RETRY_LIMIT 2u
#define SLEEP_HANDOFF_RETRY_LIMIT 3u
#define OPEN_RETRY_STEP_MS 750u

typedef enum { CMD_NONE, CMD_SCAN, CMD_CONNECT, CMD_DISCONNECT, CMD_FORGET, CMD_AUTO,
    CMD_POWERSAVE, CMD_IPV4, CMD_AP_START, CMD_AP_STOP, CMD_AP_DEAUTH, CMD_BAND } command_kind_t;
typedef struct
{
    command_kind_t kind;
    wlan_network_id_t id;
    bool reuse_password, automatic, background;
    bool directed, by_ssid, hidden, concurrent;
    unsigned channel;
    wlan_band_t band;
    int level;
    uint32_t epoch;
    wlan_ipv4_config_t ipv4;
    char password[WLAN_SERVICE_PASSWORD_BYTES + 1];
} command_t;

typedef struct
{
    rt_tick_t since;
    uint32_t epoch;
    unsigned attempts;
    bool pending;
} open_retry_t;

static rt_int32_t open_retry_wait(const open_retry_t *retry, rt_tick_t now)
{
    if (!retry->pending) return RT_WAITING_FOREVER;
    rt_tick_t delay = rt_tick_from_millisecond(retry->attempts * OPEN_RETRY_STEP_MS);
    rt_tick_t elapsed = now - retry->since;
    return elapsed < delay ? (rt_int32_t)(delay - elapsed) : 0;
}

static void open_retry_result(open_retry_t *retry, rt_err_t result, rt_bool_t powered_down)
{
    if (result == RT_EOK) retry->attempts = 0;
    retry->pending = result != RT_EOK && powered_down && retry->attempts < OPEN_RETRY_LIMIT;
    if (retry->pending)
    {
        ++retry->attempts;
        retry->since = rt_tick_get();
    }
}

static bool open_retry_due(open_retry_t *retry, uint32_t epoch, bool allowed)
{
    if (!allowed || retry->epoch != epoch) *retry = (open_retry_t){.epoch = epoch};
    if (open_retry_wait(retry, rt_tick_get()) != 0) return false;
    retry->pending = false;
    return true;
}

static struct rt_mutex lock;
static struct rt_event wakeup;
static rt_thread_t worker;
static wlan_snapshot_t published;
L2_RET_BSS_SECT_BEGIN(wlan_service)
static command_t pending L2_RET_BSS_SECT(wlan_service);
L2_RET_BSS_SECT_END
static bool enabled, automatic = true, policy_dirty, working, manual_disconnect;
static wlan_band_t sta_band = WLAN_BAND_2_4_GHZ;
static bool reset_pending, reset_paused, resetting;
static volatile bool maintenance_hold;
/* EBUSY is retryable; other load failures wait for an explicit retry/save. */
static volatile rt_err_t credentials_result = -RT_EBUSY;
static rt_err_t policy_result = -RT_EBUSY, band_result = -RT_EBUSY, ipv4_load_result = -RT_EBUSY;
static bool config_retry_requested;
static bool radio_seen, sleep_hold;
static rt_err_t sleep_result;
static bool sleep_capture, sleep_reconnect;
static wlan_network_id_t sleep_network;
static int sleep_powersave = -1;
static rt_err_t sleep_powersave_error;
static unsigned sleep_powersave_attempts;
static rt_tick_t sleep_powersave_retry_at;
#define POWERSAVE_ATTEMPTS 3u
static uint32_t operation_epoch;
static wlan_operation_t active_operation;
static wlan_capabilities_t capabilities;
static wlan_hotspot_t hotspot;
#define CLIENTS_MAX 16u
static uint8_t hotspot_clients[CLIENTS_MAX][6];
static unsigned hotspot_client_count;
static bool hotspot_clients_truncated;
static rt_err_t reset_result;
static volatile rt_err_t persistence_error, connection_error;
static volatile uint32_t scan_completed;
static volatile uint32_t cfg_persist_sequence;
static uint32_t cfg_fence_sent;
static volatile uint32_t cfg_fence_done;
static rt_err_t finish_cfg_jobs(void);
static bool background_work, auto_reset_requested;
static rt_tick_t auto_scan_at;
static unsigned auto_scan_delay;
static struct
{
    wlan_network_id_t id;
    unsigned missed_scans;
} auto_failed[RT_WLAN_CFG_INFO_MAX];

static void wake(uint32_t bits) { if (worker) (void)rt_event_send(&wakeup, bits); }
static void take(void) { rt_mutex_take(&lock, RT_WAITING_FOREVER); }
static void give(void) { rt_mutex_release(&lock); }

static bool reset_held(void)
{
    network_reset_status_t state;
    network_reset_get_status(&state);
    return maintenance_hold || state.paused;
}

static bool driver_ready(void)
{
    struct rt_wlan_device *device = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    struct wlan_control_state state = {0};
    return device && device->prot &&
        rt_wlan_get_mode(RT_WLAN_DEVICE_STA_NAME) == RT_WLAN_STATION && netdev_get_by_name("w0") &&
        rt_device_control((rt_device_t)device, WLAN_CTRL_GET_STATE, &state) == RT_EOK &&
        state.radio_enabled && (state.active_roles & WLAN_ROLE_STA);
}

static rt_err_t radio_control(int command, void *argument)
{
    rt_device_t device = rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    return device ? rt_device_control(device, command, argument) : -RT_ENOSYS;
}

static bool physical_radio_ready(void)
{
    struct wlan_control_state state = {0};
    return radio_control(WLAN_CTRL_GET_STATE, &state) == RT_EOK && state.radio_enabled;
}

/* Called with the service lock. This control only changes the driver's
 * permission to power up, including asynchronous fault recovery. */
static void update_power_permission(void)
{
    rt_bool_t allowed = enabled && !sleep_hold;
    (void)radio_control(WLAN_CTRL_ALLOW_POWER, &allowed);
}

static void refresh_capabilities(void)
{
    struct wlan_control_caps caps = {0};
    wlan_capabilities_t next = {0};
    if (radio_control(WLAN_CTRL_GET_CAPS, &caps) == RT_EOK && caps.version == 1)
    {
        next.cancel_scan = next.cancel_connect = (caps.flags & WLAN_CAP_CANCEL) != 0;
        next.directed_scan = (caps.flags & WLAN_CAP_SCAN_FILTERS) != 0;
        next.bssid_connect = (caps.flags & WLAN_CAP_JOIN_BSSID) != 0;
        next.hotspot = (caps.flags & WLAN_CAP_AP) != 0;
        next.hotspot_hidden = (caps.flags & WLAN_CAP_AP_HIDDEN) != 0;
        next.hotspot_deauth = (caps.flags & WLAN_CAP_AP_DEAUTH) != 0;
        next.hotspot_clients = (caps.flags & WLAN_CAP_AP_CLIENTS) != 0;
        next.concurrent = (caps.flags & WLAN_CAP_STA_AP) != 0;
        next.powersave = (caps.flags & WLAN_CAP_POWERSAVE) != 0;
        next.p2p = (caps.flags & WLAN_CAP_P2P) != 0;
        next.sta_security = caps.sta_security;
        next.ap_security = caps.ap_security;
#ifdef WLAN_CAP_SUSPEND
        next.suspend = (caps.flags & WLAN_CAP_SUSPEND) != 0;
#endif
        next.ipv4 = true;
    }
    take(); capabilities = next; give();
}

static void cancel_radio(uint32_t mask)
{
    /* The RT adapter only records cancellation here; the worker drains it. */
    (void)radio_control(WLAN_CTRL_CANCEL, &mask);
}

static rt_err_t claim_radio(void)
{
    rt_bool_t claim = RT_TRUE;
    rt_tick_t started = rt_tick_get();
    for (;;)
    {
        rt_err_t result = radio_control(WLAN_CTRL_CLAIM, &claim);
        if (result != -RT_EBUSY) return result;
        /* The native JOIN has a 30-second deadline. */
        if (rt_tick_get() - started >= rt_tick_from_millisecond(35000))
            return -RT_ETIMEOUT;
        rt_thread_mdelay(20);
    }
}

static void release_radio(void)
{
    rt_bool_t claim = RT_FALSE;
    (void)radio_control(WLAN_CTRL_CLAIM, &claim);
}

static wlan_operation_t operation_for(command_kind_t kind)
{
    switch (kind)
    {
    case CMD_SCAN: return WLAN_OP_SCAN;
    case CMD_CONNECT: return WLAN_OP_CONNECT;
    case CMD_DISCONNECT: return WLAN_OP_DISCONNECT;
    case CMD_FORGET: return WLAN_OP_FORGET;
    case CMD_AUTO: return WLAN_OP_AUTOCONNECT;
    case CMD_BAND: return WLAN_OP_BAND;
    case CMD_POWERSAVE: return WLAN_OP_POWERSAVE;
    case CMD_IPV4: return WLAN_OP_IPV4;
    case CMD_AP_START: return WLAN_OP_HOTSPOT_START;
    case CMD_AP_STOP: return WLAN_OP_HOTSPOT_STOP;
    case CMD_AP_DEAUTH: return WLAN_OP_HOTSPOT_DEAUTH;
    default: return WLAN_OP_NONE;
    }
}

static bool command_cancelled(const command_t *command)
{
    take(); bool cancelled = command->epoch != operation_epoch || sleep_hold || !enabled; give();
    return cancelled || reset_held() || system_reset_blocks_apps();
}

static rt_err_t stop_hotspot(void)
{
    rt_err_t result = RT_EOK;
    if (rt_wlan_ap_is_active() && physical_radio_ready())
    {
        result = rt_wlan_ap_stop();
        /* An offline module cannot acknowledge STOP_AP. MODE NONE performs
         * the guarded teardown and clears the stale RT interface state. */
        if (result != RT_EOK && !physical_radio_ready()) result = RT_EOK;
    }
    if (result == RT_EOK) result = wlan_ap_network_stop();
    if (result == RT_EOK && rt_wlan_get_mode(RT_WLAN_DEVICE_AP_NAME) != RT_WLAN_NONE)
        result = rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_NONE);
    if (result == RT_EOK)
    {
        take(); memset(&hotspot, 0, sizeof(hotspot)); hotspot_client_count = 0;
        hotspot_clients_truncated = false; give();
    }
    return result;
}

static rt_err_t disconnect_for_shutdown(void)
{
    if (!rt_wlan_is_connected() || !physical_radio_ready()) return RT_EOK;
    rt_err_t result = rt_wlan_disconnect();
    return result != RT_EOK && !physical_radio_ready() ? RT_EOK : result;
}

static rt_err_t stop_radio(void)
{
    rt_wlan_config_autoreconnect(false);
    rt_err_t result = stop_hotspot();
    if (result == RT_EOK) result = disconnect_for_shutdown();
    if (result == RT_EOK) result = finish_cfg_jobs();
    if (result == RT_EOK) result = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_NONE);
    return result;
}

bool wlan_service_reset_write_allowed(void)
{
    return resetting && worker && rt_thread_self() == worker;
}

static int cfg_length(void)
{
    struct stat st;
    if (!storage_path_available(CFG_PATH)) return -RT_EBUSY;
    if (!storage_file_recover(CFG_PATH)) return -RT_EIO;
    if (stat(CFG_PATH, &st) != 0) return (errno == ENOENT || errno == -ENOENT) ? 0 : -RT_EIO;
    /* The SDK reads its 16-byte header before validating the payload. */
    if (st.st_size < 16 || (size_t)st.st_size > CFG_MAX_BYTES ||
        ((size_t)st.st_size - 16u) % sizeof(struct rt_wlan_cfg_info)) return -RT_EIO;
    return (int)st.st_size;
}

static int cfg_read(void *data, int size)
{
    int fd = open(CFG_PATH, O_RDONLY), done = 0;
    if (fd < 0) return -1;
    while (done < size)
    {
        int n = read(fd, (char *)data + done, size - done);
        if (n <= 0) break;
        done += n;
    }
    if (close(fd) != 0) return -1;
    return done;
}

static int cfg_write(void *data, int size)
{
    /* A queued SDK connection notification must not repopulate a reset cache. */
    bool allowed = (credentials_result == RT_EOK && !reset_held() && !system_reset_blocks_apps()) ||
        wlan_service_reset_write_allowed();
    bool ok = allowed && size >= 16 && (size_t)size <= CFG_MAX_BYTES &&
        storage_file_replace(CFG_PATH, data, (size_t)size);
    if (allowed) persistence_error = ok ? RT_EOK : -RT_EIO;
    if (ok) ++cfg_persist_sequence;
    wake(WAKE_CHANGE);
    return ok ? size : -1;
}

static const struct rt_wlan_cfg_ops cfg_ops = {cfg_read, cfg_length, cfg_write};

static rt_err_t load_credentials(void)
{
    int length = cfg_length();
    if (length < 0) return length;
    if (length && rt_wlan_cfg_cache_refresh() != RT_EOK)
        return storage_path_available(CFG_PATH) ? -RT_EIO : -RT_EBUSY;
    return RT_EOK;
}

static rt_err_t load_policy(void)
{
    if (!storage_path_available(POLICY_PATH)) return -RT_EBUSY;
    if (!storage_file_recover(POLICY_PATH)) return -RT_EIO;
    struct stat st;
    uint32_t preference = 1;
    if (stat(POLICY_PATH, &st) == 0)
    {
        if (!storage_record_load(POLICY_PATH, POLICY_MAGIC, 1, &preference, sizeof(preference)) || preference > 1)
            return storage_path_available(POLICY_PATH) ? -RT_EIO : -RT_EBUSY;
    }
    else if (errno != ENOENT && errno != -ENOENT) return -RT_EIO;
    take(); automatic = preference != 0; give();
    return RT_EOK;
}

static bool channel_allowed(wlan_band_t band, int channel)
{
    return band == WLAN_BAND_DUAL ||
        (band == WLAN_BAND_2_4_GHZ ? channel >= 1 && channel <= 14 : channel > 14);
}

static rt_err_t apply_band(wlan_band_t band)
{
    rt_err_t result = radio_control(WLAN_CTRL_SET_STA_BAND, &band);
    if (result == RT_EOK)
    {
        take();
        sta_band = band;
        give();
    }
    return result;
}

static rt_err_t load_band(void)
{
    if (!storage_path_available(BAND_PATH)) return -RT_EBUSY;
    if (!storage_file_recover(BAND_PATH)) return -RT_EIO;
    uint32_t value = WLAN_BAND_2_4_GHZ;
    struct stat st;
    if (stat(BAND_PATH, &st) == 0)
    {
        if (!storage_record_load(BAND_PATH, BAND_MAGIC, 1, &value, sizeof(value)) ||
            value > WLAN_BAND_5_GHZ)
            return storage_path_available(BAND_PATH) ? -RT_EIO : -RT_EBUSY;
    }
    else if (errno != ENOENT && errno != -ENOENT) return -RT_EIO;
    rt_err_t result = apply_band((wlan_band_t)value);
    return result == -RT_ENOSYS ? -RT_EBUSY : result;
}

static rt_err_t save_band(wlan_band_t band)
{
    uint32_t value = band;
    if (!storage_record_save(BAND_PATH, BAND_MAGIC, 1, &value, sizeof(value))) return -RT_EIO;
    band_result = apply_band(band);
    return band_result;
}

static bool load_config(bool retry)
{
    bool changed = false;
    if (credentials_result == -RT_EBUSY || (retry && credentials_result != RT_EOK))
    {
        credentials_result = load_credentials();
        changed |= credentials_result == RT_EOK;
    }
    if (policy_result == -RT_EBUSY || (retry && policy_result != RT_EOK))
    {
        policy_result = load_policy();
        changed |= policy_result == RT_EOK;
    }
    if (band_result == -RT_EBUSY || (retry && band_result != RT_EOK))
    {
        band_result = load_band();
        changed |= band_result == RT_EOK;
    }
    if (ipv4_load_result == -RT_EBUSY || (retry && ipv4_load_result != RT_EOK))
        ipv4_load_result = wlan_ipv4_load();
    return changed;
}

static bool config_pending(void)
{
    return credentials_result == -RT_EBUSY || policy_result == -RT_EBUSY ||
        band_result == -RT_EBUSY || ipv4_load_result == -RT_EBUSY;
}

static rt_err_t config_error(void)
{
    return credentials_result ? credentials_result : policy_result ? policy_result :
        band_result ? band_result : ipv4_load_result;
}

static void restore_sleep_powersave(void)
{
    take();
    int powersave = sleep_powersave;
    uint32_t epoch = operation_epoch;
    bool restore = powersave >= 0 && sleep_powersave_attempts < POWERSAVE_ATTEMPTS &&
        enabled && !sleep_hold && !(hotspot.active && !hotspot.concurrent) &&
        (!sleep_powersave_attempts ||
         rt_tick_get() - sleep_powersave_retry_at >= rt_tick_from_millisecond(5000));
    give();
    if (!restore || reset_held() || system_reset_blocks_apps() || !driver_ready()) return;
    rt_err_t result = rt_wlan_set_powersave(powersave);
    take();
    if (operation_epoch == epoch && !sleep_hold && sleep_powersave == powersave)
    {
        sleep_powersave_error = result;
        sleep_powersave_retry_at = rt_tick_get();
        if (++sleep_powersave_attempts >= POWERSAVE_ATTEMPTS || result == RT_EOK || result == -RT_ENOSYS)
            sleep_powersave = -1;
    }
    give();
}

static void enable_connected_powersave(unsigned *attempts)
{
    if (*attempts >= POWERSAVE_ATTEMPTS) return;
    take();
    /* Restore any saved level before choosing the default for a new STA session. */
    bool eligible = enabled && !sleep_hold && capabilities.powersave && !hotspot.active &&
        sleep_powersave < 0 && sleep_powersave_error == RT_EOK;
    give();
    if (!eligible || reset_held() || system_reset_blocks_apps() || !driver_ready() ||
        !rt_wlan_is_connected() || rt_wlan_ap_is_active()) return;

    int level;
    rt_err_t result = radio_control(RT_WLAN_CMD_GET_POWERSAVE, &level);
    /* The D40-N adapter reports ENOSYS until a level has been selected. */
    if (result == -RT_ENOSYS)
    {
        result = rt_wlan_set_powersave(WLAN_STA_POWERSAVE_MAX_LEVEL);
        if (result == RT_EOK) LOG_I("STA power save enabled");
    }
    if (result == RT_EOK || result == -RT_ENOSYS) *attempts = POWERSAVE_ATTEMPTS;
    else if (++*attempts == POWERSAVE_ATTEMPTS) LOG_W("STA power save failed: %d", result);
}

static void sdk_event(int event, struct rt_wlan_buff *buff, void *parameter)
{
    (void)buff; (void)parameter;
    if (event == RT_WLAN_EVT_SCAN_DONE) ++scan_completed;
    if (event == RT_WLAN_EVT_STA_CONNECTED_FAIL) connection_error = -RT_ERROR;
    if (event == RT_WLAN_EVT_STA_CONNECTED) connection_error = RT_EOK;
    /* The SDK can call this with its management/device lock held. */
    bool sta_changed = event == RT_WLAN_EVT_STA_CONNECTED || event == RT_WLAN_EVT_STA_DISCONNECTED;
    wake(WAKE_CHANGE | (sta_changed ? WAKE_STA_CHANGE : 0));
    network_changed();
}

static wlan_network_t network_from_info(const struct rt_wlan_info *info)
{
    wlan_network_t row = {0};
    unsigned length = info->ssid.len;
    if (length > WLAN_SERVICE_SSID_BYTES) length = WLAN_SERVICE_SSID_BYTES;
    memcpy(row.id.ssid, info->ssid.val, length);
    row.id.ssid_length = (uint8_t)length;
    memcpy(row.id.bssid, info->bssid, 6);
    row.id.security = (uint32_t)info->security;
    row.channel = info->channel;
    row.rssi = info->rssi;
    /* UNKNOWN must still allow password entry, including an empty password. */
    row.secure = info->security != SECURITY_OPEN;
    return row;
}

static void merge_row(wlan_snapshot_t *snapshot, wlan_network_t row)
{
    if (!row.id.ssid_length) return;
    for (unsigned i = 0; i < snapshot->count; ++i)
    {
        wlan_network_t *old = &snapshot->networks[i];
        if (!wlan_network_id_equal(&old->id, &row.id)) continue;
        old->saved |= row.saved;
        old->connected |= row.connected;
        old->ready |= row.ready;
        if (row.available) { old->available = true; old->rssi = row.rssi; old->channel = row.channel; }
        return;
    }
    snapshot->networks[snapshot->count++] = row;
}

static void refresh_snapshot(rt_err_t result)
{
    refresh_capabilities();
    wlan_snapshot_t next = {0};
    next.available = radio_seen || driver_ready();
    next.connected = driver_ready() && rt_wlan_is_connected();
    next.ready = next.connected && rt_wlan_is_ready();
    next.powersave_level = -1;
    next.powersave_error = next.rssi_error = -RT_ENOSYS;
    if (driver_ready())
    {
        next.powersave_error = radio_control(RT_WLAN_CMD_GET_POWERSAVE, &next.powersave_level);
    }
    int count = rt_wlan_scan_get_info_num();
    if (count < 0) count = 0;
    if (count > RT_WLAN_SCAN_CACHE_NUM) { count = RT_WLAN_SCAN_CACHE_NUM; next.truncated = true; }
    unsigned capacity = (unsigned)count + RT_WLAN_CFG_INFO_MAX + 1;
    next.networks = epd_app_alloc(capacity * sizeof(*next.networks), EPD_APP_PSRAM);
    if (next.networks) memset(next.networks, 0, capacity * sizeof(*next.networks));
    struct rt_wlan_info *scan = count ?
        epd_app_alloc((size_t)count * sizeof(*scan), EPD_APP_PSRAM) : NULL;
    if (!next.networks || (count && !scan))
    {
        epd_app_free(scan); epd_app_free(next.networks);
        take();
        published.available = next.available;
        published.enabled = enabled; published.automatic = automatic;
        published.band = sta_band;
        published.connected = next.connected; published.ready = next.ready;
        published.rssi_error = -RT_ENOMEM;
        published.suspended = sleep_hold && sleep_result == RT_EOK;
        published.operation = active_operation;
        published.scanning = active_operation == WLAN_OP_SCAN;
        published.busy = working || pending.kind != CMD_NONE || policy_dirty || reset_pending;
        published.error = -RT_ENOMEM; ++published.revision;
        give();
        return;
    }
    int copied = count ? rt_wlan_scan_get_info(scan, count) : 0;
    for (int i = 0; i < copied; ++i)
    {
        wlan_network_t row = network_from_info(&scan[i]);
        if (!channel_allowed(sta_band, row.channel)) continue;
        row.available = true;
        merge_row(&next, row);
    }
    epd_app_free(scan);
    struct rt_wlan_cfg_info saved;
    for (int i = 0; i < RT_WLAN_CFG_INFO_MAX && rt_wlan_cfg_read_index(&saved, i) == 1; ++i)
    {
        wlan_network_t row = network_from_info(&saved.info); row.saved = true; merge_row(&next, row);
    }
    memset(&saved, 0, sizeof(saved));
    struct rt_wlan_info current;
    if (next.connected && rt_wlan_get_info(&current) == RT_EOK)
    {
        next.current = network_from_info(&current);
        /* A saved-network join can leave RT's information cache without a
         * channel. Ask the device for the live channel before AP coexistence. */
        int channel = 0;
        if (radio_control(RT_WLAN_CMD_GET_CHANNEL, &channel) == RT_EOK && channel > 0 && channel <= 165)
            next.current.channel = (int16_t)channel;
        /* The RT convenience getter returns zero on failure; keep the control result. */
        int rssi = 0;
        next.rssi_error = radio_control(RT_WLAN_CMD_GET_RSSI, &rssi);
        if (next.rssi_error == RT_EOK)
        {
            if (rssi < 0 && rssi >= -127) next.current.rssi = (int16_t)rssi;
            else next.rssi_error = -RT_EIO;
        }
        next.current.connected = true; next.current.ready = next.ready;
        merge_row(&next, next.current);
    }
    struct rt_wlan_info clients[CLIENTS_MAX];
    bool radio_live = physical_radio_ready();
    int client_count = radio_live && rt_wlan_ap_is_active() ? rt_wlan_ap_get_sta_info(clients, CLIENTS_MAX) : 0;
    if (client_count < 0) client_count = 0;
    if (client_count > (int)CLIENTS_MAX) client_count = CLIENTS_MAX;
    bool ap_active = rt_wlan_ap_is_active();
    bool clients_truncated = rt_wlan_ap_get_sta_num() > (int)CLIENTS_MAX;
    take();
    hotspot.active = ap_active;
    hotspot_client_count = (unsigned)client_count;
    for (unsigned i = 0; i < hotspot_client_count; ++i) memcpy(hotspot_clients[i], clients[i].bssid, 6);
    hotspot_clients_truncated = clients_truncated;
    hotspot.clients = hotspot_client_count;
    next.hotspot = hotspot;
    next.hotspot.active = ap_active && radio_live;
    next.enabled = enabled; next.automatic = automatic;
    next.band = sta_band;
    next.suspended = sleep_hold && sleep_result == RT_EOK;
    next.operation = active_operation;
    next.scanning = active_operation == WLAN_OP_SCAN;
    next.busy = working || pending.kind != CMD_NONE || policy_dirty || reset_pending;
    if (next.powersave_error == RT_EOK && sleep_powersave_error != RT_EOK)
        next.powersave_error = sleep_powersave_error;
    next.error = result ? result : persistence_error ? persistence_error :
        connection_error ? connection_error : sleep_powersave_error;
    if (!next.error && enabled && ap_active && !radio_live) next.error = -RT_EIO;
    bool changed = next.count != published.count || next.available != published.available ||
        next.enabled != published.enabled || next.automatic != published.automatic ||
        next.band != published.band || next.busy != published.busy ||
        next.connected != published.connected || next.ready != published.ready || next.error != published.error ||
        next.truncated != published.truncated || next.suspended != published.suspended ||
        next.operation != published.operation || next.powersave_level != published.powersave_level ||
        next.powersave_error != published.powersave_error || next.rssi_error != published.rssi_error ||
        memcmp(&next.hotspot, &published.hotspot, sizeof(next.hotspot)) ||
        memcmp(&next.current, &published.current, sizeof(next.current)) ||
        (next.count && memcmp(next.networks, published.networks, next.count * sizeof(*next.networks)));
    next.revision = published.revision + (changed ? 1u : 0u);
    wlan_network_t *old = published.networks;
    published = next;
    give();
    if (changed) ui_app_status_changed();
    epd_app_free(old);
}

static bool find_saved(const wlan_network_id_t *id, struct rt_wlan_cfg_info *out, int *index)
{
    for (int i = 0; i < RT_WLAN_CFG_INFO_MAX && rt_wlan_cfg_read_index(out, i) == 1; ++i)
    {
        wlan_network_t row = network_from_info(&out->info);
        if (wlan_network_id_equal(id, &row.id)) { if (index) *index = i; return true; }
    }
    memset(out, 0, sizeof(*out));
    return false;
}

static bool same_saved_network(const wlan_network_id_t *a, const wlan_network_id_t *b)
{
    return a->ssid_length && a->ssid_length == b->ssid_length &&
        a->security == b->security && !memcmp(a->ssid, b->ssid, a->ssid_length);
}

/* Called with lock held; SDK calls stay outside this lock. */
static bool auto_allowed(void)
{
    return enabled && (automatic || sleep_reconnect) && !manual_disconnect &&
        !reset_paused && !sleep_hold && !reset_pending && !policy_dirty &&
        !(hotspot.active && !hotspot.concurrent) &&
        credentials_result == RT_EOK && policy_result == RT_EOK && band_result == RT_EOK;
}

static void auto_scan_finished(void)
{
    auto_scan_at = rt_tick_get();
    auto_scan_delay = auto_scan_delay ? auto_scan_delay * 2 : 15000;
    if (auto_scan_delay > 60000) auto_scan_delay = 60000;
}

static bool auto_blocked(const wlan_network_id_t *id)
{
    for (unsigned i = 0; i < RT_WLAN_CFG_INFO_MAX; ++i)
        if (same_saved_network(id, &auto_failed[i].id)) return true;
    return false;
}

static void auto_connect_failed(const wlan_network_id_t *id)
{
    for (unsigned i = 0; i < RT_WLAN_CFG_INFO_MAX; ++i)
        if (!auto_failed[i].id.ssid_length || same_saved_network(id, &auto_failed[i].id))
        {
            auto_failed[i].id = *id;
            auto_failed[i].missed_scans = 0;
            return;
        }
}

static bool auto_pick(const struct rt_wlan_info *scan, int count,
                      const wlan_network_id_t *preferred, bool full_scan, command_t *command)
{
    if (full_scan && count < RT_WLAN_SCAN_CACHE_NUM)
        for (unsigned i = 0; i < RT_WLAN_CFG_INFO_MAX; ++i)
        {
            if (!auto_failed[i].id.ssid_length) continue;
            bool visible = false;
            for (int j = 0; j < count; ++j)
            {
                wlan_network_t row = network_from_info(&scan[j]);
                if (same_saved_network(&auto_failed[i].id, &row.id)) { visible = true; break; }
            }
            if (visible) auto_failed[i].missed_scans = 0;
            else if (++auto_failed[i].missed_scans >= 2)
                memset(&auto_failed[i], 0, sizeof(auto_failed[i]));
        }
    int best_rssi = -128;
    bool found = false;
    struct rt_wlan_cfg_info saved;
    for (int i = 0; i < RT_WLAN_CFG_INFO_MAX && rt_wlan_cfg_read_index(&saved, i) == 1; ++i)
    {
        wlan_network_t profile = network_from_info(&saved.info);
        if (saved.key.len > WLAN_SERVICE_PASSWORD_BYTES ||
            profile.id.security == (uint32_t)SECURITY_UNKNOWN || auto_blocked(&profile.id) ||
            (preferred && !same_saved_network(preferred, &profile.id))) continue;
        for (int j = 0; j < count; ++j)
        {
            wlan_network_t row = network_from_info(&scan[j]);
            if (!channel_allowed(sta_band, row.channel) ||
                !same_saved_network(&profile.id, &row.id) || row.rssi <= best_rssi) continue;
            memset(command, 0, sizeof(*command));
            command->kind = CMD_CONNECT;
            command->background = true;
            command->id = row.id;
            command->channel = row.channel;
            memcpy(command->password, saved.key.val, saved.key.len);
            command->password[saved.key.len] = 0;
            best_rssi = row.rssi;
            found = true;
        }
    }
    memset(&saved, 0, sizeof(saved));
    return found;
}

static void auto_after_scan(const command_t *scan_command)
{
    if (rt_wlan_is_connected() || command_cancelled(scan_command)) return;
    take();
    bool allowed = auto_allowed() && pending.kind == CMD_NONE;
    bool restore_only = !automatic && sleep_reconnect;
    wlan_network_id_t preferred = sleep_network;
    give();
    if (!allowed) return;
    int count = rt_wlan_scan_get_info_num();
    if (count < 0) return;
    if (count > RT_WLAN_SCAN_CACHE_NUM) count = RT_WLAN_SCAN_CACHE_NUM;
    struct rt_wlan_info *scan = count ? epd_app_alloc(count * sizeof(*scan), EPD_APP_PSRAM) : NULL;
    if (count && !scan) return;
    int copied = count ? rt_wlan_scan_get_info(scan, count) : 0;
    command_t next = {0};
    bool found = copied >= 0 && auto_pick(scan, copied, restore_only ? &preferred : NULL,
        !scan_command->directed, &next);
    epd_app_free(scan);
    take();
    if (auto_allowed() && pending.kind == CMD_NONE && scan_command->epoch == operation_epoch)
    {
        /* Resume with auto-connect off makes one scan for the previous link. */
        if (copied >= 0 && !found) sleep_reconnect = false;
        if (found)
        {
            pending = next;
            pending.epoch = operation_epoch;
            wake(WAKE_COMMAND);
        }
    }
    give();
    memset(&next, 0, sizeof(next));
}

static void cfg_queue_fence(void *parameter)
{
    cfg_fence_done = (uint32_t)(uintptr_t)parameter;
    wake(WAKE_CFG_IDLE);
}

/* Auto-connect is disabled by the caller. Finish the SDK's queued credential
 * saves before deleting entries so an older save cannot restore them. */
static rt_err_t finish_cfg_jobs(void)
{
    uint32_t serial = ++cfg_fence_sent, received;
    rt_err_t result = rt_wlan_workqueue_dowork(cfg_queue_fence, (void *)(uintptr_t)serial);
    if (result != RT_EOK) return result;
    rt_tick_t started = rt_tick_get(), limit = rt_tick_from_millisecond(5000);
    while (cfg_fence_done != serial)
    {
        rt_tick_t elapsed = rt_tick_get() - started;
        if (elapsed >= limit) return -RT_ETIMEOUT;
        result = rt_event_recv(&wakeup, WAKE_CFG_IDLE, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                               limit - elapsed, &received);
        if (result != RT_EOK) return result;
    }
    return RT_EOK;
}

static rt_err_t run_command(command_t *command)
{
    if (command_cancelled(command)) return -RT_EBUSY;
    if (command->kind == CMD_AUTO)
    {
        uint32_t value = command->automatic ? 1 : 0;
        if (!storage_record_save(POLICY_PATH, POLICY_MAGIC, 1, &value, sizeof(value))) return -RT_EIO;
        policy_result = RT_EOK;
        take(); automatic = command->automatic; manual_disconnect = false; sleep_reconnect = false;
        auto_reset_requested = true; give();
        return RT_EOK;
    }
    if (command->kind == CMD_BAND)
    {
        if (sta_band == command->band && band_result == RT_EOK) return RT_EOK;
        if (command->band != WLAN_BAND_DUAL && rt_wlan_is_connected())
        {
            int channel = 0;
            rt_err_t result = radio_control(RT_WLAN_CMD_GET_CHANNEL, &channel);
            if (result != RT_EOK) return result;
            if (!channel_allowed(command->band, channel))
            {
                result = rt_wlan_disconnect();
                if (result != RT_EOK) return result;
            }
        }
        if (command_cancelled(command)) return -RT_EBUSY;
        rt_err_t result = save_band(command->band);
        if (result != RT_EOK) return result;
        rt_wlan_scan_result_clean();
        take();
        manual_disconnect = false;
        sleep_reconnect = false;
        auto_reset_requested = true;
        if (command->epoch == operation_epoch && pending.kind == CMD_NONE)
            pending = (command_t){.kind = CMD_SCAN, .epoch = operation_epoch};
        give();
        wake(WAKE_COMMAND);
        return RT_EOK;
    }
    if (command->kind == CMD_SCAN)
    {
        uint32_t serial = scan_completed;
        rt_wlan_scan_result_clean();
        struct rt_wlan_info filter = {0};
        filter.ssid.len = command->id.ssid_length;
        memcpy(filter.ssid.val, command->id.ssid, filter.ssid.len);
        memcpy(filter.bssid, command->id.bssid, 6);
        filter.channel = -1;
        struct rt_wlan_scan_result *scan = command->directed ? rt_wlan_scan_with_info(&filter) : rt_wlan_scan_sync();
        if (command_cancelled(command)) { rt_wlan_scan_result_clean(); return -RT_EBUSY; }
        return scan && scan_completed != serial ? RT_EOK : -RT_ERROR;
    }
    if (command->kind == CMD_POWERSAVE)
    {
        rt_err_t result = rt_wlan_set_powersave(command->level);
        take(); sleep_powersave = -1; sleep_powersave_error = result; give();
        return result;
    }
    if (command->kind == CMD_IPV4)
    {
        rt_err_t result = wlan_ipv4_save_and_apply(&command->ipv4);
        wlan_ipv4_state_t state;
        wlan_ipv4_get(&state);
        if (state.loaded) ipv4_load_result = RT_EOK;
        return result;
    }
    if (command->kind == CMD_AP_STOP)
    {
        rt_err_t result = stop_hotspot();
        if (result == RT_EOK) result = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
        return result;
    }
    if (command->kind == CMD_AP_DEAUTH) return rt_wlan_ap_deauth_sta(command->id.bssid);
    if (command->kind == CMD_AP_START)
    {
        /* RT may treat a same-length key change as an unchanged active AP. */
        rt_err_t result = stop_hotspot();
        if (result != RT_EOK) return result;
        if (!command->concurrent)
        {
            result = disconnect_for_shutdown();
            if (result == RT_EOK) result = finish_cfg_jobs();
            if (result == RT_EOK) result = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_NONE);
            if (result != RT_EOK) return result;
        }
        else
        {
            result = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
            if (result != RT_EOK) return result;
        }
        result = rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP);
        if (result != RT_EOK)
        {
            if (!command->concurrent) (void)rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
            return result;
        }
        struct rt_wlan_info info = {0};
        info.ssid.len = command->id.ssid_length;
        memcpy(info.ssid.val, command->id.ssid, info.ssid.len);
        info.security = (rt_wlan_security_t)command->id.security;
        info.channel = (rt_int16_t)command->channel;
        info.hidden = command->hidden;
        result = rt_wlan_start_ap_adv(&info, command->password);
        if (result == RT_EOK) result = wlan_ap_network_start();
        if (result != RT_EOK)
        {
            rt_err_t stopped = stop_hotspot();
            if (stopped == RT_EOK && !command->concurrent)
                (void)rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
            return result;
        }
        if (command_cancelled(command)) { (void)stop_hotspot(); return -RT_EBUSY; }
        take();
        hotspot.active = true; hotspot.concurrent = command->concurrent; hotspot.hidden = command->hidden;
        hotspot.channel = command->channel; hotspot.security = command->id.security;
        memcpy(hotspot.ssid, command->id.ssid, sizeof(hotspot.ssid));
        give();
        /* AP remains a local network and must never replace the upstream route. */
        return network_register_local_interface(NETWORK_WIFI_AP, "w1");
    }
    if (command->kind == CMD_DISCONNECT)
    {
        take(); manual_disconnect = true; give();
        return rt_wlan_is_connected() ? rt_wlan_disconnect() : RT_EOK;
    }
    struct rt_wlan_cfg_info saved = {0};
    int index = -1;
    if ((command->kind == CMD_CONNECT || command->kind == CMD_FORGET) && credentials_result != RT_EOK)
    {
        credentials_result = load_credentials();
        if (credentials_result != RT_EOK) return credentials_result;
    }
    if (command->kind == CMD_FORGET)
    {
        rt_err_t result = finish_cfg_jobs();
        if (result != RT_EOK) return result;
    }
    bool found = find_saved(&command->id, &saved, &index);
    rt_err_t result = RT_EOK;
    if (command->kind == CMD_FORGET)
    {
        if (!found) return -RT_EINVAL;
        struct rt_wlan_info current;
        if (rt_wlan_is_connected() && rt_wlan_get_info(&current) == RT_EOK)
        {
            wlan_network_t row = network_from_info(&current);
            if (wlan_network_id_equal(&row.id, &command->id)) result = rt_wlan_disconnect();
        }
        if (result == RT_EOK)
        {
            if (rt_wlan_cfg_get_num() == 1) rt_wlan_cfg_delete_all();
            else if (rt_wlan_cfg_delete_index(index) != 0) result = -RT_ERROR;
            if (result == RT_EOK && rt_wlan_cfg_cache_save() != RT_EOK)
            {
                (void)rt_wlan_cfg_save(&saved);
                result = -RT_EIO;
            }
        }
    }
    else if (command->kind == CMD_CONNECT)
    {
        if (command->channel && !channel_allowed(sta_band, command->channel)) result = -RT_EINVAL;
        if (command->reuse_password)
        {
            if (!found || saved.key.len > WLAN_SERVICE_PASSWORD_BYTES) result = -RT_EINVAL;
            else { memcpy(command->password, saved.key.val, saved.key.len); command->password[saved.key.len] = 0; }
        }
        if (result == RT_EOK)
        {
            struct rt_wlan_info info = {0};
            info.ssid.len = command->id.ssid_length;
            memcpy(info.ssid.val, command->id.ssid, command->id.ssid_length);
            memcpy(info.bssid, command->id.bssid, 6);
            info.security = (rt_wlan_security_t)command->id.security;
            info.channel = (rt_int16_t)command->channel;
            size_t length = strlen(command->password);
            bool previously_stored = found && saved.key.len == length &&
                !memcmp(saved.key.val, command->password, length);
            uint32_t persisted_before = cfg_persist_sequence;
            if (command_cancelled(command))
            {
                memset(&saved, 0, sizeof(saved));
                return -RT_EBUSY;
            }
            take(); manual_disconnect = false; give();
            /* An explicit SSID also permits hidden networks absent from scans. */
            result = rt_wlan_connect_adv(&info, command->password);
            if (command_cancelled(command))
            {
                if (rt_wlan_is_connected()) (void)rt_wlan_disconnect();
                memset(&saved, 0, sizeof(saved));
                return -RT_EBUSY;
            }
            if (result == RT_EOK) result = finish_cfg_jobs();
            if (command_cancelled(command)) result = -RT_EBUSY;
            else if (result == RT_EOK)
            {
                if (command->by_ssid)
                {
                    struct rt_wlan_info actual;
                    if (rt_wlan_get_info(&actual) == RT_EOK)
                    {
                        wlan_network_t row = network_from_info(&actual);
                        command->id = row.id;
                        info = actual;
                    }
                }
                /* The driver reports CONNECT before returning. The SDK saves
                 * credentials in its workqueue and does not report allocation
                 * errors to the caller, so verify that save before completing. */
                bool stored = find_saved(&command->id, &saved, NULL) &&
                    saved.key.len == length && !memcmp(saved.key.val, command->password, length);
                if (stored && (persistence_error ||
                    (!previously_stored && cfg_persist_sequence == persisted_before)))
                {
                    /* cfg_save skips identical cached keys, including after a
                     * previous disk write failed. Retry the serialization here. */
                    result = rt_wlan_cfg_cache_save();
                    if (result != RT_EOK)
                    {
                        if (result == -RT_ERROR) result = -RT_EIO;
                        persistence_error = result;
                    }
                }
                else if (!stored)
                {
                    memset(&saved, 0, sizeof(saved));
                    saved.info = info;
                    saved.key.len = (rt_uint8_t)length;
                    memcpy(saved.key.val, command->password, length);
                    result = rt_wlan_cfg_save(&saved);
                    if (result != RT_EOK)
                    {
                        if (result == -RT_ERROR) result = -RT_EIO;
                        persistence_error = result;
                    }
                }
            }
        }
    }
    if (command->kind == CMD_CONNECT && command_cancelled(command))
    {
        if (rt_wlan_is_connected()) (void)rt_wlan_disconnect();
        result = -RT_EBUSY;
    }
    memset(&saved, 0, sizeof(saved));
    return result;
}

static rt_err_t perform_reset(void)
{
    rt_wlan_config_autoreconnect(false);
    rt_err_t result = stop_hotspot();
    if (result == RT_EOK && rt_wlan_is_connected()) result = rt_wlan_disconnect();
    if (result != RT_EOK) return result;
    result = finish_cfg_jobs();
    if (result != RT_EOK) return result;
    resetting = true;
    rt_wlan_cfg_delete_all();
    result = rt_wlan_cfg_cache_save() == RT_EOK ? RT_EOK : -RT_EIO;
    credentials_result = result;
    uint32_t value = 1;
    policy_result = storage_record_save(POLICY_PATH, POLICY_MAGIC, 1, &value, sizeof(value)) ? RT_EOK : -RT_EIO;
    if (policy_result != RT_EOK) result = policy_result;
    else { take(); automatic = true; give(); }
    rt_err_t band_error = save_band(WLAN_BAND_2_4_GHZ);
    if (result == RT_EOK) result = band_error;
    rt_err_t ip_result = wlan_ipv4_reset();
    wlan_ipv4_state_t ip_state;
    wlan_ipv4_get(&ip_state);
    ipv4_load_result = ip_state.loaded ? RT_EOK : ip_result;
    if (result == RT_EOK) result = ip_result;
    resetting = false;
    if (result == RT_EOK)
    {
        take(); manual_disconnect = false; auto_reset_requested = true; give();
        persistence_error = connection_error = RT_EOK;
    }
    return result;
}

static void wlan_worker(void *parameter)
{
    (void)parameter;
    rt_wlan_cfg_set_ops(&cfg_ops);
    const rt_wlan_event_t events[] = {RT_WLAN_EVT_READY, RT_WLAN_EVT_SCAN_DONE, RT_WLAN_EVT_STA_CONNECTED,
        RT_WLAN_EVT_STA_CONNECTED_FAIL, RT_WLAN_EVT_STA_DISCONNECTED, RT_WLAN_EVT_AP_START,
        RT_WLAN_EVT_AP_STOP, RT_WLAN_EVT_AP_ASSOCIATED, RT_WLAN_EVT_AP_DISASSOCIATED};
    for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); ++i)
        (void)rt_wlan_register_event_handler(events[i], sdk_event, NULL);
    (void)network_register_interface(NETWORK_WIFI, "w0");
    rt_err_t result = RT_EOK;
    bool ready_before = false, held_before = false;
    unsigned off_retries = 0, powersave_attempts = 0;
    unsigned sleep_retries = 0;
    uint32_t sleep_retry_epoch = 0;
    open_retry_t open_retry = {0};
    uint32_t received = 0;
    take(); update_power_permission(); give();
    for (;;)
    {
        bool ready = driver_ready();
        bool connected = ready && rt_wlan_is_connected();
        if (!connected || (received & WAKE_STA_CHANGE)) powersave_attempts = 0;
        if (!radio_seen)
        {
            struct wlan_control_caps caps = {0};
            radio_seen = radio_control(WLAN_CTRL_GET_CAPS, &caps) == RT_EOK && caps.version == 1;
        }
        bool held = reset_held();
        bool loaded_now = false;
        take();
        bool load = enabled && !sleep_hold && !held && !system_reset_blocks_apps();
        bool retry = config_retry_requested;
        if (load) config_retry_requested = false;
        give();
        if (load) loaded_now = load_config(retry);
        int saved_count = credentials_result == RT_EOK ? rt_wlan_cfg_get_num() : 0;
        take();
        if (auto_reset_requested)
        {
            memset(auto_failed, 0, sizeof(auto_failed));
            auto_scan_delay = 0;
            auto_reset_requested = false;
        }
        if (connected || sleep_hold || !enabled) auto_scan_delay = 0;
        if (connected) sleep_reconnect = false;
        if (reset_paused && !held) { reset_paused = false; policy_dirty = true; }
        command_t command = pending; memset(&pending, 0, sizeof(pending));
        if (command.background && (!auto_allowed() || connected || held || system_reset_blocks_apps()))
            memset(&command, 0, sizeof(command));
        if (command.kind == CMD_NONE && ready && !connected && saved_count > 0 &&
            auto_allowed() && !held && !system_reset_blocks_apps() &&
            (!auto_scan_delay || rt_tick_get() - auto_scan_at >= rt_tick_from_millisecond(auto_scan_delay)))
        {
            command.kind = CMD_SCAN;
            command.background = true;
            command.epoch = operation_epoch;
        }
        bool reset = reset_pending;
        reset_pending = false;
        bool retry_open = open_retry_due(&open_retry, operation_epoch,
            enabled && !ready && !sleep_hold && !held && !reset);
        if (command.kind == CMD_NONE) command.epoch = operation_epoch;
        bool apply = policy_dirty || loaded_now || ready != ready_before || held != held_before ||
            command.kind != CMD_NONE || reset || retry_open;
        policy_dirty = false;
        working = apply;
        background_work = command.background;
        bool on = enabled;
        bool sleeping = sleep_hold;
        uint32_t handoff_epoch = operation_epoch;
        if (!sleeping || sleep_retry_epoch != handoff_epoch)
        {
            sleep_retries = 0;
            sleep_retry_epoch = handoff_epoch;
        }
        if (on || sleeping) off_retries = 0;
        active_operation = command.kind != CMD_NONE ? operation_for(command.kind) :
            apply ? sleeping ? WLAN_OP_SUSPEND : WLAN_OP_ENABLE : WLAN_OP_NONE;
        published.operation = active_operation;
        published.scanning = active_operation == WLAN_OP_SCAN;
        if (apply) { published.busy = true; ++published.revision; }
        give();
        if (apply) network_changed();
        rt_err_t access_result = apply ? claim_radio() : RT_EOK;
        if (apply && access_result == RT_EOK) rt_wlan_config_autoreconnect(false);
        if (access_result != RT_EOK)
        {
            result = access_result;
            if (sleeping) rt_kprintf("wlan: sleep handoff claim failed (%d)\n", result);
            take();
            if (reset) reset_result = result;
            give();
        }
        else if (reset)
        {
            result = perform_reset();
            take(); reset_result = result; give();
        }
        else if (apply && (sleeping || (radio_seen && !on)))
        {
            take(); bool capture = sleeping && sleep_capture;
            uint32_t capture_epoch = operation_epoch; give();
            if (capture)
            {
                struct rt_wlan_info info;
                bool connected = rt_wlan_is_connected() && rt_wlan_get_info(&info) == RT_EOK;
                int powersave = -1;
                (void)radio_control(RT_WLAN_CMD_GET_POWERSAVE, &powersave);
                take();
                if (sleep_capture && enabled && capture_epoch == operation_epoch)
                {
                    sleep_capture = false;
                    sleep_reconnect = connected && !manual_disconnect;
                    if (sleep_reconnect) sleep_network = network_from_info(&info).id;
                    sleep_powersave = powersave;
                    sleep_powersave_attempts = 0;
                }
                give();
            }
            result = stop_radio();
            if (result != RT_EOK)
            {
                if (sleeping) rt_kprintf("wlan: sleep handoff stop failed (%d)\n", result);
            }
            else
            {
                result = radio_control(WLAN_CTRL_PREPARE_DEEP, NULL);
                if (sleeping && result != RT_EOK)
                    rt_kprintf("wlan: sleep handoff SDIO/power failed (%d)\n", result);
            }
        }
        else if (command.kind != CMD_NONE || (apply && on && radio_seen && !held && !open_retry.pending))
        {
            if (!on || !radio_seen || held || sleeping || system_reset_blocks_apps() ||
                command_cancelled(&command)) result = -RT_EBUSY;
            else
            {
                rt_bool_t powered_down = RT_FALSE;
                result = physical_radio_ready() ? RT_EOK : radio_control(WLAN_CTRL_POWER_ON, &powered_down);
                open_retry_result(&open_retry, result, powered_down);
                if (result == RT_EOK && command.kind != CMD_AP_START && command.kind != CMD_AP_STOP &&
                    !(hotspot.active && !hotspot.concurrent))
                    result = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
                if (result == RT_EOK && command.kind != CMD_AP_START && command.kind != CMD_AP_STOP &&
                    !(hotspot.active && !hotspot.concurrent) && !driver_ready()) result = -RT_EIO;
                if (result == RT_EOK && command.kind != CMD_POWERSAVE) restore_sleep_powersave();
                if (result == RT_EOK && command.kind != CMD_NONE) result = run_command(&command);
            }
            bool cancelled = command.kind != CMD_NONE && command_cancelled(&command);
            if (command.kind == CMD_SCAN)
            {
                if (result == RT_EOK && !cancelled) auto_after_scan(&command);
            }
            if (command.kind == CMD_CONNECT && command.background)
            {
                if (result != RT_EOK && !cancelled && !rt_wlan_is_connected())
                    auto_connect_failed(&command.id);
                take(); sleep_reconnect = false; give();
            }
            if (cancelled) { result = RT_EOK; connection_error = RT_EOK; }
            if (command.kind == CMD_CONNECT && result != RT_EOK && !persistence_error)
            {
                /* A later connection success clears only the connection
                 * error, without hiding a credential persistence failure. */
                connection_error = result;
                if (!command.background) { take(); manual_disconnect = true; give(); }
                result = RT_EOK;
            }
        }
        if (command.kind == CMD_SCAN) auto_scan_finished();
        if (!apply) restore_sleep_powersave();
        memset(&command, 0, sizeof(command));
        rt_err_t ip_result = RT_EOK;
        if (access_result == RT_EOK && ipv4_load_result == RT_EOK && !held && !sleeping)
        {
            ip_result = wlan_ipv4_apply();
        }
        if (access_result == RT_EOK && rt_wlan_ap_is_active() && !held && !sleeping)
        {
            rt_err_t ap_result = wlan_ap_network_check();
            if (ap_result != RT_EOK)
            {
                /* A local AP conflict must not stop the upstream STA. */
                rt_err_t stopped = stop_hotspot();
                result = stopped != RT_EOK ? stopped : ap_result;
            }
        }
        if (access_result == RT_EOK && !held && !reset)
            enable_connected_powersave(&powersave_attempts);
        take();
        working = false;
        background_work = false;
        active_operation = WLAN_OP_NONE;
        give();
        /* The platform worker owns scan/selection; SDK cyclic joins stay off. */
        /* CLAIM reserves the caller while waiting for the radio lock. Release
         * that reservation after a timeout too; another owner's is untouched. */
        if (apply) release_radio();
        if (apply && sleeping && result == -RT_ENOSYS && wlan_radio_power_off_confirmed())
            result = RT_EOK;
        bool retry_sleep = false;
        take();
        if (apply && sleeping && !reset && sleep_hold && handoff_epoch == operation_epoch)
        {
            if (result != -RT_EBUSY) sleep_result = result;
            else if (sleep_retries == SLEEP_HANDOFF_RETRY_LIMIT) sleep_result = -RT_ETIMEOUT;
            else { ++sleep_retries; retry_sleep = true; }
        }
        give();
        /* A busy bus or an in-flight teardown can outlast the first attempt.
         * Keep the power veto in place while retrying the checked shutdown. */
        bool retry_off = apply && !on && !sleeping && !held &&
            (result == -RT_EBUSY || result == -RT_ETIMEOUT) && off_retries < 3;
        if (retry_off) ++off_retries;
        else if (apply) off_retries = 0;
        ready_before = driver_ready();
        held_before = held;
        refresh_snapshot(result ? result : config_error() ? config_error() : ip_result);
        network_changed();
        take();
        bool sample_radio = enabled && !sleep_hold;
        bool poll = reset_paused || held || (sample_radio && (!radio_seen || config_pending()));
        bool auto_next = apply && result == RT_EOK && ready_before && saved_count > 0 &&
            !held && !system_reset_blocks_apps() &&
            (auto_reset_requested || (!auto_scan_delay && auto_allowed()));
        give();
        if (auto_next && !rt_wlan_is_connected()) wake(WAKE_CHANGE);
        received = 0;
        rt_int32_t wait = poll || retry_off || retry_sleep ? rt_tick_from_millisecond(500) :
            sample_radio ? rt_tick_from_millisecond(5000) : RT_WAITING_FOREVER;
        rt_int32_t open_wait = open_retry_wait(&open_retry, rt_tick_get());
        if (open_wait != RT_WAITING_FOREVER && (wait == RT_WAITING_FOREVER || open_wait < wait)) wait = open_wait;
        (void)rt_event_recv(&wakeup, WAKE_CHANGE | WAKE_COMMAND | WAKE_STA_CHANGE,
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, wait, &received);
        if (retry_off || retry_sleep)
        {
            take();
            if (retry_off || (sleep_hold && handoff_epoch == operation_epoch)) policy_dirty = true;
            give();
        }
    }
}

rt_err_t wlan_service_start(bool on)
{
    if (worker) return RT_EOK;
    rt_err_t result = rt_mutex_init(&lock, "wlan_s", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK) return result;
    result = rt_event_init(&wakeup, "wlan_w", RT_IPC_FLAG_FIFO);
    if (result != RT_EOK) { rt_mutex_detach(&lock); return result; }
    enabled = on; policy_dirty = true;
    published.enabled = on; published.automatic = true;
    published.powersave_level = -1;
    published.powersave_error = published.rssi_error = -RT_ENOSYS;
    /* The service lives until shutdown. Keep the RT object in SRAM, but leave
     * SRAM for peripheral DMA instead of spending it on this waiting stack. */
    void *stack = epd_app_alloc(6144, EPD_APP_PSRAM);
    worker = stack ? rt_malloc(sizeof(*worker)) : NULL;
    if (worker)
    {
        result = rt_thread_init(worker, "wlan_app", wlan_worker, NULL, stack, 6144, 20, 10);
        if (result == RT_EOK)
        {
            if (rt_thread_startup(worker) == RT_EOK) return RT_EOK;
            rt_thread_detach(worker);
        }
        rt_free(worker);
    }
    epd_app_free(stack);
    worker = NULL;
    rt_event_detach(&wakeup); rt_mutex_detach(&lock);
    return -RT_ENOMEM;
}

static rt_err_t submit(const command_t *command)
{
    if (!worker) return -RT_ENOSYS;
    if (reset_held() || system_reset_blocks_apps()) return -RT_EBUSY;
    take();
    rt_err_t result = RT_EOK;
    bool replace_connect = (command->kind == CMD_CONNECT || command->kind == CMD_DISCONNECT) &&
        (!working || active_operation == WLAN_OP_CONNECT) &&
        (pending.kind == CMD_NONE || pending.kind == CMD_CONNECT);
    bool replace_background = (background_work || pending.background) &&
        (pending.kind == CMD_NONE || pending.background);
    bool replace_policy = (command->kind == CMD_AUTO || command->kind == CMD_BAND) &&
        pending.kind == CMD_NONE &&
        working && (active_operation == WLAN_OP_SCAN || active_operation == WLAN_OP_CONNECT);
    if (!enabled || !published.available) result = -RT_ENOSYS;
    else if (reset_paused || sleep_hold ||
             ((working || pending.kind != CMD_NONE) && !replace_connect && !replace_background && !replace_policy)) result = -RT_EBUSY;
    else if (hotspot.active && !hotspot.concurrent &&
             command->kind != CMD_AP_STOP && command->kind != CMD_AP_START &&
             command->kind != CMD_AP_DEAUTH && command->kind != CMD_AUTO)
        result = -RT_EBUSY;
    else if ((command->kind == CMD_POWERSAVE && !capabilities.powersave) ||
             (command->kind == CMD_IPV4 && !capabilities.ipv4) ||
             (command->kind == CMD_AP_START && (!capabilities.hotspot ||
                (command->hidden && !capabilities.hotspot_hidden) ||
                (command->concurrent && !capabilities.concurrent))) ||
             (command->kind == CMD_AP_DEAUTH && !capabilities.hotspot_deauth) ||
             (command->kind == CMD_SCAN && command->directed && !capabilities.directed_scan))
        result = -RT_ENOSYS;
    else
    {
        if (replace_connect || replace_background || replace_policy)
        {
            ++operation_epoch;
            cancel_radio(replace_background || replace_policy ? WLAN_CANCEL_SCAN | WLAN_CANCEL_JOIN : WLAN_CANCEL_JOIN);
        }
        if (command->kind == CMD_CONNECT || command->kind == CMD_FORGET) auto_reset_requested = true;
        if (command->kind == CMD_CONNECT || command->kind == CMD_DISCONNECT ||
            command->kind == CMD_FORGET || command->kind == CMD_AP_START)
        {
            sleep_reconnect = false;
            memset(&sleep_network, 0, sizeof(sleep_network));
        }
        pending = *command;
        pending.epoch = operation_epoch;
        connection_error = RT_EOK;
        published.busy = true; published.error = RT_EOK; ++published.revision;
        published.operation = operation_for(command->kind);
        published.scanning = command->kind == CMD_SCAN;
    }
    give();
    if (result == RT_EOK) wake(WAKE_COMMAND);
    return result;
}

rt_err_t wlan_service_enable(bool on)
{
    if (!worker) return -RT_ENOSYS;
    take();
    enabled = on; policy_dirty = true; manual_disconnect = false;
    update_power_permission();
    auto_reset_requested = true;
    if (on) config_retry_requested = true;
    sleep_capture = sleep_reconnect = false;
    sleep_powersave = -1;
    sleep_powersave_error = RT_EOK;
    memset(&sleep_network, 0, sizeof(sleep_network));
    if (!on) { ++operation_epoch; memset(&pending, 0, sizeof(pending)); }
    published.enabled = on; published.busy = true; ++published.revision;
    give();
    ui_app_status_changed();
    if (!on) cancel_radio(WLAN_CANCEL_ALL);
    wake(WAKE_COMMAND);
    return RT_EOK;
}

rt_err_t wlan_service_scan(void) { command_t c = {.kind = CMD_SCAN}; return submit(&c); }
rt_err_t wlan_service_disconnect(void) { command_t c = {.kind = CMD_DISCONNECT}; return submit(&c); }

#ifdef RT_USING_FINSH
rt_err_t rt_wlan_cmd_connect(const char *ssid, const char *key)
{
    rt_err_t error = wlan_service_connect_ssid(ssid, key);
    if (error == RT_EOK) rt_kprintf("wifi join queued\n");
    return error;
}

rt_err_t rt_wlan_cmd_disconnect(void)
{
    rt_err_t error = wlan_service_disconnect();
    if (error == RT_EOK) rt_kprintf("wifi disc queued\n");
    return error;
}
#endif
rt_err_t wlan_service_set_autoconnect(bool on) { command_t c = {.kind = CMD_AUTO, .automatic = on}; return submit(&c); }

rt_err_t wlan_service_set_band(wlan_band_t band)
{
    if (band != WLAN_BAND_2_4_GHZ && band != WLAN_BAND_DUAL && band != WLAN_BAND_5_GHZ)
        return -RT_EINVAL;
    command_t command = {.kind = CMD_BAND, .band = band};
    return submit(&command);
}

static bool valid_id(const wlan_network_id_t *id)
{
    return id && id->ssid_length && id->ssid_length <= WLAN_SERVICE_SSID_BYTES &&
        id->ssid[id->ssid_length] == 0 && memchr(id->ssid, 0, id->ssid_length) == NULL;
}

rt_err_t wlan_service_connect(const wlan_network_id_t *id, const char *password)
{
    if (!valid_id(id) || (password && strlen(password) > WLAN_SERVICE_PASSWORD_BYTES)) return -RT_EINVAL;
    command_t command = {.kind = CMD_CONNECT, .id = *id, .reuse_password = password == NULL};
    if (password) strcpy(command.password, password);
    rt_err_t result = submit(&command);
    memset(&command, 0, sizeof(command));
    return result;
}

rt_err_t wlan_service_connect_ssid(const char *ssid, const char *password)
{
    if (!ssid || !*ssid || strlen(ssid) > WLAN_SERVICE_SSID_BYTES ||
        (password && strlen(password) > WLAN_SERVICE_PASSWORD_BYTES)) return -RT_EINVAL;
    command_t command = {.kind = CMD_CONNECT, .by_ssid = true};
    command.id.ssid_length = (uint8_t)strlen(ssid);
    strcpy(command.id.ssid, ssid);
    command.id.security = (uint32_t)SECURITY_UNKNOWN;
    if (password) strcpy(command.password, password);
    rt_err_t result = submit(&command);
    memset(&command, 0, sizeof(command));
    return result;
}

rt_err_t wlan_service_scan_network(const wlan_network_id_t *id)
{
    if (!valid_id(id)) return -RT_EINVAL;
    command_t command = {.kind = CMD_SCAN, .id = *id, .directed = true};
    return submit(&command);
}

static rt_err_t cancel_operation(command_kind_t kind)
{
    if (!worker) return -RT_ENOSYS;
    take();
    bool active = active_operation == operation_for(kind);
    if (kind == CMD_CONNECT && (active || pending.kind == kind))
    {
        manual_disconnect = true;
        policy_dirty = true;
    }
    if (pending.kind == kind)
    {
        memset(&pending, 0, sizeof(pending));
        published.busy = working || policy_dirty || reset_pending;
        published.operation = active_operation;
        published.scanning = active_operation == WLAN_OP_SCAN;
        ++published.revision;
    }
    if (active) ++operation_epoch;
    give();
    cancel_radio(kind == CMD_SCAN ? WLAN_CANCEL_SCAN : WLAN_CANCEL_JOIN);
    wake(WAKE_COMMAND);
    return RT_EOK;
}

rt_err_t wlan_service_cancel_scan(void) { return cancel_operation(CMD_SCAN); }
rt_err_t wlan_service_cancel_connect(void) { return cancel_operation(CMD_CONNECT); }

rt_err_t wlan_service_capabilities(wlan_capabilities_t *out)
{
    if (!out) return -RT_EINVAL;
    memset(out, 0, sizeof(*out));
    if (!worker) return -RT_ENOSYS;
    take(); *out = capabilities; out->available = published.available; give();
    return RT_EOK;
}

rt_err_t wlan_service_set_powersave(int level)
{
    if (level < WLAN_STA_POWERSAVE_MIN_LEVEL || level > WLAN_STA_POWERSAVE_MAX_LEVEL) return -RT_EINVAL;
    command_t command = {.kind = CMD_POWERSAVE, .level = level}; return submit(&command);
}

rt_err_t wlan_service_get_powersave(int *level)
{
    if (!level) return -RT_EINVAL;
    if (!worker) return -RT_ENOSYS;
    take(); rt_err_t result = published.powersave_error;
    if (result == RT_EOK) *level = published.powersave_level;
    give(); return result;
}

rt_err_t wlan_service_set_ipv4(const wlan_ipv4_config_t *config)
{
    if (!config) return -RT_EINVAL;
    command_t command = {.kind = CMD_IPV4, .ipv4 = *config}; return submit(&command);
}

rt_err_t wlan_service_get_ipv4(wlan_ipv4_state_t *state)
{
    if (!state) return -RT_EINVAL;
    return wlan_ipv4_get(state);
}

static rt_err_t start_hotspot(const char *ssid, const char *password, unsigned channel,
                              bool hidden, uint32_t security, bool concurrent)
{
    /* The unmodified RT AP entry reserves the last SSID/key byte. */
    if (!ssid || !*ssid || strlen(ssid) >= RT_WLAN_SSID_MAX_LENGTH || channel < 1 || channel > 165 ||
        (password && strlen(password) >= RT_WLAN_PASSWORD_MAX_LENGTH)) return -RT_EINVAL;
    if (security == SECURITY_OPEN)
    {
        if (password && *password) return -RT_EINVAL;
    }
    else if (!password || strlen(password) < WLAN_SAE_PASSWORD_MIN_LENGTH) return -RT_EINVAL;
    uint32_t required;
    switch (security)
    {
    case SECURITY_OPEN: required = WLAN_AUTH_OPEN; break;
    case SECURITY_WPA2_AES_PSK: required = WLAN_AUTH_WPA2_PSK; break;
    case WLAN_SECURITY_WPA3_SAE: required = WLAN_AUTH_WPA3_SAE; break;
    case WLAN_SECURITY_WPA2_WPA3_PSK: required = WLAN_AUTH_WPA2_WPA3; break;
    default: return -RT_ENOSYS;
    }
    wlan_capabilities_t caps;
    rt_err_t capability_result = wlan_service_capabilities(&caps);
    if (capability_result != RT_EOK) return capability_result;
    if (!(caps.ap_security & required)) return -RT_ENOSYS;
    command_t command = {.kind = CMD_AP_START, .channel = channel, .hidden = hidden, .concurrent = concurrent};
    command.id.ssid_length = (uint8_t)strlen(ssid);
    command.id.security = security;
    strcpy(command.id.ssid, ssid);
    if (password) strcpy(command.password, password);
    rt_err_t result = submit(&command);
    memset(&command, 0, sizeof(command));
    return result;
}

rt_err_t wlan_service_hotspot_start_security(const char *ssid, const char *password,
                                            unsigned channel, bool hidden, uint32_t security)
{ return start_hotspot(ssid, password, channel, hidden, security, false); }
rt_err_t wlan_service_hotspot_start_ex(const char *ssid, const char *password, unsigned channel, bool hidden)
{ return start_hotspot(ssid, password, channel, hidden, password && *password ? SECURITY_WPA2_AES_PSK : SECURITY_OPEN, false); }
rt_err_t wlan_service_hotspot_start(const char *ssid, const char *password, unsigned channel)
{ return wlan_service_hotspot_start_ex(ssid, password, channel, false); }
rt_err_t wlan_service_concurrent_start(const char *ssid, const char *password,
                                      unsigned channel, bool hidden, uint32_t security)
{ return start_hotspot(ssid, password, channel, hidden, security, true); }
rt_err_t wlan_service_hotspot_stop(void)
{ command_t command = {.kind = CMD_AP_STOP}; return submit(&command); }
rt_err_t wlan_service_concurrent_stop(void) { return wlan_service_hotspot_stop(); }
rt_err_t wlan_service_hotspot_deauth(const uint8_t mac[6])
{
    if (!mac || (mac[0] & 1u)) return -RT_EINVAL;
    command_t command = {.kind = CMD_AP_DEAUTH}; memcpy(command.id.bssid, mac, 6); return submit(&command);
}
rt_err_t wlan_service_hotspot_clients(uint8_t (*macs)[6], unsigned capacity, unsigned *count)
{
    if (!count || (capacity && !macs)) return -RT_EINVAL;
    *count = 0;
    if (!worker) return -RT_ENOSYS;
    take();
    unsigned copied = capacity < hotspot_client_count ? capacity : hotspot_client_count;
    if (copied) memcpy(macs, hotspot_clients, copied * 6);
    *count = copied;
    rt_err_t result = hotspot_clients_truncated || copied < hotspot_client_count ? -RT_EFULL : RT_EOK;
    give(); return result;
}

rt_err_t wlan_service_forget(const wlan_network_id_t *id)
{
    if (!valid_id(id)) return -RT_EINVAL;
    command_t command = {.kind = CMD_FORGET, .id = *id}; return submit(&command);
}

rt_err_t wlan_service_radio_status(wlan_radio_status_t *out)
{
    if (!out) return -RT_EINVAL;
    *out = (wlan_radio_status_t){0};
    if (!worker) return -RT_ENOSYS;
    take();
    out->enabled = enabled;
    out->connected = enabled && published.ready && !published.suspended;
    if (out->connected && published.rssi_error == RT_EOK) out->rssi = published.current.rssi;
    give();
    return RT_EOK;
}

rt_err_t wlan_service_snapshot(wlan_snapshot_t *out)
{
    if (!out) return -RT_EINVAL;
    memset(out, 0, sizeof(*out));
    if (!worker) return -RT_ENOSYS;
    take();
    *out = published;
    out->networks = out->count ?
        epd_app_alloc(out->count * sizeof(*out->networks), EPD_APP_PSRAM) : NULL;
    if (out->count && !out->networks) { give(); memset(out, 0, sizeof(*out)); return -RT_ENOMEM; }
    if (out->count) memcpy(out->networks, published.networks, out->count * sizeof(*out->networks));
    give(); return RT_EOK;
}

rt_err_t wlan_service_reset_begin(void)
{
    if (!worker) return -RT_ENOSYS;
    take();
    if (reset_pending || resetting || reset_result == -RT_EBUSY) { give(); return -RT_EBUSY; }
    memset(&pending, 0, sizeof(pending));
    ++operation_epoch;
    reset_pending = reset_paused = true;
    reset_result = -RT_EBUSY; published.busy = true; ++published.revision;
    give(); cancel_radio(WLAN_CANCEL_ALL); wake(WAKE_COMMAND); return RT_EOK;
}

rt_err_t wlan_service_reset_result(void)
{ if (!worker) return -RT_ENOSYS; take(); rt_err_t r = reset_result; give(); return r; }

rt_err_t wlan_service_reset_hold(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    maintenance_hold = true;
    rt_hw_interrupt_enable(level);
    cancel_radio(WLAN_CANCEL_ALL);
    wake(WAKE_CHANGE);
    return RT_EOK;
}

void wlan_service_resume_for_reset(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    maintenance_hold = false;
    rt_hw_interrupt_enable(level);
    wake(WAKE_CHANGE);
}

rt_err_t wlan_service_suspend(void)
{
    if (!worker) return wlan_radio_power_off_confirmed() ? RT_EOK : -RT_ENOSYS;
    take();
    /* Reject before changing the epoch or cancelling driver work: an accepted
     * AP start must finish, and an active AP belongs to its explicit owner. */
    if (hotspot.active || pending.kind == CMD_AP_START ||
        active_operation == WLAN_OP_HOTSPOT_START)
    { give(); return -RT_EBUSY; }
    if (!sleep_hold)
    {
        sleep_hold = true; sleep_result = -RT_EBUSY; policy_dirty = true; ++operation_epoch;
        update_power_permission();
        sleep_capture = true;
        sleep_powersave_error = RT_EOK;
        memset(&pending, 0, sizeof(pending));
        published.busy = true; ++published.revision;
    }
    give(); cancel_radio(WLAN_CANCEL_ALL); wake(WAKE_COMMAND); return RT_EOK;
}
rt_err_t wlan_service_suspend_result(void)
{
    if (!worker) return wlan_radio_power_off_confirmed() ? RT_EOK : -RT_ENOSYS;
    take(); rt_err_t result = sleep_result; give(); return result;
}
void wlan_service_resume(void)
{
    if (!worker) return;
    take();
    bool resume = sleep_hold;
    if (resume) { sleep_hold = false; policy_dirty = true; update_power_permission(); }
    give();
    if (resume) wake(WAKE_COMMAND);
}

#else
rt_err_t wlan_service_start(bool on) { (void)on; return -RT_ENOSYS; }
rt_err_t wlan_service_enable(bool on) { return on ? -RT_ENOSYS : RT_EOK; }
rt_err_t wlan_service_scan(void) { return -RT_ENOSYS; }
rt_err_t wlan_service_scan_network(const wlan_network_id_t *id) { (void)id; return -RT_ENOSYS; }
rt_err_t wlan_service_cancel_scan(void) { return -RT_ENOSYS; }
rt_err_t wlan_service_cancel_connect(void) { return -RT_ENOSYS; }
rt_err_t wlan_service_capabilities(wlan_capabilities_t *out)
{ if (!out) return -RT_EINVAL; memset(out, 0, sizeof(*out)); return RT_EOK; }
rt_err_t wlan_service_connect(const wlan_network_id_t *id, const char *password)
{ (void)id; (void)password; return -RT_ENOSYS; }
rt_err_t wlan_service_connect_ssid(const char *ssid, const char *password)
{ (void)ssid; (void)password; return -RT_ENOSYS; }
rt_err_t wlan_service_disconnect(void) { return -RT_ENOSYS; }
rt_err_t wlan_service_forget(const wlan_network_id_t *id) { (void)id; return -RT_ENOSYS; }
rt_err_t wlan_service_set_autoconnect(bool on) { (void)on; return -RT_ENOSYS; }
rt_err_t wlan_service_set_band(wlan_band_t band) { (void)band; return -RT_ENOSYS; }
rt_err_t wlan_service_radio_status(wlan_radio_status_t *out)
{ if (!out) return -RT_EINVAL; *out = (wlan_radio_status_t){0}; return RT_EOK; }
rt_err_t wlan_service_snapshot(wlan_snapshot_t *out)
{ if (!out) return -RT_EINVAL; memset(out, 0, sizeof(*out)); return RT_EOK; }
rt_err_t wlan_service_suspend(void) { return RT_EOK; }
rt_err_t wlan_service_suspend_result(void) { return RT_EOK; }
void wlan_service_resume(void) {}
rt_err_t wlan_service_reset_hold(void) { return RT_EOK; }
void wlan_service_resume_for_reset(void) {}
rt_err_t wlan_service_reset_begin(void) { return RT_EOK; }
rt_err_t wlan_service_reset_result(void) { return RT_EOK; }
bool wlan_service_reset_write_allowed(void) { return false; }
rt_err_t wlan_service_set_powersave(int level) { (void)level; return -RT_ENOSYS; }
rt_err_t wlan_service_get_powersave(int *level) { return level ? -RT_ENOSYS : -RT_EINVAL; }
rt_err_t wlan_service_set_ipv4(const wlan_ipv4_config_t *config) { return config ? -RT_ENOSYS : -RT_EINVAL; }
rt_err_t wlan_service_get_ipv4(wlan_ipv4_state_t *state)
{ if (!state) return -RT_EINVAL; memset(state, 0, sizeof(*state)); return -RT_ENOSYS; }
rt_err_t wlan_service_hotspot_start(const char *ssid, const char *password, unsigned channel)
{ (void)ssid; (void)password; (void)channel; return -RT_ENOSYS; }
rt_err_t wlan_service_hotspot_start_ex(const char *ssid, const char *password, unsigned channel, bool hidden)
{ (void)hidden; return wlan_service_hotspot_start(ssid, password, channel); }
rt_err_t wlan_service_hotspot_start_security(const char *ssid, const char *password, unsigned channel, bool hidden, uint32_t security)
{ (void)security; return wlan_service_hotspot_start_ex(ssid, password, channel, hidden); }
rt_err_t wlan_service_concurrent_start(const char *ssid, const char *password, unsigned channel, bool hidden, uint32_t security)
{ return wlan_service_hotspot_start_security(ssid, password, channel, hidden, security); }
rt_err_t wlan_service_hotspot_stop(void) { return -RT_ENOSYS; }
rt_err_t wlan_service_concurrent_stop(void) { return -RT_ENOSYS; }
rt_err_t wlan_service_hotspot_deauth(const uint8_t mac[6]) { (void)mac; return -RT_ENOSYS; }
rt_err_t wlan_service_hotspot_clients(uint8_t (*macs)[6], unsigned capacity, unsigned *count)
{ (void)macs; (void)capacity; if (!count) return -RT_EINVAL; *count = 0; return -RT_ENOSYS; }
#endif

rt_err_t wlan_service_radio_state(bool *on, bool *connected)
{
    if (!on || !connected) return -RT_EINVAL;
    wlan_radio_status_t status;
    rt_err_t result = wlan_service_radio_status(&status);
    *on = status.enabled;
    *connected = status.connected;
    return result;
}

void wlan_service_snapshot_free(wlan_snapshot_t *out)
{
    if (!out) return;
    epd_app_free(out->networks); memset(out, 0, sizeof(*out));
}
