#include "ui_settings_internal.h"
#include "ui_settings.h"
#include "ui_scale.h"
#include "platform/epd_i18n.h"
#include "ui_settings_store.h"
#include "bt_pan.h"
#include "wlan_service.h"
#include "reading_font.h"
#include "epd_waveform.h"   /* 屏驱波形接口（SDK: customer/peripherals/display/epd_e0470a03）*/
#include "system_reset_guard.h"
#include "storage.h"
#include "boards/board_service.h"
#include "platform/epd_system_internal.h"
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const unsigned timeout_minutes[] = {1, 3, 5, 10, 15, 0};
static const unsigned sleep_minutes[] = {1, 5, 15, 30, 60, 0};
static const unsigned refresh_counts[] = {1, 5, 8, 10, 15, 20};
static const char *const font_sizes[] = {"28 px", "30 px", "32 px", "34 px", "36 px", "40 px", "44 px", "48 px"};
static const char *const line_spacings[] = {"1.0x", "1.2x", "1.4x", "1.6x", "1.8x"};
static const char *const margins[] = {"4 px", "8 px", "12 px", "16 px", "20 px"};

#define SETTING_CHOICE(count) {NULL, NULL, count, false}
#define SETTING_SWITCH {NULL, NULL, 2, true}

static const ui_setting_t settings[UI_SETTING_COUNT] = {
    [UI_SETTING_TOUCH] = SETTING_SWITCH,
    [UI_SETTING_TIMEOUT] = SETTING_CHOICE(6),
    [UI_SETTING_FULL_REFRESH] = SETTING_CHOICE(6),
    [UI_SETTING_BLUETOOTH] = SETTING_SWITCH,
    [UI_SETTING_WIFI] = SETTING_SWITCH,
    [UI_SETTING_LOW_POWER] = SETTING_SWITCH,
    [UI_SETTING_FONT] = SETTING_CHOICE(0),
    [UI_SETTING_FONT_SIZE] = SETTING_CHOICE(8),
    [UI_SETTING_FONT_WEIGHT] = SETTING_CHOICE(3),
    [UI_SETTING_LINE_SPACING] = SETTING_CHOICE(5),
    [UI_SETTING_MARGIN] = SETTING_CHOICE(5),
    [UI_SETTING_ENCODING] = SETTING_CHOICE(3),
    [UI_SETTING_UI_SCALE] = SETTING_CHOICE(5),
    [UI_SETTING_SYSTEM_SLEEP] = SETTING_CHOICE(6),
    [UI_SETTING_NAVIGATION] = SETTING_CHOICE(2)
};

#define SETTINGS_DEFAULTS { \
    [UI_SETTING_TOUCH] = 1, \
    [UI_SETTING_TIMEOUT] = 2, \
    [UI_SETTING_FULL_REFRESH] = 3, \
    [UI_SETTING_BLUETOOTH] = 1, \
    [UI_SETTING_WIFI] = 1, \
    [UI_SETTING_LOW_POWER] = 1, \
    [UI_SETTING_FONT_SIZE] = 2, \
    [UI_SETTING_LINE_SPACING] = 1, \
    [UI_SETTING_MARGIN] = 1, \
    [UI_SETTING_UI_SCALE] = 2, \
    [UI_SETTING_SYSTEM_SLEEP] = 2 \
}

static const uint8_t defaults[UI_SETTING_COUNT] = SETTINGS_DEFAULTS;
/* Accessed only by the application/UI thread. */
static uint8_t selected[UI_SETTING_COUNT] = SETTINGS_DEFAULTS;

unsigned ui_settings_default(ui_setting_id_t id)
{
    return (unsigned)id < UI_SETTING_COUNT ? defaults[id] : 0;
}

const ui_setting_t *ui_settings_item(ui_setting_id_t id)
{
    return (unsigned)id < UI_SETTING_COUNT ? &settings[id] : NULL;
}

const char *ui_settings_label(ui_setting_id_t id)
{
    switch (id)
    {
    case UI_SETTING_TOUCH: return epd_get_str(platform_setting_touch);
    case UI_SETTING_TIMEOUT: return epd_get_str(settings_config_screen_sleep);
    case UI_SETTING_FULL_REFRESH: return epd_get_str(platform_setting_full_refresh);
    case UI_SETTING_BLUETOOTH: return epd_get_str(platform_setting_bluetooth);
    case UI_SETTING_WIFI: return epd_get_str(platform_setting_wifi);
    case UI_SETTING_LOW_POWER: return epd_get_str(platform_setting_low_power);
    case UI_SETTING_FONT: return epd_get_str(platform_setting_font);
    case UI_SETTING_FONT_SIZE: return epd_get_str(platform_setting_font_size);
    case UI_SETTING_FONT_WEIGHT: return epd_get_str(platform_setting_font_weight);
    case UI_SETTING_LINE_SPACING: return epd_get_str(platform_setting_line_spacing);
    case UI_SETTING_MARGIN: return epd_get_str(platform_setting_margin);
    case UI_SETTING_ENCODING: return epd_get_str(platform_setting_encoding);
    case UI_SETTING_UI_SCALE: return epd_get_str(settings_config_scale);
    case UI_SETTING_SYSTEM_SLEEP: return epd_get_str(settings_config_system_sleep);
    case UI_SETTING_NAVIGATION: return epd_get_str(settings_config_navigation);
    default: return "";
    }
}

static const char *refresh_text(unsigned count)
{
    static char value[48];
    if (count == 1) return epd_get_str(platform_refresh_every);
    EPD_FMT_platform_refresh_count(value, sizeof(value), count);
    return value;
}

const char *ui_settings_option(ui_setting_id_t id, unsigned index)
{
    const ui_setting_t *setting = ui_settings_item(id);
    if (!setting || index >= setting->option_count) return NULL;
    if (setting->is_switch)
        return index ? epd_get_str(settings_config_on) : epd_get_str(settings_config_off);
    static char value[48];
    switch (id)
    {
    case UI_SETTING_NAVIGATION:
        return index ? epd_get_str(settings_config_navigation_gestures) : epd_get_str(settings_config_navigation_buttons);
    case UI_SETTING_TIMEOUT:
        if (!timeout_minutes[index]) return epd_get_str(settings_config_never);
        EPD_FMT_platform_minutes(value, sizeof(value), timeout_minutes[index]);
        return value;
    case UI_SETTING_SYSTEM_SLEEP:
        if (!sleep_minutes[index]) return epd_get_str(settings_config_never);
        EPD_FMT_platform_minutes(value, sizeof(value), sleep_minutes[index]);
        return value;
    case UI_SETTING_FULL_REFRESH: return refresh_text(refresh_counts[index]);
    case UI_SETTING_FONT_SIZE: return font_sizes[index];
    case UI_SETTING_FONT_WEIGHT:
        return index == 0 ? epd_get_str(platform_weight_normal) :
               index == 1 ? epd_get_str(platform_weight_bold) : epd_get_str(platform_weight_light);
    case UI_SETTING_LINE_SPACING: return line_spacings[index];
    case UI_SETTING_MARGIN: return margins[index];
    case UI_SETTING_ENCODING:
        return index == 0 ? epd_get_str(platform_auto) : index == 1 ? "UTF-8" : "GBK";
    case UI_SETTING_UI_SCALE:
        switch (index)
        {
        case 0: return epd_get_str(settings_config_scale_smaller);
        case 1: return epd_get_str(settings_config_scale_small);
        case 2: return epd_get_str(settings_config_scale_standard);
        case 3: return epd_get_str(settings_config_scale_large);
        default: return epd_get_str(settings_config_scale_larger);
        }
    default: return NULL;
    }
}

const char *ui_settings_value(ui_setting_id_t id)
{
    const ui_setting_t *setting = ui_settings_item(id);
    if (!setting || setting->is_switch) return NULL;
    if (id == UI_SETTING_FONT)
    {
        static reading_font_info_t font;
        return reading_font_get(reading_font_selected(), &font) ? font.name : "MiSans";
    }
    if (id == UI_SETTING_FULL_REFRESH) return refresh_text(ui_settings_refresh_count());
    return ui_settings_option(id, selected[id]);
}

bool ui_settings_enabled(ui_setting_id_t id)
{
    if (id == UI_SETTING_BLUETOOTH) return btpan_get_state() != BTPAN_STATE_OFF;
    const ui_setting_t *setting = ui_settings_item(id);
    return setting && setting->is_switch && selected[id] != 0;
}

ui_settings_load_result_t ui_settings_reload(void)
{
    uint8_t candidate[UI_SETTING_COUNT];
    ui_settings_load_result_t result = ui_settings_store_reload(candidate);
    if (result == UI_SETTINGS_LOAD_IO_ERROR) return result;
    if (!ui_scale_prepare(candidate[UI_SETTING_UI_SCALE])) return UI_SETTINGS_LOAD_APPLY_FAILED;
    bool previous_bt = ui_settings_enabled(UI_SETTING_BLUETOOTH);
    bool enabled = candidate[UI_SETTING_BLUETOOTH] != 0;
    if (enabled != previous_bt && btpan_enable(enabled) != RT_EOK)
        return UI_SETTINGS_LOAD_APPLY_FAILED;
    rt_err_t wifi_result = wlan_service_enable(candidate[UI_SETTING_WIFI] != 0);
    /* Startup loads preferences before WLAN starts; a board may also lack WLAN. */
    if (wifi_result != RT_EOK && wifi_result != -RT_ENOSYS)
    {
        if (enabled != previous_bt) btpan_enable(previous_bt);
        return UI_SETTINGS_LOAD_APPLY_FAILED;
    }
    memcpy(selected, candidate, sizeof(selected));
    ui_scale_apply(selected[UI_SETTING_UI_SCALE]);
    epd_wave_set_part_times(refresh_counts[selected[UI_SETTING_FULL_REFRESH]]);
    return result;
}

void ui_settings_init(void)
{
    selected[UI_SETTING_BLUETOOTH] = (uint8_t)ui_settings_enabled(UI_SETTING_BLUETOOTH);
    ui_settings_load_result_t result = ui_settings_reload();
    if (result == UI_SETTINGS_LOAD_IO_ERROR || result == UI_SETTINGS_LOAD_APPLY_FAILED)
    {
        epd_wave_set_part_times(refresh_counts[selected[UI_SETTING_FULL_REFRESH]]);
    }
    (void)ui_settings_store_start();
}

bool ui_settings_set(ui_setting_id_t id, unsigned next)
{
    const ui_setting_t *setting = ui_settings_item(id);
    if (!setting || next >= setting->option_count) return false;
    if (system_reset_blocks_apps() || storage_changing() || board_power_pending()) return false;
    if (id == UI_SETTING_UI_SCALE && !ui_scale_prepare(next)) return false;
    uint8_t candidate[UI_SETTING_COUNT], previous[UI_SETTING_COUNT];
    memcpy(candidate, selected, sizeof(candidate));
    memcpy(previous, selected, sizeof(previous));
    candidate[id] = (uint8_t)next;
    bool previous_bt = ui_settings_enabled(UI_SETTING_BLUETOOTH);
    unsigned previous_refresh = ui_settings_refresh_count();
    if (id == UI_SETTING_BLUETOOTH && btpan_enable(next != 0) != RT_EOK) return false;
    if (id == UI_SETTING_WIFI && wlan_service_enable(next != 0) != RT_EOK) return false;
    if (id == UI_SETTING_FULL_REFRESH) epd_wave_set_part_times(refresh_counts[next]);
    memcpy(selected, candidate, sizeof(selected));
    if (!ui_settings_store_save(candidate))
    {
        memcpy(selected, previous, sizeof(selected));
        if (id == UI_SETTING_BLUETOOTH) btpan_enable(previous_bt);
        if (id == UI_SETTING_WIFI) wlan_service_enable(selected[id] != 0);
        if (id == UI_SETTING_FULL_REFRESH) epd_wave_set_part_times(previous_refresh);
        return false;
    }
    if (id == UI_SETTING_UI_SCALE) ui_scale_apply(next);
    return true;
}

ui_settings_save_status_t ui_settings_save_status(void)
{
    return ui_settings_store_status(NULL);
}

bool ui_settings_flush(void)
{
    /* Shutdown/storage handoff waits for I/O to finish. A completed failure
     * remains visible through status/notifications without trapping shutdown
     * in its save timeout on every attempt. */
    return ui_settings_save_status() != UI_SETTINGS_SAVE_PENDING;
}

void ui_settings_process(void)
{
    static uint32_t observed;
    static char notice_id[EPD_NOTIFICATION_ID_SIZE];
    uint32_t revision;
    ui_settings_save_status_t status = ui_settings_store_status(&revision);
    if (status == UI_SETTINGS_SAVE_PENDING || revision == observed) return;
    if (status == UI_SETTINGS_SAVE_FAILED)
    {
        epd_notification_t notice = {0};
        snprintf(notice.app_id, sizeof(notice.app_id), "settings");
        snprintf(notice.id, sizeof(notice.id), "settings-save");
        snprintf(notice.title, sizeof(notice.title), "%s", epd_get_str(platform_settings_save_failed));
        snprintf(notice.text, sizeof(notice.text), "%s", epd_get_str(platform_settings_save_failed_detail));
        if (!epd_notification_publish_resident(&notice)) return;
        memcpy(notice_id, notice.id, sizeof(notice_id));
    }
    else if (notice_id[0])
    {
        (void)epd_notification_dismiss("settings", notice_id);
        notice_id[0] = 0;
    }
    observed = revision;
}

bool ui_settings_cycle(ui_setting_id_t id)
{
    const ui_setting_t *setting = ui_settings_item(id);
    return setting && setting->option_count &&
        ui_settings_set(id, (ui_settings_index(id) + 1) % setting->option_count);
}

bool ui_settings_reset_network(void)
{
    static const ui_setting_id_t fields[] = {UI_SETTING_BLUETOOTH, UI_SETTING_WIFI};
    uint8_t candidate[UI_SETTING_COUNT];
    memcpy(candidate, selected, sizeof(candidate));
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
        candidate[fields[i]] = defaults[fields[i]];
    /* Reset completion means durable defaults, not merely an accepted job.
     * Serialize with older writes before reading/applying the committed result. */
    if (!ui_settings_store_save_sync(candidate)) return false;
    return ui_settings_reload() == UI_SETTINGS_LOAD_OK;
}

unsigned ui_settings_index(ui_setting_id_t id)
{
    if (id == UI_SETTING_FONT) return reading_font_selected();
    if (id == UI_SETTING_BLUETOOTH) return ui_settings_enabled(id);
    if (id == UI_SETTING_FULL_REFRESH)
    {
        unsigned count = ui_settings_refresh_count();
        for (unsigned i = 0; i < sizeof(refresh_counts) / sizeof(refresh_counts[0]); ++i)
            if (refresh_counts[i] == count) return i;
    }
    return (unsigned)id < UI_SETTING_COUNT ? selected[id] : 0;
}

unsigned ui_settings_refresh_count(void)
{
    return (unsigned)epd_wave_get_part_times();
}

unsigned ui_settings_lock_timeout_ms(void)
{
    return timeout_minutes[selected[UI_SETTING_TIMEOUT]] * 60000u;
}

unsigned ui_settings_sleep_timeout_ms(void)
{
    return sleep_minutes[selected[UI_SETTING_SYSTEM_SLEEP]] * 60000u;
}
