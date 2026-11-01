#include "platform/epd_lock_internal.h"
#include "ui_settings_internal.h"
#include "storage_file.h"
#include "storage_internal.h"
#include "platform/app_image.h"
#include "platform/epd_locale.h"
#include "ui_shell.h"
#include "ui_shell_battery.h"
#include "ui_popup_style.h"
#include "platform/epd_power.h"
#include "system_reset_guard.h"
#include "ui_shell_model.h"
#include "ui_nav_gesture.h"
#include "ui_internal.h"
#include "ui_control_center.h"
#include "ui_voice_input.h"
#include "platform/epd_app.h"
#include "platform/epd_input.h"
#include "platform/epd_refresh.h"
#include "platform/app_catalog.h"
#include "platform/app_card.h"
#include "platform/epd_system.h"
#include "platform/epd_sound.h"
#include "platform/epd_lock.h"
#include "storage.h"
#include "boards/board_service.h"
#include <rtm.h>
#include "src/misc/lv_event_private.h"
#include "src/misc/lv_text_private.h"
#include "src/indev/lv_indev_private.h"

#define RECENT_COUNT 5
#define NOTICE_COUNT 3
enum {
    STATUS_CLOCK_HEIGHT = 29, STATUS_ICON_HEIGHT = 32, STATUS_NOTICE_SIZE = 30, STATUS_ICON_GAP = 8,
    STATUS_HEADSET_TEXT_HEIGHT = 28,
    STATUS_BATTERY_WIDTH = 72, STATUS_BATTERY_HEIGHT = 40
};

typedef struct shell_page {
    struct shell_page *next;
    lv_obj_t *screen;
    bool immersive;
} shell_page_t;

enum { ACTION_NONE, ACTION_HIDE_NAV, ACTION_BACK, ACTION_HOME, ACTION_RECENT,
       ACTION_CLOSE_RECENT, ACTION_OPEN_RECENT, ACTION_POWER, ACTION_CLOSE_POWER,
       ACTION_POWER_OFF, ACTION_RESTART, ACTION_LOCK, ACTION_SETTINGS, ACTION_VOICE,
       ACTION_CLOSE_NAV_ERROR };
static struct {
    shell_page_t *pages;
    lv_obj_t *screen, *header, *clock, *clock_prefix, *battery, *percent, *flash, *wifi, *bluetooth;
    lv_obj_t *headset, *headset_percent;
    lv_obj_t *notice[NOTICE_COUNT], *top_edge, *bottom_edge, *nav, *recent, *power, *nav_error;
    lv_obj_t *power_icon, *power_title, *power_off, *power_restart, *power_cancel;
    lv_font_t *clock_font, *clock_fallback, *power_font, *power_fallback;
    lv_font_t *clock_prefix_font, *clock_prefix_fallback;
    lv_font_t *status_font, *status_fallback;
    char recent_ids[RECENT_COUNT][GUI_APP_ID_MAX_LEN];
    char opened_ids[RECENT_COUNT][GUI_APP_ID_MAX_LEN];
    char active_app[GUI_APP_ID_MAX_LEN];
    unsigned recent_count, selected_recent, clock_percent, power_percent;
    bool clock_chinese_prefix;
    int32_t clock_top, clock_height, clock_placeholder_top, clock_placeholder_height;
    int32_t clock_prefix_top, clock_prefix_height;
    int32_t notice_right;
    uint32_t notice_revision, catalog_revision, locale_revision, scale_revision;
    uint32_t hide_status_at;
    unsigned navigation_option, navigation_scope;
    uint32_t navigation_popup;
    uint32_t lock_attempt_tick;
    bool lock_retry;
    board_power_error_t power_error;
    int action;
    ui_shell_gesture_t gesture;
    lv_point_t origin;
    lv_point_t center_origin;
    lv_indev_t *center_indev;
    lv_point_t power_origin;
    lv_indev_t *power_indev;
    bool initialized, nav_visible, transient_status, edge_consumed;
} shell;

static int32_t text_visible_height(const lv_font_t *font, const char *text, int32_t *top)
{
    int32_t bottom = 0;
    *top = font->line_height;
    uint32_t i = 0;
    while (text[i])
    {
        uint32_t code = lv_text_encoded_next(text, &i);
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, code, 0) || !glyph.box_h || !glyph.box_w) continue;
        int32_t y = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        int32_t first = 0, last = glyph.box_h;
        /* Tiny TTF bitmap boxes include empty edge pixels. */
        const lv_draw_buf_t *bitmap = glyph.format == LV_FONT_GLYPH_FORMAT_A8 ?
                                      lv_font_get_glyph_bitmap(&glyph, NULL) : NULL;
        if (bitmap)
        {
            first = glyph.box_h;
            last = 0;
            for (unsigned row = 0; row < glyph.box_h; ++row)
                for (unsigned column = 0; column < glyph.box_w; ++column)
                    if (bitmap->data[row * bitmap->header.stride + column])
                    {
                        first = LV_MIN(first, (int32_t)row);
                        last = row + 1;
                        break;
                    }
        }
        lv_font_glyph_release_draw_data(&glyph);
        if (last <= first) continue;
        *top = LV_MIN(*top, y + first);
        bottom = LV_MAX(bottom, y + last);
    }
    if (bottom > *top) return bottom - *top;
    *top = 0;
    return font->line_height;
}

static const char *clock_sample(bool chinese_prefix)
{
    return chinese_prefix ? "上午下午" : "0123456789AMPM";
}

static lv_font_t *status_font_create(const char *sample, unsigned height, unsigned weight,
                                     lv_font_t **fallback)
{
    const int32_t target = epd_ui_px(height);
    unsigned size = epd_ui_px(33);
    lv_font_t *best = NULL;
    *fallback = NULL;
    for (unsigned attempt = 0; attempt < 8; ++attempt)
    {
        uint32_t identity;
        lv_font_t *extra = NULL;
        lv_font_t *font = ui_font_reader_create(0, size, weight, &identity, &extra);
        if (!font) break;
        int32_t top;
        int32_t visible_height = text_visible_height(font, sample, &top);
        if (visible_height <= target)
        {
            ui_font_reader_destroy(best);
            ui_font_reader_destroy(*fallback);
            best = font;
            *fallback = extra;
            if (visible_height == target) break;
            ++size;
        }
        else
        {
            ui_font_reader_destroy(font);
            ui_font_reader_destroy(extra);
            if (best) break;
            size = LV_MAX(1, LV_MIN(size - 1, size * target / visible_height));
        }
    }
    return best;
}

static lv_font_t *clock_font_create(bool chinese_prefix, lv_font_t **fallback)
{
    return status_font_create(clock_sample(chinese_prefix), STATUS_CLOCK_HEIGHT, 1, fallback);
}

static void shell_font_select(bool power)
{
    unsigned percent = epd_ui_scale_percent();
    bool chinese_prefix = strstr(status.clock_text, "午") != NULL;
    if (power ? shell.power_percent == percent :
        shell.clock_percent == percent && shell.clock_chinese_prefix == chinese_prefix) return;
    uint32_t identity;
    lv_font_t *old = power ? shell.power_font : shell.clock_font;
    lv_font_t *fallback = power ? shell.power_fallback : shell.clock_fallback;
    if (power)
    {
        shell.power_font = ui_font_reader_create(0, epd_ui_px(48), 0, &identity, &shell.power_fallback);
        shell.power_percent = percent;
        lv_obj_t *labels[] = {shell.power_off, shell.power_restart, shell.power_cancel};
        for (unsigned i = 0; i < 3; ++i)
            if (labels[i]) lv_obj_set_style_text_font(labels[i], shell.power_font ? shell.power_font : ui_font_title(), 0);
    }
    else
    {
        lv_font_t *extra = NULL;
        lv_font_t *font = clock_font_create(false, &extra);
        if (!font) return;
        lv_font_t *prefix_extra = NULL;
        lv_font_t *prefix = chinese_prefix ? clock_font_create(true, &prefix_extra) : NULL;
        if (chinese_prefix && !prefix)
        {
            ui_font_reader_destroy(font);
            ui_font_reader_destroy(extra);
            return;
        }
        lv_font_t *status_extra = NULL;
        lv_font_t *status_font = status_font_create("0123456789%", STATUS_HEADSET_TEXT_HEIGHT,
                                                   0, &status_extra);
        if (!status_font)
        {
            ui_font_reader_destroy(font);
            ui_font_reader_destroy(extra);
            ui_font_reader_destroy(prefix);
            ui_font_reader_destroy(prefix_extra);
            return;
        }
        lv_font_t *old_status = shell.status_font;
        lv_font_t *old_status_fallback = shell.status_fallback;
        shell.status_font = status_font;
        shell.status_fallback = status_extra;
        lv_font_t *old_prefix = shell.clock_prefix_font;
        lv_font_t *old_prefix_fallback = shell.clock_prefix_fallback;
        shell.clock_prefix_font = prefix;
        shell.clock_prefix_fallback = prefix_extra;
        shell.clock_font = font;
        shell.clock_fallback = extra;
        shell.clock_percent = percent;
        shell.clock_chinese_prefix = chinese_prefix;
        shell.clock_height = text_visible_height(font, clock_sample(false), &shell.clock_top);
        shell.clock_placeholder_height = text_visible_height(font, "--:--", &shell.clock_placeholder_top);
        shell.clock_prefix_height = text_visible_height(prefix ? prefix : font,
                                    chinese_prefix ? clock_sample(true) : "AMPM", &shell.clock_prefix_top);
        if (shell.clock) lv_obj_set_style_text_font(shell.clock, shell.clock_font ? shell.clock_font : ui_font_body(), 0);
        if (shell.clock_prefix) lv_obj_set_style_text_font(shell.clock_prefix, prefix ? prefix : font, 0);
        if (shell.headset_percent) lv_obj_set_style_text_font(shell.headset_percent, status_font, 0);
        lv_draw_wait_for_finish();
        ui_font_reader_destroy(old_status);
        ui_font_reader_destroy(old_status_fallback);
        ui_font_reader_destroy(old_prefix);
        ui_font_reader_destroy(old_prefix_fallback);
    }
    lv_draw_wait_for_finish();
    ui_font_reader_destroy(old);
    ui_font_reader_destroy(fallback);
}

static void shell_image_size(lv_obj_t *image, int size)
{
    lv_obj_set_size(image, size, size);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
}

static int32_t status_image_width(const lv_image_dsc_t *source)
{
    return ui_icon_at_height(source, epd_ui_px(STATUS_ICON_HEIGHT))->header.w;
}

static int32_t status_image_layout(lv_obj_t *image, const lv_image_dsc_t *source, int32_t right)
{
    source = ui_icon_at_height(source, epd_ui_px(STATUS_ICON_HEIGHT));
    int32_t height = source->header.h;
    int32_t width = source->header.w;
    image_update(image, source);
    lv_obj_set_size(image, width, height);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_set_pos(image, right - width, (epd_ui_px(64) - height) / 2);
    return right - width;
}

static void shell_scale_layout(void);
static void recent_layout(void);
static void power_layout(void);

static bool immersive(void)
{
    for (shell_page_t *page = shell.pages; page; page = page->next)
        if (page->screen == lv_screen_active()) return page->immersive;
    return true;
}

static bool gesture_navigation(void)
{
    return ui_settings_index(UI_SETTING_NAVIGATION) == 1;
}

bool ui_shell_locked(void)
{
    return epd_lock_active() || launcher_current_page() == UI_PAGE_LOCK;
}

bool ui_shell_modal_visible(void)
{
    return shell.action == ACTION_POWER || shell.power || shell.recent || shell.nav_error || ui_control_center_visible() ||
           ui_voice_input_visible();
}

int32_t ui_shell_content_bottom(void)
{
    lv_obj_t *screen = lv_screen_active();
    int32_t height = screen ? lv_obj_get_height(screen) : HEIGHT;
    return height - epd_ui_px(gesture_navigation() ? 24 : shell.nav_visible && !ui_shell_locked() ? 84 : 32);
}

static bool page_popup_visible(void)
{
    return current && current->foreground && current->popup;
}

static uint32_t navigation_popup(void)
{
    return page_popup_visible() ? current->popup_serial : 0;
}

static void page_deleted(lv_event_t *event)
{
    shell_page_t *page = lv_event_get_user_data(event);
    shell_page_t **link = &shell.pages;
    while (*link && *link != page) link = &(*link)->next;
    if (*link) *link = page->next;
    if (shell.screen == page->screen) shell.screen = NULL;
    lv_free(page);
}

void ui_shell_page_mode(lv_obj_t *screen, bool fullscreen)
{
    if (!screen) return;
    for (shell_page_t *page = shell.pages; page; page = page->next)
        if (page->screen == screen) { page->immersive = fullscreen; return; }
    shell_page_t *page = lv_malloc_zeroed(sizeof(*page));
    if (!page) return;
    page->screen = screen;
    page->immersive = fullscreen;
    if (!lv_obj_add_event_cb(screen, page_deleted, LV_EVENT_DELETE, page))
    { lv_free(page); return; }
    page->next = shell.pages;
    shell.pages = page;
    const lv_area_t area = {0, 0, WIDTH - 1, epd_ui_px(64) - 1};
    epd_refresh_status_area(screen, &area);
}

void epd_app_set_immersive(lv_obj_t *screen, bool fullscreen)
{
    ui_shell_page_mode(screen, fullscreen);
}
RTM_EXPORT(epd_app_set_immersive);

static const epd_app_entry_t *entry_find(const char *id)
{
    for (unsigned i = 0; i < app_catalog_count(); ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get(i);
        if (entry && !strcmp(entry->id, id)) return entry;
    }
    return NULL;
}

static void app_icon(lv_obj_t *image, const epd_app_entry_t *entry, int size)
{
    const lv_image_dsc_t *fallback = &ui_icon_quick;
    const char *file = NULL;
    char path[256];
    if (entry && entry->icon)
    {
        if (lv_image_src_get_type(entry->icon) == LV_IMAGE_SRC_FILE) file = entry->icon;
        else fallback = entry->icon;
    }
    if (entry && entry->icon_file[0] &&
        storage_app_path(path, sizeof(path), entry->id, STORAGE_APP_CODE, entry->icon_file)) file = path;
    epd_app_image_set(image, file, fallback);
    lv_obj_set_size(image, size, size);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
}

static void recent_close(void)
{
    if (!shell.recent) return;
    lv_obj_delete(shell.recent);
    shell.recent = NULL;
}

static void power_close(void)
{
    if (!shell.power) return;
    lv_draw_wait_for_finish();
    lv_obj_delete(shell.power);
    shell.power = NULL;
    shell.power_indev = NULL;
    shell.power_icon = shell.power_title = shell.power_off = shell.power_restart = shell.power_cancel = NULL;
    ui_font_reader_destroy(shell.power_font);
    ui_font_reader_destroy(shell.power_fallback);
    shell.power_font = shell.power_fallback = NULL;
    shell.power_percent = 0;
}

static void action_clicked(lv_event_t *event)
{
    shell.action = (int)(intptr_t)lv_event_get_user_data(event);
}

static void outside_clicked(lv_event_t *event)
{
    if (lv_event_get_target(event) == lv_event_get_current_target(event)) action_clicked(event);
}

static lv_obj_t *flat(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *object = panel_create(parent, x, y, w, h);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static lv_obj_t *touch_button(lv_obj_t *parent, int x, int y, int w, int h,
                              const lv_image_dsc_t *icon, int action)
{
    lv_obj_t *button = flat(parent, x, y, w, h);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, action_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    if (icon) lv_obj_center(icon_create(button, icon, 0, 0));
    return button;
}

static lv_obj_t *modal(int close_action)
{
    lv_obj_t *root = flat(lv_layer_top(), 0, 0, WIDTH, HEIGHT);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, outside_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)close_action);
    epd_app_input_group(root);
    return root;
}

static void navigation_error_close(void)
{
    if (shell.nav_error) lv_obj_delete(shell.nav_error);
    shell.nav_error = NULL;
}

void ui_shell_navigation_failed(void)
{
    if (!shell.initialized || ui_shell_locked() || board_power_pending()) return;
    navigation_error_close();
    shell.nav_error = modal(ACTION_CLOSE_NAV_ERROR);
    int width = LV_MIN(epd_ui_px(576), WIDTH - 2 * epd_ui_px(32));
    lv_obj_t *dialog = panel_create(shell.nav_error, 0, 0, width, epd_ui_px(280));
    lv_obj_center(dialog);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);
    ui_popup_style_dialog(dialog);
    epd_app_centered_label(dialog, epd_get_str(platform_app_open_failed), epd_ui_px(24),
                           epd_ui_px(40), width - epd_ui_px(48), ui_font_body());
    lv_obj_t *button = epd_app_button(dialog, epd_ui_px(48), epd_ui_px(184),
                         width - epd_ui_px(96), epd_ui_touch(64), epd_get_str(platform_ok),
                         action_clicked, (void *)(intptr_t)ACTION_CLOSE_NAV_ERROR);
    ui_popup_style_button(button);
    epd_app_input_back(shell.nav_error, button);
}

static void recent_clicked(lv_event_t *event)
{
    shell.selected_recent = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    shell.action = ACTION_OPEN_RECENT;
}

static void recent_open(void)
{
    if (ui_shell_locked()) return;
    if (shell.recent) { recent_close(); return; }
    shell.recent = modal(ACTION_CLOSE_RECENT);
    /* The navigation controls remain reachable below the outside-dismiss layer. */
    lv_obj_set_height(shell.recent, HEIGHT - 84);
    lv_obj_t *panel = panel_create(shell.recent, 13, HEIGHT - 84 - 140, 658, 130);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    ui_popup_style_dialog(panel);
    unsigned count = 0;
    for (unsigned i = 0; i < shell.recent_count; ++i)
        if (entry_find(shell.recent_ids[i]))
            rt_strncpy(shell.opened_ids[count++], shell.recent_ids[i], GUI_APP_ID_MAX_LEN);
    int gap = count ? (658 - (int)count * 100) / ((int)count + 1) : 0;
    for (unsigned i = 0; i < count; ++i)
    {
        lv_obj_t *button = epd_app_button(panel, gap + (100 + gap) * i, 15, 100, 100,
                                          NULL, recent_clicked, (void *)(uintptr_t)i);
        ui_popup_style_button(button);
        lv_obj_t *image = lv_image_create(button);
        app_icon(image, entry_find(shell.opened_ids[i]), 100);
        lv_obj_center(image);
    }
    recent_layout();
}

static void recent_layout(void)
{
    if (!shell.recent) return;
    int bottom = HEIGHT - epd_ui_px(gesture_navigation() ? 24 : 84), margin = epd_ui_px(13);
    lv_obj_t *panel = lv_obj_get_child(shell.recent, 0);
    unsigned count = lv_obj_get_child_count(panel);
    int width = WIDTH - 2 * margin;
    int side = LV_MIN(epd_ui_touch(100), (width - epd_ui_px(12) * ((int)count + 1)) / (int)LV_MAX(1u, count));
    int height = side + epd_ui_px(30);
    lv_obj_set_height(shell.recent, bottom);
    lv_obj_set_pos(panel, margin, bottom - height - epd_ui_px(10));
    lv_obj_set_size(panel, width, height);
    ui_popup_style_dialog(panel);
    int gap = (width - side * (int)count) / ((int)count + 1);
    for (unsigned i = 0; i < count; ++i)
    {
        lv_obj_t *button = lv_obj_get_child(panel, i);
        lv_obj_set_pos(button, gap + (side + gap) * (int)i, epd_ui_px(15));
        lv_obj_set_size(button, side, side);
        ui_popup_style_button(button);
        shell_image_size(lv_obj_get_child(button, 0), side);
        lv_obj_center(lv_obj_get_child(button, 0));
    }
}

static void power_text(void)
{
    if (!shell.power) return;
    bool pending = board_power_pending();
    bool failed = board_power_error() != BOARD_POWER_ERROR_NONE;
    object_visible(shell.power_icon, !pending && !failed);
    object_visible(shell.power_title, pending || failed);
    if (pending || failed)
        text_update(shell.power_title, pending ? epd_get_str(shell_power_saving) : epd_get_str(shell_power_failed));
    text_update(shell.power_off, epd_get_str(shell_power_off));
    text_update(shell.power_restart, epd_get_str(shell_restart));
    text_update(shell.power_cancel, epd_get_str(platform_cancel));
}

static void power_open(void)
{
    if (ui_shell_locked()) return;
    recent_close();
    if (shell.power) return;
    shell.power = modal(ACTION_CLOSE_POWER);
    shell_font_select(true);
    lv_obj_t *dialog = panel_create(shell.power, 82, 346, 520, 473);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);
    ui_popup_style_dialog(dialog);
    shell.power_icon = icon_create(dialog, &ui_icon_power, 220, 14);
    shell.power_title = centered_label(dialog, "", 20, 32, 480, ui_font_body());
    const int actions[] = {ACTION_POWER_OFF, ACTION_RESTART, ACTION_CLOSE_POWER};
    lv_obj_t **labels[] = {&shell.power_off, &shell.power_restart, &shell.power_cancel};
    for (unsigned i = 0; i < 3; ++i)
    {
        lv_obj_t *button = epd_app_button(dialog, 16, 105 + i * 125, 488, 100,
                                          "", action_clicked, (void *)(intptr_t)actions[i]);
        ui_popup_style_button(button);
        *labels[i] = lv_obj_get_child(button, 0);
        lv_obj_set_style_text_font(*labels[i], shell.power_font ? shell.power_font : ui_font_title(), 0);
        lv_obj_set_height(*labels[i], LV_SIZE_CONTENT);
        lv_obj_center(*labels[i]);
    }
    power_layout();
    power_text();
}

static void power_layout(void)
{
    if (!shell.power) return;
    shell_font_select(true);
    lv_obj_t *dialog = lv_obj_get_child(shell.power, 0);
    int width = LV_MIN(epd_ui_px(520), WIDTH - 2 * epd_ui_px(32));
    int height = epd_ui_px(473);
    lv_obj_set_size(dialog, width, height);
    lv_obj_center(dialog);
    ui_popup_style_dialog(dialog);
    shell_image_size(shell.power_icon, epd_ui_px(80));
    lv_obj_set_pos(shell.power_icon, (width - epd_ui_px(80)) / 2, epd_ui_px(14));
    lv_obj_set_style_text_font(shell.power_title, ui_font_body(), 0);
    lv_obj_set_pos(shell.power_title, epd_ui_px(20), epd_ui_px(32));
    lv_obj_set_size(shell.power_title, width - epd_ui_px(40), LV_SIZE_CONTENT);
    lv_obj_t *labels[] = {shell.power_off, shell.power_restart, shell.power_cancel};
    for (unsigned i = 0; i < 3; ++i)
    {
        lv_obj_t *button = lv_obj_get_parent(labels[i]);
        lv_obj_set_pos(button, epd_ui_px(16), epd_ui_px(105 + (int)i * 125));
        lv_obj_set_size(button, width - epd_ui_px(32), epd_ui_touch(100));
        ui_popup_style_button(button);
        lv_obj_set_style_text_font(labels[i], shell.power_font ? shell.power_font : ui_font_title(), 0);
        lv_obj_set_size(labels[i], width - epd_ui_px(56), LV_SIZE_CONTENT);
        lv_obj_center(labels[i]);
    }
}

static void notices_layout(void)
{
    int32_t x = lv_obj_get_x(shell.clock) + lv_obj_get_width(shell.clock) + epd_ui_px(14);
    int32_t right = shell.notice_right;
    for (unsigned i = 0; i < NOTICE_COUNT; ++i)
    {
        lv_obj_set_x(shell.notice[i], x);
        /* Clipping hides an icon without losing its image or notification ID. */
        lv_opa_t opacity = x + epd_ui_px(STATUS_NOTICE_SIZE) <= right ? LV_OPA_COVER : LV_OPA_TRANSP;
        if (lv_obj_get_style_opa(shell.notice[i], 0) != opacity)
            lv_obj_set_style_opa(shell.notice[i], opacity, 0);
        x += epd_ui_px(STATUS_NOTICE_SIZE + STATUS_ICON_GAP);
    }
}

static void clock_align(void)
{
    const lv_font_t *font = lv_obj_get_style_text_font(shell.clock, 0);
    /* Keep the digit baseline fixed throughout each clock format. */
    const char *text = lv_label_get_text(shell.clock);
    bool numeric = strpbrk(text, "0123456789") != NULL;
    int32_t top = numeric ? shell.clock_top : shell.clock_placeholder_top;
    int32_t height = numeric ? shell.clock_height : shell.clock_placeholder_height;
    if (!shell.clock_font) height = text_visible_height(font, text, &top);
    lv_obj_set_y(shell.clock, (epd_ui_px(64) - height) / 2 - top);
}

static void header_render(void)
{
    shell_font_select(false);
    const char *clock_text = status.clock_text[0] ? status.clock_text : "--:--";
    const char *separator = strrchr(clock_text, ' ');
    char prefix[16] = "";
    if (separator && (size_t)(separator - clock_text) < sizeof(prefix))
    {
        memcpy(prefix, clock_text, separator - clock_text);
        clock_text = separator + 1;
    }
    text_update(shell.clock_prefix, prefix);
    object_visible(shell.clock_prefix, prefix[0] != '\0');
    int32_t clock_x = epd_ui_px(5) + 5;
    if (prefix[0])
    {
        lv_point_t size;
        lv_text_get_size(&size, prefix, lv_obj_get_style_text_font(shell.clock_prefix, 0),
                         0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_width(shell.clock_prefix, size.x);
        lv_obj_set_pos(shell.clock_prefix, clock_x,
                       (epd_ui_px(64) - shell.clock_prefix_height) / 2 - shell.clock_prefix_top);
        clock_x += size.x + epd_ui_px(8);
    }
    lv_obj_set_x(shell.clock, clock_x);
    text_update(shell.clock, clock_text);
    lv_point_t clock_size;
    lv_text_get_size(&clock_size, clock_text,
                     lv_obj_get_style_text_font(shell.clock, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_width(shell.clock, clock_size.x);
    lv_obj_update_layout(shell.clock);
    clock_align();
    ui_shell_battery_text(shell.percent, status.battery_percent);
    int battery_right = WIDTH - epd_ui_px(11);
    if (status.charging)
        battery_right = status_image_layout(shell.flash, &ui_icon_status_flash, battery_right) - epd_ui_px(5);
    int battery_x = battery_right - epd_ui_px(STATUS_BATTERY_WIDTH);
    lv_obj_set_x(shell.battery, battery_x);
    object_visible(shell.flash, status.charging);
    int right = battery_x - epd_ui_px(10);
    lv_obj_t *radios[] = {shell.bluetooth, shell.wifi};
    ui_radio_state_t states[] = {status.bluetooth, status.wifi};
    const lv_image_dsc_t *on[] = {&ui_icon_bt_on, &ui_icon_wifi_on};
    const lv_image_dsc_t *off[] = {&ui_icon_bt_disconnected, &ui_icon_wifi_disconnected};
    int32_t radio_width = 0;
    for (unsigned i = 0; i < 2; ++i)
        if (states[i] != UI_RADIO_OFF)
            radio_width += status_image_width(states[i] == UI_RADIO_CONNECTED ? on[i] : off[i]) +
                           epd_ui_px(STATUS_ICON_GAP);
    bool headset_battery_known = status.headset_connected &&
        status.headset_battery_percent >= 0 && status.headset_battery_percent <= 100;
    object_visible(shell.headset, status.headset_connected);
    char headset_text[8] = "";
    if (headset_battery_known)
        snprintf(headset_text, sizeof(headset_text), "%d%%", status.headset_battery_percent);
    text_update(shell.headset_percent, headset_text);
    if (status.headset_connected)
    {
        if (headset_battery_known)
        {
            const lv_font_t *font = lv_obj_get_style_text_font(shell.headset_percent, 0);
            lv_point_t size;
            lv_text_get_size(&size, headset_text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int32_t room = right - radio_width - status_image_width(&ui_icon_headset) -
                           epd_ui_px(STATUS_ICON_GAP + 3);
            headset_battery_known = room - size.x >= lv_obj_get_x(shell.clock) +
                                                    clock_size.x + epd_ui_px(14);
            if (headset_battery_known)
            {
                int32_t top;
                int32_t height = text_visible_height(font, headset_text, &top);
                lv_obj_set_width(shell.headset_percent, size.x);
                lv_obj_set_pos(shell.headset_percent, right - size.x,
                               (epd_ui_px(64) - height) / 2 - top);
                right -= size.x + epd_ui_px(3);
            }
        }
        right = status_image_layout(shell.headset, &ui_icon_headset, right) - epd_ui_px(STATUS_ICON_GAP);
    }
    object_visible(shell.headset_percent, headset_battery_known);
    for (unsigned i = 0; i < 2; ++i)
    {
        object_visible(radios[i], states[i] != UI_RADIO_OFF);
        if (states[i] == UI_RADIO_OFF) continue;
        right = status_image_layout(radios[i], states[i] == UI_RADIO_CONNECTED ? on[i] : off[i], right) -
                epd_ui_px(STATUS_ICON_GAP);
    }
    shell.notice_right = right;
    notices_layout();
}

static void notices_render(void)
{
    char ids[NOTICE_COUNT][EPD_SYSTEM_APP_ID_SIZE];
    size_t count = epd_notification_app_ids(ids, NOTICE_COUNT);
    for (unsigned i = 0; i < NOTICE_COUNT; ++i)
    {
        const epd_app_entry_t *entry = i < count ? entry_find(ids[i]) : NULL;
        object_visible(shell.notice[i], entry != NULL);
        if (entry) app_icon(shell.notice[i], entry, epd_ui_px(STATUS_NOTICE_SIZE));
        else epd_app_image_release(shell.notice[i]);
    }
    notices_layout();
}

static void edge_event(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED)
    {
        shell.origin = point;
        shell.edge_consumed = false;
        shell.hide_status_at = 0;
    }
    else if (!shell.edge_consumed && (lv_event_get_code(event) == LV_EVENT_PRESSING ||
                                      lv_event_get_code(event) == LV_EVENT_RELEASED))
    {
        int origin_y = shell.origin.y >= HEIGHT - epd_ui_px(32) ?
            HEIGHT - (HEIGHT - shell.origin.y) * 100 / (int)epd_ui_scale_percent() :
            shell.origin.y * 100 / (int)epd_ui_scale_percent();
        ui_shell_gesture_t action = ui_shell_edge_gesture(shell.origin.x, origin_y,
            (point.x - shell.origin.x) * 100 / (int)epd_ui_scale_percent(),
            (point.y - shell.origin.y) * 100 / (int)epd_ui_scale_percent(), immersive(),
            !immersive() || shell.transient_status, ui_control_center_visible(), ui_shell_locked());
        if (action != UI_SHELL_NONE)
        {
            shell.gesture = action;
            shell.edge_consumed = true;
            lv_indev_wait_release(indev);
        }
    }
}

static lv_point_t sound_press_point;

static void power_pointer_process(lv_indev_t *indev)
{
    if (!indev) return;
    if (!shell.power) { shell.power_indev = NULL; return; }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    bool released = lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED;
    if (LV_ABS(point.x - shell.power_origin.x) > indev->scroll_limit ||
        LV_ABS(point.y - shell.power_origin.y) > indev->scroll_limit)
    {
        shell.power_indev = NULL;
        /* Cancel before the button receives RELEASED/CLICKED. The popup keeps
         * ownership; a drag must neither activate it nor dismiss the center. */
        if (released) lv_indev_reset(indev, NULL);
        else lv_indev_wait_release(indev);
    }
    if (released) shell.power_indev = NULL;
}

static void pointer_power_gesture(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_target(event);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED)
    {
        shell.power_indev = shell.power ? indev : NULL;
        if (shell.power) lv_indev_get_point(indev, &shell.power_origin);
    }
    else if (shell.power_indev == indev) power_pointer_process(indev);
}

static bool center_swipe_allowed(void)
{
    return ui_settings_enabled(UI_SETTING_TOUCH) && ui_control_center_visible() &&
           !ui_shell_locked() && !shell.power && !shell.recent && !page_popup_visible();
}

static bool center_swipe_up(const lv_point_t *point)
{
    int scale = (int)epd_ui_scale_percent();
    return ui_shell_edge_gesture(shell.center_origin.x, shell.center_origin.y,
                (point->x - shell.center_origin.x) * 100 / scale,
                (point->y - shell.center_origin.y) * 100 / scale,
                false, true, true, false) == UI_SHELL_CLOSE_CENTER;
}

static void center_swipe_close(lv_indev_t *indev, bool released)
{
    shell.gesture = UI_SHELL_CLOSE_CENTER;
    shell.edge_consumed = true;
    shell.center_indev = NULL;
    /* Input-device release callbacks run before the target's release/click
     * handlers, including brightness and volume tracks. */
    if (released) lv_indev_reset(indev, NULL);
    else lv_indev_wait_release(indev);
}

static void pointer_center_gesture(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_target(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (!center_swipe_allowed())
    {
        shell.center_indev = NULL;
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED)
    {
        shell.center_origin = point;
        shell.center_indev = indev;
        return;
    }
    bool close = code == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(indev) == LV_DIR_TOP;
    if (code == LV_EVENT_RELEASED && shell.center_indev == indev)
    {
        close = center_swipe_up(&point);
        shell.center_indev = NULL;
    }
    if (close) center_swipe_close(indev, code == LV_EVENT_RELEASED);
}

static void center_pointer_process(void)
{
    if (!center_swipe_allowed())
    {
        shell.center_indev = NULL;
        return;
    }
    /* Disabled controls do not emit press/release events. Include the final
     * released sample: it can contain all movement since the initial press. */
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
    {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        bool released = lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED;
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        if (shell.center_indev != indev)
        {
            if (released) continue;
            shell.center_origin = point;
            shell.center_indev = indev;
        }
        else if (center_swipe_up(&point)) center_swipe_close(indev, released);
        else if (released) shell.center_indev = NULL;
        return;
    }
    shell.center_indev = NULL;
}

static bool sound_click_target(lv_obj_t *object)
{
    if (!object || !lv_obj_get_parent(object) || object == shell.top_edge || object == shell.bottom_edge ||
        !lv_obj_has_flag(object, LV_OBJ_FLAG_CLICKABLE) || lv_obj_has_state(object, LV_STATE_DISABLED)) return false;
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_CHECKABLE)) return true;
    for (uint32_t i = 0; i < lv_obj_get_event_count(object); ++i)
    {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(object, i);
        lv_event_code_t filter = descriptor->filter & ~LV_EVENT_PREPROCESS;
        if (filter == LV_EVENT_CLICKED || filter == LV_EVENT_SHORT_CLICKED || filter == LV_EVENT_ALL) return true;
    }
    return false;
}

static void pointer_clicked(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_target(event);
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    /* The input event carries the clicked object; empty space and drag releases
     * are not control activation, even when an ancestor is clickable. */
    int32_t limit = indev->scroll_limit;
    if (LV_ABS(point.x - sound_press_point.x) <= limit && LV_ABS(point.y - sound_press_point.y) <= limit &&
        (!current || current->id != UI_PAGE_SOUND_TONE) &&
        sound_click_target(lv_event_get_param(event))) epd_sound_touch();
    if (!shell.transient_status || ui_control_center_visible() || shell.power) return;
    if (point.y >= epd_ui_px(64)) shell.hide_status_at = lv_tick_get() + 1000;
    /* No event consumption: the underlying application receives this same click. */
}

static void pointer_interaction(lv_event_t *event)
{
    epd_power_activity();
    if (lv_event_get_code(event) == LV_EVENT_PRESSED)
        lv_indev_get_point(lv_event_get_target(event), &sound_press_point);
    epd_refresh_request_interaction();
}

static lv_obj_t *edge_create(int y, int height)
{
    lv_obj_t *edge = flat(lv_layer_top(), 0, y, WIDTH, height);
    lv_obj_set_style_bg_opa(edge, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(edge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(edge, edge_event, LV_EVENT_ALL, NULL);
    return edge;
}

static void navigation_gesture(ui_nav_action_t action)
{
    if (!gesture_navigation() || shell.action != ACTION_NONE ||
        shell.navigation_scope != epd_input_scope_id() || shell.navigation_popup != navigation_popup()) return;
    shell.action = action == UI_NAV_BACK ? ACTION_BACK : action == UI_NAV_HOME ? ACTION_HOME :
                   action == UI_NAV_RECENT ? ACTION_RECENT : ACTION_NONE;
}

void ui_shell_init(void)
{
    if (shell.initialized) return;
    shell.initialized = true;
    shell.navigation_option = ui_settings_index(UI_SETTING_NAVIGATION);
    lv_display_trigger_activity(NULL);
    shell_font_select(false);
    shell.header = flat(lv_layer_top(), 0, 0, WIDTH, 64);
    shell.clock = label_create(shell.header, "--:--", 5, 6, 100,
                               shell.clock_font ? shell.clock_font : ui_font_body());
    shell.clock_prefix = label_create(shell.header, "", 5, 0, 100,
                            shell.clock_prefix_font ? shell.clock_prefix_font :
                            shell.clock_font ? shell.clock_font : ui_font_body());
    lv_obj_set_height(shell.clock_prefix, LV_SIZE_CONTENT);
    shell.battery = flat(shell.header, 610, 15, 63, 35);
    lv_obj_set_style_border_color(shell.battery, lv_color_hex(0x828282), 0);
    lv_obj_set_style_bg_color(shell.battery, lv_color_hex(0xeaeaea), 0);
    lv_obj_set_style_radius(shell.battery, LV_RADIUS_CIRCLE, 0);
    shell.percent = centered_label(shell.battery, "", 2, 0, 56, ui_font_small());
    lv_obj_center(shell.percent);
    shell.flash = icon_create(shell.header, &ui_icon_status_flash, 644, 16);
    shell.bluetooth = icon_create(shell.header, &ui_icon_bt_disconnected, 0, 18);
    shell.wifi = icon_create(shell.header, &ui_icon_wifi_disconnected, 0, 18);
    shell.headset = icon_create(shell.header, &ui_icon_headset, 0, 18);
    shell.headset_percent = label_create(shell.header, "", 0, 0, 1, ui_font_small());
    lv_label_set_long_mode(shell.headset_percent, LV_LABEL_LONG_CLIP);
    for (unsigned i = 0; i < NOTICE_COUNT; ++i)
    {
        shell.notice[i] = lv_image_create(shell.header);
        lv_obj_set_pos(shell.notice[i], 105 + 40 * i, 18);
        lv_obj_add_flag(shell.notice[i], LV_OBJ_FLAG_HIDDEN);
    }
    shell.nav = flat(lv_layer_top(), 0, HEIGHT - 84, WIDTH, 84);
    touch_button(shell.nav, 0, 0, 86, 84, &ui_icon_hide, ACTION_HIDE_NAV);
    touch_button(shell.nav, 86, 0, 86, 84, &ui_icon_nav_back, ACTION_BACK);
    touch_button(shell.nav, 299, 0, 86, 84, &ui_icon_home, ACTION_HOME);
    touch_button(shell.nav, 512, 0, 86, 84, &ui_icon_recent, ACTION_RECENT);
    touch_button(shell.nav, 598, 0, 86, 84, &ui_icon_microphone, ACTION_VOICE);
    shell.top_edge = edge_create(0, 64);
    shell.bottom_edge = edge_create(HEIGHT - 32, 32);
    ui_nav_gesture_init(navigation_gesture);
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
        {
            lv_timer_t *read_timer = lv_indev_get_read_timer(indev);
            if (read_timer) lv_timer_set_period(read_timer, 8);
            lv_indev_add_event_cb(indev, pointer_power_gesture, LV_EVENT_PRESSED, NULL);
            lv_indev_add_event_cb(indev, pointer_power_gesture, LV_EVENT_GESTURE, NULL);
            lv_indev_add_event_cb(indev, pointer_power_gesture, LV_EVENT_RELEASED, NULL);
            lv_indev_add_event_cb(indev, pointer_center_gesture, LV_EVENT_PRESSED, NULL);
            lv_indev_add_event_cb(indev, pointer_center_gesture, LV_EVENT_GESTURE, NULL);
            lv_indev_add_event_cb(indev, pointer_center_gesture, LV_EVENT_RELEASED, NULL);
            lv_indev_add_event_cb(indev, pointer_interaction, LV_EVENT_PRESSED, NULL);
            lv_indev_add_event_cb(indev, pointer_interaction, LV_EVENT_RELEASED, NULL);
            lv_indev_add_event_cb(indev, pointer_clicked, LV_EVENT_CLICKED, NULL);
        }
    ui_voice_input_init();
    shell_scale_layout();
}

static void shell_scale_layout(void)
{
    shell_font_select(false);
    lv_obj_set_height(shell.header, epd_ui_px(64));
    lv_obj_set_style_text_font(shell.clock, shell.clock_font ? shell.clock_font : ui_font_body(), 0);
    lv_obj_set_x(shell.clock, epd_ui_px(5) + 5);
    lv_obj_set_height(shell.clock, LV_SIZE_CONTENT);
    lv_obj_set_y(shell.battery, (epd_ui_px(64) - epd_ui_px(STATUS_BATTERY_HEIGHT)) / 2);
    const lv_font_t *status_font = shell.status_font ? shell.status_font : ui_font_caption();
    ui_shell_battery_layout(shell.battery, shell.percent, ui_font_small(),
                           epd_ui_px(STATUS_BATTERY_WIDTH), epd_ui_px(STATUS_BATTERY_HEIGHT),
                           epd_ui_px(6));
    lv_obj_set_style_text_font(shell.headset_percent, status_font, 0);
    lv_obj_set_height(shell.headset_percent, LV_SIZE_CONTENT);
    for (unsigned i = 0; i < NOTICE_COUNT; ++i)
    {
        lv_obj_set_y(shell.notice[i], (epd_ui_px(64) - epd_ui_px(STATUS_NOTICE_SIZE)) / 2);
        shell_image_size(shell.notice[i], epd_ui_px(STATUS_NOTICE_SIZE));
    }
    int nav_h = epd_ui_touch(84), button_w = epd_ui_touch(86);
    lv_obj_set_pos(shell.nav, 0, HEIGHT - nav_h);
    lv_obj_set_height(shell.nav, nav_h);
    const int x[] = {0, button_w, (WIDTH - button_w) / 2, WIDTH - 2 * button_w, WIDTH - button_w};
    for (unsigned i = 0; i < 5; ++i)
    {
        lv_obj_t *button = lv_obj_get_child(shell.nav, i);
        lv_obj_set_pos(button, x[i], 0);
        lv_obj_set_size(button, button_w, nav_h);
        lv_obj_t *icon = lv_obj_get_child(button, 0);
        shell_image_size(icon, epd_ui_px(48));
        lv_obj_center(icon);
    }
    lv_obj_set_pos(shell.bottom_edge, 0, HEIGHT - epd_ui_px(32));
    lv_obj_set_height(shell.bottom_edge, epd_ui_px(32));
    for (shell_page_t *page = shell.pages; page; page = page->next)
    {
        if (ui_shell_locked() && page->screen == lv_screen_active()) continue;
        const lv_area_t area = {0, 0, WIDTH - 1, epd_ui_px(64) - 1};
        epd_refresh_status_area(page->screen, &area);
    }
    header_render();
    recent_layout();
    power_layout();
    shell.scale_revision = epd_ui_scale_revision();
}

void ui_shell_status_changed(void)
{
    if (shell.initialized) header_render();
}

void ui_shell_power_menu(void)
{
    if (ui_shell_locked()) return;
    epd_refresh_request_interaction();
    shell.action = ACTION_POWER;
}
void ui_shell_lock(void) { shell.action = ACTION_LOCK; }
void ui_shell_settings(void) { shell.action = ACTION_SETTINGS; }

void ui_shell_prepare_lock(void)
{
    navigation_error_close();
    ui_navigation_cancel();
    ui_nav_gesture_cancel();
    app_card_suspend();
    ui_voice_input_close();
    ui_control_center_close();
    recent_close();
    power_close();
    shell.action = ACTION_NONE;
    shell.gesture = UI_SHELL_NONE;
}

void ui_shell_storage_changed(void)
{
    navigation_error_close();
    ui_navigation_cancel();
    ui_nav_gesture_cancel();
    ui_voice_input_close();
    recent_close();
    ui_control_center_storage_changed();
    for (unsigned i = 0; i < NOTICE_COUNT; ++i) epd_app_image_release(shell.notice[i]);
    shell.catalog_revision = 0;
    shell.action = ACTION_NONE;
    shell.gesture = UI_SHELL_NONE;
}

static void remember_active(void)
{
    intent_t intent = gui_app_get_intent();
    const char *id = intent ? intent_get_action(intent) : NULL;
    if (!id || !strcmp(id, shell.active_app)) return;
    rt_strncpy(shell.active_app, id, sizeof(shell.active_app) - 1);
    if (!entry_find(id)) return;
    unsigned index = 0;
    while (index < shell.recent_count && strcmp(shell.recent_ids[index], id)) ++index;
    if (index == shell.recent_count && shell.recent_count < RECENT_COUNT) ++shell.recent_count;
    if (index >= RECENT_COUNT) index = RECENT_COUNT - 1;
    for (; index > 0; --index) memcpy(shell.recent_ids[index], shell.recent_ids[index - 1], GUI_APP_ID_MAX_LEN);
    rt_strncpy(shell.recent_ids[0], id, GUI_APP_ID_MAX_LEN - 1);
}

static void gesture_process(void)
{
    ui_shell_gesture_t gesture = shell.gesture;
    shell.gesture = UI_SHELL_NONE;
    if (page_popup_visible() || shell.power || shell.nav_error || ui_shell_locked()) return;
    switch (gesture)
    {
    case UI_SHELL_REVEAL_STATUS: shell.transient_status = true; break;
    case UI_SHELL_NOTIFICATIONS: ui_voice_input_close(); recent_close(); ui_control_center_open(true); break;
    case UI_SHELL_CONTROLS: ui_voice_input_close(); recent_close(); ui_control_center_open(false); break;
    case UI_SHELL_REVEAL_NAV: if (!gesture_navigation()) shell.nav_visible = true; break;
    case UI_SHELL_CLOSE_CENTER: ui_control_center_close(); break;
    default: break;
    }
}

static void action_process(void)
{
    int action = shell.action;
    shell.action = ACTION_NONE;
    if (shell.nav_error)
    {
        if (action == ACTION_BACK || action == ACTION_CLOSE_NAV_ERROR) navigation_error_close();
        return;
    }
    if (action == ACTION_BACK || action == ACTION_HOME || action == ACTION_RECENT)
    {
        if (board_power_pending() || system_reset_blocks_apps() || storage_changing() || storage_release_requested()) return;
        if (ui_shell_locked()) return;
        if (action == ACTION_BACK)
        {
            if (shell.power) { power_close(); return; }
            if (shell.recent) { recent_close(); return; }
            if (ui_control_center_visible()) { ui_control_center_close(); return; }
            if (page_popup_visible())
            {
                if (pending_page == NAV_IDLE) pending_page = POPUP_CLOSE;
                return;
            }
        }
        else if (shell.power || page_popup_visible()) return;
    }
    if (page_popup_visible() && action != ACTION_POWER && action != ACTION_CLOSE_POWER &&
        action != ACTION_POWER_OFF && action != ACTION_RESTART) return;
    if (ui_shell_locked()) return;
    if (ui_voice_input_visible() && action == ACTION_BACK) { ui_voice_input_close(); return; }
    if (action != ACTION_NONE && action != ACTION_VOICE) ui_voice_input_close();
    switch (action)
    {
    case ACTION_HIDE_NAV: recent_close(); shell.nav_visible = false; break;
    case ACTION_BACK: recent_close(); epd_input_back(); break;
    case ACTION_HOME: recent_close(); ui_navigation_run("Main"); break;
    case ACTION_RECENT: recent_open(); break;
    case ACTION_CLOSE_RECENT: recent_close(); break;
    case ACTION_OPEN_RECENT:
    {
        char id[GUI_APP_ID_MAX_LEN];
        rt_strncpy(id, shell.opened_ids[shell.selected_recent], sizeof(id));
        recent_close();
        if (entry_find(id)) ui_navigation_run(id);
        break;
    }
    case ACTION_POWER: power_open(); break;
    case ACTION_CLOSE_POWER: power_close(); break;
    case ACTION_POWER_OFF:
    case ACTION_RESTART:
        if (!board_power_request(action == ACTION_RESTART))
            power_text();
        break;
    case ACTION_LOCK: ui_control_center_close(); recent_close(); epd_lock_enter(); break;
    case ACTION_SETTINGS: ui_control_center_close(); recent_close(); ui_navigation_run("settings"); break;
    case ACTION_VOICE: ui_voice_input_open(); break;
    default: break;
    }
}

void ui_shell_process(void)
{
    if (!shell.initialized) return;
    unsigned navigation_option = ui_settings_index(UI_SETTING_NAVIGATION);
    if (shell.navigation_option != navigation_option)
    {
        ui_nav_gesture_cancel();
        shell.navigation_option = navigation_option;
        shell.nav_visible = navigation_option == 0;
        recent_layout();
    }
    if (shell.scale_revision != epd_ui_scale_revision()) shell_scale_layout();
    power_pointer_process(shell.power_indev);
    center_pointer_process();
    if (!board_power_pending() && !system_reset_blocks_apps()) epd_lock_process();
    if (shell.screen != lv_screen_active())
    {
        ui_nav_gesture_cancel();
        ui_voice_input_close();
        shell.screen = lv_screen_active();
        shell.transient_status = false;
        shell.hide_status_at = 0;
        recent_close();
        bool found = false;
        for (shell_page_t *page = shell.pages; page; page = page->next)
            if (page->screen == shell.screen) { found = true; break; }
        if (!found) ui_shell_page_mode(shell.screen, true);
    }
    if (ui_shell_locked())
    {
        ui_shell_prepare_lock();
    }
    else if (board_power_pending())
    {
        if (!shell.power) power_open();
        power_text();
        for (unsigned i = 0; i < 3; ++i)
            lv_obj_add_state(lv_obj_get_parent(i == 0 ? shell.power_off : i == 1 ? shell.power_restart : shell.power_cancel), LV_STATE_DISABLED);
        shell.action = ACTION_NONE;
        shell.gesture = UI_SHELL_NONE;
    }
    else if (system_reset_blocks_apps())
    {
        ui_control_center_close();
        recent_close();
        power_close();
        shell.action = ACTION_NONE;
        shell.gesture = UI_SHELL_NONE;
    }
    else
    {
        remember_active();
        if (!storage_changing()) { gesture_process(); action_process(); }
        if (shell.power)
            for (unsigned i = 0; i < 3; ++i)
                lv_obj_remove_state(lv_obj_get_parent(i == 0 ? shell.power_off : i == 1 ? shell.power_restart : shell.power_cancel), LV_STATE_DISABLED);
        ui_control_center_process();
        unsigned timeout = ui_settings_lock_timeout_ms();
        if (!ui_shell_locked() && !shell.power && !storage_changing() && !ui_voice_input_recording() && timeout &&
            lv_display_get_inactive_time(NULL) >= timeout &&
            (!shell.lock_retry || lv_tick_elaps(shell.lock_attempt_tick) >= 1000))
        {
            ui_control_center_close();
            recent_close();
            shell.lock_retry = !epd_lock_enter();
            shell.lock_attempt_tick = lv_tick_get();
        }
    }
    if (shell.power_error != board_power_error())
    {
        shell.power_error = board_power_error();
        power_text();
    }
    if (shell.hide_status_at && (int32_t)(lv_tick_get() - shell.hide_status_at) >= 0)
    { shell.transient_status = false; shell.hide_status_at = 0; }
    uint32_t revision = epd_notification_revision(), catalog = app_catalog_revision();
    if (!board_power_pending() && (revision != shell.notice_revision || catalog != shell.catalog_revision))
    {
        notices_render();
        shell.notice_revision = revision;
        shell.catalog_revision = catalog;
    }
    if (shell.locale_revision != epd_locale_revision())
    { power_text(); shell.locale_revision = epd_locale_revision(); }
    bool locked = ui_shell_locked(), center = ui_control_center_visible();
    bool page_popup = page_popup_visible() || shell.nav_error;
    ui_voice_input_process(locked || center || shell.power || shell.recent || page_popup ||
                           board_power_pending() || system_reset_blocks_apps() || storage_changing());
    bool header = !locked && (center || !immersive() || shell.transient_status);
    object_visible(shell.header, header);
    bool gestures = gesture_navigation();
    object_visible(shell.nav, !gestures && shell.nav_visible && !locked && !center && !page_popup);
    object_visible(shell.top_edge, !locked && !center && !shell.power && !shell.recent && !page_popup);
    lv_obj_set_height(shell.top_edge, epd_ui_px(header ? 64 : 24));
    object_visible(shell.bottom_edge, !locked && !shell.power && !shell.recent && !page_popup && (center || (!gestures && !shell.nav_visible)));
    bool enabled = gestures && ui_settings_enabled(UI_SETTING_TOUCH) && !locked &&
                   !board_power_pending() && !system_reset_blocks_apps() &&
                   !storage_changing() && !storage_release_requested() && !epd_power_sleeping();
    uint32_t popup_serial = navigation_popup();
    if (shell.navigation_popup != popup_serial) ui_nav_gesture_cancel();
    shell.navigation_popup = popup_serial;
    shell.navigation_scope = epd_input_scope_id();
    ui_nav_gesture_update(enabled, !shell.power && !page_popup && !center && !ui_voice_input_visible(),
                          epd_ui_scale_percent(), shell.navigation_scope);
    if (shell.power) lv_obj_move_to_index(shell.power, enabled ? -2 : -1);
    if (center)
    {
        /* Keep the edge layer above overlays without reordering every frame. */
        int edge = enabled ? 1 : 0;
        lv_obj_move_to_index(shell.bottom_edge, (shell.power ? -2 : -1) - edge);
        lv_obj_move_to_index(shell.header, (shell.power ? -3 : -2) - edge);
    }
}
