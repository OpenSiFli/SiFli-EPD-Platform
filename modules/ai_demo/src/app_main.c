#include "app_service.h"
#include "epd_memory.h"
#include "platform/epd_locale.h"
#include "platform/epd_app.h"
#include "platform/epd_i18n.h"
#include "platform/epd_keyboard.h"
#include "demo.h"
#include "chat_markdown.h"
#include "chat_icons.h"
#include <stdio.h>
#include <string.h>

EPD_APP_DEFINE("ai_demo");
MODULE_INIT_DEF(demo_init)
MODULE_CLEANUP_DEF(demo_deinit)

typedef enum { CHAT, HISTORY, MODELS } chat_page_kind_t;
typedef enum {
    A_NONE, A_BACK, A_HISTORY, A_NEW, A_MODELS, A_SEND, A_RETRY, A_SAVE,
    A_LATEST, A_OPEN, A_MENU, A_RENAME, A_DELETE, A_CONFIRM_DELETE,
    A_DIALOG_CLOSE, A_RENAME_SAVE, A_GRANT, A_SELECT, A_SELECT_NEW,
    A_KEYBOARD, A_HIDE_KEYBOARD,
    A_AUTO_READ, A_SEARCH, A_VOICE
} chat_action_t;
typedef struct {
    lv_obj_t *box, *text, *mark, *speaker;
    chat_markdown_t *markdown;
    size_t length;
    uint32_t revision;
    bool user;
} chat_bubble_t;
typedef struct { char profile[EPD_AI_ID_SIZE], model[EPD_AI_MODEL_SIZE]; } model_choice_t;
typedef struct {
    lv_obj_t *screen, *title, *back, *history, *new_chat, *model, *search;
    lv_obj_t *body, *empty, *input, *send, *notice, *retry, *latest;
    lv_obj_t *composer, *voice, *empty_icon, *header_line;
    lv_obj_t *keyboard_root;
    lv_obj_t *overlay, *dialog, *rename_input;
    lv_obj_t *auto_read, *speech_notice;
    lv_timer_t *timer;
    demo_view_t view;
    chat_bubble_t bubbles[DEMO_MAX_MESSAGES];
    model_choice_t choices[EPD_AI_MAX_PROFILES * EPD_AI_MAX_MODELS];
    size_t choice_count;
    size_t bubble_count;
    uint32_t revision, locale, scale, painted, current_id, target_id, failed_at;
    chat_page_kind_t mode;
    chat_action_t action;
    lv_area_t area;
    int width, message_height;
    bool force, stopped, refresh_profiles, keyboard_open, paint_failed;
    bool changing_input, draft_pending, following, scrolling_programmatically, rebuilding;
    char selected_profile[EPD_AI_ID_SIZE], selected_model[EPD_AI_MODEL_SIZE];
} demo_page_t;
static demo_page_t *page_binding;
static void layout(demo_page_t *page);
static bool rebuild_list(demo_page_t *page);
static void keyboard_close(demo_page_t *page);
static void dialog_close(demo_page_t *page);
static void composer_state(demo_page_t *page);

static void show(lv_obj_t *object, bool visible)
{
    if (!object) return;
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
static void enabled(lv_obj_t *object, bool enable)
{
    if (enable) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
    lv_obj_t *caption = lv_obj_get_child(object, 0);
    if (caption) lv_obj_set_style_text_color(caption, lv_color_hex(enable ? 0x000000 : 0x999999), 0);
}
static void white(lv_obj_t *object)
{
    lv_obj_set_style_bg_color(object, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(object, lv_color_black(), 0);
}
static lv_obj_t *label(lv_obj_t *parent, const char *text, int width)
{
    lv_obj_t *object = epd_app_label(parent, text, 0, 0, width, epd_app_font_role(EPD_FONT_BODY));
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(object, LV_SIZE_CONTENT);
    return object;
}
static void clicked(lv_event_t *event)
{
    demo_page_t *page = page_binding;
    if (!page || page->stopped) return;
    chat_action_t action = (chat_action_t)(uintptr_t)lv_event_get_user_data(event);
    if (action != A_NONE) page->action = action;
}
static lv_obj_t *button(lv_obj_t *parent, const char *text, chat_action_t action, bool outlined)
{
    lv_obj_t *object = epd_app_button(parent, 0, 0, 80, 64, text, clicked, (void *)(uintptr_t)action);
    lv_obj_set_style_border_width(object, outlined ? 1 : 0, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(0x999999), 0);
    lv_obj_set_style_radius(object, epd_ui_px(20), 0);
    lv_obj_set_style_outline_width(object, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(object, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(object, lv_color_hex(0x999999), LV_STATE_DISABLED);
    return object;
}
static void place_button(lv_obj_t *object, int x, int y, int width, int height, const char *text)
{
    lv_obj_set_pos(object, x, y); lv_obj_set_size(object, width, height);
    lv_obj_t *caption = lv_obj_get_child(object, 0);
    if (text) epd_app_text(caption, text);
    lv_obj_set_style_text_font(caption, epd_app_font_role(EPD_FONT_SMALL), 0);
    lv_obj_set_size(caption, width - epd_ui_px(12), LV_SIZE_CONTENT);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
    lv_obj_center(caption);
}
static lv_obj_t *icon_button(lv_obj_t *parent, chat_action_t action, chat_icon_t icon)
{
    lv_obj_t *object = button(parent, "", action, false);
    lv_obj_t *image = lv_image_create(object);
    epd_app_object_init(image);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(image, chat_icon(icon, epd_ui_scale_percent(), true));
    lv_obj_center(image);
    return object;
}
static void place_icon(lv_obj_t *object, int x, int y, int width, int height, chat_icon_t icon, bool active)
{
    place_button(object, x, y, width, height, NULL);
    lv_obj_t *image = lv_obj_get_child(object, 1);
    lv_image_set_src(image, chat_icon(icon, epd_ui_scale_percent(), active));
    lv_obj_center(image);
}
static void model_caption(demo_page_t *page)
{
    lv_obj_update_layout(page->model);
    lv_obj_t *caption = lv_obj_get_child(page->model, 0);
    epd_app_text(caption, page->view.model[0] ? page->view.model : epd_get_str(ai_demo_models));
    lv_obj_set_style_text_font(caption, epd_app_font_role(EPD_FONT_SMALL), 0);
    lv_obj_set_width(caption, lv_obj_get_width(page->model) - epd_ui_px(48));
    lv_obj_set_height(caption, lv_font_get_line_height(epd_app_font_role(EPD_FONT_SMALL)));
    lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, epd_ui_px(16), 0);
    lv_obj_t *arrow = lv_obj_get_child(page->model, 1);
    lv_image_set_src(arrow, chat_icon(CHAT_ICON_CHEVRON, epd_ui_scale_percent(), !page->view.busy));
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -epd_ui_px(12), 0);
}
static void composer_state(demo_page_t *page)
{
    const char *text = lv_textarea_get_text(page->input);
    bool has_text = false;
    for (; *text; ++text) if (!strchr(" \t\r\n", *text)) { has_text = true; break; }
    bool can_send = page->view.generating || (!page->view.busy && has_text);
    enabled(page->send, can_send);
    bool editable = !page->view.busy || page->view.generating;
    if (editable) lv_obj_remove_state(page->input, LV_STATE_DISABLED);
    else lv_obj_add_state(page->input, LV_STATE_DISABLED);
    enabled(page->voice, editable);
    lv_image_set_src(lv_obj_get_child(page->voice, 1), chat_icon(
        CHAT_ICON_MICROPHONE, epd_ui_scale_percent(), editable));
    lv_color_t color = lv_color_hex(can_send ? 0x000000 : 0xaaaaaa);
    lv_obj_set_style_bg_color(page->send, color, 0);
    lv_obj_set_style_bg_color(page->send, color, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(page->send, color, LV_STATE_DISABLED);
    lv_image_set_src(lv_obj_get_child(page->send, 1), chat_icon(
        page->view.generating ? CHAT_ICON_STOP : CHAT_ICON_SEND, epd_ui_scale_percent(), true));
    lv_textarea_set_placeholder_text(page->input, page->view.message_count ? epd_get_str(ai_demo_continue) : epd_get_str(ai_demo_prompt));
}
static bool service_changed(const demo_view_t *view)
{
    return view->error == EPD_AI_INVALID && view->failure.reason == DEMO_REASON_SERVICE_CHANGED;
}
static const char *error_text(const demo_view_t *view)
{
    if (service_changed(view)) return epd_get_str(ai_demo_service_changed);
    switch (view->error) {
    case EPD_AI_OK: return "";
    case EPD_AI_INVALID: return epd_get_str(ai_demo_invalid);
    case EPD_AI_UNSUPPORTED: return epd_get_str(ai_demo_unsupported);
    case EPD_AI_UNAUTHORIZED: return epd_get_str(ai_demo_unauthorized);
    case EPD_AI_BUSY: return epd_get_str(ai_demo_busy);
    case EPD_AI_NO_MEMORY: return epd_get_str(ai_demo_no_memory);
    case EPD_AI_NO_NETWORK: case EPD_AI_DNS: case EPD_AI_CONNECT:
        return epd_get_str(ai_demo_network_retry);
    case EPD_AI_TLS: return epd_get_str(ai_demo_tls);
    case EPD_AI_AUTH: return epd_get_str(ai_demo_auth);
    case EPD_AI_RATE_LIMIT: return epd_get_str(ai_demo_rate_limit);
    case EPD_AI_SERVER: return epd_get_str(ai_demo_server);
    case EPD_AI_PROTOCOL: return epd_get_str(ai_demo_protocol);
    case EPD_AI_LIMIT: return epd_get_str(ai_demo_limit);
    case EPD_AI_TIMEOUT: return epd_get_str(ai_demo_timeout);
    case EPD_AI_CANCELLED: return epd_get_str(ai_demo_cancelled);
    case EPD_AI_INCOMPLETE: return epd_get_str(ai_demo_incomplete);
    case EPD_AI_STORAGE: return epd_get_str(ai_demo_storage);
    case EPD_AI_NOT_FOUND: return epd_get_str(ai_demo_not_found);
    default: return epd_get_str(ai_demo_unknown);
    }
}
static const char *speech_error(epd_speech_error_t error)
{
    switch (error) {
    case EPD_SPEECH_OK: case EPD_SPEECH_CANCELLED: return "";
    case EPD_SPEECH_NOT_CONFIGURED: return epd_get_str(ai_demo_speech_setup);
    case EPD_SPEECH_BUSY: return epd_get_str(ai_demo_speech_busy);
    case EPD_SPEECH_AUTH: return epd_get_str(ai_demo_auth);
    case EPD_SPEECH_QUOTA: return epd_get_str(ai_demo_speech_quota);
    case EPD_SPEECH_NO_NETWORK: return epd_get_str(ai_demo_no_network);
    case EPD_SPEECH_NO_MEMORY: return epd_get_str(ai_demo_no_memory);
    case EPD_SPEECH_TEXT_TOO_LONG: return epd_get_str(ai_demo_speech_full);
    case EPD_SPEECH_TIMEOUT: return epd_get_str(ai_demo_timeout);
    default: return epd_get_str(ai_demo_speech_failed);
    }
}
static void speech_notice_text(const demo_view_t *view, char *out, size_t capacity)
{
    const chat_speech_view_t *s = &view->speech;
    const char *error = speech_error(s->output_error);
    if (*error) { snprintf(out, capacity, "%s", error); return; }
    if (s->output_active) {
        const char *status = s->output.state == EPD_SPEECH_PAUSED ? epd_get_str(ai_demo_read_paused) :
            s->output.state == EPD_SPEECH_PAUSING ? epd_get_str(ai_demo_read_pausing) :
            s->output.state == EPD_SPEECH_STOPPING ? epd_get_str(ai_demo_read_stopping) :
            s->output.state == EPD_SPEECH_PLAYING || s->output.state == EPD_SPEECH_DRAINING ?
            epd_get_str(ai_demo_reading) : epd_get_str(ai_demo_read_buffering);
        snprintf(out, capacity, "%s", status);
    } else out[0] = 0;
}
static void user_scroll(lv_event_t *event)
{
    demo_page_t *page = lv_event_get_user_data(event);
    if (!page->scrolling_programmatically && !page->rebuilding && page->mode == CHAT) {
        page->following = lv_obj_get_scroll_bottom(page->body) <= epd_ui_px(24);
        show(page->latest, !page->following && page->bubble_count != 0);
    }
}
static void scroll_latest(demo_page_t *page)
{
    page->scrolling_programmatically = true;
    lv_obj_update_layout(page->body);
    lv_obj_scroll_to_y(page->body, LV_MAX(0, lv_obj_get_scroll_y(page->body) +
                          lv_obj_get_scroll_bottom(page->body)), LV_ANIM_OFF);
    page->scrolling_programmatically = false;
    page->following = true;
    show(page->latest, false);
}
static void set_input(demo_page_t *page, const char *text)
{
    bool bound = page->keyboard_root &&
        epd_keyboard_get_textarea(page->keyboard_root) == page->input;
    page->changing_input = true;
    if (bound) epd_keyboard_set_textarea(page->keyboard_root, NULL);
    lv_textarea_set_text(page->input, text);
    lv_textarea_set_cursor_pos(page->input, LV_TEXTAREA_CURSOR_LAST);
    if (bound) epd_keyboard_set_textarea(page->keyboard_root, page->input);
    page->changing_input = false;
}
static bool save_input(demo_page_t *page)
{
    if (!demo_set_draft(page->current_id, lv_textarea_get_text(page->input))) return false;
    page->draft_pending = false; return true;
}
static void rename_clicked(lv_event_t *event)
{
    demo_page_t *page = lv_event_get_user_data(event);
    page->action = A_KEYBOARD;
}
static void text_limit(lv_event_t *event)
{
    lv_obj_t *input = lv_event_get_target_obj(event);
    const char *insert = lv_event_get_param(event);
    size_t capacity = (size_t)(uintptr_t)lv_event_get_user_data(event);
    /* Keep max_length unset so LVGL checks a complete UTF-8 phrase at once. */
    if (insert && (unsigned char)insert[0] != 0x7f &&
        (strlen(lv_textarea_get_text(input)) + strlen(insert) >= capacity ||
         (lv_textarea_get_one_line(input) && (strchr(insert, '\n') || strchr(insert, '\r')))))
        lv_textarea_set_insert_replace(input, "");
}
static void input_changed(lv_event_t *event)
{
    demo_page_t *page = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        page->action = A_KEYBOARD;
    }
    else if (!page->changing_input) page->draft_pending = true;
    if (page->send) composer_state(page);
}
static void textarea_style(lv_obj_t *input)
{
    epd_app_object_init(input); white(input);
    lv_obj_add_flag(input, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(input, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(input, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(input, 1, 0);
    lv_obj_set_style_border_color(input, lv_color_hex(0x999999), 0);
    lv_obj_set_style_radius(input, epd_ui_px(20), 0);
    lv_obj_set_style_pad_all(input, epd_ui_px(14), 0);
    lv_obj_set_style_text_font(input, epd_app_font_role(EPD_FONT_BODY), 0);
    lv_obj_set_style_text_color(input, lv_color_hex(0x666666), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_anim_duration(input, 0, LV_PART_CURSOR);
    lv_obj_set_style_border_width(input, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_color(input, lv_color_black(), LV_PART_CURSOR);
    lv_obj_set_style_border_side(input, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
}
static void keyboard_event(lv_event_t *event)
{
    demo_page_t *page = lv_event_get_user_data(event);
    if (page->stopped || lv_event_get_target_obj(event) != page->keyboard_root) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        page->action = A_HIDE_KEYBOARD;
}
static void keyboard_close(demo_page_t *page)
{
    if (page->keyboard_root) {
        lv_obj_t *input = epd_keyboard_get_textarea(page->keyboard_root);
        if (input) lv_obj_set_style_border_opa(input, LV_OPA_TRANSP, LV_PART_CURSOR);
        lv_obj_delete(page->keyboard_root);
    }
    page->keyboard_root = NULL;
    page->keyboard_open = false;
    if (page->input) lv_obj_set_style_border_opa(page->input, LV_OPA_TRANSP, LV_PART_CURSOR);
}
static void keyboard_open(demo_page_t *page)
{
    lv_obj_t *input = page->rename_input ? page->rename_input : page->input;
    if (!page->keyboard_root) {
        const epd_keyboard_config_t config = {
            .mode = EPD_KEYBOARD_TEXT, .chinese = true, .voice = true
        };
        page->keyboard_root = epd_keyboard_create(page->screen, input, &config);
        if (!page->keyboard_root) return;
        lv_obj_add_event_cb(page->keyboard_root, keyboard_event, LV_EVENT_READY, page);
        lv_obj_add_event_cb(page->keyboard_root, keyboard_event, LV_EVENT_CANCEL, page);
    } else if (epd_keyboard_get_textarea(page->keyboard_root) != input) {
        lv_obj_t *previous = epd_keyboard_get_textarea(page->keyboard_root);
        if (previous) lv_obj_set_style_border_opa(previous, LV_OPA_TRANSP, LV_PART_CURSOR);
        epd_keyboard_set_textarea(page->keyboard_root, input);
    }
    lv_obj_move_to_index(page->keyboard_root, -1);
    page->keyboard_open = true;
    lv_obj_set_style_border_opa(input, LV_OPA_COVER, LV_PART_CURSOR);
}

static void layout(demo_page_t *page)
{
    epd_ui_content_area(page->screen, &page->area);
    int left = epd_ui_px(24), right = lv_obj_get_width(page->screen) - left;
    int width = right - left, gap = epd_ui_px(12), row = epd_ui_touch(64);
    int top = epd_ui_px(80), action_w = epd_ui_touch(76);
    bool chat = page->mode == CHAT;
    page->width = width;
    epd_app_back_button_layout(page->back, left, top + row / 2);
    const char *title = chat ? epd_get_str(ai_demo_title) :
        page->mode == HISTORY ? epd_get_str(ai_demo_history) : epd_get_str(ai_demo_models);
    int title_left = left + epd_ui_touch(88);
    int title_right = right - (chat ? action_w * 3 : action_w) - gap;
    lv_obj_set_pos(page->title, title_left,
        top + (row - lv_font_get_line_height(epd_app_font_role(EPD_FONT_TITLE))) / 2);
    lv_obj_set_size(page->title, LV_MAX(1, title_right - title_left), LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(page->title, epd_app_font_style(EPD_FONT_TITLE, EPD_FONT_STYLE_BOLD), 0);
    lv_label_set_long_mode(page->title, LV_LABEL_LONG_DOT);
    epd_app_text(page->title, title);
    place_icon(page->new_chat, right - action_w, top, action_w, row, CHAT_ICON_NEW, !page->view.busy);
    place_icon(page->history, right - action_w * 2, top, action_w, row, CHAT_ICON_SESSIONS, !page->view.busy);
    place_icon(page->auto_read, right - action_w * 3, top, action_w, row, CHAT_ICON_SPEAKER, page->view.auto_read);
    show(page->history, chat); show(page->auto_read, chat);
    if (page->view.auto_read) lv_obj_add_state(page->auto_read, LV_STATE_CHECKED);
    else lv_obj_remove_state(page->auto_read, LV_STATE_CHECKED);
    int header_bottom = top + row + epd_ui_px(22);
    lv_obj_set_pos(page->header_line, 0, header_bottom);
    lv_obj_set_size(page->header_line, lv_obj_get_width(page->screen), 1);
    int body_top = header_bottom + epd_ui_px(24);
    int bottom = page->area.y2 + 1;
    if (page->keyboard_open) {
        lv_obj_set_width(page->keyboard_root, lv_obj_get_width(page->screen));
        epd_keyboard_refresh(page->keyboard_root);
        int height = epd_keyboard_preferred_height(page->keyboard_root);
        lv_obj_set_pos(page->keyboard_root, 0, bottom - height);
        lv_obj_set_size(page->keyboard_root, lv_obj_get_width(page->screen), height);
        bottom -= height + epd_ui_px(4);
    }
    int composer_height = epd_ui_px(176), inset = epd_ui_px(16);
    int composer_top = bottom - composer_height;
    lv_obj_set_pos(page->composer, left, composer_top);
    /* Keep controls above the system edge gesture; extend only the frame. */
    int frame_tail = page->keyboard_open ? 0 : epd_ui_px(20);
    lv_obj_set_size(page->composer, width, composer_height + frame_tail);
    lv_obj_set_style_radius(page->composer, epd_ui_px(32), 0);
    show(page->composer, chat);
    lv_obj_set_pos(page->input, inset, epd_ui_px(12));
    lv_obj_set_size(page->input, width - inset * 2, epd_ui_px(76));
    lv_obj_set_style_text_font(page->input, epd_app_font_role(EPD_FONT_BODY), 0);
    lv_obj_set_style_pad_all(page->input, epd_ui_px(8), 0);
    int controls_y = composer_height - row - inset;
    int send_x = width - inset - row;
    int voice_x = send_x - row - epd_ui_px(8);
    const lv_font_t *pill_font = epd_app_font_role(EPD_FONT_SMALL);
    lv_point_t search_size;
    lv_text_get_size(&search_size, epd_get_str(ai_demo_web_search), pill_font, 0, 0, width, LV_TEXT_FLAG_NONE);
    int search_width = search_size.x + epd_ui_px(64);
    lv_point_t model_size;
    lv_text_get_size(&model_size, page->view.model[0] ? page->view.model : epd_get_str(ai_demo_models),
                     pill_font, 0, 0, width, LV_TEXT_FLAG_NONE);
    int model_width = LV_MIN(LV_MAX(epd_ui_px(176), model_size.x + epd_ui_px(48)),
                            voice_x - inset - search_width - gap * 2);
    place_button(page->model, inset, controls_y, model_width, row, NULL);
    lv_obj_set_style_radius(page->model, LV_RADIUS_CIRCLE, 0);
    model_caption(page);
    place_button(page->search, inset + model_width + gap, controls_y, search_width, row, epd_get_str(ai_demo_web_search));
    lv_obj_set_style_radius(page->search, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(page->search, lv_color_hex(page->view.web_search ? 0xe7e7e7 : 0xffffff), 0);
    lv_obj_set_style_border_color(page->search, lv_color_hex(page->view.web_search ? 0x555555 : 0xbbbbbb), 0);
    lv_obj_t *search_label = lv_obj_get_child(page->search, 0);
    lv_obj_set_style_text_font(search_label, pill_font, 0);
    lv_obj_set_width(search_label, search_size.x);
    lv_obj_align(search_label, LV_ALIGN_RIGHT_MID, -epd_ui_px(14), 0);
    lv_obj_t *globe = lv_obj_get_child(page->search, 1);
    lv_image_set_src(globe, chat_icon(CHAT_ICON_GLOBE, epd_ui_scale_percent(), page->view.search_available && !page->view.busy));
    lv_obj_align(globe, LV_ALIGN_LEFT_MID, epd_ui_px(14), 0);
    enabled(page->search, !page->view.busy && page->view.search_available);
    place_icon(page->voice, voice_x, controls_y, row, row, CHAT_ICON_MICROPHONE, true);
    place_icon(page->send, send_x, controls_y, row, row, CHAT_ICON_SEND, true);
    lv_obj_set_style_radius(page->send, LV_RADIUS_CIRCLE, 0);
    composer_state(page);
    int body_bottom = chat ? composer_top - gap : bottom;
    bool body_visible = !chat || body_bottom - body_top >= epd_ui_touch(80);
    show(page->body, body_visible);
    page->message_height = LV_MAX(epd_ui_touch(80), body_bottom - body_top);
    lv_obj_set_pos(page->body, left, body_top);
    lv_obj_set_size(page->body, width, page->message_height);
    place_button(page->latest, left + (width - epd_ui_px(180)) / 2,
                 body_bottom - row - gap, epd_ui_px(180), row, epd_get_str(ai_demo_latest));
    show(page->latest, chat && body_visible && !page->following && page->bubble_count);
    page->scale = epd_ui_scale_revision();
    page->force = true;
}
static void clear_body(demo_page_t *page)
{
    page->rebuilding = true;
    lv_obj_clean(page->body);
    memset(page->bubbles, 0, sizeof(page->bubbles));
    page->bubble_count = 0;
    page->empty = page->empty_icon = NULL;
    page->notice = page->retry = page->speech_notice = NULL;
    lv_obj_scroll_to_y(page->body, 0, LV_ANIM_OFF);
    page->rebuilding = false;
}
static int inline_notice(lv_obj_t *object, const char *text, int x, int y, int width, lv_text_align_t align)
{
    epd_app_text(object, text);
    show(object, *text != 0);
    lv_obj_set_style_text_font(object, epd_app_font_role(EPD_FONT_SMALL), 0);
    lv_obj_set_style_text_color(object, lv_color_hex(0x777777), 0);
    lv_obj_set_style_text_align(object, align, 0);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, LV_SIZE_CONTENT);
    lv_obj_update_layout(object);
    return *text ? lv_obj_get_height(object) : 0;
}
static int failed_message_notice(demo_page_t *page, int y, int width, int height)
{
    int row = epd_ui_touch(64), gap = epd_ui_px(12);
    place_icon(page->retry, page->width - width - row - gap,
               y + LV_MAX(0, (height - row) / 2), row, row, CHAT_ICON_RETRY, true);
    show(page->retry, true);
    return gap + inline_notice(page->notice, error_text(&page->view), 0,
        y + height + gap, page->width, LV_TEXT_ALIGN_RIGHT);
}
static bool render_messages(demo_page_t *page)
{
    bool follow = page->following;
    int old_scroll = lv_obj_get_scroll_y(page->body), y = epd_ui_px(16), gap = epd_ui_px(28);
    if (page->bubble_count > page->view.message_count) clear_body(page);
    if (!page->notice) page->notice = label(page->body, "", page->width);
    if (!page->retry) page->retry = icon_button(page->body, A_RETRY, CHAT_ICON_RETRY);
    if (!page->speech_notice) page->speech_notice = label(page->body, "", page->width);
    show(page->notice, false); show(page->retry, false); show(page->speech_notice, false);
    bool failed = !page->view.busy && page->view.error != EPD_AI_OK &&
                  page->view.error != EPD_AI_CANCELLED;
    size_t failed_user = DEMO_MAX_MESSAGES;
    if (failed && page->view.failure.phase != DEMO_PHASE_PRECHECK) {
        for (size_t i = page->view.message_count; i > 0; --i) {
            demo_message_meta_t meta;
            if (!demo_message(i - 1, &meta, 0, NULL, 0)) return false;
            if (meta.user) { failed_user = i - 1; break; }
        }
    }
    if (!page->view.message_count) {
        if (!page->empty) page->empty = label(page->body, "", page->width);
        if (!page->empty_icon) {
            page->empty_icon = lv_image_create(page->body);
            epd_app_object_init(page->empty_icon);
        }
        lv_image_set_src(page->empty_icon, chat_icon(CHAT_ICON_WELCOME, epd_ui_scale_percent(), true));
        int icon_size = epd_ui_px(120), text_gap = epd_ui_px(44);
        int group_height = icon_size + text_gap + lv_font_get_line_height(epd_app_font_role(EPD_FONT_TITLE));
        int group_top = LV_MAX(0, (page->message_height - group_height) / 2);
        lv_obj_set_pos(page->empty_icon, (page->width - icon_size) / 2, group_top);
        epd_app_text(page->empty, page->view.status == DEMO_LOADING ?
                     epd_get_str(ai_demo_loading) : epd_get_str(ai_demo_welcome));
        lv_obj_set_style_text_font(page->empty, epd_app_font_style(EPD_FONT_TITLE, EPD_FONT_STYLE_BOLD), 0);
        lv_obj_set_style_text_align(page->empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(page->empty, page->width);
        lv_obj_set_y(page->empty, group_top + icon_size + text_gap);
        show(page->empty, !failed); show(page->empty_icon, !failed);
        if (failed) failed_message_notice(page, y, page->width * 72 / 100, epd_ui_touch(64));
        else {
            char speech_text[128]; speech_notice_text(&page->view, speech_text, sizeof(speech_text));
            inline_notice(page->speech_notice, speech_text, 0, y, page->width, LV_TEXT_ALIGN_RIGHT);
        }
        return true;
    }
    if (page->empty) { lv_obj_delete(page->empty); page->empty = NULL; }
    if (page->empty_icon) { lv_obj_delete(page->empty_icon); page->empty_icon = NULL; }
    page->rebuilding = true;
    bool ok = true;
    for (size_t i = 0; i < page->view.message_count && i < DEMO_MAX_MESSAGES; ++i) {
        demo_message_meta_t meta;
        if (!demo_message(i, &meta, 0, NULL, 0)) { ok = false; break; }
        chat_bubble_t *bubble = &page->bubbles[i];
        bool created = bubble->box == NULL;
        if (created) {
            bubble->box = lv_obj_create(page->body); epd_app_object_init(bubble->box);
            if (meta.user) bubble->text = label(bubble->box, "", page->width);
            else {
                bubble->markdown = chat_markdown_create(bubble->box);
                if (!bubble->markdown) {
                    lv_obj_delete(bubble->box); memset(bubble, 0, sizeof(*bubble));
                    ok = false; break;
                }
                bubble->text = chat_markdown_root(bubble->markdown);
                bubble->mark = lv_image_create(bubble->box);
                epd_app_object_init(bubble->mark);
                bubble->speaker = label(bubble->box, "AI", epd_ui_px(50));
            }
            bubble->user = meta.user;
            page->bubble_count = i + 1;
        }
        bool empty_reply = !meta.user && !meta.length && !page->view.generating;
        show(bubble->box, !empty_reply);
        if (empty_reply) continue;
        int pad = meta.user ? epd_ui_px(18) : 0;
        int width = meta.user ? page->width * 72 / 100 : page->width - epd_ui_px(8);
        int text_top = meta.user ? pad : epd_ui_px(58);
        if (created || page->force) {
            lv_obj_set_width(bubble->text, width - pad * 2);
            lv_obj_set_pos(bubble->text, pad, text_top);
            lv_obj_set_style_text_font(bubble->text, epd_app_font_role(EPD_FONT_BODY), 0);
            lv_obj_set_style_text_line_space(bubble->text, epd_ui_px(8), 0);
            lv_obj_set_style_bg_color(bubble->box, lv_color_hex(meta.user ? 0xeeeeee : 0xffffff), 0);
            lv_obj_set_style_bg_opa(bubble->box, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bubble->box, meta.user ? epd_ui_px(20) : 0, 0);
            if (!meta.user) {
                lv_image_set_src(bubble->mark, chat_icon(CHAT_ICON_ASSISTANT, epd_ui_scale_percent(), true));
                lv_obj_set_pos(bubble->mark, 0, epd_ui_px(4));
                lv_obj_set_style_text_font(bubble->speaker, epd_app_font_role(EPD_FONT_CAPTION), 0);
                lv_obj_set_pos(bubble->speaker, epd_ui_px(44),
                    (epd_ui_px(40) - lv_font_get_line_height(epd_app_font_role(EPD_FONT_CAPTION))) / 2);
            }
        }
        if (created || bubble->revision != meta.revision || bubble->length != meta.length || page->force) {
            char *text = epd_app_alloc(meta.length + 1, EPD_APP_PSRAM);
            demo_message_meta_t copied;
            if (!text) { ok = false; break; }
            bool read = demo_message(i, &copied, 0, text, meta.length + 1);
            if (!read || copied.revision != meta.revision || copied.length != meta.length) {
                epd_app_free(text); ok = false; break;
            }
            const char *display = meta.length ? text :
                page->view.generating && !meta.user ? epd_get_str(ai_demo_thinking) : "";
            bool painted = meta.user ? epd_app_text(bubble->text, display) :
                chat_markdown_update(bubble->markdown, display, strlen(display), width - pad * 2);
            epd_app_free(text);
            if (!painted) { ok = false; break; }
            bubble->length = meta.length; bubble->revision = meta.revision;
        }
        if (meta.user) {
            lv_point_t text_size;
            lv_text_get_size(&text_size, lv_label_get_text(bubble->text), epd_app_font_role(EPD_FONT_BODY),
                0, epd_ui_px(8), width - pad * 2, LV_TEXT_FLAG_NONE);
            width = LV_MIN(width, LV_MAX(epd_ui_touch(64), text_size.x + pad * 2));
            lv_obj_set_size(bubble->text, width - pad * 2, LV_SIZE_CONTENT);
        }
        lv_obj_update_layout(bubble->text);
        int height = LV_MAX(epd_ui_px(32), lv_obj_get_height(bubble->text)) + text_top + pad;
        lv_obj_set_pos(bubble->box, meta.user ? page->width - width : 0, y);
        lv_obj_set_size(bubble->box, width, height);
        if (i == failed_user) height += failed_message_notice(page, y, width, height);
        y += height + gap;
    }
    if (failed && failed_user == DEMO_MAX_MESSAGES)
        y += failed_message_notice(page, y, page->width * 72 / 100, epd_ui_touch(64)) + epd_ui_touch(64) + gap;
    if (!failed) {
        const char *status = page->view.error == EPD_AI_CANCELLED ? error_text(&page->view) :
            page->view.search_failed ? epd_get_str(ai_demo_search_failed) :
            page->view.searching ? epd_get_str(ai_demo_searching) : "";
        int height = inline_notice(page->notice, status, 0, y, page->width, LV_TEXT_ALIGN_LEFT);
        if (height) y += height + gap;
        char speech_text[128]; speech_notice_text(&page->view, speech_text, sizeof(speech_text));
        inline_notice(page->speech_notice, speech_text, 0, y, page->width, LV_TEXT_ALIGN_LEFT);
    }
    lv_obj_update_layout(page->body);
    page->rebuilding = false;
    if (follow) scroll_latest(page);
    else {
        page->scrolling_programmatically = true;
        lv_obj_scroll_to_y(page->body, old_scroll, LV_ANIM_OFF);
        page->scrolling_programmatically = false;
    }
    return ok;
}
static void session_click(lv_event_t *event)
{
    if (!page_binding) return;
    page_binding->target_id = (demo_id_t)(uintptr_t)lv_event_get_user_data(event);
    page_binding->action = A_OPEN;
}
static void session_menu(lv_event_t *event)
{
    if (!page_binding) return;
    page_binding->target_id = (demo_id_t)(uintptr_t)lv_event_get_user_data(event);
    page_binding->action = A_MENU;
}
static void model_click(lv_event_t *event)
{
    demo_page_t *page = page_binding;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (!page || index >= page->choice_count) return;
    snprintf(page->selected_profile, sizeof(page->selected_profile), "%s", page->choices[index].profile);
    snprintf(page->selected_model, sizeof(page->selected_model), "%s", page->choices[index].model);
    page->action = A_SELECT;
}
static bool rebuild_list(demo_page_t *page)
{
    int old_scroll = lv_obj_get_scroll_y(page->body);
    int y = 0, row = epd_ui_touch(92), gap = epd_ui_px(12), width = page->width;
    if (page->mode == HISTORY) {
        demo_summary_t *items = epd_app_alloc(sizeof(*items) * DEMO_MAX_SESSIONS, EPD_APP_PSRAM);
        size_t count = 0;
        if (!items || !demo_list(items, DEMO_MAX_SESSIONS, &count)) {
            epd_app_free(items); return false;
        }
        clear_body(page);
        if (page->view.error == EPD_AI_STORAGE) {
            lv_obj_t *error = label(page->body, error_text(&page->view), width);
            lv_obj_set_style_text_font(error, epd_app_font_role(EPD_FONT_SMALL), 0);
            lv_obj_update_layout(error); y += lv_obj_get_height(error) + gap;
            lv_obj_t *save = button(page->body, epd_get_str(ai_demo_save), A_SAVE, true);
            place_button(save, 0, y, width, row, NULL); y += row + gap;
        }
        for (size_t i = 0; i < count && i < DEMO_MAX_SESSIONS; ++i) {
            int item_height = epd_ui_touch(112), pad = epd_ui_px(20);
            lv_obj_t *item = lv_obj_create(page->body);
            epd_app_object_init(item); white(item);
            lv_obj_set_pos(item, 0, y); lv_obj_set_size(item, width, item_height);
            lv_obj_set_style_radius(item, epd_ui_px(24), 0);
            lv_color_t background = lv_color_hex(items[i].id == page->view.current_id ? 0xf0f0f0 : 0xffffff);
            lv_obj_set_style_bg_color(item, background, 0);
            lv_obj_t *entry = button(item, "", A_NONE, false);
            place_button(entry, pad, 0, width - row - pad, item_height,
                items[i].title[0] ? items[i].title : epd_get_str(ai_demo_new));
            lv_obj_set_style_bg_opa(entry, LV_OPA_TRANSP, 0);
            lv_obj_t *caption = lv_obj_get_child(entry, 0);
            lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_style_text_font(caption, epd_app_font_role(EPD_FONT_BODY), 0);
            lv_obj_add_event_cb(entry, session_click, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)items[i].id);
            lv_obj_add_event_cb(entry, session_menu, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)items[i].id);
            lv_obj_t *more = button(item, "···", A_NONE, false);
            place_button(more, width - row, 0, row, item_height, NULL);
            lv_obj_set_style_bg_opa(more, LV_OPA_TRANSP, 0);
            lv_obj_add_event_cb(more, session_menu, LV_EVENT_CLICKED, (void *)(uintptr_t)items[i].id);
            y += item_height + gap;
            if (i + 1 < count) {
                lv_obj_t *line = lv_obj_create(page->body);
                epd_app_object_init(line);
                lv_obj_set_style_bg_color(line, lv_color_hex(0xe5e5e5), 0);
                lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
                lv_obj_set_pos(line, pad, y); lv_obj_set_size(line, width - pad * 2, 1);
                y += gap;
            }
        }
        epd_app_free(items);
        if (!count) label(page->body, epd_get_str(ai_demo_empty_history), width);
    } else {
        epd_ai_profile_t *profiles = epd_app_alloc(sizeof(*profiles) * EPD_AI_MAX_PROFILES, EPD_APP_PSRAM);
        size_t count = 0;
        if (!profiles || !demo_profiles(profiles, EPD_AI_MAX_PROFILES, &count)) {
            epd_app_free(profiles); return false;
        }
        clear_body(page);
        page->choice_count = 0;
        {
            for (size_t i = 0; i < count && i < EPD_AI_MAX_PROFILES; ++i) {
                lv_obj_t *heading = label(page->body, profiles[i].name, width);
                lv_obj_set_style_text_font(heading, epd_app_font_role(EPD_FONT_SMALL), 0);
                lv_obj_set_style_text_color(heading, lv_color_hex(0x666666), 0);
                lv_obj_set_pos(heading, 0, y);
                y += epd_ui_touch(56);
                for (size_t j = 0; j < profiles[i].model_count && j < EPD_AI_MAX_MODELS; ++j) {
                    if (!(profiles[i].models[j].capabilities & EPD_AI_CAP_TEXT)) continue;
                    size_t index = page->choice_count++;
                    snprintf(page->choices[index].profile, EPD_AI_ID_SIZE, "%s", profiles[i].id);
                    snprintf(page->choices[index].model, EPD_AI_MODEL_SIZE, "%s", profiles[i].models[j].id);
                    lv_obj_t *entry = button(page->body, "", A_NONE, true);
                    place_button(entry, 0, y, width, row, profiles[i].models[j].id);
                    lv_obj_add_event_cb(entry, model_click, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
                    y += row + gap;
                }
                y += gap;
            }
        }
        epd_app_free(profiles);
        lv_obj_t *grant = button(page->body, epd_get_str(ai_demo_grant), A_GRANT, true);
        place_button(grant, 0, y, width, row, NULL);
    }
    lv_obj_update_layout(page->body);
    lv_obj_scroll_to_y(page->body, old_scroll, LV_ANIM_OFF);
    return true;
}

static void dialog_close(demo_page_t *page)
{
    if (page->rename_input) keyboard_close(page);
    page->rename_input = NULL;
    if (page->overlay) lv_obj_delete(page->overlay);
    page->overlay = page->dialog = NULL;
}
static void dialog_create(demo_page_t *page, const char *title, int rows)
{
    dialog_close(page);
    page->overlay = lv_obj_create(page->screen);
    epd_app_object_init(page->overlay); white(page->overlay);
    lv_obj_set_pos(page->overlay, 0, epd_ui_px(64));
    lv_obj_set_size(page->overlay, lv_obj_get_width(page->screen), page->area.y2 + 1 - epd_ui_px(64));
    lv_obj_add_flag(page->overlay, LV_OBJ_FLAG_CLICKABLE);
    epd_app_input_group(page->overlay);
    page->dialog = lv_obj_create(page->overlay);
    epd_app_object_init(page->dialog); white(page->dialog);
    int gap = epd_ui_px(20), row = epd_ui_touch(72);
    int width = page->width;
    lv_obj_set_pos(page->dialog, page->area.x1, epd_ui_px(100));
    lv_obj_set_size(page->dialog, width, gap * 3 + row * (rows + 1));
    lv_obj_set_style_radius(page->dialog, epd_ui_px(25), 0);
    lv_obj_set_style_border_width(page->dialog, 2, 0);
    lv_obj_set_style_border_color(page->dialog, lv_color_hex(0x828282), 0);
    lv_obj_t *heading = label(page->dialog, title, width - gap * 2);
    lv_obj_set_pos(heading, gap, gap);
    lv_obj_set_style_text_font(heading, epd_app_font_role(EPD_FONT_BODY), 0);
}
static void dialog_action(demo_page_t *page, const char *text, chat_action_t action, int index)
{
    int gap = epd_ui_px(20), row = epd_ui_touch(72);
    lv_obj_t *entry = button(page->dialog, text, action, false);
    place_button(entry, gap, gap * 2 + row * (index + 1), page->width - gap * 2, row, NULL);
}
static void open_source(const char *url)
{
    demo_page_t *page = page_binding;
    if (!page || page->stopped) return;
    keyboard_close(page);
    layout(page);
    dialog_create(page, epd_get_str(ai_demo_source), 6);
    int pad = epd_ui_px(20), size = LV_MIN(epd_ui_px(320), page->width - pad * 2);
    lv_obj_set_y(page->dialog, pad);
    lv_obj_set_height(page->dialog, LV_MIN(size + epd_ui_px(250), page->area.y2 - page->area.y1));
    lv_obj_t *qr = lv_qrcode_create(page->dialog);
    if (qr) {
        lv_qrcode_set_size(qr, size);
        lv_obj_set_pos(qr, (page->width - size) / 2, epd_ui_px(85));
        if (lv_qrcode_update(qr, url, strlen(url)) != LV_RESULT_OK) show(qr, false);
    }
    lv_obj_t *address = label(page->dialog, url, page->width - pad * 2);
    lv_obj_set_pos(address, pad, size + epd_ui_px(100));
    lv_obj_set_height(address, epd_ui_px(60));
    lv_label_set_long_mode(address, LV_LABEL_LONG_DOT);
    lv_obj_t *close = button(page->dialog, epd_get_str(ai_demo_done), A_DIALOG_CLOSE, false);
    place_button(close, pad, size + epd_ui_px(172), page->width - pad * 2, epd_ui_touch(56), NULL);
    page->force = true;
}
static bool session_title(demo_id_t id, char *title, size_t capacity)
{
    demo_summary_t *items = epd_app_alloc(sizeof(*items) * DEMO_MAX_SESSIONS, EPD_APP_PSRAM);
    size_t count = 0;
    bool found = false;
    if (items && demo_list(items, DEMO_MAX_SESSIONS, &count)) {
        for (size_t i = 0; i < count && i < DEMO_MAX_SESSIONS; ++i)
            if (items[i].id == id) {
                snprintf(title, capacity, "%s", items[i].title);
                found = true; break;
            }
    }
    epd_app_free(items); return found;
}
static void change_mode(demo_page_t *page, chat_page_kind_t mode)
{
    keyboard_close(page); dialog_close(page); clear_body(page);
    page->mode = mode; page->following = true;
    page->force = true; layout(page);
}
static void act(demo_page_t *page)
{
    chat_action_t action = page->action;
    page->action = A_NONE;
    switch (action) {
    case A_AUTO_READ: demo_set_auto_read(!page->view.auto_read); break;
    case A_SEARCH: demo_set_web_search(!page->view.web_search); break;
    case A_BACK:
        if (page->overlay) dialog_close(page);
        else if (page->keyboard_open) keyboard_close(page);
        else if (page->mode != CHAT) change_mode(page, CHAT);
        else { gui_app_goback(); return; }
        break;
    case A_HISTORY: change_mode(page, HISTORY); break;
    case A_MODELS: change_mode(page, MODELS); page->refresh_profiles = true; break;
    case A_NEW:
        if (demo_submit(DEMO_NEW)) change_mode(page, CHAT);
        break;
    case A_SEND:
        if (page->view.generating) demo_cancel();
        else if (page->view.busy) break;
        else if (page->view.profile_id[0] && page->view.model[0]) {
            if (demo_send(lv_textarea_get_text(page->input))) {
                set_input(page, ""); page->draft_pending = false;
                keyboard_close(page); page->following = true;
            }
        } else change_mode(page, MODELS);
        break;
    case A_RETRY:
        if (page->view.error == EPD_AI_STORAGE) demo_submit(DEMO_SAVE);
        else if (page->view.error == EPD_AI_UNAUTHORIZED || page->view.error == EPD_AI_NOT_FOUND ||
                 service_changed(&page->view)) change_mode(page, MODELS);
        else { demo_submit(DEMO_RETRY); page->following = true; }
        break;
    case A_SAVE: demo_submit(DEMO_SAVE); break;
    case A_LATEST: scroll_latest(page); break;
    case A_OPEN:
        if (demo_open(page->target_id)) change_mode(page, CHAT);
        break;
    case A_MENU: {
        char title[DEMO_TITLE_SIZE];
        if (!session_title(page->target_id, title, sizeof(title))) break;
        dialog_create(page, title[0] ? title : epd_get_str(ai_demo_new), 3);
        dialog_action(page, epd_get_str(ai_demo_rename), A_RENAME, 0);
        dialog_action(page, epd_get_str(ai_demo_delete), A_DELETE, 1);
        dialog_action(page, epd_get_str(ai_demo_cancel), A_DIALOG_CLOSE, 2);
        return;
    }
    case A_RENAME: {
        char title[DEMO_TITLE_SIZE];
        if (!session_title(page->target_id, title, sizeof(title))) break;
        dialog_create(page, epd_get_str(ai_demo_rename), 3);
        page->rename_input = lv_textarea_create(page->dialog);
        textarea_style(page->rename_input); lv_textarea_set_one_line(page->rename_input, true);
        lv_obj_add_event_cb(page->rename_input, text_limit, LV_EVENT_INSERT, (void *)(uintptr_t)DEMO_TITLE_SIZE);
        lv_obj_add_event_cb(page->rename_input, rename_clicked, LV_EVENT_CLICKED, page);
        size_t length = 0;
        for (size_t i = 0; title[i]; ++i)
            if (title[i] != '\n' && title[i] != '\r') title[length++] = title[i];
        title[length] = 0;
        lv_textarea_set_text(page->rename_input, title);
        lv_obj_set_pos(page->rename_input, epd_ui_px(20), epd_ui_touch(72) + epd_ui_px(20));
        lv_obj_set_size(page->rename_input, page->width - epd_ui_px(40), epd_ui_touch(72));
        dialog_action(page, epd_get_str(ai_demo_done), A_RENAME_SAVE, 1);
        dialog_action(page, epd_get_str(ai_demo_cancel), A_DIALOG_CLOSE, 2);
        keyboard_open(page);
        break;
    }
    case A_RENAME_SAVE:
        if (page->rename_input && demo_rename(page->target_id, lv_textarea_get_text(page->rename_input)))
            dialog_close(page);
        break;
    case A_DELETE:
        dialog_create(page, epd_get_str(ai_demo_delete_question), 2);
        dialog_action(page, epd_get_str(ai_demo_confirm), A_CONFIRM_DELETE, 0);
        dialog_action(page, epd_get_str(ai_demo_cancel), A_DIALOG_CLOSE, 1);
        return;
    case A_CONFIRM_DELETE:
        if (demo_delete(page->target_id)) dialog_close(page);
        break;
    case A_DIALOG_CLOSE: {
        bool resize = page->keyboard_open;
        dialog_close(page);
        if (!resize) return;
        break;
    }
    case A_GRANT: {
        epd_service_t *owner = demo_owner();
        if (owner && epd_ai_select_service(owner)) page->refresh_profiles = true;
        break;
    }
    case A_SELECT:
        if (page->view.message_count) {
            if (!strcmp(page->selected_profile, page->view.profile_id) &&
                !strcmp(page->selected_model, page->view.model)) change_mode(page, CHAT);
            else {
                dialog_create(page, epd_get_str(ai_demo_select_new), 2);
                dialog_action(page, epd_get_str(ai_demo_new), A_SELECT_NEW, 0);
                dialog_action(page, epd_get_str(ai_demo_cancel), A_DIALOG_CLOSE, 1);
            }
        } else if (demo_select(page->selected_profile, page->selected_model)) change_mode(page, CHAT);
        break;
    case A_SELECT_NEW:
        if (demo_new_with(page->selected_profile, page->selected_model)) change_mode(page, CHAT);
        break;
    case A_KEYBOARD: if (!page->keyboard_open) keyboard_open(page); break;
    case A_VOICE:
        keyboard_open(page);
        if (page->keyboard_root) epd_keyboard_open_voice(page->keyboard_root);
        break;
    case A_HIDE_KEYBOARD: keyboard_close(page); break;
    default: return;
    }
    layout(page); page->force = true;
}
static void poll(lv_timer_t *timer)
{
    demo_page_t *page = lv_timer_get_user_data(timer);
    if (page->stopped) return;
    if (!demo_start()) return;
    if (page->draft_pending) save_input(page);
    bool changes_session = page->action == A_NEW || page->action == A_OPEN ||
                           page->action == A_SELECT || page->action == A_SELECT_NEW ||
                           page->action == A_RENAME_SAVE;
    if (page->action != A_NONE && (!page->draft_pending || !changes_session)) {
        act(page);
        if (page_binding != page) return;
    }
    if (page->refresh_profiles && demo_submit(DEMO_REFRESH)) page->refresh_profiles = false;
    if (!epd_app_refresh_done()) return;
    demo_view_t view;
    if (!demo_snapshot(&view)) return;
    bool switched = page->current_id != view.current_id;
    bool terminal = page->view.busy != view.busy || page->view.generating != view.generating || page->view.error != view.error ||
                    memcmp(&page->view.failure, &view.failure, sizeof(view.failure));
    if (switched) {
        page->current_id = view.current_id;
        clear_body(page); page->following = true; page->force = true;
        set_input(page, view.draft); page->draft_pending = false;
    }
    else if (!view.busy && !page->draft_pending &&
             strcmp(lv_textarea_get_text(page->input), view.draft)) {
        set_input(page, view.draft);
    }
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    uint32_t now = rt_tick_get();
    bool speech_controls = view.auto_read != page->view.auto_read ||
        view.speech.output_active != page->view.speech.output_active ||
        view.speech.output.state != page->view.speech.output.state ||
        view.speech.output_error != page->view.speech.output_error ||
        view.web_search != page->view.web_search || view.search_available != page->view.search_available ||
        view.searching != page->view.searching || view.search_failed != page->view.search_failed;
    bool changed_layout = page->scale != epd_ui_scale_revision() ||
        memcmp(&area, &page->area, sizeof(area)) || page->locale != epd_locale_revision() || terminal ||
        speech_controls || strcmp(page->view.model, view.model);
    bool first_message = !!view.message_count != !!page->view.message_count;
    page->view = view;
    if (changed_layout) { layout(page); page->locale = epd_locale_revision(); }
    else if (first_message) composer_state(page);
    if (page->paint_failed && !switched &&
        (rt_tick_t)(now - page->failed_at) < rt_tick_from_millisecond(1000)) return;
    if (!page->force && page->revision == view.revision) return;
    if (!page->force && !terminal && view.busy &&
        (rt_tick_t)(now - page->painted) < rt_tick_from_millisecond(1000)) return;
    enabled(page->new_chat, !view.busy);
    enabled(page->history, !view.busy);
    lv_image_set_src(lv_obj_get_child(page->new_chat, 1), chat_icon(CHAT_ICON_NEW, epd_ui_scale_percent(), !view.busy));
    lv_image_set_src(lv_obj_get_child(page->history, 1), chat_icon(CHAT_ICON_SESSIONS, epd_ui_scale_percent(), !view.busy));
    enabled(page->model, !view.busy);
    enabled(page->search, !view.busy && view.search_available);
    if (page->mode == CHAT) {
        model_caption(page);
        if (!render_messages(page)) {
            page->paint_failed = true; page->failed_at = now; page->force = true; return;
        }
    } else if ((page->force || !view.busy) && !rebuild_list(page)) {
        page->paint_failed = true; page->failed_at = now; page->force = true; return;
    }
    page->paint_failed = false;
    page->revision = view.revision; page->painted = now; page->force = false;
}
bool demo_ui_present(void) { return page_binding != NULL; }

bool demo_ui_flush(void)
{
    demo_page_t *page = page_binding;
    return !page || !page->draft_pending || save_input(page);
}

void demo_ui_release(void)
{
    demo_page_t *page = page_binding;
    if (!page) return;
    if (page->timer) lv_timer_delete(page->timer);
    page->timer = NULL; page->stopped = true;
    keyboard_close(page); dialog_close(page);
    lv_image_set_src(lv_obj_get_child(page->history, 1), NULL);
    lv_image_set_src(lv_obj_get_child(page->new_chat, 1), NULL);
    lv_image_set_src(lv_obj_get_child(page->auto_read, 1), NULL);
    lv_image_set_src(lv_obj_get_child(page->model, 1), NULL);
    lv_image_set_src(lv_obj_get_child(page->search, 1), NULL);
    lv_image_set_src(lv_obj_get_child(page->voice, 1), NULL);
    lv_image_set_src(lv_obj_get_child(page->send, 1), NULL);
    if (page->retry) lv_image_set_src(lv_obj_get_child(page->retry, 1), NULL);
    if (page->empty_icon) lv_image_set_src(page->empty_icon, NULL);
    for (size_t i = 0; i < page->bubble_count; ++i)
        if (page->bubbles[i].mark) lv_image_set_src(page->bubbles[i].mark, NULL);
    chat_icons_release();
    clear_body(page);
    page->notice = label(page->body, epd_get_str(ai_demo_stopped), page->width);
    enabled(page->send, false); page_binding = NULL;
}
static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    demo_page_t **slot = gui_app_this_page_memory();
    demo_page_t *page = *slot;
    if (message == GUI_APP_MSG_ONSTART) {
        page = epd_app_alloc(sizeof(*page), EPD_APP_PSRAM);
        if (!page) return;
        memset(page, 0, sizeof(*page)); *slot = page;
        page_binding = page; page->screen = lv_screen_active();
        chat_markdown_set_link_handler(open_source);
        gui_app_close_anim(); epd_app_object_init(page->screen); white(page->screen);
        epd_app_input_group(page->screen); epd_app_header(page->screen);
        page->back = epd_app_back_button_cb(page->screen, clicked, (void *)(uintptr_t)A_BACK);
        page->title = label(page->screen, epd_get_str(ai_demo_title), 300);
        page->history = icon_button(page->screen, A_HISTORY, CHAT_ICON_SESSIONS);
        page->new_chat = icon_button(page->screen, A_NEW, CHAT_ICON_NEW);
        page->header_line = lv_obj_create(page->screen); epd_app_object_init(page->header_line);
        lv_obj_set_style_bg_color(page->header_line, lv_color_hex(0xdddddd), 0);
        lv_obj_set_style_bg_opa(page->header_line, LV_OPA_COVER, 0);
        page->body = lv_obj_create(page->screen); epd_app_object_init(page->body); white(page->body);
        lv_obj_add_flag(page->body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(page->body, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_set_scroll_dir(page->body, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(page->body, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(page->body, user_scroll, LV_EVENT_SCROLL, page);
        page->composer = lv_obj_create(page->screen); epd_app_object_init(page->composer); white(page->composer);
        lv_obj_set_style_border_width(page->composer, 1, 0);
        lv_obj_set_style_border_color(page->composer, lv_color_hex(0x888888), 0);
        page->model = icon_button(page->composer, A_MODELS, CHAT_ICON_CHEVRON);
        lv_obj_set_style_border_width(page->model, 1, 0);
        lv_obj_set_style_border_color(page->model, lv_color_hex(0xbbbbbb), 0);
        page->search = icon_button(page->composer, A_SEARCH, CHAT_ICON_GLOBE);
        lv_obj_set_style_border_width(page->search, 1, 0);
        page->input = lv_textarea_create(page->composer); textarea_style(page->input);
        lv_obj_set_style_border_width(page->input, 0, 0);
        lv_obj_set_style_radius(page->input, 0, 0);
        lv_obj_set_style_border_opa(page->input, LV_OPA_TRANSP, LV_PART_CURSOR);
        lv_obj_add_event_cb(page->input, text_limit, LV_EVENT_INSERT, (void *)(uintptr_t)DEMO_DRAFT_SIZE);
        lv_obj_add_event_cb(page->input, input_changed, LV_EVENT_VALUE_CHANGED, page);
        lv_obj_add_event_cb(page->input, input_changed, LV_EVENT_CLICKED, page);
        page->voice = icon_button(page->composer, A_VOICE, CHAT_ICON_MICROPHONE);
        page->send = icon_button(page->composer, A_SEND, CHAT_ICON_SEND);
        page->latest = button(page->screen, epd_get_str(ai_demo_latest), A_LATEST, true);
        page->auto_read = icon_button(page->screen, A_AUTO_READ, CHAT_ICON_SPEAKER);
        page->following = true;
        page->force = page->refresh_profiles = true;
        layout(page);
        page->timer = lv_timer_create(poll, 100, page);
        if (page->timer) lv_timer_pause(page->timer);
    } else if (!page) return;
    else if (message == GUI_APP_MSG_ONRESUME) {
        epd_app_header_refresh(page->screen); page_binding = page;
        page->force = page->refresh_profiles = true;
        if (page->timer) { lv_timer_resume(page->timer); poll(page->timer); }
    } else if (message == GUI_APP_MSG_ONPAUSE) {
        page->action = A_NONE;
        if (page->draft_pending) save_input(page);
        if (page->timer) lv_timer_pause(page->timer);
    } else if (message == GUI_APP_MSG_ONSTOP) {
        if (page->draft_pending) save_input(page);
        if (page->timer) lv_timer_delete(page->timer);
        page->timer = NULL; keyboard_close(page); dialog_close(page);
        if (page_binding == page) page_binding = NULL;
        chat_markdown_set_link_handler(NULL);
        chat_icons_release();
        *slot = NULL; epd_app_free(page);
        demo_ui_closed();
    }
}
int app_main(intent_t intent)
{
    (void)intent;
    if (!demo_start() && !app_service_running("ai_demo")) return -RT_ENOMEM;
    int result = gui_app_regist_msg_handler_ext("ai_demo", page_message, NULL, sizeof(demo_page_t *));
    if (result != RT_EOK) demo_ui_closed();
    return result;
}
