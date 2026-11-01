#include "app_service_internal.h"
#include "platform/epd_sound_internal.h"
#include "ui_app.h"
#include "system_reset.h"
#include <rthw.h>
#include "launcher.h"
#include "platform/epd_input.h"
#include "platform/epd_power.h"
#include "boards/battery/battery.h"
#include "bt_pan.h"
#include "bt_audio.h"
#include "wlan_service.h"
#include "app_service.h"
#include "ai_settings.h"
#include "speech/speech_internal.h"
#include "speech/speech_settings.h"
#include "boards/board_service.h"
#include <string.h>
#include "system_time.h"
#include "system_info.h"
#include "platform/epd_locale.h"
#include "platform/epd_refresh.h"
#include "frontlight.h"
#include "platform/epd_sound.h"
#include "ui_settings_internal.h"
#include "gui_app_int.h"
#include "ui_navigation.h"
#include "ui_shell.h"
#include "app_memory_pressure.h"

static launcher_status_t published;
static rt_tick_t last_status_tick;
static bool service_deferred;

void ui_app_set_wifi(bool enabled, bool connected)
{
    rt_base_t level = rt_hw_interrupt_disable();
    published.wifi = ui_radio_state(enabled, connected);
    rt_hw_interrupt_enable(level);
}

void ui_app_set_recent_reading(const char *title, int percent)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_strncpy(published.recent_book, title ? title : "", sizeof(published.recent_book) - 1);
    published.recent_book[sizeof(published.recent_book) - 1] = '\0';
    published.reading_percent = ui_battery_percent(percent);
    rt_hw_interrupt_enable(level);
}

static void status_poll(void)
{
    launcher_status_t snapshot;
    rt_base_t level = rt_hw_interrupt_disable();
    snapshot = published;
    btpan_state_t pan_state = btpan_get_state();
    snapshot.bluetooth = ui_radio_state(
        pan_state != BTPAN_STATE_OFF,
        pan_state == BTPAN_STATE_CONNECTED || pan_state == BTPAN_STATE_NETWORK_READY);
    rt_hw_interrupt_enable(level);
    bt_audio_snapshot_t headset;
    bt_audio_get_snapshot(&headset);
    snapshot.headset_connected = headset.connected;
    snapshot.headset_battery_percent = headset.battery_percent;
    bool wifi_enabled, wifi_connected;
    if (wlan_service_radio_state(&wifi_enabled, &wifi_connected) == RT_EOK)
    {
        ui_app_set_wifi(wifi_enabled, wifi_connected);
        snapshot.wifi = ui_radio_state(wifi_enabled, wifi_connected);
    }
    epd_app_summary_read("weather", snapshot.weather, sizeof(snapshot.weather));
    battery_snapshot_t battery;
    battery_get_snapshot(&battery);
    snapshot.battery_percent = battery.percentage_valid ? battery.percentage : -1;
    snapshot.charging = battery.charge_valid && battery.charging;
    int64_t now = system_time_get_utc();
    const char *locale = epd_locale_get();
    system_time_format(now, locale, true, snapshot.time_text, sizeof(snapshot.time_text));
    system_time_format(now, locale, false, snapshot.clock_text, sizeof(snapshot.clock_text));
    launcher_set_status(&snapshot);
}

rt_err_t ui_app_init(void)
{
    epd_sound_init();
    if (!launcher_init()) return -RT_ERROR;
    battery_init();
    status_poll();
    last_status_tick = rt_tick_get();
    return epd_input_init();
}

void ui_app_process(void)
{
    service_deferred = false;
    app_service_ui_begin_iteration();
    speech_process();
    ui_settings_process();
    if (epd_power_sleeping())
    {
        /* The retained wallpaper stays on the panel while the UI owner retires
         * files, services and module pages. No normal timers or navigation run. */
        launcher_storage_process();
        app_service_process();
        return;
    }
    system_info_process();
    if (board_power_pending())
    {
        epd_input_clear();
        launcher_process();
        return;
    }
    if (epd_refresh_completed_once())
    {
        frontlight_init();
        frontlight_process();
    }
    if (!launcher_storage_process())
    {
        /* Storage owners finish asynchronous cancellation on the UI thread
         * before they can acknowledge a removable-volume handoff. */
        app_service_process();
        epd_input_clear();
        return;
    }
    system_reset_process();
    ai_settings_process();
    speech_settings_process();
    epd_sound_process();
    epd_input_read();
    epd_input_process();
    launcher_process();
    ui_navigation_process();
    if (!board_power_pending() && !epd_power_sleeping())
    {
        char app_id[GUI_APP_ID_MAX_LEN] = {0};
        gui_runing_app_t *app = gui_app_get_actived();
        if (app) rt_strncpy(app_id, app->id, sizeof(app_id) - 1);
        app_memory_pressure_process(app_id, !strcmp(app_id, "Main") &&
                                    launcher_current_page() == UI_PAGE_HOME &&
                                    !ui_navigation_busy() && !ui_shell_locked());
        service_deferred = true;
        app_service_ui_process_foreground(app_id);
    }
    rt_tick_t tick = rt_tick_get();
    if ((rt_tick_t)(tick - last_status_tick) >= rt_tick_from_millisecond(1000))
    {
        status_poll();
        last_status_tick = tick;
    }
}

void ui_app_process_deferred(void)
{
    if (!service_deferred) return;
    service_deferred = false;
    app_service_ui_process_deferred();
}
