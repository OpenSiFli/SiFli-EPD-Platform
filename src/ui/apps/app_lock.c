#include "storage.h"
#include "platform/app_image.h"
#include "platform/epd_locale.h"
#include "ui_internal.h"
#include "platform/app_catalog.h"
#include "platform/epd_app.h"
#include "platform/epd_refresh.h"
#include "platform/epd_lock.h"
#include "platform/epd_lock_internal.h"
#include "ui_shell.h"
#include "ui_shell_battery.h"
#include "app_service.h"
#include "storage_file.h"
#include "system_time.h"
#include <time.h>

#define LOCK_SUMMARY_COUNT 5u
#define LOCK_HOLD_MS 1000u
#define LOCK_CLOCK_WIDTH (WIDTH - 2 * epd_ui_px(24) + 1)
#define LOCK_CLOCK_HEIGHT epd_ui_px(201)
#define LOCK_CLOCK_SIZE ((unsigned)epd_ui_px(180))
#define LOCK_CLOCK_MIN_SIZE 64u

/* Shared icon resources are generated from the selected library SVGs. */
extern const lv_image_dsc_t ui_icon_lock;
extern const lv_image_dsc_t ui_icon_flash;

typedef struct
{
    lv_obj_t *date, *clock, *battery, *percent, *flash, *summaries, *unlock;
    lv_obj_t *summary_icons[LOCK_SUMMARY_COUNT], *summary_labels[LOCK_SUMMARY_COUNT];
    lv_font_t *date_font, *clock_font, *date_fallback, *clock_fallback;
    char clock_text[32];
    unsigned clock_size, shown;
    bool clock_ready, clock_retry, clock_hour24, clock_chinese;
    uint32_t status_tick, pressed_tick, summary_revision, catalog_revision, locale_revision, scale_revision;
    lv_indev_t *pressed_indev;
    bool pressed, summary_ready;
} lock_state_t;

static lock_state_t *prepared_lock;

static void lock_layout(lock_state_t *lock);

static lock_state_t *lock_state(void)
{
    return current && current->id == UI_PAGE_LOCK ? current->feature : NULL;
}

bool epd_lock_builtin_prepare(void)
{
    if (lock_state()) return true;
    if (!prepared_lock) prepared_lock = lv_malloc_zeroed(sizeof(*prepared_lock));
    return prepared_lock != NULL;
}

void epd_lock_builtin_cancel(void)
{
    lv_free(prepared_lock);
    prepared_lock = NULL;
}

bool epd_lock_builtin_ready(void)
{
    lock_state_t *lock = lock_state();
    return lock && lock->unlock && current->foreground;
}

static void lock_cancel_hold(lock_state_t *lock)
{
    lock->pressed = false;
    lock->pressed_indev = NULL;
}

static void lock_unlock_event(lv_event_t *event)
{
    lock_state_t *lock = lock_state();
    if (!lock || !current->foreground) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED && !current->popup && !ui_shell_modal_visible() && pending_page == NAV_IDLE)
    {
        lv_indev_t *indev = lv_event_get_indev(event);
        if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
        lock->pressed = true;
        lock->pressed_tick = lv_tick_get();
        lock->pressed_indev = indev;
    }
    else if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_RELEASED || code == LV_EVENT_GESTURE)
        lock_cancel_hold(lock);
}

static void lock_summary_icon(lv_obj_t *image, const epd_app_entry_t *entry)
{
    char path[256];
    const char *file = NULL;
    const lv_image_dsc_t *fallback = &ui_icon_quick;
    if (entry->icon)
    {
        if (lv_image_src_get_type(entry->icon) == LV_IMAGE_SRC_FILE) file = entry->icon;
        else fallback = entry->icon;
    }
    if (entry->icon_file[0] && storage_app_path(path, sizeof(path), entry->id,
                                               STORAGE_APP_CODE, entry->icon_file)) file = path;
    if (file || lv_image_get_src(image) != fallback) epd_app_image_set(image, file, fallback);
}

static void lock_summaries(lock_state_t *lock)
{
    if (storage_changing()) return;
    uint32_t catalog = app_catalog_revision();
    bool catalog_ok = true;
    if (!lock->summary_ready || lock->catalog_revision != catalog) catalog_ok = app_catalog_refresh();
    uint32_t revision = epd_app_summary_revision();
    unsigned shown = 0;
    bool text_ok = true;
    for (unsigned i = 0; i < app_catalog_count() && shown < LOCK_SUMMARY_COUNT; ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get(i);
        char summary[256];
        if (!entry || !epd_app_summary_read(entry->id, summary, sizeof(summary)) || !summary[0]) continue;
        lock_summary_icon(lock->summary_icons[shown], entry);
        text_ok &= text_update(lock->summary_labels[shown], summary);
        ++shown;
    }
    bool layout_changed = lock->shown != shown;
    lock->shown = shown;
    object_visible(lock->summaries, shown != 0);
    for (unsigned i = 0; i < LOCK_SUMMARY_COUNT; ++i)
    {
        object_visible(lock->summary_icons[i], i < shown);
        object_visible(lock->summary_labels[i], i < shown);
        if (i >= shown) epd_app_image_release(lock->summary_icons[i]);
    }
    if (layout_changed) lock_layout(lock);
    lock->summary_ready = catalog_ok && text_ok;
    if (lock->summary_ready)
    {
        lock->summary_revision = revision;
        lock->catalog_revision = catalog;
    }
}

static int32_t lock_clock_width(const lv_font_t *font, bool hour24, bool chinese, int32_t *bottom)
{
    /* Reader fonts have no kerning. Measure both periods and every hour/minute
     * once per format, so proportional digits cannot change the chosen size. */
    int32_t hour_width = 0, minute_width = 0, overhang = 0;
    *bottom = 0;
    const uint32_t letters[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '-',
                               ' ', chinese ? 0x4e0a : 'A', chinese ? 0x4e0b : 'P',
                               chinese ? 0x5348 : 'M'};
    for (unsigned i = 0; i < (hour24 ? 12u : sizeof(letters) / sizeof(letters[0])); ++i)
    {
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, letters[i], 0) ||
            glyph.is_placeholder || glyph.resolved_font != font) return 0;
        overhang = LV_MAX(overhang, -glyph.ofs_x);
        overhang = LV_MAX(overhang, glyph.ofs_x + (int32_t)glyph.box_w - glyph.adv_w);
        if (glyph.box_w && glyph.box_h)
        {
            int32_t y = font->line_height - font->base_line - glyph.ofs_y;
            if (y - glyph.box_h < 0) return 0;
            *bottom = LV_MAX(*bottom, y);
        }
    }
    lv_point_t extent;
    char text[32];
    for (unsigned period = 0; period < (hour24 ? 1u : 2u); ++period)
    {
        const char *prefix = hour24 ? "" : chinese ? (period ? "下午 " : "上午 ") :
                                                                  (period ? "PM " : "AM ");
        for (unsigned hour = hour24 ? 0u : 1u; hour < (hour24 ? 24u : 13u); ++hour)
        {
            snprintf(text, sizeof(text), "%s%02u:", prefix, hour);
            lv_text_get_size(&extent, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            hour_width = LV_MAX(hour_width, extent.x);
        }
    }
    for (unsigned minute = 0; minute < 60; ++minute)
    {
        snprintf(text, sizeof(text), "%02u", minute);
        lv_text_get_size(&extent, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        minute_width = LV_MAX(minute_width, extent.x);
    }
    lv_text_get_size(&extent, "--:--", font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return *bottom ? LV_MAX(hour_width + minute_width, extent.x) + 2 * overhang : 0;
}

static void lock_clock_layout(lock_state_t *lock, bool hour24, bool chinese)
{
    if (lock->clock_ready && lock->clock_hour24 == hour24 && lock->clock_chinese == chinese) return;
    lv_font_t *font = NULL, *fallback = NULL;
    unsigned size = LOCK_CLOCK_SIZE;
    bool fitted = false;
    while (size >= LOCK_CLOCK_MIN_SIZE)
    {
        uint32_t identity;
        font = size == lock->clock_size ? lock->clock_font :
            ui_font_reader_create(0, size, 1, &identity, &fallback);
        if (!font) break;
        int32_t bottom;
        int32_t width = lock_clock_width(font, hour24, chinese, &bottom);
        /* Font line height includes space below the clock's actual glyphs. */
        if (width && width <= LOCK_CLOCK_WIDTH && bottom <= LOCK_CLOCK_HEIGHT)
        {
            fitted = true;
            break;
        }
        unsigned next = width ? LV_MIN(size * LOCK_CLOCK_WIDTH / (unsigned)width,
                                      size * LOCK_CLOCK_HEIGHT / (unsigned)bottom) : 0;
        if (font != lock->clock_font)
        {
            ui_font_reader_destroy(font);
            ui_font_reader_destroy(fallback);
        }
        font = fallback = NULL;
        if (!next || size == LOCK_CLOCK_MIN_SIZE) break;
        size = LV_MAX(LOCK_CLOCK_MIN_SIZE, LV_MIN(size - 1, next));
    }
    if (!fitted)
    {
        /* Keep an existing font if it fits; the shared 64 px font requires no
         * allocation when a larger replacement cannot be created. */
        int32_t bottom = 0;
        int32_t width = lock->clock_font ? lock_clock_width(lock->clock_font, hour24, chinese, &bottom) : 0;
        font = width && width <= LOCK_CLOCK_WIDTH && bottom <= LOCK_CLOCK_HEIGHT ?
               lock->clock_font : NULL;
        size = font ? lock->clock_size : 0;
    }
    const lv_font_t *display_font = font ? font : ui_font_temperature();
    lv_font_t *old_font = lock->clock_font, *old_fallback = lock->clock_fallback;
    lv_obj_set_style_text_font(lock->clock, display_font, 0);
    /* Keep the original 24-hour position. Center smaller text in the same area. */
    lv_obj_set_style_pad_top(lock->clock, size == LOCK_CLOCK_SIZE ? 0 :
                           LV_MAX(0, (LOCK_CLOCK_HEIGHT - display_font->line_height) / 2), 0);
    if (font != old_font)
    {
        lock->clock_font = font;
        lock->clock_fallback = fallback;
        lv_draw_wait_for_finish();
        ui_font_reader_destroy(old_font);
        ui_font_reader_destroy(old_fallback);
    }
    lock->clock_size = size;
    lock->clock_hour24 = hour24;
    lock->clock_chinese = chinese;
    lock->clock_ready = true;
    lock->clock_retry = !fitted;
}

static void lock_clock_update(lock_state_t *lock)
{
    system_time_settings_t settings;
    system_time_get_settings(&settings);
    const char *locale = epd_locale_get();
    char text[sizeof(lock->clock_text)];
    system_time_format(system_time_get_utc(), locale, false, text, sizeof(text));
    lock_clock_layout(lock, settings.hour24, !strncmp(locale, "zh", 2));
    if (strcmp(lock->clock_text, text))
    {
        lv_draw_wait_for_finish();
        memcpy(lock->clock_text, text, strlen(text) + 1);
        lv_label_set_text_static(lock->clock, lock->clock_text);
    }
}

static void lock_layout(lock_state_t *lock)
{
    if (lock->scale_revision != epd_ui_scale_revision())
    {
        uint32_t identity;
        lv_font_t *old = lock->date_font, *fallback = lock->date_fallback;
        lock->date_font = ui_font_reader_create(0, epd_ui_px(48), 0, &identity, &lock->date_fallback);
        lv_obj_set_style_text_font(lock->date, lock->date_font ? lock->date_font : ui_font_title(), 0);
        lock->clock_ready = false;
        lv_draw_wait_for_finish();
        ui_font_reader_destroy(old);
        ui_font_reader_destroy(fallback);
    }
    const lv_font_t *date_font = lock->date_font ? lock->date_font : ui_font_title();
    int unlock_size = epd_ui_touch(120), unlock_y = HEIGHT - epd_ui_px(35) - unlock_size;
    int pitch = LV_MAX(epd_ui_px(lock->shown > 3 ? 64 : 90), ui_font_title()->line_height + epd_ui_px(10));
    int summary_h = lock->shown ? (int)lock->shown * pitch + epd_ui_px(38) : 0;
    int summary_y = LV_MIN(epd_ui_px(682), unlock_y - epd_ui_px(21) - summary_h);
    int battery_y = LV_MIN(epd_ui_px(527), summary_y - epd_ui_px(32 + 51));
    int clock_y = LV_MIN(epd_ui_px(294), battery_y - epd_ui_px(32) - LOCK_CLOCK_HEIGHT);
    int date_h = LV_MAX(epd_ui_px(78), date_font->line_height);
    lv_obj_set_style_text_font(lock->date, date_font, 0);
    lv_obj_set_pos(lock->date, epd_ui_px(24), clock_y - date_h);
    lv_obj_set_size(lock->date, LOCK_CLOCK_WIDTH, date_h);
    lv_obj_set_pos(lock->clock, epd_ui_px(24), clock_y);
    lv_obj_set_size(lock->clock, LOCK_CLOCK_WIDTH, LOCK_CLOCK_HEIGHT);
    lv_obj_set_y(lock->battery, battery_y);
    ui_shell_battery_layout(lock->battery, lock->percent, ui_font_title(),
                           epd_ui_px(91), epd_ui_px(51), epd_ui_px(8));
    lv_obj_set_y(lock->flash, battery_y);
    lv_obj_set_size(lock->flash, epd_ui_px(50), epd_ui_px(50));
    lv_obj_set_pos(lock->summaries, epd_ui_px(24), summary_y);
    lv_obj_set_size(lock->summaries, LOCK_CLOCK_WIDTH, summary_h);
    lv_obj_set_style_radius(lock->summaries, epd_ui_px(25), 0);
    lv_obj_set_style_border_width(lock->summaries, epd_ui_px(6), 0);
    for (unsigned i = 0; i < LOCK_SUMMARY_COUNT; ++i)
    {
        int y = epd_ui_px(19) + (int)i * pitch;
        lv_obj_set_pos(lock->summary_icons[i], epd_ui_px(30), y + (pitch - epd_ui_px(60)) / 2);
        lv_obj_set_size(lock->summary_icons[i], epd_ui_px(60), epd_ui_px(60));
        lv_obj_set_style_text_font(lock->summary_labels[i], ui_font_title(), 0);
        lv_obj_set_pos(lock->summary_labels[i], epd_ui_px(119), y + (pitch - ui_font_title()->line_height) / 2);
        lv_obj_set_size(lock->summary_labels[i], LOCK_CLOCK_WIDTH - epd_ui_px(155), ui_font_title()->line_height);
    }
    lv_obj_set_pos(lock->unlock, (WIDTH - unlock_size) / 2, unlock_y);
    lv_obj_set_size(lock->unlock, unlock_size, unlock_size);
    lv_obj_t *icon = lv_obj_get_child(lock->unlock, 0);
    lv_obj_set_size(icon, epd_ui_px(80), epd_ui_px(80));
    lv_obj_center(icon);
    lv_area_t status_area = {0, clock_y - date_h, WIDTH - 1, battery_y + epd_ui_px(63)};
    epd_refresh_status_area(current->screen, &status_area);
    lock->scale_revision = epd_ui_scale_revision();
}

static void lock_status(lock_state_t *lock)
{
    launcher_status_t snapshot = status;
    lock_clock_update(lock);
    struct tm calendar;
    char date[96];
    if (system_time_get_local(&calendar) &&
        calendar.tm_wday >= 0 && calendar.tm_wday < 7)
    {
        static const lv_ext_str_id_t weekdays[] = {
            LV_EXT_STR_ID(platform_weekday_sun), LV_EXT_STR_ID(platform_weekday_mon),
            LV_EXT_STR_ID(platform_weekday_tue), LV_EXT_STR_ID(platform_weekday_wed),
            LV_EXT_STR_ID(platform_weekday_thu), LV_EXT_STR_ID(platform_weekday_fri),
            LV_EXT_STR_ID(platform_weekday_sat)
        };
        EPD_FMT_platform_lock_date(date, sizeof(date), (unsigned)calendar.tm_mon + 1,
                                   (unsigned)calendar.tm_mday, LV_EXT_STR_GET(weekdays[calendar.tm_wday]));
    }
    else snprintf(date, sizeof(date), "---- -- --");
    text_update(lock->date, date);
    ui_shell_battery_text(lock->percent, snapshot.battery_percent);
    int group_width = epd_ui_px(snapshot.charging ? 149 : 91);
    int x = (WIDTH - group_width + 1) / 2;
    lv_obj_set_x(lock->battery, x);
    lv_obj_set_x(lock->flash, x + epd_ui_px(99));
    object_visible(lock->flash, snapshot.charging);
    lock->locale_revision = epd_locale_revision();
    lock->status_tick = lv_tick_get();
}

void lock_resume(void)
{
    lock_state_t *lock = lock_state();
    if (!lock) return;
    lock_cancel_hold(lock);
    if (lock->clock_retry) lock->clock_ready = false;
    lock_layout(lock);
    lock_status(lock);
    lock_summaries(lock);
}

void lock_pause(void)
{
    lock_state_t *lock = lock_state();
    if (!lock) return;
    lock_cancel_hold(lock);
    for (unsigned i = 0; i < LOCK_SUMMARY_COUNT; ++i) epd_app_image_release(lock->summary_icons[i]);
    lock->summary_ready = false;
}

void lock_stop(void)
{
    lock_state_t *lock = lock_state();
    if (!lock) return;
    lock_pause();
    lv_draw_wait_for_finish();
    lv_obj_delete(lock->clock);
    lv_obj_delete(lock->date);
    lv_obj_remove_event_cb(lock->unlock, lock_unlock_event);
    ui_font_reader_destroy(lock->clock_font);
    ui_font_reader_destroy(lock->clock_fallback);
    ui_font_reader_destroy(lock->date_font);
    ui_font_reader_destroy(lock->date_fallback);
    lv_free(lock);
    current->feature = NULL;
}

void lock_process(void)
{
    lock_state_t *lock = lock_state();
    if (!lock || !current->foreground) return;
    if (lock->scale_revision != epd_ui_scale_revision())
    {
        lock_layout(lock);
        lock_status(lock);
    }
    if (lock->pressed)
    {
        lv_point_t point;
        lv_indev_get_point(lock->pressed_indev, &point);
        if (current->popup || ui_shell_modal_visible() || lv_indev_get_state(lock->pressed_indev) != LV_INDEV_STATE_PRESSED ||
            !lv_obj_hit_test(lock->unlock, &point)) lock_cancel_hold(lock);
        else if (pending_page == NAV_IDLE && lv_tick_elaps(lock->pressed_tick) >= LOCK_HOLD_MS)
        {
            lv_indev_wait_release(lock->pressed_indev);
            lock_cancel_hold(lock);
            if (!epd_lock_unlock() && !epd_lock_active()) pending_page = NAV_BACK;
        }
    }
    if (!lv_refreshing_done() || !epd_refresh_update_due()) return;
    if (lv_tick_elaps(lock->status_tick) >= 1000 || lock->locale_revision != epd_locale_revision())
        lock_status(lock);
    if (!lock->summary_ready || lock->summary_revision != epd_app_summary_revision() ||
        lock->catalog_revision != app_catalog_revision()) lock_summaries(lock);
}

void lock_create(lv_obj_t *screen)
{
    lock_state_t *lock = prepared_lock ? prepared_lock : lv_malloc_zeroed(sizeof(*lock));
    prepared_lock = NULL;
    current->feature = lock;
    if (!lock)
    {
        centered_label_i18n(screen, LV_EXT_STR_ID(platform_no_memory), MARGIN, 300, CONTENT_WIDTH, ui_font_body());
        return;
    }
    lock->date = centered_label(screen, "", 49, 216, 587,
                                lock->date_font ? lock->date_font : ui_font_title());
    lv_obj_set_height(lock->date, 78);
    lock->clock = centered_label(screen, "", 24, 294, LOCK_CLOCK_WIDTH, ui_font_temperature());
    lv_label_set_long_mode(lock->clock, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_letter_space(lock->clock, 0, 0);
    lv_obj_set_height(lock->clock, LOCK_CLOCK_HEIGHT);
    lock->battery = panel_create(screen, 0, 527, 91, 51);
    lv_obj_remove_flag(lock->battery, LV_OBJ_FLAG_CLICKABLE);
    lock->percent = centered_label(lock->battery, "", 3, 4, 78, ui_font_title());
    lv_obj_set_height(lock->percent, ui_font_title()->line_height);
    lv_obj_center(lock->percent);
    lock->flash = icon_create(screen, &ui_icon_flash, 0, 527);
    lv_obj_set_size(lock->flash, 50, 50);
    lv_image_set_inner_align(lock->flash, LV_IMAGE_ALIGN_CONTAIN);
    lock->summaries = panel_create(screen, 24, 682, 637, 308);
    lv_obj_remove_flag(lock->summaries, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(lock->summaries, lv_color_hex(0xEAEAEA), 0);
    lv_obj_set_style_border_color(lock->summaries, lv_color_hex(0x828282), 0);
    lv_obj_set_style_border_width(lock->summaries, 6, 0);
    lv_obj_set_style_radius(lock->summaries, 25, 0);
    for (unsigned i = 0; i < LOCK_SUMMARY_COUNT; ++i)
    {
        lock->summary_icons[i] = icon_create(lock->summaries, &ui_icon_quick, 30, 0);
        lv_image_set_inner_align(lock->summary_icons[i], LV_IMAGE_ALIGN_CONTAIN);
        lock->summary_labels[i] = label_create(lock->summaries, "", 119, 0, 482, ui_font_title());
        lv_label_set_long_mode(lock->summary_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_height(lock->summary_labels[i], 50);
    }
    lock->unlock = panel_create(screen, 282, 1061, 120, 120);
    lv_obj_set_style_border_width(lock->unlock, 0, 0);
    lv_obj_set_style_bg_opa(lock->unlock, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(lock->unlock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(lock->unlock, LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *icon = icon_create(lock->unlock, &ui_icon_lock, 20, 20);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(icon, 80, 80);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_event_cb(lock->unlock, lock_unlock_event, LV_EVENT_ALL, NULL);
    /* Clock and charge updates do not advance the content refresh interval. */
    lv_area_t status_area = {0, 200, WIDTH - 1, 590};
    epd_refresh_status_area(screen, &status_area);
    lock_resume();
}
