#include "ui_settings_internal.h"
#include "ui_internal.h"
#include "platform/epd_keyboard.h"
#include "settings_wireless.h"
#include "ui_shell.h"
#include "ui_separator.h"
#include "bt_pan.h"
#include "bt_audio.h"
#include "wlan_service.h"
#include "storage.h"
#include "icons/ui_icons.h"

enum {
    W_SWITCH = -9200, W_SCAN, W_CANCEL_SCAN, W_CONNECT, W_DISCONNECT,
    W_FORGET, W_UNPAIR, W_FORGET_CONFIRM, W_UNPAIR_CONFIRM,
    W_PASSWORD, W_PASSWORD_SHOW, W_PASSWORD_DONE, W_AUTOMATIC, W_SAVED_NETWORKS
};

typedef struct {
    lv_obj_t *root, *title, *body, *editor, *input, *keyboard, *show, *password_label, *divider;
    ui_settings_list_t *list;
    wlan_snapshot_t wifi;
    btpan_snapshot_t bluetooth;
    bt_audio_snapshot_t audio;
    wlan_network_id_t selected_network;
    uint8_t selected_device[6];
    char *selected_title;
    char password[WLAN_SERVICE_PASSWORD_BYTES + 1];
    bool detail, saved_networks, editing, password_changed, password_visible, rows_valid;
    bool entry_scan_pending, entry_scan_attempted, layout_pending;
    int *keys, focus_key, confirmation;
    size_t key_count;
    uint32_t poll, locale, scale, confirmation_serial, entry_scan_tick;
    int32_t bottom;
    rt_err_t read_error;
} wireless_page_t;

bool settings_wireless_handles(ui_page_id_t page)
{
    return page == UI_PAGE_WIFI_SETTINGS || page == UI_PAGE_BLUETOOTH_SETTINGS;
}

static wireless_page_t *wireless_page(void)
{
    return current && settings_wireless_handles(current->id) ? current->feature : NULL;
}

static bool is_wifi(void) { return current->id == UI_PAGE_WIFI_SETTINGS; }

static const char *wireless_error(rt_err_t error)
{
    if (error == -RT_ENOMEM) return epd_get_str(platform_no_memory);
    if (error == -RT_ENOSYS) return epd_get_str(wireless_unavailable);
    if (error == -RT_ETIMEOUT) return epd_get_str(wireless_timeout);
    if (error == -RT_EBUSY) return epd_get_str(wireless_busy);
    if (error == -RT_EIO) return epd_get_str(wireless_storage_failed);
    return epd_get_str(wireless_failed);
}

static void selected(int key, void *context)
{
    if (!current || current != context || !current->foreground || current->popup ||
        storage_changing() || pending_page != NAV_IDLE) return;
    pending_page = key;
}

static void button_event(lv_event_t *event)
{
    selected((int)(intptr_t)lv_event_get_user_data(event), current);
}

static void keyboard_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) selected(W_PASSWORD_DONE, current);
}

static void password_divider_draw(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(event), &area);
    ui_separator_draw(lv_event_get_layer(event), area.x1, area.x2 + 1,
        area.y1 + UI_SEPARATOR_THICKNESS / 2);
}

static void format_address(const uint8_t address[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", address[5], address[4],
        address[3], address[2], address[1], address[0]);
}

static int network_key(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) hash = (hash ^ data[i]) * 16777619u;
    return -20000 - (int)(hash & 0x3fffffffu);
}

static bool update_keys(wireless_page_t *view, unsigned count)
{
    int *keys = count ? lv_malloc((size_t)count * sizeof(*keys)) : NULL;
    if (count && !keys) return false;
    for (unsigned i = 0; i < count; ++i)
    {
        if (is_wifi())
        {
            const wlan_network_id_t *id = &view->wifi.networks[i].id;
            uint8_t identity[WLAN_SERVICE_SSID_BYTES + 6];
            memcpy(identity, id->bssid, 6);
            memcpy(identity + 6, id->ssid, id->ssid_length);
            keys[i] = network_key(identity, 6 + id->ssid_length);
        }
        else keys[i] = network_key(view->bluetooth.devices[i].address, 6);
        for (unsigned j = 0; j < i; ++j)
            if (keys[j] == keys[i]) { --keys[i]; j = (unsigned)-1; }
    }
    lv_free(view->keys);
    view->keys = keys;
    view->key_count = count;
    return true;
}

static void wireless_layout(wireless_page_t *view)
{
    int32_t width = lv_obj_get_width(current->screen);
    int32_t top = LV_MAX(SETTINGS_CONTENT_Y,
        SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32));
    view->bottom = ui_shell_content_bottom();
    lv_obj_set_size(view->root, width, view->bottom);
    lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_width(view->title, width - SETTINGS_TITLE_X - MARGIN);
    lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
    lv_obj_update_layout(view->title);
    top = LV_MAX(top, SETTINGS_TITLE_Y + lv_obj_get_height(view->title) + epd_ui_px(32));
    int32_t panel_height = 0;
    if (view->editor)
    {
        int32_t left = ui_settings_list_content_margin(width), right = left;
        int32_t inner = width - left - right;
        int32_t input_y = LV_MAX(epd_ui_px(64), ui_font_body()->line_height + epd_ui_px(24));
        int32_t input_height = LV_MAX(epd_ui_touch(60), ui_font_settings_body()->line_height + epd_ui_px(16));
        panel_height = input_y + input_height + epd_ui_px(3);
        lv_obj_set_pos(view->editor, 0, top);
        lv_obj_set_size(view->editor, width, view->editing ? LV_MAX(1, view->bottom - top) : panel_height);
        lv_label_set_text(view->password_label, epd_get_str(wireless_password));
        lv_obj_set_pos(view->password_label, left, 0);
        lv_obj_set_width(view->password_label, inner);
        lv_obj_set_style_text_font(view->password_label, ui_font_body(), 0);
        lv_obj_set_pos(view->input, left, input_y);
        lv_obj_set_size(view->input, inner - epd_ui_touch(60) - epd_ui_px(16), input_height);
        lv_obj_set_style_text_font(view->input, ui_font_settings_body(), 0);
        lv_obj_set_style_pad_top(view->input, (input_height - ui_font_settings_body()->line_height) / 2, 0);
        lv_obj_set_pos(view->show, width - right - epd_ui_touch(60), input_y);
        lv_obj_set_size(view->show, epd_ui_touch(60), input_height);
        lv_obj_t *icon = lv_obj_get_child(view->show, 0);
        lv_image_set_src(icon, view->password_visible ? &ui_icon_password_visible : &ui_icon_password_hidden);
        lv_obj_set_size(icon, epd_ui_px(60), epd_ui_px(60));
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_center(icon);
        lv_obj_set_size(view->divider, inner, UI_SEPARATOR_THICKNESS);
        lv_obj_set_pos(view->divider, left, panel_height - epd_ui_px(3) - UI_SEPARATOR_THICKNESS / 2);
        if (view->keyboard)
        {
            epd_keyboard_refresh(view->keyboard);
            int32_t available = LV_MAX(1, view->bottom - top - panel_height - epd_ui_px(32));
            int32_t height = LV_MIN(epd_keyboard_preferred_height(view->keyboard), available);
            lv_obj_set_size(view->keyboard, width, height);
            lv_obj_align(view->keyboard, LV_ALIGN_BOTTOM_MID, 0, -epd_ui_px(16));
        }
    }
    top += panel_height;
    lv_obj_set_pos(view->body, 0, top);
    lv_obj_set_size(view->body, width, LV_MAX(1, view->bottom - top));
    lv_obj_update_layout(view->body);
    view->layout_pending = !ui_settings_list_refresh_fonts(view->list, ui_font_settings_body(),
        view->detail ? ui_font_settings_value() : ui_font_body());
}

static const wlan_network_t *selected_wifi(wireless_page_t *view)
{
    for (unsigned i = 0; i < view->wifi.count; ++i)
        if (wlan_network_id_equal(&view->wifi.networks[i].id, &view->selected_network)) return &view->wifi.networks[i];
    return NULL;
}

static const btpan_device_t *selected_bt(wireless_page_t *view)
{
    for (unsigned i = 0; i < view->bluetooth.count; ++i)
        if (!memcmp(view->bluetooth.devices[i].address, view->selected_device, 6)) return &view->bluetooth.devices[i];
    return NULL;
}

static const char *selected_bt_name(wireless_page_t *view)
{
    const btpan_device_t *item = selected_bt(view);
    return item && item->name && item->name[0] ? item->name : view->selected_title;
}

static const char *wifi_state(const wlan_network_t *item)
{
    if (item->ready) return epd_get_str(wireless_connected);
    if (item->connected) return epd_get_str(wireless_address_pending);
    return "";
}

static void sync_password_panel(wireless_page_t *view);

static void network_row_style(ui_settings_list_row_t *row, bool detail_action)
{
    row->icon = is_wifi() ? &ui_icon_wifi_entry : &ui_icon_bt_entry;
    row->icon_size = epd_ui_px(60);
    row->no_chevron = true;
    row->no_separator = true;
    row->black_value = true;
    row->joined_value = true;
    if (detail_action)
    {
        row->trailing_icon = &ui_icon_wireless_settings;
        row->control_size = epd_ui_px(60);
        row->min_height = epd_ui_px(110);
    }
    else row->bottom_gap = epd_ui_px(15);
}

static const char *bluetooth_state(const wireless_page_t *view, const btpan_device_t *item)
{
    if (item->connecting) return epd_get_str(wireless_connecting);
    if (item->earphone && item->connected && (!view->audio.connected ||
        memcmp(view->audio.address, item->address, sizeof(item->address))))
        return epd_get_str(wireless_audio_pending);
    return item->connected ? epd_get_str(wireless_connected) : epd_get_str(wireless_disconnected);
}

static void render_rows(wireless_page_t *view)
{
    view->rows_valid = false;
    unsigned count = is_wifi() ? view->wifi.count : view->bluetooth.count;
    ui_settings_list_row_t *rows = lv_malloc_zeroed(((size_t)count + 16) * sizeof(*rows));
    char (*addresses)[18] = lv_malloc(((size_t)count + 1) * 18);
    if (!rows || !addresses || !update_keys(view, count))
    {
        lv_free(rows); lv_free(addresses);
        view->read_error = -RT_ENOMEM;
        lv_obj_add_flag(view->body, LV_OBJ_FLAG_HIDDEN);
        ui_settings_list_set_group(view->list, NULL);
        popup_open_i18n(LV_EXT_STR_ID(platform_no_memory), NULL);
        return;
    }
    size_t n = 0;
#define ROW(key_, kind_, title_, value_, enabled_) do { \
    rows[n] = (ui_settings_list_row_t){.key=(key_), .kind=(kind_), .title=(title_), .value=(value_), .enabled=(enabled_)}; ++n; } while (0)
#define INFO(title_, value_) ROW(-9500-(int)n, UI_SETTINGS_ROW_INFO, title_, value_, false)
#define W_SECTION(title_) ROW(-9500-(int)n, UI_SETTINGS_ROW_SECTION, title_, NULL, false)
#define ACTION(key_, title_, enabled_) do { ROW(key_, UI_SETTINGS_ROW_LINK, title_, NULL, enabled_); rows[n-1].no_chevron=true; } while (0)
    bool enabled = is_wifi() ? view->wifi.enabled : view->bluetooth.enabled;
    bool busy = is_wifi() ? view->wifi.busy : view->bluetooth.busy;
    rt_err_t error = view->read_error ? view->read_error : is_wifi() ? view->wifi.error : view->bluetooth.error;
    if (!view->detail && is_wifi() && view->saved_networks)
    {
        for (unsigned i = 0; i < count; ++i)
            if (view->wifi.networks[i].saved)
            {
                ROW(view->keys[i], UI_SETTINGS_ROW_LINK, view->wifi.networks[i].id.ssid, NULL, true);
                network_row_style(&rows[n-1], false);
            }
        if (!n) INFO(epd_get_str(wireless_empty), NULL);
    }
    else if (!view->detail)
    {
        ROW(W_SWITCH, UI_SETTINGS_ROW_SWITCH, is_wifi() ? "WLAN" : epd_get_str(wireless_bluetooth), NULL,
            is_wifi() ? view->wifi.available : !busy && view->bluetooth.supported);
        rows[n-1].checked = enabled;
        rows[n-1].icon = is_wifi() ? &ui_icon_wifi_entry : &ui_icon_bt_entry;
        rows[n-1].icon_size = rows[n-1].control_size = epd_ui_px(60);
        if (is_wifi() ? view->wifi.scanning : view->bluetooth.scanning) ACTION(W_CANCEL_SCAN, epd_get_str(wireless_scan_stop), true);
        else ACTION(W_SCAN, epd_get_str(wireless_scan), enabled && !busy && (is_wifi() || view->bluetooth.ready));
        if (!(is_wifi() ? view->wifi.available : view->bluetooth.supported)) INFO(wireless_error(-RT_ENOSYS), NULL);
        else if (!is_wifi() && !view->bluetooth.ready) INFO(epd_get_str(wireless_initializing), NULL);
        if (is_wifi())
        {
            ROW(W_AUTOMATIC, UI_SETTINGS_ROW_SWITCH, epd_get_str(wireless_automatic), NULL, enabled && !busy);
            rows[n-1].checked = view->wifi.automatic;
            rows[n-1].control_size = epd_ui_px(60);
            W_SECTION(epd_get_str(wireless_current));
            if (!view->wifi.connected) INFO(epd_get_str(wireless_empty), NULL);
            else for (unsigned i = 0; i < count; ++i)
                if (view->wifi.networks[i].connected)
                {
                    ROW(view->keys[i], UI_SETTINGS_ROW_LINK, view->wifi.networks[i].id.ssid, wifi_state(&view->wifi.networks[i]), true);
                    network_row_style(&rows[n-1], true);
                    rows[n-1].no_separator = false;
                }
            W_SECTION(epd_get_str(wireless_available_networks));
            size_t start = n;
            for (unsigned i = 0; i < count; ++i)
                if (view->wifi.networks[i].available && !view->wifi.networks[i].connected)
                {
                    ROW(view->keys[i], UI_SETTINGS_ROW_LINK, view->wifi.networks[i].id.ssid, NULL, enabled);
                    network_row_style(&rows[n-1], false);
                }
            if (start == n) INFO(epd_get_str(wireless_empty), NULL);
            else rows[n-1].no_separator = false;
            ROW(W_SAVED_NETWORKS, UI_SETTINGS_ROW_LINK, epd_get_str(wireless_saved_networks), NULL, true);
        }
        else
        {
            for (int group = 0; group < 2; ++group)
            {
                W_SECTION(group == 0 ? epd_get_str(wireless_paired) : epd_get_str(wireless_available_devices));
                size_t start = n;
                for (unsigned i = 0; i < count; ++i)
                {
                    const btpan_device_t *item = &view->bluetooth.devices[i];
                    if ((group == 0) != item->paired || (!item->paired && !item->available && !item->connected)) continue;
                    format_address(item->address, addresses[i]);
                    ROW(view->keys[i], UI_SETTINGS_ROW_LINK, item->name && item->name[0] ? item->name : addresses[i],
                        item->paired || item->connecting || item->connected ? bluetooth_state(view, item) : NULL, enabled);
                    network_row_style(&rows[n-1], item->paired);
                    rows[n-1].muted_title = item->connecting;
                }
                if (n == start) INFO(epd_get_str(wireless_empty), NULL);
                else if (group == 0) rows[n-1].no_separator = false;
            }
        }
    }
    else if (is_wifi())
    {
        const wlan_network_t *item = selected_wifi(view);
        ACTION(item && item->connected ? W_DISCONNECT : W_CONNECT,
            item && item->connected ? epd_get_str(wireless_disconnect) : epd_get_str(wireless_connect), enabled && !busy && !view->read_error && item);
        if (item && item->saved) ACTION(W_FORGET, epd_get_str(wireless_forget), enabled && !busy && !view->read_error);
    }
    else
    {
        const btpan_device_t *item = selected_bt(view);
        INFO(epd_get_str(wireless_name), selected_bt_name(view));
        format_address(view->selected_device, addresses[count]);
        INFO(epd_get_str(wireless_address), addresses[count]);
        if (item && item->earphone) INFO(epd_get_str(wireless_service), epd_get_str(wireless_audio));
        INFO(epd_get_str(wireless_state), item ? bluetooth_state(view, item) : epd_get_str(wireless_disconnected));
        ACTION(item && item->connected ? W_DISCONNECT : W_CONNECT,
            item && item->connected ? epd_get_str(wireless_disconnect) : epd_get_str(wireless_connect), enabled && !busy && !view->read_error && item);
        if (item && item->paired) ACTION(W_UNPAIR, epd_get_str(wireless_unpair), enabled && !busy && !view->read_error);
    }
    if (busy) INFO(epd_get_str(wireless_working), NULL);
    if (error) INFO(wireless_error(error), NULL);
    if (is_wifi() ? view->wifi.truncated : view->bluetooth.truncated) INFO(epd_get_str(wireless_limited), NULL);
    view->rows_valid = ui_settings_list_set_rows(view->list, rows, n);
    if (!view->rows_valid)
    {
        view->read_error = -RT_ENOMEM;
        lv_obj_add_flag(view->body, LV_OBJ_FLAG_HIDDEN);
        ui_settings_list_set_group(view->list, NULL);
        popup_open_i18n(LV_EXT_STR_ID(platform_no_memory), NULL);
    }
    else lv_obj_remove_flag(view->body, LV_OBJ_FLAG_HIDDEN);
    lv_free(rows); lv_free(addresses);
#undef ROW
#undef INFO
#undef W_SECTION
#undef ACTION
}

static bool read_snapshot(wireless_page_t *view)
{
    rt_err_t previous_error = view->read_error;
    bool changed;
    if (is_wifi())
    {
        wlan_snapshot_t next = {0};
        view->read_error = wlan_service_snapshot(&next);
        changed = previous_error != view->read_error || (!view->read_error &&
            (next.revision != view->wifi.revision || next.enabled != view->wifi.enabled ||
             next.available != view->wifi.available || next.busy != view->wifi.busy));
        if (!view->read_error) { wlan_service_snapshot_free(&view->wifi); view->wifi = next; }
        else wlan_service_snapshot_free(&next);
    }
    else
    {
        btpan_snapshot_t next = {0};
        view->read_error = btpan_snapshot(&next);
        changed = previous_error != view->read_error || (!view->read_error &&
            (next.revision != view->bluetooth.revision || next.enabled != view->bluetooth.enabled ||
             next.ready != view->bluetooth.ready || next.busy != view->bluetooth.busy));
        if (!view->read_error) { btpan_snapshot_free(&view->bluetooth); view->bluetooth = next; }
        else btpan_snapshot_free(&next);
        bt_audio_snapshot_t previous_audio = view->audio;
        bt_audio_get_snapshot(&view->audio);
        changed = changed || previous_audio.connected != view->audio.connected ||
            memcmp(previous_audio.address, view->audio.address, sizeof(view->audio.address));
    }
    return changed;
}

static bool entry_scan(wireless_page_t *view, rt_err_t *error)
{
    *error = RT_EOK;
    if (!view->entry_scan_pending || view->read_error || !current->foreground ||
        current->popup || storage_changing() || pending_page != NAV_IDLE ||
        view->detail || view->saved_networks || view->editing) return false;
    if (!is_wifi() && view->bluetooth.scanning)
    {
        view->entry_scan_pending = false;
        return false;
    }
    if (is_wifi())
    {
        if (!view->wifi.available || !view->wifi.enabled || view->wifi.busy) return false;
    }
    else if (!view->bluetooth.supported || !view->bluetooth.enabled ||
             !view->bluetooth.ready || view->bluetooth.busy) return false;
    if (view->entry_scan_attempted && lv_tick_elaps(view->entry_scan_tick) < 500) return false;
    view->entry_scan_attempted = true;
    view->entry_scan_tick = lv_tick_get();
    rt_err_t result = is_wifi() ? wlan_service_scan() : btpan_scan();
    /* A service can become busy between the snapshot and the queued request. */
    if (result == -RT_EBUSY) return false;
    view->entry_scan_pending = false;
    if (result == RT_EOK) return read_snapshot(view);
    *error = result;
    return false;
}

void settings_wireless_resume(void)
{
    wireless_page_t *view = wireless_page();
    if (!view || !view->list || current->popup) return;
    read_snapshot(view);
    rt_err_t scan_error;
    entry_scan(view, &scan_error);
    const char *title = is_wifi() ? view->detail ? view->selected_title :
        view->saved_networks ? epd_get_str(wireless_saved_networks) : "WLAN" : epd_get_str(wireless_bluetooth);
    if (strcmp(lv_label_get_text(view->title), title)) lv_label_set_text(view->title, title);
    sync_password_panel(view);
    wireless_layout(view);
    if (!view->editing) render_rows(view);
    settings_wireless_restore_focus();
    if (!view->layout_pending)
    {
        view->locale = epd_locale_revision();
        view->scale = epd_ui_scale_revision();
    }
    view->poll = lv_tick_get();
    /* Capture popup focus only after layout and row replacement have finished. */
    if (scan_error != RT_EOK) popup_open(wireless_error(scan_error), NULL);
}

void settings_wireless_create(lv_obj_t *screen)
{
    wireless_page_t *view = lv_malloc_zeroed(sizeof(*view));
    current->feature = view;
    if (!view) return;
    view->entry_scan_pending = true;
    view->root = lv_obj_create(screen);
    epd_obj_init(view->root);
    view->title = label_create(view->root, "", SETTINGS_TITLE_X, SETTINGS_TITLE_Y,
        WIDTH - MARGIN - SETTINGS_TITLE_X, ui_font_settings_title());
    view->body = lv_obj_create(view->root);
    epd_obj_init(view->body);
    view->list = ui_settings_list_create(view->body, ui_font_settings_body(), ui_font_body(), selected, current);
    settings_wireless_resume();
}

static void close_editor(wireless_page_t *view)
{
    if (!view->keyboard) return;
    lv_obj_delete(view->keyboard);
    view->keyboard = NULL;
    view->editing = false;
    view->password_visible = false;
    lv_textarea_set_password_mode(view->input, true);
    lv_textarea_set_text(view->input, view->password);
    lv_obj_remove_flag(view->body, LV_OBJ_FLAG_HIDDEN);
}

static void sync_password_panel(wireless_page_t *view)
{
    const wlan_network_t *item = is_wifi() && view->detail ? selected_wifi(view) : NULL;
    if (!item || !item->secure)
    {
        if (view->editor)
        {
            lv_textarea_set_text(view->input, "");
            lv_obj_delete(view->editor);
            view->editor = view->input = view->keyboard = view->show = view->password_label = view->divider = NULL;
            view->editing = view->password_visible = false;
        }
        return;
    }
    if (!view->editor)
    {
        view->editor = lv_obj_create(view->root);
        epd_obj_init(view->editor);
        view->password_label = lv_label_create(view->editor);
        view->input = lv_textarea_create(view->editor);
        epd_obj_init(view->input);
        lv_obj_set_style_text_color(view->input, lv_color_hex(0x666666), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_border_width(view->input, 1, (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_border_side(view->input, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
        lv_obj_set_style_border_color(view->input, lv_color_black(), LV_PART_CURSOR);
        lv_textarea_set_one_line(view->input, true);
        lv_textarea_set_max_length(view->input, WLAN_SERVICE_PASSWORD_BYTES);
        lv_textarea_set_password_mode(view->input, true);
        lv_textarea_set_password_show_time(view->input, 0);
        lv_textarea_set_text(view->input, view->password);
        lv_obj_set_style_anim_duration(view->input, 0, LV_PART_CURSOR);
        lv_obj_add_event_cb(view->input, button_event, LV_EVENT_CLICKED, (void *)(intptr_t)W_PASSWORD);
        view->show = lv_button_create(view->editor);
        epd_obj_init(view->show);
        lv_obj_add_event_cb(view->show, button_event, LV_EVENT_CLICKED, (void *)(intptr_t)W_PASSWORD_SHOW);
        lv_image_create(view->show);
        view->divider = lv_obj_create(view->editor);
        epd_obj_init(view->divider);
        lv_obj_remove_flag(view->divider, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(view->divider, password_divider_draw, LV_EVENT_DRAW_MAIN, NULL);
    }
    const char *placeholder = item->saved ?
        epd_get_str(wireless_password_saved) : epd_get_str(wireless_password_enter);
    const char *previous = lv_textarea_get_placeholder_text(view->input);
    if (!previous || strcmp(previous, placeholder)) lv_textarea_set_placeholder_text(view->input, placeholder);
    if (!view->editing && strcmp(lv_textarea_get_text(view->input), view->password))
        lv_textarea_set_text(view->input, view->password);
    bool enabled = view->wifi.enabled && !view->wifi.busy && !view->read_error;
    if (enabled)
    {
        lv_obj_remove_state(view->input, LV_STATE_DISABLED);
        lv_obj_remove_state(view->show, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_add_state(view->input, LV_STATE_DISABLED);
        lv_obj_add_state(view->show, LV_STATE_DISABLED);
    }
}

static void open_editor(wireless_page_t *view)
{
    if (!view->editor || view->editing || lv_obj_has_state(view->input, LV_STATE_DISABLED)) return;
    ui_settings_list_set_group(view->list, NULL);
    lv_obj_add_flag(view->body, LV_OBJ_FLAG_HIDDEN);
    view->editing = true;
    const epd_keyboard_config_t config = {.mode = EPD_KEYBOARD_PASSWORD};
    view->keyboard = epd_keyboard_create(view->editor, view->input, &config);
    lv_obj_add_event_cb(view->keyboard, keyboard_event, LV_EVENT_READY, NULL);
    wireless_layout(view);
    settings_wireless_restore_focus();
}

bool settings_wireless_action(int *request)
{
    wireless_page_t *view = wireless_page();
    if (!view || !request) return false;
    int key = *request;
    if (key == W_FORGET_CONFIRM || key == W_UNPAIR_CONFIRM)
    {
        if (!view->detail || !current->popup || key != view->confirmation ||
            current->popup_serial != view->confirmation_serial) return true;
        view->confirmation = 0;
        popup_close();
    }
    else if (current->popup) return false;
    else view->confirmation = 0;
    if (key == NAV_BACK)
    {
        if (view->editing) { close_editor(view); settings_wireless_resume(); return true; }
        if (!view->detail)
        {
            if (!view->saved_networks) return false;
            view->saved_networks = false;
            settings_wireless_resume();
            ui_settings_list_focus_key(view->list, W_SAVED_NETWORKS);
            return true;
        }
        view->detail = false;
        memset(view->password, 0, sizeof(view->password));
        view->password_changed = false;
        settings_wireless_resume();
        ui_settings_list_focus_key(view->list, view->focus_key);
        return true;
    }
    if (key == W_PASSWORD_SHOW && view->editor)
    {
        view->password_visible = !view->password_visible;
        lv_textarea_set_password_mode(view->input, !view->password_visible);
        wireless_layout(view);
        return true;
    }
    if (view->editing)
    {
        if (key == W_PASSWORD_DONE)
        {
            const char *password = lv_textarea_get_text(view->input);
            if (strlen(password) > WLAN_SERVICE_PASSWORD_BYTES)
                popup_open_i18n(LV_EXT_STR_ID(wireless_password_invalid), NULL);
            else
            {
                strcpy(view->password, password);
                view->password_changed = true;
                close_editor(view);
                settings_wireless_resume();
            }
        }
        return true;
    }
    if (!view->rows_valid) return true;
    if (!view->detail)
        for (size_t i = 0; i < view->key_count; ++i)
            if (view->keys[i] == key)
            {
                char address[18];
                const char *title;
                if (is_wifi()) { view->selected_network = view->wifi.networks[i].id; title = view->selected_network.ssid; }
                else
                {
                    const btpan_device_t *item = &view->bluetooth.devices[i];
                    memcpy(view->selected_device, item->address, 6);
                    format_address(item->address, address);
                    title = item->name && item->name[0] ? item->name : address;
                }
                char *copy = lv_malloc(strlen(title) + 1);
                if (!copy) { popup_open_i18n(LV_EXT_STR_ID(platform_no_memory), NULL); return true; }
                strcpy(copy, title);
                if (!is_wifi() || view->wifi.scanning)
                {
                    view->entry_scan_pending = false;
                    rt_err_t result = is_wifi() ? wlan_service_cancel_scan() : btpan_cancel_scan();
                    if (result != RT_EOK)
                    {
                        lv_free(copy);
                        settings_wireless_resume();
                        popup_open(wireless_error(result), NULL);
                        return true;
                    }
                }
                lv_free(view->selected_title);
                view->selected_title = copy;
                view->focus_key = key;
                view->detail = true;
                settings_wireless_resume();
                return true;
            }
    rt_err_t result = RT_EOK;
    switch (key)
    {
    case W_SAVED_NETWORKS:
        if (!is_wifi() || view->detail) return true;
        view->saved_networks = true;
        break;
    case W_SWITCH:
    {
        bool next = is_wifi() ? !view->wifi.enabled : !view->bluetooth.enabled;
        if (is_wifi())
        {
            if (!view->wifi.available) result = -RT_EBUSY;
            else if (!ui_settings_set(UI_SETTING_WIFI, next)) result = -RT_EIO;
        }
        else if (!ui_settings_set(UI_SETTING_BLUETOOTH, next)) result = -RT_ERROR;
        if (result == RT_EOK)
        {
            view->entry_scan_pending = next;
            view->entry_scan_attempted = false;
        }
        break;
    }
    case W_SCAN:
        view->entry_scan_pending = false;
        result = is_wifi() ? wlan_service_scan() : btpan_scan();
        break;
    case W_CANCEL_SCAN:
        view->entry_scan_pending = false;
        result = is_wifi() ? wlan_service_cancel_scan() : btpan_cancel_scan();
        break;
    case W_PASSWORD: if (view->detail && is_wifi()) open_editor(view); return true;
    case W_AUTOMATIC: result = wlan_service_set_autoconnect(!view->wifi.automatic); break;
    case W_CONNECT:
        if (!view->detail) return true;
        if (is_wifi())
        {
            const wlan_network_t *item = selected_wifi(view);
            if (!item || view->read_error || !view->wifi.enabled || view->wifi.busy) return true;
            if (item->secure && !item->saved && !view->password_changed) { open_editor(view); return true; }
            result = wlan_service_connect(&item->id, view->password_changed ? view->password :
                item->saved ? NULL : "");
            if (result == RT_EOK) { memset(view->password, 0, sizeof(view->password)); view->password_changed = false; }
        }
        else result = btpan_connect(view->selected_device);
        break;
    case W_DISCONNECT:
        result = is_wifi() ? wlan_service_disconnect() : btpan_disconnect(view->selected_device); break;
    case W_FORGET:
        if (!view->detail || !is_wifi()) return true;
        view->confirmation = W_FORGET_CONFIRM;
        popup_confirm_i18n(LV_EXT_STR_ID(wireless_forget_confirm), view->selected_title, W_FORGET_CONFIRM);
        view->confirmation_serial = current->popup_serial;
        return true;
    case W_UNPAIR:
        if (!view->detail || is_wifi()) return true;
        view->confirmation = W_UNPAIR_CONFIRM;
        popup_confirm_i18n(LV_EXT_STR_ID(wireless_unpair_confirm), selected_bt_name(view), W_UNPAIR_CONFIRM);
        view->confirmation_serial = current->popup_serial;
        return true;
    case W_FORGET_CONFIRM: result = wlan_service_forget(&view->selected_network); break;
    case W_UNPAIR_CONFIRM: result = btpan_unpair(view->selected_device); break;
    default: return false;
    }
    settings_wireless_resume();
    if (result != RT_EOK) popup_open(wireless_error(result), NULL);
    return true;
}

void settings_wireless_process(void)
{
    wireless_page_t *view = wireless_page();
    if (!view || !current->foreground || current->popup || pending_page != NAV_IDLE || !lv_refreshing_done()) return;
    if (view->layout_pending && lv_tick_elaps(view->poll) < 500) return;
    if (view->layout_pending || view->locale != epd_locale_revision() || view->scale != epd_ui_scale_revision() ||
        view->bottom != ui_shell_content_bottom()) { settings_wireless_resume(); return; }
    if (view->editing || lv_tick_elaps(view->poll) < 500) return;
    view->poll = lv_tick_get();
    bool changed = read_snapshot(view);
    rt_err_t scan_error;
    changed = entry_scan(view, &scan_error) || changed;
    if (changed || !view->rows_valid)
    {
        bool had_editor = view->editor != NULL;
        sync_password_panel(view);
        if (had_editor != (view->editor != NULL)) wireless_layout(view);
        render_rows(view);
        settings_wireless_restore_focus();
    }
    if (scan_error != RT_EOK) popup_open(wireless_error(scan_error), NULL);
}

void settings_wireless_restore_focus(void)
{
    wireless_page_t *view = wireless_page();
    if (!view || !view->list || current->popup) return;
    if (view->editor)
    {
        if (lv_obj_get_group(view->input) != current->group) lv_group_add_obj(current->group, view->input);
        if (lv_obj_get_group(view->show) != current->group) lv_group_add_obj(current->group, view->show);
        if (view->keyboard && lv_obj_get_group(view->keyboard) != current->group)
            lv_group_add_obj(current->group, view->keyboard);
    }
    if (!view->editing) ui_settings_list_set_group(view->list, view->rows_valid ? current->group : NULL);
}

ui_settings_list_t *settings_wireless_list(void)
{
    wireless_page_t *view = wireless_page();
    return view && !view->editing ? view->list : NULL;
}

void settings_wireless_stop(void)
{
    wireless_page_t *view = wireless_page();
    if (!view) return;
    if (!is_wifi() && view->bluetooth.scanning) btpan_cancel_scan();
    if (view->list) ui_settings_list_destroy(view->list);
    if (view->editor) lv_textarea_set_text(view->input, "");
    lv_obj_delete(view->root);
    wlan_service_snapshot_free(&view->wifi);
    btpan_snapshot_free(&view->bluetooth);
    lv_free(view->keys);
    lv_free(view->selected_title);
    memset(view->password, 0, sizeof(view->password));
    lv_free(view);
    current->feature = NULL;
}

void settings_wireless_delete(void) { settings_wireless_stop(); }
