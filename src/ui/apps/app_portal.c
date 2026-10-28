#include "ui_internal.h"
#include "ui_shell.h"
#include "ui_portal_accounts.h"
#include "ui_portal_qr.h"
#include "portal.h"

enum { PORTAL_TOGGLE = -740, PORTAL_ACCOUNTS };
typedef struct
{
    lv_obj_t *body, *qr, *hint, *status, *button, *accounts_button, *web_button;
    lv_obj_t *headings[4], *values[4];
    ui_portal_accounts_t *accounts;
    uint32_t revision, locale, scale;
    int32_t bottom, width, qr_size;
    bool accounts_visible;
    char qr_value[PORTAL_WIFI_QR_CAPACITY];
} portal_view_t;

static int32_t place_label(lv_obj_t *label, const char *text, const lv_font_t *font,
                           int32_t width, int32_t y)
{
    text_update(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_pos(label, 0, y);
    lv_obj_set_size(label, width, LV_SIZE_CONTENT);
    lv_obj_update_layout(label);
    return y + lv_obj_get_height(label);
}

static void render(bool layout)
{
    portal_view_t *view = current->feature;
    if (!view) return;
    bool web_page = current->id == UI_PAGE_PORTAL_WEB;
    bool accounts = ui_portal_accounts_visible(view->accounts);
    if (view->accounts_visible != accounts) {
        view->accounts_visible = accounts; layout = true;
        object_visible(view->body, !accounts);
        object_visible(view->button, !accounts);
        object_visible(view->accounts_button, !accounts);
        object_visible(view->web_button, !accounts);
        if (accounts) {
            lv_group_remove_obj(view->button);
            lv_group_remove_obj(view->accounts_button);
            lv_group_remove_obj(view->web_button);
        }
    }
    if (accounts) { ui_portal_accounts_process(view->accounts, layout); return; }
    if (current->popup || ui_shell_modal_visible()) return;
    lv_obj_t *actions[] = {view->web_button, view->button, view->accounts_button};
    for (unsigned i = 0; i < 3; ++i)
        if (actions[i] && !lv_obj_has_flag(actions[i], LV_OBJ_FLAG_HIDDEN) &&
            lv_obj_get_group(actions[i]) != current->group)
            lv_group_add_obj(current->group, actions[i]);
    portal_snapshot_t state;
    portal_snapshot(&state);
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    int32_t width = lv_obj_get_width(current->screen) - 2 * MARGIN;
    uint32_t locale = epd_locale_revision(), scale = epd_ui_scale_revision();
    if (!layout && view->revision == state.revision && view->bottom == bottom &&
        view->width == width && view->locale == locale && view->scale == scale) return;
    view->bottom = bottom; view->width = width; view->revision = state.revision;
    view->locale = locale; view->scale = scale;
    bool on = state.phase == PORTAL_ON;
    bool ap = !state.lan && state.hotspot_requested;
    bool wifi_qr = on && ap && !web_page;
    const char *url = on ? state.url : "";
    char payload[PORTAL_WIFI_QR_CAPACITY] = {0};
    if (wifi_qr) ui_portal_wifi_qr(payload, sizeof(payload), state.ssid, state.password);
    else snprintf(payload, sizeof(payload), "%s", url);
    const char *mode = on ? (state.lan ? epd_get_str(portal_mode_lan) : epd_get_str(portal_mode_ap)) :
        state.hotspot_requested ? epd_get_str(portal_mode_ap_pending) :
        state.lan_available ? epd_get_str(portal_mode_lan_pending) : epd_get_str(portal_mode_off);
    const char *status = state.phase == PORTAL_STARTING ? epd_get_str(portal_starting) :
        state.phase == PORTAL_STOPPING ? epd_get_str(portal_stopping) :
        state.phase == PORTAL_ERROR ? epd_get_str(portal_failed) :
        !on ? epd_get_str(portal_connect_hint) : "";
    char clients[80];
    if (on && ap && !web_page) {
        if (state.clients) {
            EPD_FMT_portal_clients(clients, sizeof(clients), state.clients);
            status = clients;
        } else status = epd_get_str(portal_no_clients);
    }
    int32_t gap = epd_ui_px(16);
    int32_t top = PAGE_TITLE_Y + ui_font_title()->line_height + epd_ui_px(26);
    int32_t button_h = LV_MAX(epd_ui_touch(64), ui_font_body()->line_height + epd_ui_px(30));
    int32_t button_count = 0;
    if (!web_page) {
        object_visible(view->web_button, ap);
        if (on) lv_obj_remove_state(view->web_button, LV_STATE_DISABLED);
        else lv_obj_add_state(view->web_button, LV_STATE_DISABLED);
        if (ap && lv_obj_get_group(view->web_button) != current->group)
            lv_group_add_obj(current->group, view->web_button);
        else if (!ap) lv_group_remove_obj(view->web_button);
        button_count = ap ? 3 : 2;
        const char *captions[] = {epd_get_str(portal_open_web),
            state.hotspot_requested ? epd_get_str(portal_stop) : epd_get_str(portal_start),
            epd_get_str(portal_accounts)};
        int32_t row = 0;
        for (unsigned i = ap ? 0 : 1; i < 3; ++i, ++row) {
            lv_obj_set_pos(actions[i], MARGIN, bottom - (button_count - row) * (button_h + gap));
            lv_obj_set_size(actions[i], width, button_h);
            lv_obj_t *label = lv_obj_get_child(actions[i], 0);
            text_update(label, captions[i]); lv_obj_set_style_text_font(label, ui_font_body(), 0);
            lv_obj_set_width(label, LV_MAX(1, width - 2 * gap)); lv_obj_center(label);
        }
    }
    lv_obj_set_pos(view->body, MARGIN, top);
    lv_obj_set_size(view->body, width, LV_MAX(1, bottom - button_count * (button_h + gap) - gap - top));
    int32_t y = 0;
    y = place_label(view->headings[0], epd_get_str(portal_mode), ui_font_small(), width, y) + epd_ui_px(6);
    y = place_label(view->values[0], mode, ui_font_body(), width, y) + gap;
    object_visible(view->status, status[0] != 0);
    if (status[0]) y = place_label(view->status, status, ui_font_body(), width, y) + gap;
    int32_t qr_size = LV_MAX(1, LV_MIN(width - epd_ui_px(48), epd_ui_px(260)));
    bool qr_changed = qr_size != view->qr_size;
    if (qr_changed) { lv_qrcode_set_size(view->qr, qr_size); view->qr_size = qr_size; }
    bool qr_ok = on && payload[0];
    if (qr_ok && (qr_changed || strcmp(view->qr_value, payload))) {
        qr_ok = lv_qrcode_update(view->qr, payload, (uint32_t)strlen(payload)) == LV_RESULT_OK;
        if (qr_ok) snprintf(view->qr_value, sizeof(view->qr_value), "%s", payload);
    }
    if (!qr_ok) memset(view->qr_value, 0, sizeof(view->qr_value));
    object_visible(view->qr, qr_ok);
    object_visible(view->hint, qr_ok);
    if (qr_ok) {
        y = place_label(view->hint, wifi_qr ? epd_get_str(portal_scan_wifi) : epd_get_str(portal_scan_web),
                        ui_font_body(), width, y) + gap;
        lv_obj_set_pos(view->qr, (width - qr_size) / 2, y);
        y += qr_size + gap;
    }
    const char *values[] = {url, state.ssid, state.password};
    const lv_ext_str_id_t headings[] = {LV_EXT_STR_ID(portal_address), LV_EXT_STR_ID(portal_ssid), LV_EXT_STR_ID(portal_password)};
    for (unsigned i = 1; i < 4; ++i) {
        bool visible = on && (wifi_qr ? i >= 2 : i == 1);
        object_visible(view->headings[i], visible); object_visible(view->values[i], visible);
        if (visible) {
            y = place_label(view->headings[i], LV_EXT_STR_GET(headings[i - 1]), ui_font_small(), width, y) + epd_ui_px(6);
            y = place_label(view->values[i], values[i - 1], ui_font_body(), width, y) + gap;
        } else text_update(view->values[i], "");
    }
}

void portal_page_create(lv_obj_t *screen)
{
    bool web_page = current->id == UI_PAGE_PORTAL_WEB;
    page_title_create(screen, web_page ? LV_EXT_STR_ID(portal_web_title) : LV_EXT_STR_ID(platform_app_portal), ui_font_title());
    portal_view_t *view = lv_malloc_zeroed(sizeof(*view)); current->feature = view;
    if (!view) return;
    view->body = lv_obj_create(screen); epd_obj_init(view->body);
    lv_obj_add_flag(view->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(view->body, LV_DIR_VER);
    view->qr = lv_qrcode_create(view->body);
    lv_qrcode_set_dark_color(view->qr, lv_color_black());
    lv_qrcode_set_light_color(view->qr, lv_color_white());
    lv_qrcode_set_quiet_zone(view->qr, true);
    view->hint = label_create(view->body, "", 0, 0, 1, ui_font_body());
    lv_obj_set_style_text_align(view->hint, LV_TEXT_ALIGN_CENTER, 0);
    view->status = label_create(view->body, "", 0, 0, 1, ui_font_body());
    for (unsigned i = 0; i < 4; ++i) {
        view->headings[i] = label_create(view->body, "", 0, 0, 1, ui_font_small());
        view->values[i] = label_create(view->body, "", 0, 0, 1, ui_font_body());
        lv_obj_set_style_text_color(view->headings[i], lv_color_hex(0x555555), 0);
    }
    if (!web_page) {
        view->web_button = button_create(screen, 0, 0, 1, 1, "", UI_PAGE_PORTAL_WEB);
        view->button = button_create(screen, 0, 0, 1, 1, "", PORTAL_TOGGLE);
        view->accounts_button = button_create(screen, 0, 0, 1, 1, "", PORTAL_ACCOUNTS);
        view->accounts = ui_portal_accounts_create(screen);
    }
}
void portal_page_resume(void) { render(true); }
void portal_page_process(void)
{
    if (current && current->foreground &&
        (current->id == UI_PAGE_PORTAL || current->id == UI_PAGE_PORTAL_WEB) &&
        pending_page == NAV_IDLE && lv_refreshing_done())
        render(false);
}
void portal_page_stop(void)
{
    portal_view_t *view = current->feature;
    if (!view) return;
    ui_portal_accounts_destroy(view->accounts);
    memset(view->qr_value, 0, sizeof(view->qr_value));
    lv_free(view); current->feature = NULL;
}
bool portal_page_action(int *target)
{
    portal_view_t *view = current->feature;
    if (!view || !target) return false;
    if (ui_portal_accounts_action(view->accounts, *target)) { render(true); return true; }
    if (current->popup) {
        if (*target == NAV_BACK || *target == POPUP_CLOSE) { popup_close(); render(true); return true; }
        return false;
    }
    if (current->id == UI_PAGE_PORTAL_WEB) return false;
    if (*target == PORTAL_ACCOUNTS) {
        if (view->accounts) ui_portal_accounts_open(view->accounts);
        else popup_open_i18n(LV_EXT_STR_ID(platform_no_memory), NULL);
        render(true); return true;
    }
    if (*target != PORTAL_TOGGLE) return false;
    portal_snapshot_t state; portal_snapshot(&state);
    if (state.hotspot_requested) portal_stop();
    else if (portal_start() != RT_EOK) popup_open_i18n(LV_EXT_STR_ID(portal_failed), NULL);
    render(true); return true;
}
#ifdef RT_USING_WIFI
static int app_main(intent_t intent)
{ (void)intent; return ui_navigation_start("portal", UI_PAGE_PORTAL); }
BUILTIN_APP_EXPORT(LV_EXT_STR_ID(platform_app_portal), &ui_icon_portal_app, "portal", app_main, 1);
#endif
