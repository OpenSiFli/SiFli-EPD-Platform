#include "storage_internal.h"
#include "ui_settings_internal.h"
#include "portal.h"
#include "portal_http.h"
#include "portal_auth.h"
#include "portal_package.h"
#include "app_service.h"
#include "wlan_service.h"
#include "network.h"
#include "ui_settings.h"
#include "storage.h"
#include "system_reset_guard.h"
#include "boards/board_service.h"
#include <wlan_mgnt.h>
#include <lwip/inet.h>
#include <rthw.h>
#include <stdio.h>
#include <string.h>

static portal_snapshot_t published;
static uint32_t last_poll, retry_after;
static bool polled, storage_hold;
static bool sleep_paused, worker_active;
static epd_service_t *worker_service;
static uint32_t hotspot_generation;
static struct
{
    bool prepared, requested, restart;
    char ssid[33], password[17];
} power_restore;
/* The HTTP worker owns the bound interface until its listener has closed. */
static network_source_t bound_source;
static uint32_t bound_address, bound_mask;

static bool accepting_work(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool accepting = !sleep_paused;
    rt_hw_interrupt_enable(level);
    return accepting;
}

static bool begin_work(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool accepted = !sleep_paused && epd_service_sleep_acquire(service);
    if (accepted) worker_active = true;
    rt_hw_interrupt_enable(level);
    return accepted;
}

bool portal_prepare_sleep(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    /* An explicitly requested hotspot owns its radio until stopped. */
    bool ready = false;
    if (!published.hotspot_requested)
    {
        sleep_paused = true;
        ready = !worker_active;
    }
    rt_hw_interrupt_enable(level);
    return ready;
}

void portal_resume(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    sleep_paused = false;
    polled = false;
    retry_after = 0;
    /* Wake while protected; service retirement must not free this pointer. */
    epd_service_wake(worker_service);
    rt_hw_interrupt_enable(level);
}

void portal_snapshot(portal_snapshot_t *out)
{
    if (!out) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = published;
    rt_hw_interrupt_enable(level);
}

static bool hotspot_requested(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool requested = published.hotspot_requested;
    rt_hw_interrupt_enable(level);
    return requested;
}

static void request_hotspot(bool requested)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (published.hotspot_requested != requested)
    {
        published.hotspot_requested = requested;
        ++hotspot_generation;
        ++published.revision;
    }
    rt_hw_interrupt_enable(level);
}

static void stop_failed_request(uint32_t generation)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (hotspot_generation == generation) request_hotspot(false);
    rt_hw_interrupt_enable(level);
}

void portal_power_prepare(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!power_restore.prepared)
    {
        power_restore.prepared = true;
        power_restore.requested = published.hotspot_requested || power_restore.restart;
        /* A second shutdown can precede the previous rollback's AP restart. */
        if (!power_restore.restart)
        {
            memcpy(power_restore.ssid, published.ssid, sizeof(power_restore.ssid));
            memcpy(power_restore.password, published.password, sizeof(power_restore.password));
        }
    }
    rt_hw_interrupt_enable(level);
}

void portal_power_cancel(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool restore = power_restore.prepared;
    if (restore)
    {
        power_restore.prepared = false;
        power_restore.restart = power_restore.requested;
        ++hotspot_generation;
        request_hotspot(power_restore.requested);
        if (!power_restore.requested) memset(&power_restore, 0, sizeof(power_restore));
    }
    rt_hw_interrupt_enable(level);
    /* Resume admission and polling without changing the user's Wi-Fi switch. */
    if (restore) portal_resume();
}

static void publish(portal_phase_t phase, rt_err_t error)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool changed = published.phase != phase || published.error != error;
    published.phase = phase;
    published.error = error;
    if (changed) ++published.revision;
    rt_hw_interrupt_enable(level);
    if (changed && phase == PORTAL_ERROR) rt_kprintf("portal: failed rc=%d\n", error);
}

static void http_ready(void) { publish(PORTAL_ON, RT_EOK); }

static bool interface_ready(network_source_t source, network_interface_snapshot_t *state)
{
    memset(state, 0, sizeof(*state));
    return network_interface_snapshot(source, state) == RT_EOK &&
           state->present && state->up && state->link_up && state->ready &&
           state->address && state->netmask;
}

static void address_text(uint32_t value, char *out, size_t capacity)
{
    struct in_addr address;
    address.s_addr = value;
    if (!inet_ntoa_r(address, out, (int)capacity)) out[0] = 0;
}

static void refresh_lan(void)
{
    network_interface_snapshot_t state;
    bool ready = interface_ready(NETWORK_WIFI, &state);
    char address[48] = "", url[96] = "";
    if (ready)
    {
        address_text(state.address, address, sizeof(address));
        snprintf(url, sizeof(url), "http://%s/", address);
    }
    rt_base_t level = rt_hw_interrupt_disable();
    if (published.lan_available != ready || strcmp(published.lan_address, address))
    {
        published.lan_available = ready;
        strcpy(published.lan_address, address);
        strcpy(published.lan_url, url);
        ++published.revision;
    }
    rt_hw_interrupt_enable(level);
}

static bool http_current(void)
{
    if (hotspot_requested() != (bound_source == NETWORK_WIFI_AP)) return false;
    network_interface_snapshot_t state;
    return interface_ready(bound_source, &state) &&
           state.address == bound_address && state.netmask == bound_mask;
}

static bool credentials(char ssid[33], char password[17])
{
    uint32_t words[2] = {0};
    bool good = portal_auth_random_bytes(words, sizeof(words));
    if (good)
    {
        snprintf(ssid, 33, "EPD-%04lx", (unsigned long)(words[0] & 0xffff));
        snprintf(password, 17, "%08lu", (unsigned long)(words[1] % 100000000U));
    }
    memset(words, 0, sizeof(words));
    return good;
}

static rt_err_t wait_wlan(epd_service_t *service, bool active, bool station, unsigned timeout_ms)
{
    rt_tick_t started = rt_tick_get();
    do
    {
        wlan_snapshot_t state = {0};
        if (wlan_service_snapshot(&state) == RT_EOK)
        {
            bool done = !state.busy && state.hotspot.active == active && (!station || state.connected);
            bool failed = !state.busy && state.error != RT_EOK;
            rt_err_t error = state.error;
            wlan_service_snapshot_free(&state);
            if (done) return RT_EOK;
            if (failed) return error;
        }
        if (active && (epd_service_cancelled(service) || !hotspot_requested())) return -RT_EBUSY;
        rt_thread_mdelay(100);
    } while ((rt_tick_t)(rt_tick_get() - started) < rt_tick_from_millisecond(timeout_ms));
    return -RT_ETIMEOUT;
}

static bool wait_interface(epd_service_t *service, network_source_t source,
                           network_interface_snapshot_t *state)
{
    rt_tick_t started = rt_tick_get();
    do
    {
        if (interface_ready(source, state)) return true;
        if (source != NETWORK_WIFI_AP || !hotspot_requested() ||
            epd_service_cancelled(service) || board_power_pending() || system_reset_busy())
            return false;
        /* WLAN completion can precede the tcpip-thread address snapshot. */
        rt_thread_mdelay(100);
    } while ((rt_tick_t)(rt_tick_get() - started) < rt_tick_from_millisecond(5000));
    return false;
}

static rt_err_t start_hotspot(epd_service_t *service, wlan_network_id_t *previous,
                              bool *accepted, bool *concurrent)
{
    char ssid[33] = "", password[17] = "";
    rt_base_t level = rt_hw_interrupt_disable();
    /* A stopped worker must leave rollback credentials for its successor. */
    if (epd_service_cancelled(service) || !published.hotspot_requested || power_restore.prepared)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    bool reuse = power_restore.restart && power_restore.ssid[0] && power_restore.password[0];
    if (reuse)
    {
        memcpy(ssid, power_restore.ssid, sizeof(ssid));
        memcpy(password, power_restore.password, sizeof(password));
    }
    power_restore.restart = false;
    if (!power_restore.prepared) memset(&power_restore, 0, sizeof(power_restore));
    rt_hw_interrupt_enable(level);
    rt_tick_t started = rt_tick_get();
    int channel = 6;
    for (;;)
    {
        wlan_snapshot_t state = {0};
        bool ready = false;
        if (wlan_service_snapshot(&state) == RT_EOK)
        {
            ready = state.available && state.enabled && !state.busy;
            if (state.connected) { channel = state.current.channel; *previous = state.current.id; }
            wlan_service_snapshot_free(&state);
        }
        if (ready) break;
        if (epd_service_cancelled(service) || !hotspot_requested()) return -RT_EBUSY;
        if ((rt_tick_t)(rt_tick_get() - started) >= rt_tick_from_millisecond(30000)) return -RT_ETIMEOUT;
        rt_thread_mdelay(100);
    }
    if (epd_service_cancelled(service) || !hotspot_requested() ||
        board_power_pending() || system_reset_busy())
    {
        memset(password, 0, sizeof(password));
        return -RT_EBUSY;
    }
    wlan_capabilities_t caps;
    if (wlan_service_capabilities(&caps) != RT_EOK || !caps.hotspot) return -RT_ENOSYS;
    *concurrent = caps.concurrent;
    if (!*concurrent) channel = 6;
    if (channel < 1 || channel > 165) return -RT_EIO;
    if (!reuse && !credentials(ssid, password)) return -RT_EIO;
    level = rt_hw_interrupt_disable();
    strcpy(published.ssid, ssid);
    strcpy(published.password, password);
    published.concurrent = *concurrent;
    ++published.revision;
    rt_hw_interrupt_enable(level);
    rt_err_t error = *concurrent ?
        wlan_service_concurrent_start(ssid, password, (unsigned)channel, false, SECURITY_WPA2_AES_PSK) :
        wlan_service_hotspot_start_security(ssid, password, (unsigned)channel, false, SECURITY_WPA2_AES_PSK);
    memset(password, 0, sizeof(password));
    if (error != RT_EOK) return error;
    *accepted = true;
    return wait_wlan(service, true, false, 30000);
}

static void stop_hotspot(epd_service_t *service, const wlan_network_id_t *previous, bool concurrent)
{
    /* Do not release the radio lifetime while an accepted start/stop is pending. */
    for (;;)
    {
        wlan_snapshot_t state = {0};
        bool idle = false, active = true;
        if (wlan_service_snapshot(&state) == RT_EOK)
        {
            idle = !state.busy;
            active = state.hotspot.active;
            wlan_service_snapshot_free(&state);
        }
        if (idle && !active) break;
        if (idle)
        {
            rt_err_t error = wlan_service_concurrent_stop();
            if (error == RT_EOK) error = wait_wlan(service, false, false, 15000);
            if (error != RT_EOK) publish(PORTAL_ERROR, error);
        }
        rt_thread_mdelay(500);
    }
    if (!concurrent && previous->ssid_length && !board_power_pending() && !system_reset_busy())
    {
        rt_err_t error = wlan_service_connect(previous, NULL);
        if (error == RT_EOK) error = wait_wlan(service, false, true, 30000);
        if (error != RT_EOK) publish(PORTAL_ERROR, error);
    }
    rt_base_t level = rt_hw_interrupt_disable();
    memset(published.password, 0, sizeof(published.password));
    published.ssid[0] = 0;
    published.clients = 0;
    ++published.revision;
    rt_hw_interrupt_enable(level);
}

static void run(epd_service_t *service)
{
    bool owns_ap = false, concurrent = false, auth_ready = false;
    wlan_network_id_t previous = {0};
    rt_err_t error = RT_EOK;
    rt_base_t level = rt_hw_interrupt_disable();
    worker_service = service;
    rt_hw_interrupt_enable(level);
    while (!epd_service_cancelled(service) && !board_power_pending() && !system_reset_busy())
    {
        if (!owns_ap && !begin_work(service))
        {
            /* The HTTP loop has closed every socket and freed its buffers.
             * Stay registered so preparation does not see a stopping service. */
            portal_auth_invalidate_sessions();
            level = rt_hw_interrupt_disable();
            published.address[0] = published.url[0] = 0;
            ++published.revision;
            worker_active = false;
            rt_hw_interrupt_enable(level);
            publish(PORTAL_OFF, RT_EOK);
            epd_service_sleep_release(service);
            epd_service_wait(service, RT_WAITING_FOREVER);
            continue;
        }
        if (!auth_ready)
        {
            /* The account store is read on a worker while sleep is excluded. */
            level = rt_hw_interrupt_disable();
            uint32_t auth_generation = hotspot_generation;
            rt_hw_interrupt_enable(level);
            rt_tick_t auth_started = rt_tick_get();
            while ((error = portal_auth_init()) == -RT_EBUSY)
            {
                if (epd_service_cancelled(service) || board_power_pending() || system_reset_busy())
                { error = RT_EOK; goto done; }
                if ((rt_tick_t)(rt_tick_get() - auth_started) >= rt_tick_from_millisecond(5000))
                    goto done;
                rt_thread_mdelay(100);
            }
            if (error != RT_EOK)
            {
                if (portal_auth_init_failed()) stop_failed_request(auth_generation);
                goto done;
            }
            auth_ready = true;
        }
        level = rt_hw_interrupt_disable();
        bool wanted = published.hotspot_requested;
        uint32_t request_generation = hotspot_generation;
        rt_hw_interrupt_enable(level);
        if (owns_ap && !wanted)
        {
            publish(PORTAL_STOPPING, RT_EOK);
            stop_hotspot(service, &previous, concurrent);
            owns_ap = false;
            memset(&previous, 0, sizeof(previous));
        }
        if (!owns_ap && !accepting_work()) continue;
        if (wanted && !owns_ap)
        {
            publish(PORTAL_STARTING, RT_EOK);
            error = start_hotspot(service, &previous, &owns_ap, &concurrent);
            if (error != RT_EOK)
            {
                if (!hotspot_requested() || epd_service_cancelled(service)) error = RT_EOK;
                stop_failed_request(request_generation);
                break;
            }
        }
        if (epd_service_cancelled(service)) break;
        network_interface_snapshot_t state;
        bound_source = owns_ap ? NETWORK_WIFI_AP : NETWORK_WIFI;
        if (!wait_interface(service, bound_source, &state))
        {
            if (!owns_ap) break;
            error = hotspot_requested() && !epd_service_cancelled(service) ? -RT_ETIMEOUT : RT_EOK;
            stop_failed_request(request_generation);
            break;
        }
        bound_address = state.address;
        bound_mask = state.netmask;
        char address[48], url[96];
        address_text(state.address, address, sizeof(address));
        snprintf(url, sizeof(url), "http://%s/", address);
        level = rt_hw_interrupt_disable();
        published.lan = !owns_ap;
        strcpy(published.address, address);
        strcpy(published.url, url);
        ++published.revision;
        rt_hw_interrupt_enable(level);
        publish(PORTAL_STARTING, RT_EOK);
        portal_http_config_t config = {
            .address = address, .netmask = state.netmask, .service = service,
            .ready = http_ready, .keep_running = http_current,
            .accepting = accepting_work, .keep_awake = owns_ap
        };
        /* AP mode keeps the lifetime hold through radio teardown. LAN holds
         * only initialization and accepted requests, not idle listening. */
        if (!owns_ap) epd_service_sleep_release(service);
        error = portal_http_run(&config);
        if (error != RT_EOK) break;
    }
done:
    if (owns_ap) stop_hotspot(service, &previous, concurrent);
    portal_auth_invalidate_sessions();
    level = rt_hw_interrupt_disable();
    published.address[0] = published.url[0] = 0;
    memset(published.password, 0, sizeof(published.password));
    published.ssid[0] = 0;
    ++published.revision;
    rt_hw_interrupt_enable(level);
    publish(error == RT_EOK ? PORTAL_OFF : PORTAL_ERROR, error);
    epd_service_sleep_release(service);
    level = rt_hw_interrupt_disable();
    worker_active = false;
    worker_service = NULL;
    rt_hw_interrupt_enable(level);
}

static bool ui_process(epd_service_t *service, bool stopping)
{
    portal_package_process();
    portal_settings_process(stopping);
    /* The settings bridge is freed only after the HTTP worker has exited. */
    if (stopping) return true;
    if (board_power_pending() || system_reset_busy())
    {
        request_hotspot(false);
        portal_http_storage_block(true);
        app_service_stop("portal");
        return false;
    }
    if (storage_hold && !storage_changing() && !storage_release_requested())
    {
        storage_hold = false;
        portal_http_storage_block(false);
        portal_auth_storage_block(false);
    }
    if (accepting_work()) epd_service_ui_request(service, 100);
    return false;
}

static const epd_background_t definition = {
    .run = run, .stack_size = 12288, .priority = 23,
    .ui_process = ui_process, .network = true
};

static rt_err_t ensure_service(void)
{
    if (!accepting_work()) return -RT_EBUSY;
    if (portal_auth_init_failed())
    {
        publish(PORTAL_ERROR, -RT_EIO);
        return -RT_EIO;
    }
    if (app_service_running("portal")) return RT_EOK;
    portal_settings_deinit();
    if (!portal_settings_init()) return -RT_ENOMEM;
    storage_hold = storage_changing() || storage_release_requested();
    portal_http_storage_block(storage_hold);
    portal_auth_storage_block(storage_hold);
    publish(PORTAL_STARTING, RT_EOK);
    if (!app_service_start("portal", &definition, NULL))
    {
        portal_settings_deinit();
        publish(PORTAL_ERROR, -RT_ENOMEM);
        return -RT_ENOMEM;
    }
    return RT_EOK;
}

void portal_process(void)
{
    portal_package_process();
    if (!accepting_work() || board_power_pending() || system_reset_blocks_apps()) return;
    uint32_t now = (uint32_t)rt_tick_get();
    if (polled && (uint32_t)(now - last_poll) < rt_tick_from_millisecond(1000)) return;
    polled = true;
    last_poll = now;
    if (!storage_changing() && !storage_release_requested())
    {
        storage_hold = false;
        portal_http_storage_block(false);
        portal_auth_storage_block(false);
        portal_auth_start();
    }
    refresh_lan();
    if (app_service_running("portal"))
    {
        if (published.hotspot_requested)
        {
            wlan_snapshot_t state = {0};
            if (wlan_service_snapshot(&state) == RT_EOK)
            {
                rt_base_t level = rt_hw_interrupt_disable();
                if (published.clients != state.hotspot.clients)
                { published.clients = state.hotspot.clients; ++published.revision; }
                rt_hw_interrupt_enable(level);
                wlan_service_snapshot_free(&state);
            }
        }
        return;
    }
    portal_settings_deinit();
    if (portal_auth_init_failed())
    {
        request_hotspot(false);
        publish(PORTAL_ERROR, -RT_EIO);
        return;
    }
    /* A retiring worker may have cleared the request after power_cancel().
     * Reassert it only after retirement, before the one restored start. */
    rt_base_t level = rt_hw_interrupt_disable();
    if (power_restore.restart && !power_restore.prepared) request_hotspot(true);
    rt_hw_interrupt_enable(level);
    if (!published.lan_available && !hotspot_requested()) return;
    if (retry_after && (int32_t)(now - retry_after) < 0) return;
    retry_after = now + rt_tick_from_millisecond(5000);
    (void)ensure_service();
}

rt_err_t portal_start(void)
{
    if (!accepting_work() || board_power_pending() || system_reset_busy() || storage_changing()) return -RT_EBUSY;
    if (!ui_settings_index(UI_SETTING_WIFI) && !ui_settings_set(UI_SETTING_WIFI, 1))
        return -RT_EIO;
    request_hotspot(true);
    rt_err_t error = ensure_service();
    if (error != RT_EOK) request_hotspot(false);
    return error;
}

void portal_stop(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    memset(&power_restore, 0, sizeof(power_restore));
    ++hotspot_generation;
    request_hotspot(false);
    rt_hw_interrupt_enable(level);
    /* A station listener remains available independently of the hotspot. */
}

bool portal_release_storage(void)
{
    storage_hold = true;
    portal_http_storage_block(true);
    portal_auth_storage_block(true);
    bool shutdown = board_power_pending() || system_reset_busy();
    if (shutdown)
    {
        request_hotspot(false);
        if (app_service_running("portal")) app_service_stop("portal");
    }
    return portal_auth_storage_idle() && portal_http_storage_idle() &&
           (!shutdown || !app_service_running("portal"));
}
