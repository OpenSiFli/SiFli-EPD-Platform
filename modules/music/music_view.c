#include "storage.h"
#include "epd_memory.h"
#include "platform/app_content.h"
#include "platform/epd_keyboard.h"
#include "platform/epd_locale.h"
#include "music_service.h"
#include "music_cover.h"
#include "music_icons.h"
#include "platform/epd_i18n.h"
#include "src/draw/lv_image_decoder_private.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define MUSIC_TEXTS(X) \
    X(title) X(files) X(favorites) X(playlists) X(queue) X(now) X(sd) X(flash) \
    X(search) X(reset) \
    X(previous_page) X(next_page) X(loading) X(empty) X(saving) \
    X(saved) X(unavailable) X(busy) X(insert_next) X(append) X(favorite) \
    X(unfavorite) X(remove) X(up) X(down) X(clear) X(save) X(import) X(name) \
    X(invalid_name) X(too_long) \
    X(confirm) X(cancel) X(previous) X(play) X(pause) X(next) X(stop) X(no_track) \
    X(opening) \
    X(seeking) X(suspended) X(mode) X(timer) X(mode_sequence) \
    X(mode_list) X(mode_one) X(mode_shuffle) X(timer_off) X(timer_15) X(timer_30) \
    X(timer_60) X(timer_track) X(err_memory) X(err_storage) X(err_format) \
    X(err_open) X(err_decode) X(err_output) X(err_save) X(err_path) X(err_limit) \
    X(more) X(details) X(choose_playlist) X(clear_confirm) X(volume_title)
#define TEXT_ENUM(key) T_##key,
typedef enum { T_none, MUSIC_TEXTS(TEXT_ENUM) } text_id_t;
#undef TEXT_ENUM

typedef enum { VIEW_HOME, VIEW_FILES, VIEW_FAVORITES, VIEW_PLAYLISTS,
               VIEW_QUEUE, VIEW_NOW } view_kind_t;
typedef enum { DIALOG_NONE, DIALOG_ROW, DIALOG_SEARCH, DIALOG_SAVE,
               DIALOG_MODE, DIALOG_TIMER, DIALOG_MORE, DIALOG_DETAILS, DIALOG_VOLUME, DIALOG_CLEAR } dialog_kind_t;
typedef enum { A_VIEW, A_SD, A_FLASH, A_SEARCH, A_RESET, A_PAGE_PREV, A_PAGE_NEXT,
    A_SAVE, A_IMPORT, A_CLEAR, A_PREV, A_PLAY, A_NEXT, A_STOP, A_MODE, A_TIMER,
    A_CANCEL, A_CONFIRM, A_INSERT, A_APPEND, A_FAVORITE, A_REMOVE, A_UP, A_DOWN,
    A_SET_MODE, A_SET_TIMER, A_VOLUME_DOWN, A_VOLUME_UP,
    A_MORE, A_DETAILS, A_VOLUME, A_ROW_MENU, A_CLEAR_CONFIRM } action_t;

typedef struct music_page music_page_t;
typedef struct {
    music_page_t *page;
    lv_obj_t *object, *label, *icon, *marker;
    music_icon_id_t icon_id;
    bool icon_only;
    text_id_t text;
    action_t action;
    int value;
} button_t;
struct music_page {
    lv_obj_t *screen, *back, *body, *heading, *status, *breadcrumb_bar, *breadcrumb, *page_number;
    lv_obj_t *rows[MUSIC_PAGE_ROWS], *row_labels[MUSIC_PAGE_ROWS], *row_icons[MUSIC_PAGE_ROWS];
    lv_obj_t *mini, *mini_title, *mini_artist, *mini_rule, *cover_frame, *placeholder, *duration;
    lv_obj_t *dialog_card, *details, *volume_slider, *volume_rail, *progress_rail;
    bool picking_playlist, volume_dragging, layout_dirty, mini_shown;
    lv_obj_t *track, *artist, *cover, *elapsed, *progress, *volume_label;
    lv_obj_t *dialog, *dialog_title, *dialog_hint, *dialog_status, *input, *keyboard;
    button_t buttons[48];
    unsigned button_count, base_buttons;
    lv_timer_t *timer;
    music_snapshot_t snapshot;
    music_list_entry_t displayed[MUSIC_PAGE_ROWS], selected;
    music_command_t query;
    view_kind_t view, return_view;
    dialog_kind_t dialog_kind;
    text_id_t local_status, input_status;
    uint32_t locale_revision, scale_revision, rendered_revision, rendered_list, rendered_result;
    uint32_t requested_list, request_result, operation_result, progress_tick, seek_id, seek_duration, list_tick;
    uint32_t shown_id, shown_duration, layout_track_id;
    uint32_t cover_revision, cover_id;
    music_cover_image_t cover_image;
    lv_image_dsc_t cover_descriptor;
    music_state_t shown_state;
    size_t count, total, selected_index;
    bool foreground, stopped, force, text_retry, list_pending, operation_pending, import_pending, save_pending;
    bool shown_estimated;
    bool seek_dragging, cover_checked, folder_initialized;
    rt_err_t content_error;
    char folder[STORAGE_PATH_MAX], launch_path[STORAGE_PATH_MAX];
};

static music_page_t *current_page;
static char incoming_path[STORAGE_PATH_MAX];
static rt_err_t incoming_error;
static void render(music_page_t *page);
static void show_view(music_page_t *page, view_kind_t view);
static void action_event(lv_event_t *event);
static void dialog_close(music_page_t *page);
static void input_confirm(music_page_t *page);
static bool is_list(view_kind_t view);
static bool is_m3u8(const char *path);
static void volume_event(lv_event_t *event);

static const char *text(text_id_t id)
{
    switch (id) {
#define TEXT_CASE(key) case T_##key: return epd_get_str(music_##key);
    MUSIC_TEXTS(TEXT_CASE)
#undef TEXT_CASE
    default: return "";
    }
}

static bool set_text(music_page_t *page, lv_obj_t *label, const char *value)
{
    bool ok = label && epd_app_text(label, value);
    if (label && !ok) page->text_retry = true;
    return ok;
}

static void visible(lv_obj_t *object, bool show)
{
    if (!object) return;
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void enabled(lv_obj_t *object, bool enable)
{
    if (enable) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *object = epd_app_label(parent, "", x, y, width, epd_app_font_role(EPD_FONT_SMALL));
    lv_obj_set_height(object, height);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

static lv_obj_t *surface(lv_obj_t *parent)
{
    lv_obj_t *object = epd_app_panel(parent, 0, 164, 684, 1020);
    lv_obj_set_style_bg_color(object, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_left(object, 0, 0);
    lv_obj_set_style_pad_top(object, 0, 0);
    lv_obj_set_style_pad_right(object, 0, 0);
    lv_obj_set_style_pad_bottom(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static button_t *button(music_page_t *page, lv_obj_t *parent, int x, int y,
                        int width, int height, text_id_t title, action_t action, int value)
{
    RT_ASSERT(page->button_count < sizeof(page->buttons) / sizeof(page->buttons[0]));
    button_t *item = &page->buttons[page->button_count++];
    *item = (button_t){.page = page, .text = title, .action = action, .value = value};
    item->object = epd_app_button(parent, x, y, width, height, "", action_event, item);
    item->label = lv_obj_get_child(item->object, 0);
    lv_obj_set_style_text_font(item->label, epd_app_font_role(EPD_FONT_SMALL), 0);
    lv_label_set_long_mode(item->label, LV_LABEL_LONG_DOT);
    return item;
}

static void plain_button(lv_obj_t *object)
{
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(object, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(object, 0, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(object, 0, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_left(object, 0, 0);
    lv_obj_set_style_pad_right(object, 0, 0);
    lv_obj_set_style_pad_top(object, 0, 0);
    lv_obj_set_style_pad_bottom(object, 0, 0);
    lv_obj_set_style_anim_duration(object, 0, 0);
}

static lv_obj_t *icon(lv_obj_t *parent, music_icon_id_t id)
{
    lv_obj_t *object = lv_image_create(parent);
    epd_app_object_init(object);
    lv_image_set_src(object, music_icon(id, epd_ui_scale_percent()));
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static button_t *icon_button(music_page_t *page, lv_obj_t *parent, text_id_t title,
                             action_t action, int value, music_icon_id_t id, bool icon_only)
{
    button_t *item = button(page, parent, 0, 0, 64, 64, title, action, value);
    plain_button(item->object);
    item->icon_id = id;
    item->icon_only = icon_only;
    item->icon = icon(item->object, id);
    if (icon_only && (action == A_MODE || action == A_TIMER)) {
        item->marker = surface(item->object);
        lv_obj_set_style_bg_color(item->marker, lv_color_black(), 0);
        lv_obj_set_style_radius(item->marker, LV_RADIUS_CIRCLE, 0);
    }
    visible(item->label, !icon_only);
    return item;
}

static button_t *base_action(music_page_t *page, action_t action, int value)
{
    for (unsigned i = 0; i < page->base_buttons; ++i)
        if (page->buttons[i].action == action && page->buttons[i].value == value) return &page->buttons[i];
    return NULL;
}

static void change_icon(button_t *item, music_icon_id_t id)
{
    if (item->icon && item->icon_id != id) {
        item->icon_id = id;
        lv_image_set_src(item->icon, music_icon(id, epd_ui_scale_percent()));
        lv_obj_center(item->icon);
    }
}

static void box(lv_obj_t *object, int x, int y, int width, int height)
{
    lv_obj_align(object, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_size(object, width, height);
}

static int line_height(epd_font_role_t role)
{
    return lv_font_get_line_height(epd_app_font_role(role)) + epd_ui_px(4);
}

static void label_box(lv_obj_t *object, int x, int y, int width, int height, epd_font_role_t role)
{
    box(object, x, y, width, height);
    lv_obj_set_style_text_font(object, epd_app_font_role(role), 0);
}

static void button_box(button_t *item, int x, int y, int width, int height)
{
    box(item->object, x, y, width, height);
    if (item->action == A_PLAY && item->page->view == VIEW_NOW) {
        lv_obj_set_style_border_width(item->object, epd_ui_px(2), 0);
        lv_obj_set_style_border_width(item->object, epd_ui_px(2), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(item->object, lv_color_black(), 0);
        lv_obj_set_style_radius(item->object, width / 2, 0);
    }
    int left = epd_ui_px(8);
    if (item->icon) {
        const lv_image_dsc_t *image = music_icon(item->icon_id, epd_ui_scale_percent());
        lv_image_set_src(item->icon, image);
        lv_obj_center(item->icon);
        if (item->icon_only) {
            if (item->marker)
                box(item->marker, (width - epd_ui_px(6)) / 2, height - epd_ui_px(8), epd_ui_px(6), epd_ui_px(6));
            return;
        }
        lv_obj_align(item->icon, LV_ALIGN_LEFT_MID, epd_ui_px(12), 0);
        left = epd_ui_px(12) + image->header.w + epd_ui_px(16);
    }
    int text_width = width - left - epd_ui_px(8);
    label_box(item->label, left, (height - line_height(EPD_FONT_BODY)) / 2,
              text_width, line_height(EPD_FONT_BODY), EPD_FONT_BODY);
    lv_obj_set_style_text_align(item->label, item->icon ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
}

static void action_box(music_page_t *page, action_t action, int value, int x, int y, int width, int height)
{
    button_t *item = base_action(page, action, value);
    if (item) button_box(item, x, y, width, height);
}

static void layout(music_page_t *page)
{
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    int width = area.x2 - area.x1 + 1, height = area.y2 - area.y1 + 1;
    int gap = epd_ui_px(12), line = line_height(EPD_FONT_SMALL), touch = epd_ui_touch(64);
    int header_x = area.x1 + epd_ui_touch(60) + epd_ui_px(16);
    lv_obj_set_style_text_font(page->screen, epd_app_font_role(EPD_FONT_BODY), 0);
    epd_app_back_button_layout(page->back, epd_ui_px(24),
        epd_ui_px(81) + epd_app_font_role(EPD_FONT_TITLE)->line_height / 2);
    label_box(page->heading, header_x, epd_ui_px(81), area.x2 + 1 - header_x,
              line_height(EPD_FONT_TITLE), EPD_FONT_TITLE);
    box(page->body, area.x1, area.y1, width, height);
    if (is_list(page->view)) {
        bool library = page->view != VIEW_QUEUE && !page->picking_playlist;
        int tabs = library ? touch + gap : 0;
        for (int i = VIEW_FILES; i <= VIEW_PLAYLISTS; ++i) {
            button_t *item = base_action(page, A_VIEW, i);
            if (item) {
                visible(item->object, library);
                int x = (i - VIEW_FILES) * width / 3;
                button_box(item, x, 0, width / 3, touch);
                box(item->marker, 0, touch - epd_ui_px(2), width / 3, epd_ui_px(2));
                visible(item->marker, page->view == (view_kind_t)i);
            }
        }
        action_box(page, A_SEARCH, 0, width - 2 * touch, tabs, touch, touch);
        action_box(page, A_MORE, 0, width - touch, tabs, touch, touch);
        action_box(page, A_RESET, 0, width - 3 * touch, tabs, touch, touch);
        box(page->breadcrumb_bar, 0, tabs,
            width - (page->query.filter[0] ? 3 : 2) * touch - gap, touch);
        label_box(page->breadcrumb, 0, (touch - line) / 2,
                  LV_SIZE_CONTENT, line, EPD_FONT_SMALL);
        int y = tabs + touch + gap;
        bool mini = !page->picking_playlist && (page->snapshot.current_id || page->snapshot.current_path[0]);
        page->mini_shown = mini;
        int mini_height = mini ? epd_ui_touch(104) + gap : 0;
        int nav_y = height - mini_height - touch;
        int status_y = nav_y - line * 2 - gap;
        int row_height = (status_y - y) / MUSIC_PAGE_ROWS;
        for (unsigned i = 0; i < MUSIC_PAGE_ROWS; ++i) {
            box(page->rows[i], 0, y + (int)i * row_height, width, row_height);
            const lv_image_dsc_t *image = music_icon(page->displayed[i].directory ? MUSIC_ICON_FOLDER :
                is_m3u8(page->displayed[i].path) ? MUSIC_ICON_QUEUE :
                page->displayed[i].favorite ? MUSIC_ICON_FAVORITE_ON : MUSIC_ICON_MUSIC, epd_ui_scale_percent());
            lv_image_set_src(page->row_icons[i], image);
            lv_obj_set_pos(page->row_icons[i], epd_ui_px(8), (row_height - (int)image->header.h) / 2);
            int left = epd_ui_px(8) + image->header.w + epd_ui_px(16);
            label_box(page->row_labels[i], left, (row_height - line_height(EPD_FONT_BODY)) / 2,
                      width - left - touch, line_height(EPD_FONT_BODY), EPD_FONT_BODY);
            action_box(page, A_ROW_MENU, i, width - touch, 0, touch, row_height);
        }
        label_box(page->status, 0, page->count ? status_y : y + row_height * 2, width, line * 2, EPD_FONT_SMALL);
        lv_obj_set_style_text_align(page->status, LV_TEXT_ALIGN_CENTER, 0);
        action_box(page, A_PAGE_PREV, 0, width / 2 - touch * 2, nav_y, touch, touch);
        action_box(page, A_PAGE_NEXT, 0, width / 2 + touch, nav_y, touch, touch);
        label_box(page->page_number, width / 2 - touch, nav_y + (touch - line) / 2,
                  touch * 2, line, EPD_FONT_SMALL);
        lv_obj_set_style_text_align(page->page_number, LV_TEXT_ALIGN_CENTER, 0);
        visible(page->mini, mini);
        if (mini) {
            box(page->mini, 0, height - mini_height + gap, width, mini_height - gap);
            int mini_h = mini_height - gap;
            box(page->mini_rule, 0, 0, width, epd_ui_px(2));
            action_box(page, A_VIEW, VIEW_NOW, 0, 0, width - touch * 2, mini_h);
            label_box(page->mini_title, gap, mini_h / 2 - line_height(EPD_FONT_BODY),
                      width - touch * 2 - gap * 2, line_height(EPD_FONT_BODY), EPD_FONT_BODY);
            label_box(page->mini_artist, gap, mini_h / 2 + epd_ui_px(4),
                      width - touch * 2 - gap * 2, line, EPD_FONT_SMALL);
            action_box(page, A_PLAY, 0, width - touch * 2, (mini_h - touch) / 2, touch, touch);
            action_box(page, A_NEXT, 0, width - touch, (mini_h - touch) / 2, touch, touch);
        }
    } else {
        lv_point_t title_size;
        lv_text_get_size(&title_size, lv_label_get_text(page->track), epd_app_font_role(EPD_FONT_TITLE),
                        0, 0, width, LV_TEXT_FLAG_NONE);
        int title_height = LV_MIN(line_height(EPD_FONT_TITLE) * 2, LV_MAX(title_size.y, line_height(EPD_FONT_TITLE)));
        int main_height = epd_ui_touch(128);
        int fixed = title_height + 4 * line + main_height + touch * 2 + gap * 9;
        int cover_size = LV_MIN(epd_ui_px(320), LV_MAX(180, height - fixed));
        int y = gap;
        box(page->cover_frame, (width - cover_size) / 2, y, cover_size, cover_size);
        lv_obj_set_style_radius(page->cover_frame, epd_ui_px(28), 0);
        lv_image_set_src(page->placeholder, music_icon(MUSIC_ICON_MUSIC_LARGE, epd_ui_scale_percent()));
        lv_obj_center(page->placeholder);
        visible(page->placeholder, !page->cover_image.pixels);
        if (page->cover_image.pixels) {
            lv_obj_set_size(page->cover, cover_size - 2, cover_size - 2);
            lv_obj_center(page->cover);
        }
        y += cover_size + gap * 2;
        label_box(page->track, 0, y, width, title_height, EPD_FONT_TITLE);
        lv_obj_set_style_text_align(page->track, LV_TEXT_ALIGN_CENTER, 0);
        y += title_height;
        label_box(page->artist, 0, y, width, line, EPD_FONT_SMALL);
        lv_obj_set_style_text_align(page->artist, LV_TEXT_ALIGN_CENTER, 0);
        y += line + gap * 2;
        int rail = epd_ui_px(8), slider_pad = (touch - rail) / 2;
        box(page->progress_rail, gap, y + slider_pad, width - gap * 2, rail);
        box(page->progress, gap, y, width - gap * 2, touch);
        lv_obj_set_style_pad_top(page->progress, slider_pad, 0);
        lv_obj_set_style_pad_bottom(page->progress, touch - rail - slider_pad, 0);
        lv_obj_set_style_pad_all(page->progress, (epd_ui_px(28) - touch) / 2, LV_PART_KNOB);
        lv_obj_set_style_radius(page->progress, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        y += touch + gap;
        label_box(page->elapsed, gap, y, width / 2 - gap, line, EPD_FONT_SMALL);
        label_box(page->duration, width / 2, y, width / 2 - gap, line, EPD_FONT_SMALL);
        lv_obj_set_style_text_align(page->duration, LV_TEXT_ALIGN_RIGHT, 0);
        y += line + gap;
        int side = epd_ui_touch(80), play = epd_ui_touch(112);
        action_box(page, A_MODE, 0, 0, y + (main_height - touch) / 2, touch, touch);
        action_box(page, A_PREV, 0, width / 2 - play / 2 - gap - side, y + (main_height - side) / 2, side, side);
        action_box(page, A_PLAY, 0, (width - play) / 2, y + (main_height - play) / 2, play, play);
        action_box(page, A_NEXT, 0, width / 2 + play / 2 + gap, y + (main_height - side) / 2, side, side);
        action_box(page, A_VIEW, VIEW_QUEUE, width - touch, y + (main_height - touch) / 2, touch, touch);
        y += main_height + gap;
        action_box(page, A_VOLUME, 0, width / 2 - touch * 2, y, touch, touch);
        action_box(page, A_TIMER, 0, (width - touch) / 2, y, touch, touch);
        action_box(page, A_MORE, 0, width / 2 + touch, y, touch, touch);
        y += touch + gap;
        label_box(page->status, 0, y, width, LV_MAX(line, height - y), EPD_FONT_SMALL);
        lv_obj_set_style_text_align(page->status, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (page->dialog) {
        box(page->dialog, 0, area.y1, lv_obj_get_width(page->screen), height);
        int pad = epd_ui_px(24), card_w = width - gap * 2;
        int card_x = area.x1 + gap, row_h = epd_ui_touch(76);
        unsigned count = page->button_count - page->base_buttons - 1;
        int card_h = line * 2 + pad * 2 + (int)count * row_h, keyboard_h = 0;
        if (page->input) {
            card_h = line * 2 + pad * 2 + epd_ui_touch(80) + gap * 2 + row_h +
                (page->input_status != T_none ? line * 2 + gap : 0);
            epd_keyboard_refresh(page->keyboard);
            keyboard_h = LV_MIN(epd_keyboard_preferred_height(page->keyboard), LV_MAX(1, height - card_h - gap));
            card_h = LV_MIN(card_h, LV_MAX(1, height - keyboard_h - gap));
        } else if (page->details) card_h = LV_MIN(height - gap * 2, line * 13 + pad * 2);
        else if (page->volume_slider) card_h += line + touch + gap * 3;
        else if (page->dialog_kind == DIALOG_CLEAR) card_h += line * 2 + gap;
        bool menu_error = !page->input && page->dialog_status && page->local_status == T_busy;
        if (menu_error) card_h += line * 2 + gap;
        card_h = LV_MIN(card_h, height - (page->input ? 0 : gap * 2));
        int card_y = page->input ? 0 : (height - card_h) / 2;
        box(page->dialog_card, card_x, card_y, card_w, card_h);
        lv_obj_set_style_radius(page->dialog_card, epd_ui_px(32), 0);
        label_box(page->dialog_title, pad, pad, card_w - pad * 2 - touch, line * 2, EPD_FONT_BODY);
        button_box(&page->buttons[page->button_count - 1], card_w - pad - touch, pad, touch, touch);
        int y = pad + line * 2 + gap, inner = card_w - pad * 2;
        if (page->input) {
            box(page->input, pad, y, inner, epd_ui_touch(80));
            lv_obj_set_style_text_font(page->input, epd_app_font_role(EPD_FONT_BODY), 0);
            y += epd_ui_touch(80) + gap;
            label_box(page->dialog_status, pad, y, inner, line * 2, EPD_FONT_SMALL);
            if (page->input_status != T_none) y += line * 2 + gap;
            button_box(&page->buttons[page->base_buttons], pad, y, inner, row_h);
            box(page->keyboard, 0, height - keyboard_h, lv_obj_get_width(page->screen), keyboard_h);
        } else if (page->details) {
            label_box(page->details, pad, y, inner, card_h - y - pad, EPD_FONT_SMALL);
            lv_label_set_long_mode(page->details, LV_LABEL_LONG_WRAP);
        } else {
            if (page->volume_slider) {
                label_box(page->volume_label, pad, y, inner, line, EPD_FONT_BODY);
                lv_obj_set_style_text_align(page->volume_label, LV_TEXT_ALIGN_CENTER, 0);
                y += line + gap * 2;
                int rail = epd_ui_px(10), slider_pad = (touch - rail) / 2;
                box(page->volume_rail, pad + gap, y + slider_pad, inner - gap * 2, rail);
                box(page->volume_slider, pad + gap, y, inner - gap * 2, touch);
                lv_obj_set_style_pad_top(page->volume_slider, slider_pad, 0);
                lv_obj_set_style_pad_bottom(page->volume_slider, touch - rail - slider_pad, 0);
                lv_obj_set_style_pad_all(page->volume_slider, (epd_ui_px(28) - touch) / 2, LV_PART_KNOB);
                lv_obj_set_style_radius(page->volume_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
                y += touch;
            }
            if (page->dialog_hint) {
                label_box(page->dialog_hint, pad, y, inner, line * 2, EPD_FONT_SMALL);
                y += line * 2 + gap;
            }
            row_h = LV_MIN(row_h, (card_h - y - pad - (menu_error ? line * 2 + gap : 0)) / (int)LV_MAX(count, 1));
            for (unsigned i = 0; i < count; ++i)
                button_box(&page->buttons[page->base_buttons + i], pad, y + (int)i * row_h, inner, row_h);
            if (menu_error)
                label_box(page->dialog_status, pad, card_h - pad - line * 2, inner, line * 2, EPD_FONT_SMALL);
        }
    }
    page->scale_revision = epd_ui_scale_revision();
    page->layout_track_id = page->snapshot.current_id;
    page->layout_dirty = false;
}

static bool is_list(view_kind_t view)
{ return view >= VIEW_FILES && view <= VIEW_QUEUE; }

static music_list_kind_t list_kind(view_kind_t view)
{
    switch (view) {
    case VIEW_QUEUE: return MUSIC_LIST_QUEUE;
    case VIEW_FAVORITES: return MUSIC_LIST_FAVORITES;
    case VIEW_PLAYLISTS: return MUSIC_LIST_PLAYLISTS;
    default: return MUSIC_LIST_FILES;
    }
}

static text_id_t view_title(view_kind_t view)
{
    static const text_id_t titles[] = {T_title, T_files, T_favorites, T_playlists, T_queue, T_now};
    return titles[view];
}


static bool copy_text(char *out, size_t size, const char *source)
{
    size_t length = strlen(source);
    if (length >= size) return false;
    memcpy(out, source, length + 1);
    return true;
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void render_breadcrumb(music_page_t *page)
{
    if (page->view != VIEW_FILES) {
        set_text(page, page->breadcrumb, page->query.filter);
        return;
    }

    const char *path = page->folder;
    const char *root = "";
    if (!strncmp(path, STORAGE_SD_ROOT, strlen(STORAGE_SD_ROOT)) &&
        (path[strlen(STORAGE_SD_ROOT)] == '/' || !path[strlen(STORAGE_SD_ROOT)])) {
        root = text(T_sd);
        path += strlen(STORAGE_SD_ROOT);
    } else if (!strncmp(path, STORAGE_FLASH_ROOT, strlen(STORAGE_FLASH_ROOT)) &&
               (path[strlen(STORAGE_FLASH_ROOT)] == '/' || !path[strlen(STORAGE_FLASH_ROOT)])) {
        root = text(T_flash);
        path += strlen(STORAGE_FLASH_ROOT);
    }

    char caption[3 * STORAGE_PATH_MAX + sizeof(page->query.filter) + 128];
    size_t used = (size_t)snprintf(caption, sizeof(caption), "%s", root);
    if (used >= sizeof(caption)) return;
    while (*path && used + 3 < sizeof(caption)) {
        if (*path == '/') {
            memcpy(caption + used, " / ", 3);
            used += 3;
        } else {
            caption[used++] = *path;
        }
        ++path;
    }
    caption[used] = 0;
    if (page->query.filter[0])
        snprintf(caption + used, sizeof(caption) - used, " · %s: %s", text(T_search), page->query.filter);
    set_text(page, page->breadcrumb, caption);
}

static bool restore_folder(music_page_t *page)
{
    if (page->folder_initialized || !page->snapshot.ready) return false;
    page->folder_initialized = true;
    if (page->snapshot.directory[0] == '/')
        copy_text(page->folder, sizeof(page->folder), page->snapshot.directory);
    return true;
}

static void append_info(char *out, size_t capacity, const char *value)
{
    size_t used = strlen(out);
    snprintf(out + used, capacity - used, "%s%s", used ? " · " : "", value);
}

static bool is_m3u8(const char *path)
{
    size_t size = strlen(path);
    if (size < 5) return false;
    const char *ext = path + size - 5;
    return ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') && ext[2] == '3' &&
           (ext[3] == 'u' || ext[3] == 'U') && ext[4] == '8';
}

static bool submit(music_page_t *page, const music_command_t *command)
{
    if (page->stopped || !music_submit(command)) {
        page->local_status = page->stopped ? T_unavailable : T_busy;
        page->force = true;
        return false;
    }
    if (command->kind != MUSIC_CMD_LIST) page->content_error = RT_EOK;
    page->local_status = command->kind == MUSIC_CMD_SAVE_PLAYLIST ? T_saving : T_none;
    if (command->kind != MUSIC_CMD_LIST) {
        page->operation_pending = true;
        page->operation_result = page->snapshot.result_revision;
        page->import_pending = command->kind == MUSIC_CMD_IMPORT;
        page->save_pending = command->kind == MUSIC_CMD_SAVE_PLAYLIST;
    }
    page->force = true;
    return true;
}

static void request_list(music_page_t *page)
{
    if (page->local_status == T_unavailable) return;
    page->layout_dirty = true;
    page->query.kind = MUSIC_CMD_LIST;
    page->query.list = list_kind(page->view);
    copy_text(page->query.path, sizeof(page->query.path), page->view == VIEW_FILES ? page->folder : "");
    page->list_tick = lv_tick_get();
    page->requested_list = page->snapshot.list_revision;
    page->request_result = page->snapshot.result_revision;
    if (!page->snapshot.ready) {
        page->list_pending = true;
        page->local_status = T_none;
        page->force = true;
        return;
    }
    page->list_pending = submit(page, &page->query);
    if (!page->list_pending) page->count = page->total = 0;
}

static void poll(lv_timer_t *timer)
{
    music_page_t *page = lv_timer_get_user_data(timer);
    if (page == current_page) music_view_update();
}

static void timer_release(music_page_t *page)
{
    if (page->timer) lv_timer_delete(page->timer);
    page->timer = NULL;
}

static void cover_release(music_page_t *page)
{
    if (page->cover) {
        lv_image_set_src(page->cover, NULL);
        visible(page->cover, false);
    }
    if (page->cover_image.pixels) {
        lv_draw_wait_for_finish();
        lv_image_cache_drop(&page->cover_descriptor);
        lv_image_header_cache_drop(&page->cover_descriptor);
        music_cover_image_free(&page->cover_image);
    }
    memset(&page->cover_descriptor, 0, sizeof(page->cover_descriptor));
    page->cover_checked = false;
}

static void icons_release(void)
{
    /* Cached image sources must not outlive the dynamic module that owns them. */
    static const unsigned scales[] = {90, 95, 100, 110, 120};
    lv_draw_wait_for_finish();
    for (unsigned id = 0; id < MUSIC_ICON_COUNT; ++id) {
        for (unsigned scale = 0; scale < sizeof(scales) / sizeof(scales[0]); ++scale) {
            const lv_image_dsc_t *source = music_icon((music_icon_id_t)id, scales[scale]);
            lv_image_cache_drop(source);
            lv_image_header_cache_drop(source);
        }
    }
}

static void cover_update(music_page_t *page)
{
    if (!page->cover || page->stopped) return;
    uint32_t revision = music_cover_revision();
    uint32_t id = page->snapshot.current_id;
    if (page->cover_checked && page->cover_revision == revision && page->cover_id == id) return;
    cover_release(page);
    if (id && music_cover_copy(id, &page->cover_image)) {
        unsigned width = page->cover_image.width, height = page->cover_image.height;
        if (!page->cover_image.pixels || !width || !height || width > 180 || height > 180) {
            music_cover_image_free(&page->cover_image);
        } else {
            lv_image_dsc_t *image = &page->cover_descriptor;
            image->header.magic = LV_IMAGE_HEADER_MAGIC;
            image->header.cf = LV_COLOR_FORMAT_RGB565;
            image->header.w = width;
            image->header.h = height;
            image->header.stride = width * 2;
            image->data = (const uint8_t *)page->cover_image.pixels;
            image->data_size = width * height * 2;
            lv_draw_buf_t buffer = {.data = (uint8_t *)image->data, .data_size = image->data_size};
            epd_app_clean_draw_buffer(&buffer);
            lv_image_set_src(page->cover, image);
            visible(page->cover, true);
        }
    }
    page->force = true;
    page->cover_revision = revision;
    page->cover_id = id;
    page->cover_checked = true;
    page->layout_dirty = true;
}

static void dialog_close(music_page_t *page)
{
    lv_obj_t *dialog = page->dialog;
    page->dialog = page->dialog_title = page->dialog_hint = page->dialog_status = page->input = page->keyboard = NULL;
    page->dialog_card = page->details = page->volume_slider = page->volume_label = page->volume_rail = NULL;
    page->volume_dragging = false;
    page->dialog_kind = DIALOG_NONE;
    page->button_count = page->base_buttons;
    page->input_status = T_none;
    page->force = true;
    if (dialog) lv_obj_delete(dialog);
}

static void keyboard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_CANCEL && code != LV_EVENT_READY) return;
    music_page_t *page = lv_event_get_user_data(event);
    if (code == LV_EVENT_READY) input_confirm(page);
    page->force = true;
    render(page);
}

static button_t *menu_item(music_page_t *page, text_id_t title, action_t action, int value, music_icon_id_t id)
{
    return icon_button(page, page->dialog_card, title, action, value, id, false);
}

static void dialog_open(music_page_t *page, dialog_kind_t kind)
{
    dialog_close(page);
    page->dialog_kind = kind;
    page->dialog = surface(page->screen);
    page->dialog_card = surface(page->dialog);
    lv_obj_set_style_border_width(page->dialog_card, 2, 0);
    lv_obj_set_style_border_color(page->dialog_card, lv_color_black(), 0);
    lv_group_t *group = epd_app_input_group(page->dialog);
    page->dialog_title = label(page->dialog_card, 0, 0, 500, 80);
    if (kind == DIALOG_ROW) {
        menu_item(page, T_insert_next, A_INSERT, 0, MUSIC_ICON_NEXT);
        menu_item(page, T_append, A_APPEND, 0, MUSIC_ICON_QUEUE);
        menu_item(page, page->selected.favorite ? T_unfavorite : T_favorite, A_FAVORITE, 0,
                  page->selected.favorite ? MUSIC_ICON_FAVORITE_ON : MUSIC_ICON_FAVORITE);
        if (page->view == VIEW_QUEUE) {
            menu_item(page, T_remove, A_REMOVE, 0, MUSIC_ICON_REMOVE);
            if (!page->query.filter[0]) {
                button_t *up = menu_item(page, T_up, A_UP, 0, MUSIC_ICON_UP);
                button_t *down = menu_item(page, T_down, A_DOWN, 0, MUSIC_ICON_DOWN);
                enabled(up->object, page->selected_index > 0);
                enabled(down->object, page->selected_index + 1 < page->total);
            }
        }
    } else if (kind == DIALOG_MORE) {
        if (page->view == VIEW_FILES) {
            menu_item(page, T_sd, A_SD, 0, MUSIC_ICON_FOLDER);
            menu_item(page, T_flash, A_FLASH, 0, MUSIC_ICON_FOLDER);
        } else if (page->view == VIEW_QUEUE) {
            menu_item(page, T_save, A_SAVE, 0, MUSIC_ICON_SAVE);
            menu_item(page, T_clear, A_CLEAR, 0, MUSIC_ICON_REMOVE);
        } else if (page->view == VIEW_PLAYLISTS) menu_item(page, T_import, A_IMPORT, 0, MUSIC_ICON_ADD);
        if (page->view != VIEW_QUEUE && !page->picking_playlist)
            menu_item(page, T_queue, A_VIEW, VIEW_QUEUE, MUSIC_ICON_QUEUE);
        if (page->view == VIEW_NOW) {
            menu_item(page, T_details, A_DETAILS, 0, MUSIC_ICON_INFO);
            menu_item(page, T_stop, A_STOP, 0, MUSIC_ICON_STOP);
        }
    } else if (kind == DIALOG_MODE || kind == DIALOG_TIMER) {
        static const text_id_t modes[] = {T_mode_sequence, T_mode_list, T_mode_one, T_mode_shuffle};
        static const music_icon_id_t mode_icons[] = {MUSIC_ICON_REPEAT, MUSIC_ICON_REPEAT, MUSIC_ICON_REPEAT_ONE, MUSIC_ICON_SHUFFLE};
        static const text_id_t timers[] = {T_timer_off, T_timer_15, T_timer_30, T_timer_60, T_timer_track};
        static const int minutes[] = {0, 15, 30, 60, -1};
        for (unsigned i = 0; i < (kind == DIALOG_MODE ? 4u : 5u); ++i) {
            bool selected = kind == DIALOG_MODE ? page->snapshot.mode == i :
                            page->snapshot.timer_minutes == (unsigned)minutes[i];
            menu_item(page, kind == DIALOG_MODE ? modes[i] : timers[i],
                      kind == DIALOG_MODE ? A_SET_MODE : A_SET_TIMER,
                      kind == DIALOG_MODE ? (int)i : minutes[i],
                      selected ? MUSIC_ICON_CHECK : kind == DIALOG_MODE ? mode_icons[i] : MUSIC_ICON_TIMER);
        }
    } else if (kind == DIALOG_DETAILS) {
        page->details = label(page->dialog_card, 0, 0, 500, 500);
    } else if (kind == DIALOG_VOLUME) {
        page->volume_label = label(page->dialog_card, 0, 0, 500, 60);
        page->volume_rail = surface(page->dialog_card);
        lv_obj_set_style_bg_color(page->volume_rail, lv_color_hex(0xaaaaaa), 0);
        lv_obj_set_style_radius(page->volume_rail, LV_RADIUS_CIRCLE, 0);
        page->volume_slider = lv_slider_create(page->dialog_card);
        epd_app_object_init(page->volume_slider);
        lv_slider_set_range(page->volume_slider, 0, 100);
        lv_slider_set_value(page->volume_slider, page->snapshot.volume, LV_ANIM_OFF);
        lv_obj_set_style_bg_opa(page->volume_slider, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(page->volume_slider, lv_color_black(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(page->volume_slider, lv_color_black(), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(page->volume_slider, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(page->volume_slider, LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_radius(page->volume_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_radius(page->volume_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(page->volume_slider, epd_ui_px(10), LV_PART_KNOB);
        lv_obj_set_style_anim_duration(page->volume_slider, 0, 0);
        lv_obj_add_event_cb(page->volume_slider, volume_event, LV_EVENT_ALL, page);
        menu_item(page, T_none, A_VOLUME_DOWN, 0, MUSIC_ICON_DOWN);
        menu_item(page, T_none, A_VOLUME_UP, 0, MUSIC_ICON_UP);
    } else if (kind == DIALOG_CLEAR) {
        page->dialog_hint = label(page->dialog_card, 0, 0, 500, 80);
        menu_item(page, T_clear, A_CLEAR_CONFIRM, 0, MUSIC_ICON_REMOVE);
    } else {
        page->input = lv_textarea_create(page->dialog_card);
        epd_app_object_init(page->input);
        lv_obj_set_style_border_width(page->input, 1, 0);
        lv_obj_set_style_border_color(page->input, lv_color_black(), 0);
        lv_obj_set_style_radius(page->input, epd_ui_px(12), 0);
        lv_obj_set_style_pad_all(page->input, epd_ui_px(12), 0);
        lv_textarea_set_one_line(page->input, true);
        lv_textarea_set_max_length(page->input, kind == DIALOG_SEARCH ? sizeof(page->query.filter) - 1 : STORAGE_PATH_MAX - 1);
        lv_textarea_set_text(page->input, kind == DIALOG_SEARCH ? page->query.filter : "");
        lv_obj_set_style_anim_duration(page->input, 0, 0);
        lv_obj_set_style_anim_duration(page->input, 0, LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(page->input, LV_OPA_TRANSP, LV_PART_CURSOR);
        lv_obj_set_style_border_width(page->input, 0, LV_PART_CURSOR);
        lv_obj_set_style_shadow_width(page->input, 0, LV_PART_CURSOR);
        page->dialog_status = label(page->dialog_card, 0, 0, 500, 80);
        menu_item(page, T_confirm, A_CONFIRM, 0, MUSIC_ICON_CHECK);
        const epd_keyboard_config_t keyboard_config = {
            .mode = EPD_KEYBOARD_TEXT, .chinese = true, .voice = true
        };
        page->keyboard = epd_keyboard_create(page->dialog, page->input, &keyboard_config);
        lv_obj_add_event_cb(page->keyboard, keyboard_event, LV_EVENT_READY, page);
        lv_obj_add_event_cb(page->keyboard, keyboard_event, LV_EVENT_CANCEL, page);
        lv_group_add_obj(group, page->input);
    }
    if (!page->input) page->dialog_status = label(page->dialog_card, 0, 0, 500, 80);
    button_t *cancel = icon_button(page, page->dialog_card, T_cancel, A_CANCEL, 0, MUSIC_ICON_CLOSE, true);
    epd_app_input_back(page->dialog, cancel->object);
    page->layout_dirty = true;
    page->force = true;
    render(page);
}

static void row_event(lv_event_t *event)
{
    music_page_t *page = lv_event_get_user_data(event);
    if (page->stopped || !page->foreground || page->list_pending || page->dialog) return;
    unsigned index;
    for (index = 0; index < page->count; ++index)
        if (page->rows[index] == lv_event_get_target_obj(event)) break;
    if (index == page->count) return;
    music_list_entry_t *entry = &page->displayed[index];
    if (page->picking_playlist && !entry->directory && !is_m3u8(entry->path)) return;
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
        if (!entry->directory && !is_m3u8(entry->path)) {
            page->selected = *entry;
            page->selected_index = page->query.first + index;
            dialog_open(page, DIALOG_ROW);
        }
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_SHORT_CLICKED) return;
    if (entry->directory) {
        page->folder_initialized = true;
        copy_text(page->folder, sizeof(page->folder), entry->path);
        page->query.first = 0;
        page->query.filter[0] = 0;
        if (page->view != VIEW_FILES) show_view(page, VIEW_FILES);
        else request_list(page);
    } else {
        music_command_t command = {.kind = is_m3u8(entry->path) ? MUSIC_CMD_IMPORT : MUSIC_CMD_OPEN,
                                   .list = list_kind(page->view)};
        if (page->view == VIEW_QUEUE) command.id = entry->id;
        copy_text(command.path, sizeof(command.path), entry->path);
        copy_text(command.filter, sizeof(command.filter), page->query.filter);
        if (submit(page, &command)) {
            page->return_view = page->view;
            if (command.kind != MUSIC_CMD_IMPORT) show_view(page, VIEW_NOW);
        }
    }
    render(page);
}

static void time_text(char *out, size_t size, uint32_t seconds)
{
    if (seconds >= 3600)
        snprintf(out, size, "%lu:%02lu:%02lu", (unsigned long)(seconds / 3600),
                 (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
    else snprintf(out, size, "%lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}

static void progress_text(music_page_t *page, uint32_t position)
{
    char elapsed[32], duration[32], value[40];
    time_text(elapsed, sizeof(elapsed), position);
    if (page->snapshot.duration_seconds) time_text(duration, sizeof(duration), page->snapshot.duration_seconds);
    else strcpy(duration, "--:--");
    snprintf(value, sizeof(value), "%s%s",
             page->snapshot.duration_estimated && page->snapshot.duration_seconds ? "≈" : "", duration);
    set_text(page, page->elapsed, elapsed);
    set_text(page, page->duration, value);
}

static void volume_event(lv_event_t *event)
{
    music_page_t *page = lv_event_get_user_data(event);
    if (!page->foreground || page->stopped || !page->snapshot.volume_available) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) page->volume_dragging = true;
    else if (code == LV_EVENT_VALUE_CHANGED && page->volume_dragging) {
        char value[32];
        snprintf(value, sizeof(value), "%d%%", (int)lv_slider_get_value(page->volume_slider));
        set_text(page, page->volume_label, value);
    } else if (code == LV_EVENT_RELEASED && page->volume_dragging) {
        page->volume_dragging = false;
        music_command_t command = {.kind = MUSIC_CMD_VOLUME, .value = lv_slider_get_value(page->volume_slider)};
        submit(page, &command);
    } else if (code == LV_EVENT_PRESS_LOST) {
        page->volume_dragging = false;
        page->force = true;
    }
}

static void slider_event(lv_event_t *event)
{
    music_page_t *page = lv_event_get_user_data(event);
    if (page->stopped || !page->foreground) return;
    lv_obj_t *object = lv_event_get_target_obj(event);
    lv_event_code_t code = lv_event_get_code(event);
    bool *dragging = &page->seek_dragging;
    if (code == LV_EVENT_PRESSED) {
        *dragging = true;
        page->seek_id = page->snapshot.current_id;
        page->seek_duration = page->snapshot.duration_seconds;
    } else if (code == LV_EVENT_VALUE_CHANGED && *dragging) {
        progress_text(page, (uint32_t)((uint64_t)page->seek_duration * lv_slider_get_value(object) / 1000));
    } else if (code == LV_EVENT_RELEASED && *dragging) {
        *dragging = false;
        music_command_t command = {.kind = MUSIC_CMD_SEEK};
        uint64_t target = (uint64_t)page->seek_duration * lv_slider_get_value(object) / 1000;
        music_snapshot_read(&page->snapshot);
        if (!page->seek_id || page->seek_id != page->snapshot.current_id || target > INT32_MAX) {
            page->force = true;
            render(page);
            return;
        }
        command.id = page->seek_id;
        command.value = (int32_t)target;
        submit(page, &command);
        render(page);
    } else if (code == LV_EVENT_PRESS_LOST) {
        *dragging = false;
        page->force = true;
        render(page);
    }
}

static lv_obj_t *slider(music_page_t *page, int y, int maximum)
{
    lv_obj_t *object = lv_slider_create(page->body);
    epd_app_object_init(object);
    lv_obj_set_pos(object, 56, y);
    lv_obj_set_size(object, 572, 32);
    lv_slider_set_range(object, 0, maximum);
    lv_obj_set_style_bg_color(object, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(object, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(object, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(object, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(object, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_anim_duration(object, 0, 0);
    lv_obj_add_event_cb(object, slider_event, LV_EVENT_ALL, page);
    return object;
}

static void show_view(music_page_t *page, view_kind_t view)
{
    if (view == VIEW_HOME) view = VIEW_FILES;
    dialog_close(page);
    cover_release(page);
    if (page->body) lv_obj_delete(page->body);
    page->body = surface(page->screen);
    page->view = view;
    if (!is_list(view)) {
        page->list_pending = false;
        if (page->local_status == T_loading) page->local_status = T_none;
    }
    page->count = page->total = 0;
    page->button_count = page->base_buttons = 0;
    page->breadcrumb_bar = page->breadcrumb = page->page_number = NULL;
    page->track = page->artist = page->elapsed = page->duration = NULL;
    page->cover = page->cover_frame = page->placeholder = NULL;
    page->progress = page->volume_label = NULL;
    page->mini = page->mini_title = page->mini_artist = NULL;
    memset(page->rows, 0, sizeof(page->rows));
    memset(page->row_labels, 0, sizeof(page->row_labels));
    memset(page->row_icons, 0, sizeof(page->row_icons));
    page->seek_dragging = false;
    if (is_list(view)) {
        if (view != VIEW_QUEUE && !page->picking_playlist) {
            for (int i = VIEW_FILES; i <= VIEW_PLAYLISTS; ++i) {
                button_t *tab = button(page, page->body, 0, 0, 200, 64, view_title(i), A_VIEW, i);
                plain_button(tab->object);
                tab->marker = surface(tab->object);
                lv_obj_set_style_bg_color(tab->marker, lv_color_black(), 0);
            }
        }
        icon_button(page, page->body, T_search, A_SEARCH, 0, MUSIC_ICON_SEARCH, true);
        icon_button(page, page->body, T_reset, A_RESET, 0, MUSIC_ICON_CLOSE, true);
        icon_button(page, page->body, T_more, A_MORE, 0, MUSIC_ICON_MORE, true);
        page->breadcrumb_bar = surface(page->body);
        lv_obj_add_flag(page->breadcrumb_bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(page->breadcrumb_bar, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
        lv_obj_set_scroll_dir(page->breadcrumb_bar, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(page->breadcrumb_bar, LV_SCROLLBAR_MODE_AUTO);
        page->breadcrumb = label(page->breadcrumb_bar, 0, 0, LV_SIZE_CONTENT, 40);
        lv_label_set_long_mode(page->breadcrumb, LV_LABEL_LONG_CLIP);
        for (unsigned i = 0; i < MUSIC_PAGE_ROWS; ++i) {
            page->rows[i] = epd_app_button(page->body, 0, 0, 620, 72, NULL, NULL, NULL);
            plain_button(page->rows[i]);
            page->row_icons[i] = icon(page->rows[i], MUSIC_ICON_MUSIC);
            page->row_labels[i] = label(page->rows[i], 64, 0, 460, 42);
            icon_button(page, page->rows[i], T_more, A_ROW_MENU, i, MUSIC_ICON_MORE, true);
            lv_obj_add_event_cb(page->rows[i], row_event, LV_EVENT_ALL, page);
            visible(page->rows[i], false);
        }
        page->status = label(page->body, 0, 0, 620, 80);
        page->page_number = label(page->body, 0, 0, 200, 40);
        icon_button(page, page->body, T_previous_page, A_PAGE_PREV, 0, MUSIC_ICON_LEFT, true);
        icon_button(page, page->body, T_next_page, A_PAGE_NEXT, 0, MUSIC_ICON_RIGHT, true);
        page->mini = surface(page->body);
        page->mini_rule = surface(page->mini);
        lv_obj_set_style_bg_color(page->mini_rule, lv_color_black(), 0);
        lv_obj_set_style_radius(page->mini, 0, 0);
        button_t *open = button(page, page->mini, 0, 0, 400, 100, T_now, A_VIEW, VIEW_NOW);
        plain_button(open->object);
        visible(open->label, false);
        page->mini_title = label(open->object, 0, 0, 400, 40);
        page->mini_artist = label(open->object, 0, 40, 400, 40);
        icon_button(page, page->mini, T_play, A_PLAY, 0, MUSIC_ICON_PLAY, true);
        icon_button(page, page->mini, T_next, A_NEXT, 0, MUSIC_ICON_NEXT, true);
        if (page->query.list != list_kind(view)) {
            page->query.first = 0;
            page->query.filter[0] = 0;
        }
        request_list(page);
    } else {
        page->cover_frame = surface(page->body);
        lv_obj_set_style_border_width(page->cover_frame, 1, 0);
        lv_obj_set_style_border_color(page->cover_frame, lv_color_hex(0xaaaaaa), 0);
        lv_obj_set_style_clip_corner(page->cover_frame, true, 0);
        page->placeholder = icon(page->cover_frame, MUSIC_ICON_MUSIC_LARGE);
        page->cover = lv_image_create(page->cover_frame);
        lv_image_set_inner_align(page->cover, LV_IMAGE_ALIGN_COVER);
        visible(page->cover, false);
        page->track = label(page->body, 0, 0, 620, 110);
        page->artist = label(page->body, 0, 0, 620, 40);
        page->elapsed = label(page->body, 0, 0, 280, 40);
        page->duration = label(page->body, 0, 0, 280, 40);
        page->progress_rail = surface(page->body);
        lv_obj_set_style_bg_color(page->progress_rail, lv_color_hex(0xaaaaaa), 0);
        lv_obj_set_style_radius(page->progress_rail, LV_RADIUS_CIRCLE, 0);
        page->progress = slider(page, 0, 1000);
        lv_obj_set_style_border_width(page->progress, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(page->progress, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(page->progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_radius(page->progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(page->progress, epd_ui_px(10), LV_PART_KNOB);
        icon_button(page, page->body, T_previous, A_PREV, 0, MUSIC_ICON_PREVIOUS, true);
        icon_button(page, page->body, T_play, A_PLAY, 0, MUSIC_ICON_PLAY_LARGE, true);
        icon_button(page, page->body, T_next, A_NEXT, 0, MUSIC_ICON_NEXT, true);
        icon_button(page, page->body, T_mode, A_MODE, 0, MUSIC_ICON_REPEAT, true);
        icon_button(page, page->body, T_queue, A_VIEW, VIEW_QUEUE, MUSIC_ICON_QUEUE, true);
        icon_button(page, page->body, T_volume_title, A_VOLUME, 0, MUSIC_ICON_VOLUME, true);
        icon_button(page, page->body, T_timer, A_TIMER, 0, MUSIC_ICON_TIMER, true);
        icon_button(page, page->body, T_more, A_MORE, 0, MUSIC_ICON_MORE, true);
        page->status = label(page->body, 0, 0, 620, 80);
    }
    page->base_buttons = page->button_count;
    page->layout_dirty = true;
    page->force = true;
    render(page);
}

static void back_event(lv_event_t *event)
{
    music_page_t *page = lv_event_get_user_data(event);
    if (page->dialog) dialog_close(page);
    else if (page->view == VIEW_HOME) { gui_app_goback(); return; }
    else if (page->view == VIEW_FILES) {
        char *slash = strrchr(page->folder, '/');
        if (slash && slash != page->folder) {
            page->folder_initialized = true;
            *slash = 0;
            page->query.first = 0;
            page->query.filter[0] = 0;
            request_list(page);
        } else if (page->picking_playlist) {
            page->picking_playlist = false;
            show_view(page, VIEW_PLAYLISTS);
        } else { gui_app_goback(); return; }
    } else if (page->view == VIEW_NOW) show_view(page, page->return_view);
    else show_view(page, VIEW_HOME);
    render(page);
}

static void input_confirm(music_page_t *page)
{
    const char *value = lv_textarea_get_text(page->input);
    music_command_t command = {0};
    if (page->dialog_kind == DIALOG_SEARCH) {
        if (!copy_text(page->query.filter, sizeof(page->query.filter), value)) {
            page->input_status = T_too_long;
            return;
        }
        dialog_close(page);
        page->query.first = 0;
        request_list(page);
        return;
    }
    if (page->dialog_kind == DIALOG_SAVE) {
        bool valid = value[0] && strcmp(value, ".") && strcmp(value, "..");
        for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
            if (*p == '/' || *p == '\\' || *p == ':' || *p < 32 || *p == 127) valid = false;
        if (!valid) { page->input_status = T_invalid_name; return; }
        int length = snprintf(command.path, sizeof(command.path), "%s%s", value, is_m3u8(value) ? "" : ".m3u8");
        if (length < 0 || (size_t)length >= sizeof(command.path)) { page->input_status = T_too_long; return; }
        command.kind = MUSIC_CMD_SAVE_PLAYLIST;
    } else return;
    if (!submit(page, &command)) { page->input_status = T_busy; return; }
    dialog_close(page);
}

static void action_event(lv_event_t *event)
{
    button_t *item = lv_event_get_user_data(event);
    music_page_t *page = item->page;
    action_t action = item->action;
    int value = item->value;
    if (page->stopped || !page->foreground) return;
    page->force = true;
    music_command_t command = {0};
    bool send = false, close = false;
    switch (action) {
    case A_VIEW:
        if (value == VIEW_NOW && page->view != VIEW_NOW) page->return_view = page->view;
        show_view(page, value); break;
    case A_SD: case A_FLASH:
        dialog_close(page);
        page->folder_initialized = true;
        copy_text(page->folder, sizeof(page->folder), action == A_SD ? "/sdcard" : "/flash");
        page->query.first = 0; page->query.filter[0] = 0; request_list(page); break;
    case A_SEARCH: dialog_open(page, DIALOG_SEARCH); break;
    case A_SAVE: dialog_open(page, DIALOG_SAVE); break;
    case A_IMPORT:
        page->picking_playlist = true; show_view(page, VIEW_FILES); break;
    case A_MORE: dialog_open(page, DIALOG_MORE); break;
    case A_DETAILS: dialog_open(page, DIALOG_DETAILS); break;
    case A_VOLUME: dialog_open(page, DIALOG_VOLUME); break;
    case A_ROW_MENU:
        if (!page->list_pending && value >= 0 && (size_t)value < page->count) {
            page->selected = page->displayed[value];
            page->selected_index = page->query.first + value;
            if (!page->selected.directory && !is_m3u8(page->selected.path)) dialog_open(page, DIALOG_ROW);
        }
        break;
    case A_MODE: dialog_open(page, DIALOG_MODE); break;
    case A_TIMER: dialog_open(page, DIALOG_TIMER); break;
    case A_CANCEL: dialog_close(page); break;
    case A_CONFIRM: input_confirm(page); break;
    case A_RESET: page->query.filter[0] = 0; page->query.first = 0; request_list(page); break;
    case A_PAGE_PREV:
        if (!page->list_pending && page->query.first >= MUSIC_PAGE_ROWS) {
            page->query.first -= MUSIC_PAGE_ROWS; request_list(page);
        } break;
    case A_PAGE_NEXT:
        if (!page->list_pending && page->query.first < page->total && page->total - page->query.first > MUSIC_PAGE_ROWS) {
            page->query.first += MUSIC_PAGE_ROWS; request_list(page);
        } break;
    case A_CLEAR: dialog_open(page, DIALOG_CLEAR); break;
    case A_CLEAR_CONFIRM: command.kind = MUSIC_CMD_CLEAR; send = close = true; break;
    case A_PREV: command.kind = MUSIC_CMD_PREVIOUS; send = true; break;
    case A_NEXT: command.kind = MUSIC_CMD_NEXT; send = true; break;
    case A_STOP: command.kind = MUSIC_CMD_STOP; send = close = true; break;
    case A_PLAY:
        command.kind = page->snapshot.playing ? MUSIC_CMD_PAUSE : MUSIC_CMD_PLAY; send = true; break;
    case A_SET_MODE: command.kind = MUSIC_CMD_MODE; command.value = value; send = close = true; break;
    case A_SET_TIMER: command.kind = MUSIC_CMD_TIMER; command.value = value; send = close = true; break;
    case A_VOLUME_DOWN: case A_VOLUME_UP:
        command.kind = MUSIC_CMD_VOLUME;
        if (page->snapshot.volume_available && page->snapshot.volume <= 100) {
            unsigned volume = page->snapshot.volume;
            command.value = action == A_VOLUME_UP ? (volume < 95 ? volume + 5 : 100) :
                            (volume > 5 ? volume - 5 : 0);
            send = true;
        }
        break;
    case A_INSERT: case A_APPEND: case A_FAVORITE: case A_REMOVE: case A_UP: case A_DOWN:
        command.kind = action == A_INSERT ? MUSIC_CMD_INSERT_NEXT : action == A_APPEND ? MUSIC_CMD_APPEND :
                       action == A_FAVORITE ? MUSIC_CMD_FAVORITE : action == A_REMOVE ? MUSIC_CMD_REMOVE : MUSIC_CMD_MOVE;
        copy_text(command.path, sizeof(command.path), page->selected.path);
        command.id = page->selected.id;
        if (action == A_FAVORITE) command.value = !page->selected.favorite;
        if (action == A_UP || action == A_DOWN) {
            size_t destination = action == A_UP ? (page->selected_index ? page->selected_index - 1 : 0) : page->selected_index + 1;
            if (destination > INT32_MAX) break;
            command.value = (int32_t)destination;
        }
        send = close = true;
        break;
    }
    if (send && submit(page, &command)) {
        if (close) dialog_close(page);
    }
    render(page);
}

static text_id_t result_text(music_result_t result)
{
    switch (result) {
    case MUSIC_ERR_MEMORY: return T_err_memory;
    case MUSIC_ERR_LIMIT: return T_err_limit;
    case MUSIC_ERR_STORAGE: return T_err_storage;
    case MUSIC_ERR_FORMAT: return T_err_format;
    case MUSIC_ERR_OPEN: return T_err_open;
    case MUSIC_ERR_DECODE: return T_err_decode;
    case MUSIC_ERR_OUTPUT: return T_err_output;
    case MUSIC_ERR_SAVE: return T_err_save;
    case MUSIC_ERR_PATH: return T_err_path;
    case MUSIC_ERR_SERVICE: return T_unavailable;
    default: return T_none;
    }
}

static text_id_t timer_text(unsigned minutes)
{
    if (minutes == (unsigned)-1) return T_timer_track;
    if (minutes == 15) return T_timer_15;
    if (minutes == 30) return T_timer_30;
    if (minutes == 60) return T_timer_60;
    return T_timer_off;
}

static void render(music_page_t *page)
{
    if (!page->foreground || !page->screen) return;
    if (page->view == VIEW_NOW) cover_update(page);
    music_snapshot_t *snapshot = &page->snapshot;
    uint32_t locale = epd_locale_revision();
    bool force = page->force || page->text_retry || page->locale_revision != locale ||
                 page->scale_revision != epd_ui_scale_revision();
    bool changed = force || page->rendered_revision != snapshot->revision ||
                   page->rendered_list != snapshot->list_revision || page->rendered_result != snapshot->result_revision;
    bool relayout = page->layout_dirty || page->locale_revision != locale ||
                    page->scale_revision != epd_ui_scale_revision() ||
                    page->layout_track_id != snapshot->current_id ||
                    (is_list(page->view) && page->mini_shown !=
                     (!page->picking_playlist && !!(snapshot->current_id || snapshot->current_path[0]))) ||
                    (page->dialog && force);
    page->text_retry = false;
    if (changed) {
        static const music_icon_id_t mode_icons[] = {MUSIC_ICON_REPEAT, MUSIC_ICON_REPEAT, MUSIC_ICON_REPEAT_ONE, MUSIC_ICON_SHUFFLE};
        text_id_t heading = page->picking_playlist ? T_choose_playlist :
                           page->view == VIEW_NOW || page->view == VIEW_QUEUE ? view_title(page->view) : T_title;
        set_text(page, page->heading, text(heading));
        for (unsigned i = 0; i < page->button_count; ++i) {
            button_t *item = &page->buttons[i];
            text_id_t title = item->text;
            bool available = !page->stopped;
            if (item->action == A_PLAY) {
                title = snapshot->playing ? T_pause : T_play;
                change_icon(item, page->view == VIEW_NOW ?
                    (snapshot->playing ? MUSIC_ICON_PAUSE_LARGE : MUSIC_ICON_PLAY_LARGE) :
                    (snapshot->playing ? MUSIC_ICON_PAUSE : MUSIC_ICON_PLAY));
            }
            if (item->action == A_MODE) {
                title = T_mode_sequence + (snapshot->mode < 4 ? snapshot->mode : 0);
                change_icon(item, mode_icons[snapshot->mode < 4 ? snapshot->mode : 0]);
                visible(item->marker, snapshot->mode > 0);
            }
            if (item->action == A_TIMER) {
                title = timer_text(snapshot->timer_minutes);
                visible(item->marker, snapshot->timer_minutes != 0);
            }
            const char *caption = item->action == A_VOLUME_DOWN ? "− 5%" : item->action == A_VOLUME_UP ? "+ 5%" : text(title);
            set_text(page, item->label, caption);
            if (item->action == A_PAGE_PREV) available &= !page->list_pending && page->query.first > 0;
            if (item->action == A_PAGE_NEXT) available &= !page->list_pending && page->query.first < page->total &&
                                                        page->total - page->query.first > MUSIC_PAGE_ROWS;
            if (item->action == A_PAGE_PREV || item->action == A_PAGE_NEXT)
                visible(item->object, page->total > MUSIC_PAGE_ROWS);
            if (item->action == A_RESET) visible(item->object, page->query.filter[0] != 0);
            if (item->action == A_VOLUME || item->action == A_VOLUME_DOWN || item->action == A_VOLUME_UP)
                available &= snapshot->volume_available && snapshot->volume <= 100;
            if (item->action == A_PLAY || item->action == A_PREV || item->action == A_NEXT || item->action == A_STOP)
                available &= snapshot->current_id != 0 || snapshot->current_path[0] != 0;
            if (item->action == A_ROW_MENU) {
                unsigned n = (unsigned)item->value;
                bool show = n < page->count && !page->displayed[n].directory &&
                            !is_m3u8(page->displayed[n].path) && !page->picking_playlist;
                visible(item->object, show);
                available &= show && !page->list_pending;
            }
            if (item->action != A_UP && item->action != A_DOWN) enabled(item->object, available);
        }
        const char *track = snapshot->title[0] ? snapshot->title :
                            snapshot->current_path[0] ? basename_of(snapshot->current_path) : text(T_no_track);
        if (is_list(page->view)) {
            render_breadcrumb(page);
            for (unsigned i = 0; i < MUSIC_PAGE_ROWS; ++i) {
                visible(page->rows[i], i < page->count);
                if (i < page->count) {
                    char name[sizeof(page->displayed[i].name)];
                    copy_text(name, sizeof(name), page->displayed[i].name);
                    if (!page->displayed[i].directory) {
                        char *dot = strrchr(name, '.');
                        if (dot && (!strcmp(dot, ".mp3") || !strcmp(dot, ".wav") || !strcmp(dot, ".flac") ||
                                    !strcmp(dot, ".MP3") || !strcmp(dot, ".WAV") || !strcmp(dot, ".FLAC"))) *dot = 0;
                    }
                    set_text(page, page->row_labels[i], name);
                    enabled(page->rows[i], !page->stopped && !page->list_pending &&
                            (!page->picking_playlist || page->displayed[i].directory || is_m3u8(page->displayed[i].path)));
                }
            }
            char number[48];
            size_t pages = page->total ? 1 + (page->total - 1) / MUSIC_PAGE_ROWS : 1;
            snprintf(number, sizeof(number), "%lu / %lu", (unsigned long)(page->query.first / MUSIC_PAGE_ROWS + 1),
                     (unsigned long)pages);
            set_text(page, page->page_number, number);
            visible(page->page_number, pages > 1);
            set_text(page, page->mini_title, track);
            set_text(page, page->mini_artist, snapshot->artist);
        } else {
            if (strcmp(lv_label_get_text(page->track), track)) relayout = true;
            set_text(page, page->track, track);
            set_text(page, page->artist, snapshot->artist);
            enabled(page->progress, !page->stopped && snapshot->can_seek && snapshot->current_id && snapshot->duration_seconds);
        }
        char message[STORAGE_PATH_MAX + 160];
        const char *status = text(page->local_status);
        if (page->stopped) status = text(T_unavailable);
        else if (page->content_error) status = app_content_error(page->content_error);
        else if (snapshot->result != MUSIC_OK && !page->operation_pending && !page->list_pending && page->local_status != T_busy) {
            snprintf(message, sizeof(message), "%s%s%s", text(result_text(snapshot->result)),
                     snapshot->result_path[0] ? ": " : "", basename_of(snapshot->result_path));
            status = message;
        } else if (page->local_status == T_none && !page->operation_pending && !page->list_pending &&
                   is_list(page->view) && !page->count) status = text(T_empty);
        else if (page->view == VIEW_NOW && page->local_status == T_none) {
            if (snapshot->state == MUSIC_OPENING) status = text(T_opening);
            else if (snapshot->state == MUSIC_SEEKING) status = text(T_seeking);
            else if (snapshot->state == MUSIC_SUSPENDED) status = text(T_suspended);
        }
        set_text(page, page->status, status);
        if (page->dialog) {
            text_id_t title = page->dialog_kind == DIALOG_SEARCH ? T_search : page->dialog_kind == DIALOG_SAVE ? T_name :
                page->dialog_kind == DIALOG_MODE ? T_mode : page->dialog_kind == DIALOG_TIMER ? T_timer :
                page->dialog_kind == DIALOG_DETAILS ? T_details : page->dialog_kind == DIALOG_VOLUME ? T_volume_title :
                page->dialog_kind == DIALOG_CLEAR ? T_clear : T_more;
            set_text(page, page->dialog_title, page->dialog_kind == DIALOG_ROW ? page->selected.name : text(title));
            if (page->dialog_hint) set_text(page, page->dialog_hint, text(T_clear_confirm));
            if (page->dialog_status) {
                set_text(page, page->dialog_status, text(page->input ? page->input_status :
                         page->local_status == T_busy ? T_busy : T_none));
                visible(page->dialog_status, page->input ? page->input_status != T_none : page->local_status == T_busy);
            }
            if (page->details) {
                char info[STORAGE_PATH_MAX * 2 + sizeof(snapshot->artist) + sizeof(snapshot->album) + 176];
                char format[160] = "", value[48];
                copy_text(format, sizeof(format), snapshot->format);
                if (snapshot->sample_rate) {
                    snprintf(value, sizeof(value), "%lu Hz", (unsigned long)snapshot->sample_rate);
                    append_info(format, sizeof(format), value);
                }
                if (snapshot->channels) {
                    EPD_FMT_music_channels(value, sizeof(value), snapshot->channels);
                    append_info(format, sizeof(format), value);
                }
                if (snapshot->bits_per_sample) {
                    EPD_FMT_music_bits(value, sizeof(value), snapshot->bits_per_sample);
                    append_info(format, sizeof(format), value);
                }
                snprintf(info, sizeof(info), "%s\n\n%s%s%s%s%s\n\n%s", track,
                         snapshot->artist, snapshot->artist[0] ? "\n" : "", snapshot->album,
                         snapshot->album[0] ? "\n\n" : "", format, snapshot->current_path);
                set_text(page, page->details, info);
            }
            if (page->volume_slider) {
                enabled(page->volume_slider, snapshot->volume_available);
                if (!page->volume_dragging) {
                    char value[32];
                    snprintf(value, sizeof(value), snapshot->volume_available ? "%u%%" : "--", snapshot->volume);
                    set_text(page, page->volume_label, value);
                    lv_slider_set_value(page->volume_slider, snapshot->volume_available ? snapshot->volume : 0, LV_ANIM_OFF);
                }
            }
        }
    }
    if (page->view == VIEW_NOW && !page->seek_dragging &&
        (force || snapshot->current_id != page->shown_id || snapshot->state != page->shown_state ||
         snapshot->duration_seconds != page->shown_duration || snapshot->duration_estimated != page->shown_estimated ||
         lv_tick_elaps(page->progress_tick) >= 5000)) {
        progress_text(page, snapshot->position_seconds);
        uint32_t position = snapshot->position_seconds > snapshot->duration_seconds ? snapshot->duration_seconds : snapshot->position_seconds;
        int target = snapshot->duration_seconds ? (int)((uint64_t)position * 1000 / snapshot->duration_seconds) : 0;
        if (lv_slider_get_value(page->progress) != target) lv_slider_set_value(page->progress, target, LV_ANIM_OFF);
        if (!page->text_retry) page->progress_tick = lv_tick_get();
        page->shown_id = snapshot->current_id;
        page->shown_state = snapshot->state;
        page->shown_duration = snapshot->duration_seconds;
        page->shown_estimated = snapshot->duration_estimated;
    }
    if (!page->text_retry) {
        page->locale_revision = locale;
        page->rendered_revision = snapshot->revision;
        page->rendered_list = snapshot->list_revision;
        page->rendered_result = snapshot->result_revision;
    }
    if (relayout) layout(page);
    page->force = false;
}

void music_view_update(void)
{
    music_page_t *page = current_page;
    if (!page || !page->foreground || page->stopped) return;
    bool was_ready = page->snapshot.ready;
    music_snapshot_read(&page->snapshot);
    music_snapshot_t *snapshot = &page->snapshot;
    bool restored_folder = restore_folder(page);
    if (snapshot->ready && (restored_folder || !was_ready) && is_list(page->view)) request_list(page);
    if (page->launch_path[0] && snapshot->ready) {
        music_command_t command = {.kind = is_m3u8(page->launch_path) ? MUSIC_CMD_IMPORT : MUSIC_CMD_OPEN,
                                   .list = MUSIC_LIST_FILES};
        copy_text(command.path, sizeof(command.path), page->launch_path);
        page->launch_path[0] = 0;
        if (submit(page, &command) && command.kind != MUSIC_CMD_IMPORT) show_view(page, VIEW_NOW);
    }
    bool new_result = snapshot->result_revision != page->operation_result;
    if (page->operation_pending && new_result) {
        page->operation_pending = false;
        page->local_status = snapshot->result == MUSIC_OK && page->save_pending ? T_saved : T_none;
        page->save_pending = false;
        page->force = true;
        if (page->import_pending) {
            page->import_pending = false;
            if (snapshot->result == MUSIC_OK) {
                page->picking_playlist = false;
                show_view(page, VIEW_QUEUE);
            }
        }
    }
    if (is_list(page->view)) {
        bool matches = snapshot->list == page->query.list && snapshot->first == page->query.first &&
                       !strcmp(snapshot->filter, page->query.filter) &&
                       (page->view != VIEW_FILES || !strcmp(snapshot->directory, page->query.path));
        if (matches && !snapshot->list_loading && snapshot->list_revision != page->requested_list) {
            page->requested_list = snapshot->list_revision;
            page->list_pending = false;
            page->count = snapshot->count < MUSIC_PAGE_ROWS ? snapshot->count : MUSIC_PAGE_ROWS;
            page->total = snapshot->total;
            memcpy(page->displayed, snapshot->entries, sizeof(page->displayed));
            page->layout_dirty = true;
            if (page->local_status == T_loading) page->local_status = T_none;
            page->force = true;
            if (!page->count && page->query.first && snapshot->result == MUSIC_OK) {
                page->query.first = snapshot->total ? ((snapshot->total - 1) / MUSIC_PAGE_ROWS) * MUSIC_PAGE_ROWS : 0;
                request_list(page);
            }
        } else if (page->list_pending && snapshot->result_revision != page->request_result && snapshot->result != MUSIC_OK) {
            page->list_pending = false;
            page->local_status = T_none;
            page->count = page->total = 0;
            page->force = true;
        }
        if (page->list_pending && lv_tick_elaps(page->list_tick) >= 200 && page->local_status != T_loading) {
            page->local_status = T_loading;
            page->force = true;
        }
    }
    render(page);
}

bool music_view_present(void)
{
    return current_page != NULL;
}

void music_view_stop(void)
{
    music_page_t *page = current_page;
    if (!page) return;
    page->stopped = true;
    timer_release(page);
    cover_release(page);
    dialog_close(page);
    page->force = true;
    render(page);
    current_page = NULL;
    music_ui_active(false);
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    music_page_t **owner = gui_app_this_page_memory();
    music_page_t *page = *owner;
    if (message != GUI_APP_MSG_ONSTART && !page) return;
    if (message == GUI_APP_MSG_ONSTART) {
        gui_app_close_anim();
        page = epd_app_alloc(sizeof(*page), EPD_APP_PSRAM);
        if (!page) {
            rt_kprintf("[music] start failed: stage=page_alloc page_bytes=%lu app_psram_free=%lu\n",
                       (unsigned long)sizeof(*page), (unsigned long)epd_app_psram_available());
            incoming_path[0] = 0;
            incoming_error = RT_EOK;
            gui_app_goback();
            return;
        }
        memset(page, 0, sizeof(*page));
        *owner = page;
        current_page = page;
        page->screen = lv_screen_active();
        epd_app_object_init(page->screen);
        lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(page->screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(page->screen, lv_color_black(), 0);
        lv_obj_set_style_text_font(page->screen, epd_app_font(), 0);
        epd_app_input_group(page->screen);
        epd_app_header(page->screen);
        page->back = epd_app_back_button_cb(page->screen, back_event, page);
        page->heading = epd_app_label(page->screen, "", 104, 81, 540, epd_app_font_role(EPD_FONT_TITLE));
        copy_text(page->folder, sizeof(page->folder), "/sdcard");
        copy_text(page->launch_path, sizeof(page->launch_path), incoming_path);
        page->content_error = incoming_error;
        incoming_path[0] = 0;
        incoming_error = RT_EOK;
        page->foreground = true;
        page->locale_revision = epd_locale_revision() - 1u;
        music_snapshot_read(&page->snapshot);
        restore_folder(page);
        music_ui_active(true);
        if (!music_start()) page->local_status = T_unavailable;
        show_view(page, VIEW_HOME);
    } else if (message == GUI_APP_MSG_ONRESUME) {
        if (page->stopped) return;
        current_page = page;
        page->foreground = true;
        page->force = true;
        music_ui_active(true);
        if (is_list(page->view)) request_list(page);
        epd_app_header_refresh(page->screen);
        if (!page->timer) page->timer = lv_timer_create(poll, 250, page);
        music_view_update();
    } else if (message == GUI_APP_MSG_ONPAUSE) {
        page->foreground = false;
        page->seek_dragging = false;
        timer_release(page);
        cover_release(page);
        music_ui_active(false);
    } else if (message == GUI_APP_MSG_ONSTOP) {
        page->stopped = true;
        timer_release(page);
        cover_release(page);
        music_ui_active(false);
        if (current_page == page) current_page = NULL;
        /* The SDK deletes the screen after ONSTOP. Delete its children while
         * their event callbacks can still access the page state. */
        lv_obj_clean(page->screen);
        icons_release();
        *owner = NULL;
        epd_app_free(page);
    }
}

int music_view_open(intent_t intent)
{
    incoming_path[0] = 0;
    incoming_error = app_content_receive(intent, incoming_path, sizeof(incoming_path));
    if (incoming_error != RT_EOK) incoming_path[0] = 0;
    if (incoming_error == -ENOENT) incoming_error = RT_EOK;
    return gui_app_regist_msg_handler_ext("music", page_message, NULL, sizeof(music_page_t *));
}
