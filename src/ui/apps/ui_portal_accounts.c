#include "ui_internal.h"
#include "platform/epd_keyboard.h"
#include "ui_portal_accounts.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "portal_auth.h"
#include "storage.h"

enum {
    A_ADD = -18400, A_PASSWORD, A_DELETE, A_DELETE_CONFIRM, A_CANCEL,
    A_USERNAME, A_NEW_PASSWORD, A_CONFIRM_PASSWORD, A_SAVE,
    A_INPUT_SAVE, A_INPUT_CANCEL, A_SETUP_OPEN, A_SETUP_CLOSE,
    A_BACK, A_TITLE, A_STATUS, A_HINT, A_CODE, A_TIME,
    A_USER_BASE = -18500
};
typedef enum { V_LIST, V_ACCOUNT, V_ADD, V_PASSWORD, V_DELETE } account_mode_t;

struct ui_portal_accounts
{
    lv_obj_t *root, *body, *editor, *input, *keyboard, *save, *cancel, *input_title, *input_hint;
    ui_settings_list_t *list;
    portal_auth_snapshot_t snapshot;
    account_mode_t mode;
    uint32_t ticket, selected_id, locale, scale, poll;
    int32_t width, bottom;
    int input_key, restore_key;
    bool visible, dirty, owns_setup, last_delete;
    char username[PORTAL_AUTH_USERNAME_MAX + 1];
    char password[PORTAL_AUTH_PASSWORD_MAX + 1], confirmation[PORTAL_AUTH_PASSWORD_MAX + 1];
    lv_ext_str_id_t notice;
};

static void wipe(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;
    while (size--) *bytes++ = 0;
}

static lv_ext_str_id_t error_id(portal_auth_error_t error)
{
    switch (error) {
    case PORTAL_AUTH_BUSY: return LV_EXT_STR_ID(portal_auth_busy);
    case PORTAL_AUTH_INVALID: return LV_EXT_STR_ID(portal_auth_invalid);
    case PORTAL_AUTH_UNAVAILABLE: return LV_EXT_STR_ID(portal_auth_unavailable);
    case PORTAL_AUTH_STORE_CORRUPT: return LV_EXT_STR_ID(portal_auth_corrupt);
    case PORTAL_AUTH_STORAGE_ERROR: return LV_EXT_STR_ID(portal_auth_storage);
    case PORTAL_AUTH_CRYPTO_ERROR: return LV_EXT_STR_ID(portal_auth_crypto);
    case PORTAL_AUTH_ACCOUNT_EXISTS: return LV_EXT_STR_ID(portal_auth_exists);
    case PORTAL_AUTH_ACCOUNT_LIMIT: return LV_EXT_STR_ID(portal_auth_limit);
    case PORTAL_AUTH_NOT_FOUND: return LV_EXT_STR_ID(portal_auth_missing);
    case PORTAL_AUTH_SETUP_CLOSED: return LV_EXT_STR_ID(portal_setup_required);
    case PORTAL_AUTH_RATE_LIMITED: return LV_EXT_STR_ID(portal_auth_rate);
    default: return LV_EXT_STR_ID(portal_auth_failed);
    }
}

static void show_error(lv_ext_str_id_t id)
{
    popup_open_i18n(id, NULL);
}

static void selected(int key, void *context)
{
    ui_portal_accounts_t *view = context;
    if (view->visible && current && current->foreground && current->id == UI_PAGE_PORTAL &&
        lv_obj_get_screen(view->root) == current->screen && !current->popup &&
        !ui_shell_modal_visible() && !storage_changing() && pending_page == NAV_IDLE)
        pending_page = key;
}

static void input_event(lv_event_t *event)
{
    ui_portal_accounts_t *view = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) selected(A_INPUT_SAVE, view);
    else if (code == LV_EVENT_CLICKED)
        selected(lv_event_get_target_obj(event) == view->save ? A_INPUT_SAVE : A_INPUT_CANCEL, view);
    else if (code == LV_EVENT_VALUE_CHANGED) view->dirty = true;
}

static void clear_draft(ui_portal_accounts_t *view)
{
    wipe(view->password, sizeof(view->password));
    wipe(view->confirmation, sizeof(view->confirmation));
    wipe(view->username, sizeof(view->username));
}

static void close_editor(ui_portal_accounts_t *view)
{
    if (!view->editor) return;
    const char *text = lv_textarea_get_text(view->input);
    wipe((void *)text, strlen(text));
    lv_textarea_set_text(view->input, "");
    epd_keyboard_set_textarea(view->keyboard, NULL);
    lv_obj_delete(view->editor);
    view->editor = view->input = view->keyboard = view->save = view->cancel = NULL;
    view->input_title = view->input_hint = NULL;
    object_visible(view->body, true);
    view->restore_key = view->input_key; view->dirty = true;
}

static void restore_focus(ui_portal_accounts_t *view)
{
    if (!view->visible || current->popup || ui_shell_modal_visible()) return;
    if (view->editor) {
        lv_group_add_obj(current->group, view->input);
        lv_group_add_obj(current->group, view->cancel);
        lv_group_add_obj(current->group, view->save);
        lv_group_add_obj(current->group, view->keyboard);
    } else ui_settings_list_set_group(view->list, current->group);
}

static int list_focus_key(const ui_portal_accounts_t *view)
{
    if (view->snapshot.count) return A_USER_BASE;
    if (view->snapshot.initialized && !view->snapshot.error)
        return view->snapshot.setup_open ? A_SETUP_CLOSE : A_SETUP_OPEN;
    return A_BACK;
}

static lv_ext_str_id_t input_title_id(int key)
{
    if (key == A_USERNAME) return LV_EXT_STR_ID(portal_username);
    if (key == A_NEW_PASSWORD) return LV_EXT_STR_ID(portal_new_password);
    return LV_EXT_STR_ID(portal_confirm_password);
}

static void editor_layout(ui_portal_accounts_t *view)
{
    int32_t width = lv_obj_get_width(view->root), height = lv_obj_get_height(view->root);
    int32_t gap = epd_ui_px(12), margin = MARGIN;
    int32_t field = LV_MAX(epd_ui_touch(60), ui_font_body()->line_height + epd_ui_px(20));
    lv_obj_set_size(view->editor, width, height);
    text_update(view->input_title, LV_EXT_STR_GET(input_title_id(view->input_key)));
    lv_obj_set_style_text_font(view->input_title, ui_font_body(), 0);
    lv_obj_set_pos(view->input_title, margin, 0);
    lv_obj_set_size(view->input_title, width - 2 * margin, LV_SIZE_CONTENT);
    lv_obj_update_layout(view->input_title);
    int32_t y = lv_obj_get_height(view->input_title) + gap;
    lv_obj_set_pos(view->input, margin, y);
    lv_obj_set_size(view->input, width - 2 * margin, field);
    lv_obj_set_style_text_font(view->input, ui_font_body(), 0);
    y += field + gap;
    text_update(view->input_hint, view->input_key == A_USERNAME ?
        epd_get_str(portal_username_hint) : epd_get_str(portal_password_hint));
    lv_obj_set_style_text_font(view->input_hint, ui_font_small(), 0);
    lv_obj_set_pos(view->input_hint, margin, y);
    lv_obj_set_size(view->input_hint, width - 2 * margin, LV_SIZE_CONTENT);
    lv_obj_update_layout(view->input_hint);
    y += lv_obj_get_height(view->input_hint) + gap;
    int32_t button_width = (width - 2 * margin - gap) / 2;
    lv_obj_t *buttons[] = {view->cancel, view->save};
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_pos(buttons[i], margin + (int)i * (button_width + gap), y);
        lv_obj_set_size(buttons[i], button_width, field);
        lv_obj_t *label = lv_obj_get_child(buttons[i], 0);
        text_update(label, i ? epd_get_str(portal_done) : epd_get_str(platform_cancel));
        lv_obj_set_style_text_font(label, ui_font_body(), 0);
        lv_obj_set_size(label, button_width - 2 * gap, LV_SIZE_CONTENT); lv_obj_center(label);
    }
    y += field + gap;
    epd_keyboard_refresh(view->keyboard);
    int32_t wanted = epd_keyboard_preferred_height(view->keyboard);
    int32_t available = LV_MAX(1, height - y - gap);
    lv_obj_set_pos(view->keyboard, 0, y);
    lv_obj_set_size(view->keyboard, width, LV_MIN(wanted, available));
}

static void open_editor(ui_portal_accounts_t *view, int key)
{
    view->input_key = key;
    ui_settings_list_set_group(view->list, NULL);
    object_visible(view->body, false);
    view->editor = lv_obj_create(view->root); epd_obj_init(view->editor);
    view->input_title = label_create(view->editor, "", 0, 0, 1, ui_font_body());
    view->input_hint = label_create(view->editor, "", 0, 0, 1, ui_font_small());
    lv_label_set_long_mode(view->input_title, LV_LABEL_LONG_WRAP);
    lv_label_set_long_mode(view->input_hint, LV_LABEL_LONG_WRAP);
    view->input = lv_textarea_create(view->editor);
    lv_textarea_set_one_line(view->input, true);
    lv_textarea_set_max_length(view->input, key == A_USERNAME ? PORTAL_AUTH_USERNAME_MAX : PORTAL_AUTH_PASSWORD_MAX);
    if (key == A_USERNAME) {
        lv_textarea_set_accepted_chars(view->input, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-");
        lv_textarea_set_text(view->input, view->username);
    } else {
        lv_textarea_set_password_mode(view->input, true);
        lv_textarea_set_password_show_time(view->input, 0);
        lv_textarea_set_text(view->input, key == A_NEW_PASSWORD ? view->password : view->confirmation);
    }
    lv_obj_set_style_border_width(view->input, 2, 0);
    lv_obj_set_style_border_color(view->input, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(view->input, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(view->input, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(view->input, input_event, LV_EVENT_READY, view);
    lv_obj_add_event_cb(view->input, input_event, LV_EVENT_VALUE_CHANGED, view);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *button = lv_button_create(view->editor); epd_obj_init(button);
        lv_obj_set_style_border_width(button, 2, 0);
        lv_obj_set_style_border_color(button, lv_color_black(), 0);
        lv_obj_set_style_bg_color(button, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_pad(button, -3, LV_STATE_FOCUS_KEY);
        lv_obj_add_event_cb(button, input_event, LV_EVENT_CLICKED, view);
        label_create(button, "", 0, 0, 1, ui_font_body());
        lv_obj_set_style_text_align(lv_obj_get_child(button, 0), LV_TEXT_ALIGN_CENTER, 0);
        if (i) view->save = button; else view->cancel = button;
    }
    const epd_keyboard_config_t config = {
        .mode = key == A_USERNAME ? EPD_KEYBOARD_ASCII : EPD_KEYBOARD_PASSWORD
    };
    view->keyboard = epd_keyboard_create(view->editor, view->input, &config);
    lv_obj_add_event_cb(view->keyboard, input_event, LV_EVENT_READY, view);
    editor_layout(view); restore_focus(view); lv_group_focus_obj(view->input);
    view->dirty = true;
}

static bool valid_username(const char *text)
{
    size_t size = strlen(text);
    if (size < 3 || size > PORTAL_AUTH_USERNAME_MAX) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

static bool valid_password(const char *text)
{
    size_t size = strlen(text);
    if (size < 8 || size > PORTAL_AUTH_PASSWORD_MAX) return false;
    const unsigned char *p = (const unsigned char *)text;
    size_t i = 0;
    while (i < size) {
        unsigned char c = p[i++];
        if (c < 0x80) {
            if (c < 32 || c == 127) return false;
            continue;
        }
        unsigned tail;
        uint32_t value, minimum;
        if (c >= 0xc2 && c <= 0xdf) { tail = 1; value = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { tail = 2; value = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { tail = 3; value = c & 7; minimum = 0x10000; }
        else return false;
        if (i + tail > size) return false;
        while (tail--) {
            if ((p[i] & 0xc0) != 0x80) return false;
            value = (value << 6) | (p[i++] & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

static const portal_auth_account_t *account(ui_portal_accounts_t *view)
{
    for (unsigned i = 0; i < view->snapshot.count && i < PORTAL_AUTH_MAX_ACCOUNTS; ++i)
        if (view->snapshot.accounts[i].id == view->selected_id) return &view->snapshot.accounts[i];
    return NULL;
}

static void row(ui_settings_list_row_t *rows, size_t *count, int key,
                ui_settings_row_kind_t kind, const char *title, const char *value, bool enabled)
{
    rows[(*count)++] = (ui_settings_list_row_t){.key=key, .kind=kind,
        .title=title, .value=value, .enabled=enabled};
}
#define INFO(k,t,v) row(rows,&n,k,UI_SETTINGS_ROW_INFO,epd_get_str(t),v,false)
#define LINK(k,t,e) row(rows,&n,k,UI_SETTINGS_ROW_LINK,epd_get_str(t),NULL,e)

static bool render(ui_portal_accounts_t *view)
{
    int32_t width = lv_obj_get_width(current->screen);
    int32_t top = PAGE_TITLE_Y + ui_font_title()->line_height + epd_ui_px(26);
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    lv_obj_set_pos(view->root, 0, top);
    lv_obj_set_size(view->root, width, LV_MAX(1, bottom - top));
    lv_obj_set_size(view->body, width, LV_MAX(1, bottom - top));
    lv_obj_update_layout(view->root);
    if (view->editor) { editor_layout(view); return true; }
    if (!ui_settings_list_refresh_fonts(view->list, ui_font_body(), ui_font_small())) return false;
    ui_settings_list_row_t rows[PORTAL_AUTH_MAX_ACCOUNTS + 12];
    size_t n = 0;
    bool ready = view->snapshot.initialized && view->snapshot.error == PORTAL_AUTH_OK;
    bool enabled = ready && !view->snapshot.busy && !view->ticket;
    char count[32], time[32];
    const portal_auth_account_t *selected_account = account(view);
    if (view->mode == V_LIST) {
        snprintf(count, sizeof(count), "%u / %u", view->snapshot.count, (unsigned)PORTAL_AUTH_MAX_ACCOUNTS);
        INFO(A_TITLE, portal_accounts, count);
        if (ready && !view->snapshot.count) {
            if (view->snapshot.setup_open) {
                INFO(A_HINT, portal_setup_open_hint, NULL);
                INFO(A_CODE, portal_setup_code, view->snapshot.setup_code);
                snprintf(time, sizeof(time), "%lu:%02lu",
                    (unsigned long)(view->snapshot.setup_remaining_seconds / 60),
                    (unsigned long)(view->snapshot.setup_remaining_seconds % 60));
                INFO(A_TIME, portal_setup_remaining, time);
                LINK(A_SETUP_CLOSE, portal_setup_cancel, true);
            } else {
                INFO(A_HINT, portal_setup_hint, NULL);
                LINK(A_SETUP_OPEN, portal_setup_enable, enabled);
            }
        } else if (ready) {
            for (unsigned i = 0; i < view->snapshot.count && i < PORTAL_AUTH_MAX_ACCOUNTS; ++i)
                row(rows, &n, A_USER_BASE + (int)i, UI_SETTINGS_ROW_LINK,
                    view->snapshot.accounts[i].username, NULL, enabled);
            LINK(A_ADD, portal_account_add, enabled && view->snapshot.count < PORTAL_AUTH_MAX_ACCOUNTS);
            if (view->snapshot.count >= PORTAL_AUTH_MAX_ACCOUNTS) INFO(A_HINT, portal_auth_limit, NULL);
        }
    } else if (view->mode == V_ACCOUNT) {
        INFO(A_TITLE, portal_username, selected_account ? selected_account->username : "");
        INFO(A_HINT, portal_password_private, NULL);
        LINK(A_PASSWORD, portal_password_change, enabled && selected_account);
        LINK(A_DELETE, portal_account_delete, enabled && selected_account);
    } else if (view->mode == V_DELETE) {
        INFO(A_TITLE, portal_account_delete, selected_account ? selected_account->username : "");
        if (view->last_delete) INFO(A_HINT, portal_delete_last, NULL);
        else INFO(A_HINT, portal_delete_confirm, NULL);
        LINK(A_CANCEL, platform_cancel, !view->ticket);
        LINK(A_DELETE_CONFIRM, portal_delete_confirm_action, enabled && selected_account);
    } else {
        INFO(A_TITLE, portal_accounts, view->mode == V_ADD ? epd_get_str(portal_account_add) : epd_get_str(portal_password_change));
        row(rows, &n, A_USERNAME, view->mode == V_ADD ? UI_SETTINGS_ROW_LINK : UI_SETTINGS_ROW_INFO,
            epd_get_str(portal_username), view->username, enabled && view->mode == V_ADD);
        row(rows, &n, A_NEW_PASSWORD, UI_SETTINGS_ROW_LINK, epd_get_str(portal_new_password),
            view->password[0] ? epd_get_str(portal_secret_entered) : epd_get_str(portal_secret_empty), enabled);
        row(rows, &n, A_CONFIRM_PASSWORD, UI_SETTINGS_ROW_LINK, epd_get_str(portal_confirm_password),
            view->confirmation[0] ? epd_get_str(portal_secret_entered) : epd_get_str(portal_secret_empty), enabled);
        LINK(A_SAVE, portal_save, enabled);
        LINK(A_CANCEL, platform_cancel, !view->ticket);
    }
    if (!ready)
        row(rows, &n, A_STATUS, UI_SETTINGS_ROW_INFO,
            LV_EXT_STR_GET(view->snapshot.error ? error_id(view->snapshot.error) : LV_EXT_STR_ID(portal_auth_loading)), NULL, false);
    else if (view->ticket || view->snapshot.busy) INFO(A_STATUS, portal_auth_busy, NULL);
    else if (view->notice) row(rows, &n, A_STATUS, UI_SETTINGS_ROW_INFO, LV_EXT_STR_GET(view->notice), NULL, false);
    LINK(A_BACK, portal_back, true);
    return ui_settings_list_set_rows(view->list, rows, n);
}
#undef INFO
#undef LINK

static void refresh(ui_portal_accounts_t *view)
{
    portal_auth_snapshot_t incoming;
    portal_auth_snapshot(&incoming);
    if (incoming.revision != view->snapshot.revision || incoming.initialized != view->snapshot.initialized ||
        incoming.error != view->snapshot.error || incoming.busy != view->snapshot.busy ||
        incoming.setup_open != view->snapshot.setup_open ||
        incoming.setup_remaining_seconds != view->snapshot.setup_remaining_seconds) view->dirty = true;
    view->snapshot = incoming;
    if (!incoming.setup_open) view->owns_setup = false;
    if (view->mode != V_LIST && view->mode != V_ADD && !account(view) && !view->ticket) {
        close_editor(view); clear_draft(view); view->mode = V_LIST;
        view->notice = LV_EXT_STR_ID(portal_auth_missing); view->dirty = true;
    }
    if (view->mode == V_ADD && !incoming.count) {
        close_editor(view); clear_draft(view); view->mode = V_LIST; view->dirty = true;
    }
    if (view->mode == V_DELETE && view->last_delete != (incoming.count == 1)) {
        view->last_delete = incoming.count == 1; view->dirty = true;
    }
}

void ui_portal_accounts_process(ui_portal_accounts_t *view, bool layout)
{
    if (!view || !view->visible || current->popup || ui_shell_modal_visible()) return;
    if (layout || lv_tick_elaps(view->poll) >= 250) {
        view->poll = lv_tick_get(); refresh(view);
        if (view->ticket) {
            portal_auth_job_t result;
            bool found = portal_auth_result(view->ticket, &result);
            if (found && result.done) {
                view->ticket = 0; view->dirty = true;
                if (result.error == PORTAL_AUTH_OK) {
                    close_editor(view); clear_draft(view); view->mode = V_LIST;
                    view->notice = LV_EXT_STR_ID(portal_saved); refresh(view);
                    view->restore_key = list_focus_key(view);
                } else view->notice = error_id(result.error);
            } else if (!view->snapshot.busy && !found) {
                view->ticket = 0; view->dirty = true; view->notice = LV_EXT_STR_ID(portal_result_unavailable);
            }
        }
    }
    int32_t width = lv_obj_get_width(current->screen);
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    uint32_t locale = epd_locale_revision(), scale = epd_ui_scale_revision();
    if (layout || view->dirty || view->locale != locale || view->scale != scale ||
        view->width != width || view->bottom != bottom) {
        if (!render(view)) return;
        view->dirty = false; view->locale = locale; view->scale = scale;
        view->width = width; view->bottom = bottom;
    }
    restore_focus(view);
    if (!view->editor && view->restore_key) {
        ui_settings_list_focus_key(view->list, view->restore_key); view->restore_key = 0;
    }
}

ui_portal_accounts_t *ui_portal_accounts_create(lv_obj_t *screen)
{
    ui_portal_accounts_t *view = lv_malloc_zeroed(sizeof(*view));
    if (!view) return NULL;
    view->root = lv_obj_create(screen); epd_obj_init(view->root);
    view->body = lv_obj_create(view->root); epd_obj_init(view->body);
    view->list = ui_settings_list_create(view->body, ui_font_body(), ui_font_small(), selected, view);
    if (!view->list) { lv_obj_delete(view->root); lv_free(view); return NULL; }
    object_visible(view->root, false);
    return view;
}

void ui_portal_accounts_open(ui_portal_accounts_t *view)
{
    if (!view) return;
    view->visible = view->dirty = true;
    view->mode = V_LIST;
    refresh(view); view->restore_key = list_focus_key(view);
    object_visible(view->root, true);
    ui_portal_accounts_process(view, true);
}

bool ui_portal_accounts_visible(const ui_portal_accounts_t *view)
{ return view && view->visible; }

void ui_portal_accounts_destroy(ui_portal_accounts_t *view)
{
    if (!view) return;
    if (view->owns_setup) portal_auth_bootstrap_close();
    close_editor(view);
    ui_settings_list_set_group(view->list, NULL);
    ui_settings_list_destroy(view->list);
    lv_obj_delete(view->root);
    wipe(view, sizeof(*view)); lv_free(view);
}

static void go_back(ui_portal_accounts_t *view)
{
    if (view->editor) { close_editor(view); return; }
    clear_draft(view);
    if (view->mode != V_LIST) {
        view->mode = V_LIST; view->restore_key = list_focus_key(view);
    } else {
        if (view->owns_setup) portal_auth_bootstrap_close();
        view->owns_setup = false; view->visible = false;
        ui_settings_list_set_group(view->list, NULL);
        /* Remove the transient code from the retained, hidden list. */
        ui_settings_list_set_rows(view->list, NULL, 0);
        wipe(view->snapshot.setup_code, sizeof(view->snapshot.setup_code));
        object_visible(view->root, false);
    }
    view->dirty = true;
}

static void submit_result(ui_portal_accounts_t *view, portal_auth_error_t error)
{
    if (error != PORTAL_AUTH_OK) show_error(error_id(error));
    else {
        wipe(view->password, sizeof(view->password));
        wipe(view->confirmation, sizeof(view->confirmation));
        view->notice = 0;
    }
    view->dirty = true;
}

bool ui_portal_accounts_action(ui_portal_accounts_t *view, int target)
{
    if (!view || !view->visible) return false;
    if (current->popup) {
        if (target == POPUP_CLOSE || target == NAV_BACK) {
            popup_close(); view->dirty = true; restore_focus(view); return true;
        }
        return true;
    }
    if (view->editor) {
        if (target == NAV_BACK || target == A_INPUT_CANCEL) close_editor(view);
        else if (target == A_INPUT_SAVE) {
            const char *text = lv_textarea_get_text(view->input);
            bool valid = view->input_key == A_USERNAME ? valid_username(text) : valid_password(text);
            if (!valid) show_error(view->input_key == A_USERNAME ?
                LV_EXT_STR_ID(portal_username_hint) : LV_EXT_STR_ID(portal_password_hint));
            else {
                char *field = view->input_key == A_USERNAME ? view->username :
                    view->input_key == A_NEW_PASSWORD ? view->password : view->confirmation;
                size_t capacity = view->input_key == A_USERNAME ? sizeof(view->username) : sizeof(view->password);
                wipe(field, capacity); memcpy(field, text, strlen(text) + 1); close_editor(view);
            }
        }
        return true;
    }
    if (target == NAV_BACK || target == A_BACK || target == A_CANCEL) { go_back(view); return true; }
    bool was_last = view->last_delete;
    refresh(view);
    if (target == A_SETUP_CLOSE) {
        portal_auth_bootstrap_close(); view->owns_setup = false;
        view->notice = LV_EXT_STR_ID(portal_setup_cancelled); refresh(view); view->dirty = true;
        return true;
    }
    if (view->ticket || view->snapshot.busy) return true;
    if (!view->snapshot.initialized || view->snapshot.error) {
        show_error(view->snapshot.error ? error_id(view->snapshot.error) : LV_EXT_STR_ID(portal_auth_loading));
        return true;
    }
    view->notice = 0;
    if (target >= A_USER_BASE && target < A_USER_BASE + PORTAL_AUTH_MAX_ACCOUNTS && view->mode == V_LIST) {
        unsigned index = (unsigned)(target - A_USER_BASE);
        if (index < view->snapshot.count) {
            view->selected_id = view->snapshot.accounts[index].id; view->mode = V_ACCOUNT;
            view->restore_key = A_PASSWORD;
        }
    } else switch (target) {
    case A_SETUP_OPEN:
        if (view->mode == V_LIST && !view->snapshot.count) {
            portal_auth_error_t error = portal_auth_bootstrap_open();
            if (error) show_error(error_id(error));
            else { view->owns_setup = true; view->restore_key = A_SETUP_CLOSE; }
            refresh(view);
        }
        break;
    case A_ADD:
        if (view->mode == V_LIST && view->snapshot.count && view->snapshot.count < PORTAL_AUTH_MAX_ACCOUNTS) {
            clear_draft(view); view->mode = V_ADD; view->restore_key = A_USERNAME;
        }
        break;
    case A_PASSWORD:
        if (view->mode == V_ACCOUNT && account(view)) {
            clear_draft(view);
            snprintf(view->username, sizeof(view->username), "%s", account(view)->username);
            view->mode = V_PASSWORD; view->restore_key = A_NEW_PASSWORD;
        }
        break;
    case A_DELETE:
        if (view->mode == V_ACCOUNT && account(view)) {
            view->mode = V_DELETE; view->last_delete = view->snapshot.count == 1; view->restore_key = A_CANCEL;
        }
        break;
    case A_DELETE_CONFIRM:
        if (view->mode == V_DELETE && account(view)) {
            if (view->last_delete && !was_last) break;
            submit_result(view, portal_auth_delete_async(view->selected_id, &view->ticket));
        }
        break;
    case A_USERNAME: case A_NEW_PASSWORD: case A_CONFIRM_PASSWORD:
        if ((view->mode == V_ADD || view->mode == V_PASSWORD) &&
            (target != A_USERNAME || view->mode == V_ADD)) open_editor(view, target);
        break;
    case A_SAVE:
        if (view->mode != V_ADD && view->mode != V_PASSWORD) break;
        if (!valid_username(view->username)) show_error(LV_EXT_STR_ID(portal_username_hint));
        else if (!valid_password(view->password)) show_error(LV_EXT_STR_ID(portal_password_hint));
        else if (strcmp(view->password, view->confirmation)) show_error(LV_EXT_STR_ID(portal_password_mismatch));
        else submit_result(view, view->mode == V_ADD ?
            portal_auth_add_async(view->username, view->password, &view->ticket) :
            portal_auth_password_async(view->selected_id, view->password, &view->ticket));
        break;
    default: break;
    }
    view->dirty = true; return true;
}
