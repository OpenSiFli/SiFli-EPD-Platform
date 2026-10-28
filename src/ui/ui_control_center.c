#include "storage_internal.h"
#include "platform/epd_system_internal.h"
#include "storage_file.h"
#include "platform/app_image.h"
#include "platform/epd_locale.h"
#include "platform/epd_app_font.h"
#include "ui_control_center.h"
#include "ui_shell.h"
#include "ui_navigation.h"
#include "ui_settings.h"
#include "frontlight.h"
#include "platform/epd_app.h"
#include "platform/epd_system.h"
#include "platform/epd_sound.h"
#include "platform/epd_i18n.h"
#include "platform/epd_input.h"
#include "platform/app_catalog.h"
#include "platform/app_recovery.h"
#include "icons/ui_icons.h"
#include <rtdevice.h>
#include <stdio.h>
#include <string.h>

enum { SCREEN_W = 684, SCREEN_H = 1216, NOTICE_ROWS = 8, TRACK_X = 71, TRACK_W = 596 };
typedef enum
{
    ACTION_NONE, ACTION_CLOSE, ACTION_CONTROLS, ACTION_NOTICES, ACTION_LOCK, ACTION_SETTINGS,
    ACTION_WIFI, ACTION_BLUETOOTH, ACTION_WIFI_SETTINGS, ACTION_BT_SETTINGS,
    ACTION_LIGHT_TOGGLE, ACTION_VOLUME_TOGGLE, ACTION_PREVIOUS, ACTION_PLAY, ACTION_NEXT,
    ACTION_OPEN_NOTICE, ACTION_DISMISS_NOTICE
} action_t;
typedef struct
{
    lv_obj_t *panel, *icon, *name, *text, *remove;
    char app_id[EPD_SYSTEM_APP_ID_SIZE], id[EPD_NOTIFICATION_ID_SIZE];
    bool populated;
} notice_row_t;
static struct
{
    lv_obj_t *root, *content, *error, *empty, *dots;
    lv_obj_t *light, *volume, *light_button, *volume_button, *wifi, *bluetooth;
    lv_obj_t *artist, *title, *previous, *play, *next, *play_icon;
    lv_obj_t *tabs[2], *tab_labels[2], *lock, *settings, *media_panel, *media_icon, *handle;
    notice_row_t rows[NOTICE_ROWS];
    epd_media_t media;
    char media_navigation[EPD_SYSTEM_APP_ID_SIZE];
    lv_font_t *heading_font, *name_font, *heading_fallback, *name_fallback;
    bool notifications, has_media, dirty, failed, text_retry;
    size_t page, total, page_rows;
    size_t dots_pages, dots_page;
    unsigned dots_percent;
    int page_delta, light_request, volume_request;
    unsigned light_level, light_restore, volume_level, volume_max, font_percent;
    bool light_available, volume_muted;
    uint32_t notices_revision, media_revision, locale_revision, catalog_revision, scale_revision;
    action_t action;
    char action_app[EPD_SYSTEM_APP_ID_SIZE], action_id[EPD_NOTIFICATION_ID_SIZE];
} center = {.light_request = -1, .volume_request = -1, .light_restore = 4};
static int deferred_settings_page = -1;

static void rebuild(void);
static void center_layout(void);
static void center_track_layout(lv_obj_t *track, int x, int y, int w, int h);

static void center_font_select(void)
{
    unsigned percent = epd_ui_scale_percent();
    if (center.font_percent == percent) return;
    uint32_t identity;
    lv_font_t *old[] = {center.heading_font, center.name_font, center.heading_fallback, center.name_fallback};
    center.heading_font = epd_app_font_create(0, epd_ui_px(48), 0, &identity, &center.heading_fallback);
    center.name_font = epd_app_font_create(0, epd_ui_px(36), 0, &identity, &center.name_fallback);
    center.font_percent = percent;
    if (center.content)
    {
        const lv_font_t *heading = center.heading_font ? center.heading_font : epd_app_font_role(EPD_FONT_TITLE);
        const lv_font_t *name = center.name_font ? center.name_font : epd_app_font_role(EPD_FONT_BODY);
        for (unsigned i = 0; i < 2; ++i) lv_obj_set_style_text_font(center.tab_labels[i], heading, 0);
        for (unsigned i = 0; i < NOTICE_ROWS; ++i)
            if (center.rows[i].name) lv_obj_set_style_text_font(center.rows[i].name, name, 0);
        if (!center.notifications)
        {
            lv_obj_set_style_text_font(center.artist, name, 0);
            lv_obj_set_style_text_font(center.title, heading, 0);
        }
    }
    lv_draw_wait_for_finish();
    for (unsigned i = 0; i < 4; ++i) epd_app_font_destroy(old[i]);
}

static void center_text(lv_obj_t *label, const char *text)
{
    if (!epd_app_text(label, text)) center.text_retry = true;
}

static const lv_font_t *heading_font(void)
{ return center.heading_font ? center.heading_font : epd_app_font_role(EPD_FONT_TITLE); }
static const lv_font_t *name_font(void)
{ return center.name_font ? center.name_font : epd_app_font_role(EPD_FONT_BODY); }

static const epd_app_entry_t *catalog_entry(const char *id)
{
    for (unsigned i = 0; i < app_catalog_count(); ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get(i);
        if (entry && !strcmp(entry->id, id)) return entry;
    }
    return NULL;
}

static bool consume_move(const epd_input_event_t *events, unsigned count)
{ (void)events; (void)count; return true; }

static bool consume_key(uint32_t key)
{
    if (key == LV_KEY_ESC) center.action = ACTION_CLOSE;
    return true;
}

static const epd_input_ops_t center_keys = {.move = consume_move, .key = consume_key};

static void disable(lv_obj_t *obj, bool disabled)
{
    if (!obj) return;
    if (disabled != lv_obj_has_state(obj, LV_STATE_DISABLED))
    {
        if (disabled) lv_obj_add_state(obj, LV_STATE_DISABLED);
        else lv_obj_remove_state(obj, LV_STATE_DISABLED);
    }
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, bool border)
{
    lv_obj_t *obj = epd_app_panel(parent, x, y, w, h);
    lv_obj_set_style_border_width(obj, border ? 5 : 0, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x747474), 0);
    lv_obj_set_style_radius(obj, border ? 26 : 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return obj;
}

static void picture_fit(lv_obj_t *image, int side)
{
    lv_obj_set_size(image, side, side);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
}

static lv_obj_t *picture(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y, int side)
{
    lv_obj_t *image = epd_app_icon(parent, src, x, y);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(image, LV_OBJ_FLAG_GESTURE_BUBBLE);
    picture_fit(image, side);
    return image;
}

static void action_event(lv_event_t *event)
{
    action_t action = (action_t)(uintptr_t)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SHORT_CLICKED) center.action = action;
    else if (code == LV_EVENT_LONG_PRESSED)
    {
        if (action == ACTION_WIFI) center.action = ACTION_WIFI_SETTINGS;
        if (action == ACTION_BLUETOOTH) center.action = ACTION_BT_SETTINGS;
    }
}

static lv_obj_t *icon_button(lv_obj_t *parent, const lv_image_dsc_t *src,
                              int x, int y, int w, int h, int side, bool border, action_t action)
{
    lv_obj_t *button = box(parent, x, y, w, h, border);
    if (!border) lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    picture(button, src, (w - side) / 2, (h - side) / 2, side);
    lv_obj_add_event_cb(button, action_event, LV_EVENT_ALL, (void *)(uintptr_t)action);
    return button;
}

static void track_event(lv_event_t *event)
{
    lv_indev_t *input = lv_indev_active();
    if (!input || lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    lv_obj_t *track = lv_event_get_current_target(event);
    lv_area_t area;
    lv_point_t point;
    lv_obj_get_coords(track, &area);
    lv_indev_get_point(input, &point);
    bool volume = (uintptr_t)lv_event_get_user_data(event) != 0;
    unsigned maximum = volume ? center.volume_max : 8;
    if (!maximum) return;
    int x = point.x - area.x1;
    if (x < 0) x = 0;
    int width = lv_area_get_width(&area);
    if (width <= 0) return;
    if (x >= width) x = width - 1;
    int value = (int)((unsigned)x * maximum / width) + 1;
    if (volume) center.volume_request = value;
    else center.light_request = value;
}

static lv_obj_t *track_create(lv_obj_t *parent, int y, unsigned maximum, bool volume)
{
    /* The visible bars are 50 px high; the touch target extends above/below. */
    lv_obj_t *track = box(parent, TRACK_X, y - 15, TRACK_W, 80, false);
    lv_obj_add_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(track, track_event, LV_EVENT_RELEASED, (void *)(uintptr_t)volume);
    lv_obj_t *bar = box(track, 0, 15, TRACK_W, 50, false);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    for (unsigned i = 0; i < maximum; ++i)
    {
        lv_obj_t *segment = box(bar, 0, 0, 1, 50, false);
        lv_obj_set_style_bg_color(segment, lv_color_hex(0xc4c4c4), 0);
        lv_obj_remove_flag(segment, LV_OBJ_FLAG_CLICKABLE);
    }
    center_track_layout(track, TRACK_X, y - 15, TRACK_W, 80);
    disable(track, maximum == 0);
    return track;
}

static void track_paint(lv_obj_t *track, unsigned level)
{
    if (!track) return;
    lv_obj_t *bar = lv_obj_get_child(track, 0);
    unsigned count = lv_obj_get_child_count(bar);
    for (unsigned i = 0; i < count; ++i)
    {
        lv_obj_t *segment = lv_obj_get_child(bar, i);
        lv_color_t color = lv_color_hex(i < level ? 0x333333 : 0xc4c4c4);
        if (!lv_color_eq(lv_obj_get_style_bg_color(segment, 0), color))
            lv_obj_set_style_bg_color(segment, color, 0);
    }
}

static void read_controls(void)
{
    frontlight_snapshot_t light;
    frontlight_get(&light);
    center.light_available = light.available && !light.locked;
    center.light_level = light.output_valid ? ((unsigned)light.output_percent * 8u + 50u) / 100u : 0;
    if (center.light_level > 8) center.light_level = 8;
    if (center.light_level) center.light_restore = center.light_level;
    epd_sound_snapshot_t sound = {0};
    epd_sound_read(&sound);
    center.volume_max = sound.initialized && sound.available && sound.output_ready ? sound.max_level : 0;
    center.volume_level = ((unsigned)sound.volume[EPD_SOUND_MASTER] * center.volume_max + 50u) / 100u;
    center.volume_muted = sound.muted;
}

static void gesture(lv_event_t *event)
{
    if (!center.notifications || lv_event_get_code(event) != LV_EVENT_GESTURE) return;
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(input);
    if (direction == LV_DIR_LEFT) center.page_delta = 1;
    else if (direction == LV_DIR_RIGHT) center.page_delta = -1;
    else return;
    lv_indev_wait_release(input);
    lv_event_stop_bubbling(event);
}

static void notice_event(lv_event_t *event)
{
    notice_row_t *row = lv_event_get_user_data(event);
    if (!row->populated) return;
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED)
    {
        for (unsigned i = 0; i < NOTICE_ROWS; ++i)
            if (center.rows[i].remove) lv_obj_add_flag(center.rows[i].remove, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(row->remove, LV_OBJ_FLAG_HIDDEN);
    }
    else if (lv_event_get_code(event) == LV_EVENT_SHORT_CLICKED)
    {
        center.action = lv_event_get_current_target(event) == row->remove ?
                        ACTION_DISMISS_NOTICE : ACTION_OPEN_NOTICE;
        strcpy(center.action_app, row->app_id);
        strcpy(center.action_id, row->id);
    }
}

static void notice_create(unsigned index, int y)
{
    notice_row_t *row = &center.rows[index];
    row->panel = box(center.content, 13, y, 658, 120, true);
    lv_obj_add_flag(row->panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row->panel, notice_event, LV_EVENT_ALL, row);
    row->icon = picture(row->panel, &ui_icon_quick, 24, 20, 80);
    row->name = epd_app_label(row->panel, "", 120, 12, 455, name_font());
    row->text = epd_app_label(row->panel, "", 120, 53, 455, epd_app_font_role(EPD_FONT_BODY));
    lv_obj_set_height(row->text, 60);
    row->remove = box(row->panel, 580, 8, 68, 104, false);
    lv_obj_set_style_bg_opa(row->remove, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row->remove, 18, 0);
    picture(row->remove, &ui_icon_notice_close, 4, 22, 60);
    lv_obj_add_flag(row->remove, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(row->remove, notice_event, LV_EVENT_SHORT_CLICKED, row);
}

static void notices_render(void)
{
    epd_notification_t values[NOTICE_ROWS];
    size_t count = center.notifications ? center.page_rows : 3;
    size_t copied = epd_notification_read(center.notifications ? center.page * count : 0,
                                         values, count, &center.total, &center.notices_revision);
    size_t pages = center.total ? (center.total - 1) / count + 1 : 1;
    if (center.page >= pages)
    {
        center.page = pages - 1;
        copied = epd_notification_read(center.page * count, values, count,
                                       &center.total, &center.notices_revision);
    }
    for (size_t i = 0; i < NOTICE_ROWS; ++i)
    {
        notice_row_t *row = &center.rows[i];
        if (!row->panel) continue;
        bool populated = i < copied;
        bool changed = row->populated != populated || (populated &&
            (strcmp(row->app_id, values[i].app_id) || strcmp(row->id, values[i].id)));
        if (changed) lv_obj_add_flag(row->remove, LV_OBJ_FLAG_HIDDEN);
        row->populated = populated;
        if (!populated)
        {
            lv_obj_add_flag(row->panel, LV_OBJ_FLAG_HIDDEN);
            center_text(row->name, "");
            center_text(row->text, "");
            lv_obj_add_flag(row->icon, LV_OBJ_FLAG_HIDDEN);
            epd_app_image_release(row->icon);
            continue;
        }
        lv_obj_remove_flag(row->panel, LV_OBJ_FLAG_HIDDEN);
        strcpy(row->app_id, values[i].app_id);
        strcpy(row->id, values[i].id);
        const epd_app_entry_t *entry = catalog_entry(row->app_id);
        center_text(row->name, entry ? entry->name : row->app_id);
        center_text(row->text, values[i].text[0] ? values[i].text : values[i].title);
        const lv_image_dsc_t *fallback = &ui_icon_quick;
        const char *file = NULL;
        char path[256];
        if (entry && entry->icon)
        {
            if (lv_image_src_get_type(entry->icon) == LV_IMAGE_SRC_FILE) file = (const char *)entry->icon;
            else if (lv_image_src_get_type(entry->icon) == LV_IMAGE_SRC_VARIABLE) fallback = entry->icon;
        }
        if (entry && entry->icon_file[0] &&
            storage_app_path(path, sizeof(path), entry->id, STORAGE_APP_CODE, entry->icon_file)) file = path;
        epd_app_image_set(row->icon, file, fallback);
        picture_fit(row->icon, LV_MIN(epd_ui_px(80), lv_obj_get_height(row->panel) - epd_ui_px(24)));
        lv_obj_remove_flag(row->icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (center.empty)
    {
        if (copied) lv_obj_add_flag(center.empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(center.empty, LV_OBJ_FLAG_HIDDEN);
    }
    if (center.dots && (center.dots_pages != pages || center.dots_page != center.page ||
                        center.dots_percent != epd_ui_scale_percent()))
    {
        center.dots_pages = pages;
        center.dots_page = center.page;
        center.dots_percent = epd_ui_scale_percent();
        lv_obj_clean(center.dots);
        if (pages <= 1) { lv_obj_add_flag(center.dots, LV_OBJ_FLAG_HIDDEN); return; }
        lv_obj_remove_flag(center.dots, LV_OBJ_FLAG_HIDDEN);
        /* Indicator spacing contracts with the actual number of pages. */
        lv_obj_update_layout(center.dots);
        int width = lv_obj_get_width(center.dots);
        unsigned spacing = LV_MIN((unsigned)epd_ui_px(34), (unsigned)(width / pages));
        if (!spacing) spacing = 1;
        int start = (width - (int)(pages * spacing)) / 2;
        for (size_t i = 0; i < pages; ++i)
        {
            unsigned size = LV_MIN((unsigned)epd_ui_px(15), spacing > 1 ? spacing - 1 : 1);
            lv_obj_t *dot = box(center.dots, start + (int)(i * spacing), epd_ui_px(5), size, size, false);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, i == center.page ? lv_color_black() : lv_color_hex(0xc4c4c4), 0);
        }
    }
}

static bool media_pause_control(void)
{
    return center.has_media && (center.media.playing ||
        (!(center.media.capabilities & EPD_MEDIA_CAN_PLAY) &&
          (center.media.capabilities & EPD_MEDIA_CAN_PAUSE)));
}

static void controls_render(void)
{
    if (center.notifications) return;
    disable(center.light, !center.light_available);
    disable(center.light_button, !center.light_available);
    disable(center.volume_button, !center.volume_max);
    track_paint(center.light, center.light_level);
    track_paint(center.volume, center.volume_muted ? 0 : center.volume_level);
    lv_color_t on = lv_color_hex(0xdddddd), off = lv_color_white();
    lv_color_t wifi = ui_settings_enabled(UI_SETTING_WIFI) ? on : off;
    lv_color_t bt = ui_settings_enabled(UI_SETTING_BLUETOOTH) ? on : off;
    if (!lv_color_eq(lv_obj_get_style_bg_color(center.wifi, 0), wifi))
        lv_obj_set_style_bg_color(center.wifi, wifi, 0);
    if (!lv_color_eq(lv_obj_get_style_bg_color(center.bluetooth, 0), bt))
        lv_obj_set_style_bg_color(center.bluetooth, bt, 0);
    const char *artist = center.has_media ? center.media.artist : "";
    if (center.has_media && strcmp(center.media_navigation, center.media.app_id))
    {
        const epd_app_entry_t *entry = catalog_entry(center.media_navigation);
        artist = entry ? entry->name : center.media_navigation;
    }
    center_text(center.artist, artist);
    center_text(center.title, center.has_media ? center.media.title : epd_get_str(platform_no_media));
    const lv_image_dsc_t *source = media_pause_control() ? &ui_icon_pause : &ui_icon_play;
    if (lv_image_get_src(center.play_icon) != source) lv_image_set_src(center.play_icon, source);
    uint32_t flags = center.has_media ? center.media.capabilities : 0;
    disable(center.previous, !(flags & EPD_MEDIA_CAN_PREVIOUS));
    disable(center.next, !(flags & EPD_MEDIA_CAN_NEXT));
    disable(center.play, !(flags & (media_pause_control() ? EPD_MEDIA_CAN_PAUSE : EPD_MEDIA_CAN_PLAY)));
}

static void center_button_layout(lv_obj_t *button, int x, int y, int w, int h, int icon)
{
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    picture_fit(lv_obj_get_child(button, 0), icon);
    lv_obj_center(lv_obj_get_child(button, 0));
}

static void center_track_layout(lv_obj_t *track, int x, int y, int w, int h)
{
    lv_obj_set_pos(track, x, y);
    lv_obj_set_size(track, w, h);
    lv_obj_t *bar = lv_obj_get_child(track, 0);
    int bar_h = LV_MAX(1, LV_MIN(epd_ui_px(50), h - epd_ui_px(8)));
    lv_obj_set_pos(bar, 0, (h - bar_h) / 2);
    lv_obj_set_size(bar, w, bar_h);
    unsigned count = lv_obj_get_child_count(bar);
    for (unsigned i = 0; i < count; ++i)
    {
        /* Match the integer intervals used by track_event(). */
        int left = (int)((i * w + count - 1) / count);
        int right = (int)(((i + 1) * w + count - 1) / count);
        int gap = i + 1 < count ? LV_MIN(epd_ui_px(2), LV_MAX(0, right - left - 1)) : 0;
        lv_obj_t *segment = lv_obj_get_child(bar, i);
        lv_obj_set_pos(segment, left, 0);
        lv_obj_set_size(segment, LV_MAX(1, right - left - gap), bar_h);
    }
}

static void center_notice_layout(notice_row_t *row, int y, int height)
{
    int pad = epd_ui_px(13), width = SCREEN_W - 2 * pad;
    int icon = LV_MIN(epd_ui_px(80), height - epd_ui_px(24));
    int remove_w = epd_ui_touch(68), text_x = epd_ui_px(27) + icon + epd_ui_px(16);
    int text_w = width - text_x - remove_w - epd_ui_px(18);
    lv_obj_set_pos(row->panel, pad, y);
    lv_obj_set_size(row->panel, width, height);
    lv_obj_set_style_radius(row->panel, epd_ui_px(26), 0);
    lv_obj_set_style_border_width(row->panel, epd_ui_px(5), 0);
    picture_fit(row->icon, icon);
    lv_obj_set_pos(row->icon, epd_ui_px(24), (height - icon) / 2);
    lv_obj_set_style_text_font(row->name, name_font(), 0);
    lv_obj_set_pos(row->name, text_x, epd_ui_px(16));
    lv_obj_set_size(row->name, text_w, name_font()->line_height);
    const lv_font_t *body = epd_app_font_role(EPD_FONT_BODY);
    lv_obj_set_style_text_font(row->text, body, 0);
    int text_y = epd_ui_px(16) + name_font()->line_height;
    lv_obj_set_pos(row->text, text_x, text_y);
    lv_obj_set_size(row->text, text_w, height - text_y - epd_ui_px(10));
    lv_obj_set_pos(row->remove, width - remove_w - epd_ui_px(8), epd_ui_px(8));
    lv_obj_set_size(row->remove, remove_w, height - epd_ui_px(16));
    picture_fit(lv_obj_get_child(row->remove, 0), epd_ui_px(60));
    lv_obj_center(lv_obj_get_child(row->remove, 0));
}

static void center_layout(void)
{
    center_font_select();
    int margin = epd_ui_px(32), gap = epd_ui_px(16);
    int tab_space = SCREEN_W - 2 * margin - gap;
    lv_point_t natural[2];
    for (unsigned i = 0; i < 2; ++i)
    {
        /* Dots mode temporarily shortens the text; restore it before measuring. */
        lv_label_set_long_mode(center.tab_labels[i], LV_LABEL_LONG_WRAP);
        lv_text_get_size(&natural[i], lv_label_get_text(center.tab_labels[i]), heading_font(),
                         0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    int tab_w[2];
    tab_w[0] = tab_space * natural[0].x / LV_MAX(1, natural[0].x + natural[1].x);
    tab_w[0] = LV_CLAMP(56, tab_w[0], tab_space - 56);
    tab_w[1] = tab_space - tab_w[0];
    int top = epd_ui_px(90), tab_h = epd_ui_touch(80);
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_point_t measured;
        lv_text_get_size(&measured, lv_label_get_text(center.tab_labels[i]), heading_font(),
                         0, 0, tab_w[i], LV_TEXT_FLAG_NONE);
        tab_h = LV_MAX(tab_h, measured.y + epd_ui_px(16));
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_set_pos(center.tabs[i], margin + (i ? tab_w[0] + gap : 0), top);
        lv_obj_set_size(center.tabs[i], tab_w[i], tab_h);
        lv_obj_set_style_text_font(center.tab_labels[i], heading_font(), 0);
        lv_obj_set_style_text_align(center.tab_labels[i], i ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_size(center.tab_labels[i], tab_w[i], LV_SIZE_CONTENT);
        lv_obj_center(center.tab_labels[i]);
    }
    top += tab_h;
    const lv_font_t *caption = epd_app_font_role(EPD_FONT_CAPTION);
    int bottom = SCREEN_H - epd_ui_px(72);
    lv_obj_set_style_text_font(center.error, caption, 0);
    lv_obj_set_pos(center.error, margin, bottom);
    lv_obj_set_size(center.error, SCREEN_W - 2 * margin, caption->line_height);
    int handle_w = LV_MIN(epd_ui_px(375), SCREEN_W - 2 * margin);
    lv_obj_set_pos(center.handle, (SCREEN_W - handle_w) / 2, SCREEN_H - epd_ui_px(19));
    lv_obj_set_size(center.handle, handle_w, epd_ui_px(15));
    int row_h = LV_MAX(epd_ui_px(120),
        name_font()->line_height + epd_app_font_role(EPD_FONT_BODY)->line_height + epd_ui_px(26));
    if (center.notifications)
    {
        top += epd_ui_px(18);
        int dots_h = epd_ui_px(30);
        int dots_y = SCREEN_H - epd_ui_px(66);
        lv_obj_set_pos(center.dots, margin, dots_y);
        lv_obj_set_size(center.dots, SCREEN_W - 2 * margin, dots_h);
        int notice_bottom = dots_y - caption->line_height - epd_ui_px(8);
        lv_obj_set_y(center.error, notice_bottom);
        int notice_gap = epd_ui_px(40), pitch = row_h + notice_gap;
        center.page_rows = LV_CLAMP(1, (notice_bottom - top + notice_gap) / pitch, NOTICE_ROWS);
        for (unsigned i = 0; i < center.page_rows; ++i)
            center_notice_layout(&center.rows[i], top + (int)i * pitch, row_h);
        lv_obj_set_style_text_font(center.empty, epd_app_font_role(EPD_FONT_BODY), 0);
        lv_obj_set_pos(center.empty, margin, top + epd_ui_px(20));
        lv_obj_set_size(center.empty, SCREEN_W - 2 * margin, LV_SIZE_CONTENT);
    }
    else
    {
        top += epd_ui_px(11);
        int pad = epd_ui_px(18), controls_h = epd_ui_touch(64);
        int minimum_panel = pad + LV_MAX(epd_ui_px(48), name_font()->line_height) + epd_ui_px(8) +
                            heading_font()->line_height + controls_h + epd_ui_px(12);
        int panel_h = LV_MAX(epd_ui_px(290), minimum_panel), track_h = epd_ui_touch(80);
        int track_gap = epd_ui_px(15), panel_gap = epd_ui_px(36);
        int notices_gap = epd_ui_px(50), row_gap = epd_ui_px(20);
        int overflow = 2 * track_h + track_gap + panel_gap + panel_h + notices_gap +
                       3 * row_h + 2 * row_gap - (bottom - top);
        /* Compress spacing before reducing controls; text keeps its scaled size. */
        int *spaces[] = {&notices_gap, &panel_gap, &track_gap, &row_gap};
        const int minimum[] = {16, 12, 8, 8};
        for (unsigned i = 0; i < 4 && overflow > 0; ++i)
        {
            int copies = i == 3 ? 2 : 1;
            int reduction = LV_MIN(*spaces[i] - epd_ui_px(minimum[i]), (overflow + copies - 1) / copies);
            *spaces[i] -= reduction;
            overflow -= reduction * copies;
        }
        if (overflow > 0)
        {
            int reduction = LV_MIN(overflow, panel_h - minimum_panel);
            panel_h -= reduction; overflow -= reduction;
            track_h = LV_MAX(56, track_h - (overflow + 1) / 2);
        }
        int track_x = epd_ui_touch(71);
        int track_w = SCREEN_W - track_x - epd_ui_px(17);
        center_button_layout(center.light_button, 0, top, track_x, track_h, epd_ui_px(48));
        center_track_layout(center.light, track_x, top, track_w, track_h);
        top += track_h + track_gap;
        center_button_layout(center.volume_button, 0, top, track_x, track_h, epd_ui_px(48));
        center_track_layout(center.volume, track_x, top, track_w, track_h);
        top += track_h + panel_gap;
        int notices_y = top + panel_h + notices_gap;
        int quick_margin = epd_ui_px(27), panel_width = (SCREEN_W - 2 * quick_margin - epd_ui_px(49)) / 2;
        int half_w = panel_width;
        int side = LV_MIN(epd_ui_touch(120), LV_MIN((half_w - gap) / 2, (panel_h - gap) / 2));
        lv_obj_t *quick[] = {center.wifi, center.bluetooth, center.lock, center.settings};
        for (unsigned i = 0; i < 4; ++i)
            center_button_layout(quick[i], quick_margin + (int)(i % 2) * (half_w - side),
                                 top + (int)(i / 2) * (panel_h - side), side, side, LV_MIN(epd_ui_px(80), side - epd_ui_px(16)));
        lv_obj_set_pos(center.media_panel, SCREEN_W - quick_margin - half_w, top);
        lv_obj_set_size(center.media_panel, half_w, panel_h);
        lv_obj_set_style_radius(center.media_panel, epd_ui_px(26), 0);
        lv_obj_set_style_border_width(center.media_panel, epd_ui_px(5), 0);
        controls_h = LV_MIN(epd_ui_touch(80), panel_h - minimum_panel + controls_h);
        int music_side = LV_MIN(epd_ui_px(80), panel_h - pad - epd_ui_px(20) -
                               heading_font()->line_height - controls_h);
        lv_obj_set_style_text_font(center.artist, name_font(), 0);
        lv_obj_set_pos(center.artist, pad, pad);
        lv_obj_set_size(center.artist, half_w - 2 * pad - music_side, name_font()->line_height);
        picture_fit(center.media_icon, music_side);
        lv_obj_set_pos(center.media_icon, half_w - pad - music_side, pad);
        int title_y = pad + LV_MAX(music_side, name_font()->line_height) + epd_ui_px(8);
        lv_obj_set_style_text_font(center.title, heading_font(), 0);
        lv_obj_set_pos(center.title, pad, title_y);
        lv_obj_set_size(center.title, half_w - 2 * pad, panel_h - title_y - controls_h - epd_ui_px(12));
        lv_obj_t *transport[] = {center.previous, center.play, center.next};
        int transport_w = (half_w - epd_ui_px(12)) / 3;
        for (unsigned i = 0; i < 3; ++i)
            center_button_layout(transport[i], epd_ui_px(6) + (int)i * transport_w,
                                 panel_h - controls_h - epd_ui_px(6), transport_w, controls_h,
                                 LV_MIN(epd_ui_px(80), transport_w - epd_ui_px(8)));
        for (unsigned i = 0; i < 3; ++i)
            center_notice_layout(&center.rows[i], notices_y + (int)i * (row_h + row_gap), row_h);
    }
    center.scale_revision = epd_ui_scale_revision();
}

static void rebuild(void)
{
    center.text_retry = false;
    if (center.content) lv_obj_delete(center.content);
    center.content = NULL;
    center_font_select();
    memset(center.rows, 0, sizeof(center.rows));
    center.dots = center.empty = NULL;
    center.dots_pages = 0;
    center.light = center.volume = NULL;
    center.content = box(center.root, 0, 0, SCREEN_W, SCREEN_H, false);
    lv_obj_move_to_index(center.content, 0);
    lv_obj_remove_flag(center.content, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(center.content, gesture, LV_EVENT_GESTURE, NULL);
    const char *titles[] = {epd_get_str(platform_notification_center), epd_get_str(platform_control_center)};
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_t *button = box(center.content, 23 + i * 331, 90, 307, 80, false);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(button, action_event, LV_EVENT_SHORT_CLICKED,
                             (void *)(uintptr_t)(i ? ACTION_CONTROLS : ACTION_NOTICES));
        lv_obj_t *label = epd_app_centered_label(button, titles[i], 0, 8, 307,
                                                  heading_font());
        center.tabs[i] = button;
        center.tab_labels[i] = label;
        lv_obj_set_style_text_color(label, ((i == 0) == center.notifications) ?
                                      lv_color_black() : lv_color_hex(0xa6a3a3), 0);
    }
    if (center.notifications)
    {
        for (unsigned i = 0; i < NOTICE_ROWS; ++i) notice_create(i, 192 + i * 160);
        center.empty = epd_app_centered_label(center.content, epd_get_str(platform_no_notifications),
                                               100, 220, 484, epd_app_font_role(EPD_FONT_BODY));
        center.dots = box(center.content, 32, 1150, 620, 30, false);
    }
    else
    {
        center.light_button = icon_button(center.content, &ui_icon_light, 0, 181, 70, 80, 48, false, ACTION_LIGHT_TOGGLE);
        center.volume_button = icon_button(center.content, &ui_icon_volume, 0, 276, 70, 80, 48, false, ACTION_VOLUME_TOGGLE);
        center.light = track_create(center.content, 196, 8, false);
        center.volume = track_create(center.content, 291, center.volume_max, true);
        center.wifi = icon_button(center.content, &ui_icon_wifi_entry, 27, 392, 120, 120, 72, true, ACTION_WIFI);
        center.bluetooth = icon_button(center.content, &ui_icon_bt_entry, 197, 392, 120, 120, 72, true, ACTION_BLUETOOTH);
        center.lock = icon_button(center.content, &ui_icon_lock, 27, 562, 120, 120, 72, true, ACTION_LOCK);
        center.settings = icon_button(center.content, &ui_icon_center_settings, 197, 562, 120, 120, 80, true, ACTION_SETTINGS);
        lv_obj_t *panel = box(center.content, 366, 392, 291, 290, true);
        center.media_panel = panel;
        center.artist = epd_app_label(panel, "", 18, 24, 184, name_font());
        center.media_icon = picture(panel, &ui_icon_music, 207, 20, 64);
        center.title = epd_app_label(panel, "", 18, 108, 255, heading_font());
        center.previous = icon_button(panel, &ui_icon_previous, 12, 198, 82, 80, 56, false, ACTION_PREVIOUS);
        center.play = icon_button(panel, &ui_icon_play, 103, 198, 82, 80, 56, false, ACTION_PLAY);
        center.play_icon = lv_obj_get_child(center.play, 0);
        center.next = icon_button(panel, &ui_icon_next, 194, 198, 82, 80, 56, false, ACTION_NEXT);
        for (unsigned i = 0; i < 3; ++i) notice_create(i, 732 + i * 140);
    }
    center.error = epd_app_centered_label(center.content, "", 32, 1115, 620,
                                           epd_app_font_role(EPD_FONT_CAPTION));
    lv_obj_t *handle = box(center.content, 157, 1197, 375, 15, false);
    center.handle = handle;
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0xe5e5e5), 0);
    center_layout();
    notices_render();
    controls_render();
    center.locale_revision = epd_locale_revision();
    center.dirty = false;
}

void ui_control_center_open(bool notifications)
{
    if (ui_shell_locked()) return;
    center.notifications = notifications;
    center.page = 0;
    center.failed = false;
    center.has_media = epd_media_read_display(&center.media, &center.media_revision,
        center.media_navigation, sizeof(center.media_navigation));
    read_controls();
    if (!center.root)
    {
        center.root = box(lv_layer_top(), 0, 0, SCREEN_W, SCREEN_H, false);
        lv_obj_add_flag(center.root, LV_OBJ_FLAG_CLICKABLE);
        lv_group_t *group = epd_app_input_group(center.root);
        if (!group) { ui_control_center_close(); return; }
        epd_input_set_ops(group, &center_keys);
    }
    lv_obj_move_foreground(center.root);
    rebuild();
}

void ui_control_center_close(void)
{
    if (!center.root) return;
    lv_obj_delete(center.root);
    lv_draw_wait_for_finish();
    epd_app_font_destroy(center.heading_font);
    epd_app_font_destroy(center.name_font);
    epd_app_font_destroy(center.heading_fallback);
    epd_app_font_destroy(center.name_fallback);
    center.font_percent = 0;
    center.heading_font = center.name_font = NULL;
    center.heading_fallback = center.name_fallback = NULL;
    center.root = center.content = NULL;
    center.action = ACTION_NONE;
    center.light_request = center.volume_request = -1;
    center.page_delta = 0;
    lv_obj_invalidate(lv_screen_active());
}

bool ui_control_center_visible(void) { return center.root != NULL; }

static bool open_notification(const char *app_id, const char *id)
{
    epd_notification_t notice;
    if (!epd_notification_find(app_id, id, &notice) || !catalog_entry(app_id) ||
        app_recovery_check_dynamic() != RT_EOK || storage_changing()) return false;
    intent_t intent = intent_init(app_id);
    if (!intent) return false;
    bool ok = true;
    if (notice.page[0])
    {
        ok = intent_set_string(intent, "page", notice.page) == RT_EOK;
        const char *readback = ok ? intent_get_string(intent, "page") : NULL;
        ok = readback && !strcmp(readback, notice.page);
    }
    if (ok) ok = intent_runapp(intent) == RT_EOK;
    intent_deinit(intent);
    if (ok) ui_control_center_close();
    return ok;
}

static bool perform(action_t action)
{
    switch (action)
    {
    case ACTION_CLOSE: ui_control_center_close(); return true;
    case ACTION_CONTROLS: case ACTION_NOTICES:
        center.notifications = action == ACTION_NOTICES;
        center.page = 0;
        center.dirty = true;
        return true;
    case ACTION_LOCK: ui_control_center_close(); ui_shell_lock(); return true;
    case ACTION_SETTINGS: ui_control_center_close(); ui_shell_settings(); return true;
    case ACTION_WIFI_SETTINGS: case ACTION_BT_SETTINGS:
        ui_control_center_close();
        ui_shell_settings();
        deferred_settings_page = action == ACTION_WIFI_SETTINGS ? UI_PAGE_WIFI_SETTINGS : UI_PAGE_BLUETOOTH_SETTINGS;
        return true;
    case ACTION_WIFI: return ui_settings_cycle(UI_SETTING_WIFI);
    case ACTION_BLUETOOTH: return ui_settings_cycle(UI_SETTING_BLUETOOTH);
    case ACTION_LIGHT_TOGGLE:
        center.light_request = center.light_level ? 0 : (int)center.light_restore;
        return true;
    case ACTION_VOLUME_TOGGLE:
        return epd_sound_set_muted(!center.volume_muted);
    case ACTION_PREVIOUS: return epd_media_send(center.media.app_id, EPD_MEDIA_PREVIOUS);
    case ACTION_NEXT: return epd_media_send(center.media.app_id, EPD_MEDIA_NEXT);
    case ACTION_PLAY: return epd_media_send(center.media.app_id, media_pause_control() ? EPD_MEDIA_PAUSE : EPD_MEDIA_PLAY);
    case ACTION_OPEN_NOTICE: return open_notification(center.action_app, center.action_id);
    case ACTION_DISMISS_NOTICE: return epd_notification_dismiss(center.action_app, center.action_id);
    default: return true;
    }
}

void ui_control_center_storage_changed(void)
{
    /* Release copied EZIP sources before the storage layer completes removal. */
    for (unsigned i = 0; center.root && i < NOTICE_ROWS; ++i)
        if (center.rows[i].icon) epd_app_image_release(center.rows[i].icon);
    size_t count = app_catalog_count(), used = 0;
    if (count > SIZE_MAX / EPD_SYSTEM_APP_ID_SIZE) return;
    char (*ids)[EPD_SYSTEM_APP_ID_SIZE] = count ? rt_malloc(count * EPD_SYSTEM_APP_ID_SIZE) : NULL;
    if (count && !ids) { center.dirty = true; return; }
    for (size_t i = 0; i < count; ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get((unsigned)i);
        if (!entry || strlen(entry->id) >= EPD_SYSTEM_APP_ID_SIZE) continue;
        if (entry->volume == STORAGE_SD && (!storage_available(STORAGE_SD) || !storage_apps_enabled(STORAGE_SD))) continue;
        strcpy(ids[used++], entry->id);
    }
    epd_system_reconcile_apps((const char (*)[EPD_SYSTEM_APP_ID_SIZE])ids, used);
    rt_free(ids);
    center.catalog_revision = app_catalog_revision();
    center.dirty = true;
}

void ui_control_center_process(void)
{
    if (center.catalog_revision != app_catalog_revision()) ui_control_center_storage_changed();
    if (deferred_settings_page >= 0)
    {
        int page = deferred_settings_page;
        deferred_settings_page = -1;
        if (gui_app_is_actived("settings")) ui_navigation_open((ui_page_id_t)page);
    }
    if (!center.root) return;
    if (ui_shell_locked()) { ui_control_center_close(); return; }
    action_t action = center.action;
    center.action = ACTION_NONE;
    if (action != ACTION_NONE) center.failed = !perform(action);
    if (!center.root) return;
    if (center.light_request >= 0)
    {
        uint8_t percent = (uint8_t)((center.light_request * 100 + 4) / 8);
        center.light_request = -1;
        center.failed = !frontlight_commit(percent);
    }
    if (center.volume_request >= 0)
    {
        center.failed = !center.volume_max || !epd_sound_set_volume(EPD_SOUND_MASTER,
            ((unsigned)center.volume_request * 100u + center.volume_max / 2u) / center.volume_max);
        center.volume_request = -1;
    }
    unsigned previous_max = center.volume_max;
    uint32_t previous_media = center.media_revision;
    center.has_media = epd_media_read_display(&center.media, &center.media_revision,
        center.media_navigation, sizeof(center.media_navigation));
    read_controls();
    if (center.page_delta)
    {
        size_t pages = center.total ? (center.total - 1) / center.page_rows + 1 : 1;
        if (center.page_delta > 0 && center.page + 1 < pages) ++center.page;
        else if (center.page_delta < 0 && center.page) --center.page;
        center.page_delta = 0;
        notices_render();
    }
    if (center.dirty || center.locale_revision != epd_locale_revision() || previous_max != center.volume_max)
        rebuild();
    else
    {
        if (center.scale_revision != epd_ui_scale_revision())
        {
            center_layout();
            notices_render();
        }
        bool retry = center.text_retry;
        center.text_retry = false;
        if (retry || center.notices_revision != epd_notification_revision()) notices_render();
        if (retry || previous_media != center.media_revision || !center.notifications) controls_render();
    }
    center_text(center.error, center.failed ? epd_get_str(platform_control_failed) : "");
}
