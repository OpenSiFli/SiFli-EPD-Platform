#include "platform/epd_render_profile.h"
#include "app_service.h"
#include "platform/app_content.h"
#include "platform/epd_app_font.h"
#include "epd_app.h"
#include "epd_input.h"
#include "epd_refresh.h"
#include "ui_font.h"
#include "ui_reader_font_cache.h"
#include "ui_reader_font_remote.h"
#include "reading_font.h"
#include "frontlight.h"
#include "apps/reading_fonts.h"
#include <rtdevice.h>
#include "ui_internal.h"
#include "icons/ui_icons.h"
#include <rtm.h>
#include "src/misc/lv_text_private.h"
#include "src/draw/lv_image_decoder_private.h"

RTM_EXPORT(app_content_query);
RTM_EXPORT(app_content_open);
RTM_EXPORT(app_content_receive);
RTM_EXPORT(app_content_error);

static struct {
    uint32_t calls, ticks, started, depth;
} render_profile[EPD_RENDER_PROFILE_COUNT];
volatile uint32_t epd_render_profile_generation;
static uint32_t render_profile_serial;

uint32_t epd_render_profile_begin_active(unsigned stage)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t generation = epd_render_profile_generation;
    if (generation) {
        ++render_profile[stage].calls;
        if (!render_profile[stage].depth++)
            render_profile[stage].started = (uint32_t)rt_tick_get();
    }
    rt_hw_interrupt_enable(level);
    return generation;
}

void epd_render_profile_end_active(unsigned stage, uint32_t generation)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (generation == epd_render_profile_generation &&
        render_profile[stage].depth && !--render_profile[stage].depth)
        render_profile[stage].ticks += (uint32_t)rt_tick_get() - render_profile[stage].started;
    rt_hw_interrupt_enable(level);
}

void epd_render_profile_window(bool enabled)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t now = (uint32_t)rt_tick_get();
    epd_render_profile_generation = 0;
    for (unsigned i = 0; i < EPD_RENDER_PROFILE_COUNT; ++i) {
        if (render_profile[i].depth)
            render_profile[i].ticks += now - render_profile[i].started;
        render_profile[i].depth = 0;
    }
    if (enabled) {
        memset(render_profile, 0, sizeof(render_profile));
        if (!++render_profile_serial) ++render_profile_serial;
        epd_render_profile_generation = render_profile_serial;
    }
    rt_hw_interrupt_enable(level);
}
RTM_EXPORT(epd_render_profile_window);

void epd_render_profile_snapshot(epd_render_profile_t *out)
{
    if (!out) return;
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t now = (uint32_t)rt_tick_get();
    for (unsigned i = 0; i < EPD_RENDER_PROFILE_COUNT; ++i) {
        out->stage[i].calls = render_profile[i].calls;
        out->stage[i].ticks = render_profile[i].ticks + (render_profile[i].depth ?
            now - render_profile[i].started : 0);
    }
    rt_hw_interrupt_enable(level);
}
RTM_EXPORT(epd_render_profile_snapshot);

static void back_clicked(lv_event_t *event)
{
    (void)event;
    gui_app_goback();
}

const lv_font_t *epd_app_font(void)
{
    return ui_font_body();
}
RTM_EXPORT(epd_app_font);

const void *epd_app_builtin_font_data(size_t *size)
{
    return ui_font_builtin_data(size);
}
RTM_EXPORT(epd_app_builtin_font_data);

const lv_font_t *epd_app_font_role(epd_font_role_t role)
{
    switch (role)
    {
    case EPD_FONT_SMALL: return ui_font_small();
    case EPD_FONT_TITLE: return ui_font_title();
    case EPD_FONT_CAPTION: return ui_font_caption();
    case EPD_FONT_TEMPERATURE: return ui_font_temperature();
    default: return ui_font_body();
    }
}
const lv_font_t *epd_app_font_style(epd_font_role_t role, unsigned flags)
{
    return ui_font_style(epd_app_font_role(role), flags);
}
RTM_EXPORT(epd_app_font_style);
void epd_app_object_init(lv_obj_t *obj) { epd_obj_init(obj); }
lv_obj_t *epd_app_label(lv_obj_t *p, const char *t, int x, int y, int w, const lv_font_t *f)
{ return label_create(p, t, x, y, w, f); }
lv_obj_t *epd_app_centered_label(lv_obj_t *p, const char *t, int x, int y, int w, const lv_font_t *f)
{ return centered_label(p, t, x, y, w, f); }
lv_obj_t *epd_app_panel(lv_obj_t *p, int x, int y, int w, int h) { return panel_create(p, x, y, w, h); }
lv_obj_t *epd_app_icon(lv_obj_t *p, const lv_image_dsc_t *s, int x, int y) { return icon_create(p, s, x, y); }
bool epd_app_text(lv_obj_t *label, const char *text) { return text_update(label, text); }
bool epd_app_refresh_done(void) { return lv_refreshing_done(); }
void epd_app_defer_first_frame(lv_obj_t *screen, bool defer)
{ epd_refresh_defer_first_frame(screen, defer); }
size_t epd_app_ui_available(void)
{
#ifdef RT_USING_MEMHEAP
    struct rt_memheap *heap = (struct rt_memheap *)rt_object_find("lv_psram", RT_Object_Class_MemHeap);
    if (!heap || rt_sem_take(&heap->lock, RT_WAITING_FOREVER) != RT_EOK) return 0;
    size_t available = heap->available_size;
    rt_sem_release(&heap->lock);
    return available;
#else
    return 0;
#endif
}
uint32_t epd_app_refresh_submission(void) { return epd_refresh_submission(); }
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer)
{
#if defined(PSRAM_CACHE_WB)
    if (buffer) mpu_dcache_clean(buffer->data, buffer->data_size);
#else
    (void)buffer;
#endif
}
lv_obj_t *epd_app_button(lv_obj_t *parent, int x, int y, int width, int height,
                         const char *text, lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = panel_create(parent, x, y, width, height);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_border_color(button, lv_color_black(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(button, lv_color_black(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(button, -4, LV_STATE_FOCUS_KEY);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    epd_input_add_object(button);
    if (text)
    {
        lv_obj_t *label = centered_label(button, text, 0, 0, width - 24, ui_font_body());
        lv_obj_center(label);
    }
    return button;
}
RTM_EXPORT(epd_app_font_role);
RTM_EXPORT(epd_app_object_init);
RTM_EXPORT(epd_app_label);
RTM_EXPORT(epd_app_centered_label);
RTM_EXPORT(epd_app_panel);
RTM_EXPORT(epd_app_icon);
RTM_EXPORT(epd_app_text);
RTM_EXPORT(epd_app_button);
RTM_EXPORT(epd_app_header);
RTM_EXPORT(epd_app_header_refresh);
RTM_EXPORT(epd_app_refresh_done);
RTM_EXPORT(epd_app_defer_first_frame);
RTM_EXPORT(epd_app_ui_available);
RTM_EXPORT(epd_app_refresh_submission);
RTM_EXPORT(epd_app_clean_draw_buffer);
RTM_EXPORT(intent_get_string);

void epd_app_back_button_layout(lv_obj_t *button, int32_t x, int32_t center_y)
{
    int32_t size = epd_ui_touch(60);
    lv_obj_set_pos(button, x, center_y - size / 2);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_t *icon = lv_obj_get_child(button, 0);
    lv_image_set_src(icon, &ui_icon_nav_back);
    lv_obj_set_size(icon, epd_ui_px(60), epd_ui_px(60));
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(icon);
}
RTM_EXPORT(epd_app_back_button_layout);

lv_obj_t *epd_app_back_button_cb(lv_obj_t *screen, lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = lv_obj_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    lv_obj_set_style_outline_color(button, lv_color_black(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(button, -4, LV_STATE_FOCUS_KEY);
    epd_input_add_object(button);
    epd_app_input_back(screen, button);
    lv_obj_t *icon = lv_image_create(button);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    epd_app_back_button_layout(button, epd_ui_px(24),
        epd_ui_px(81) + ui_font_title()->line_height / 2);
    return button;
}
RTM_EXPORT(epd_app_back_button_cb);
lv_obj_t *epd_app_back_button(lv_obj_t *screen)
{
    return epd_app_back_button_cb(screen, back_clicked, NULL);
}
RTM_EXPORT(epd_app_back_button);

/* The SDK already exports app run/exit, root registration and create_page_ext. */
RTM_EXPORT(gui_app_goback);
RTM_EXPORT(gui_app_goback_to_page);
RTM_EXPORT(gui_app_this_page_memory);
RTM_EXPORT(gui_app_this_page_userdata);
RTM_EXPORT(gui_app_create_page_for_app_ext);
RTM_EXPORT(gui_app_close_anim);
RTM_EXPORT(lv_screen_active);
RTM_EXPORT(lv_color_white);
RTM_EXPORT(lv_color_black);
RTM_EXPORT(lv_obj_create);
RTM_EXPORT(lv_image_create);
RTM_EXPORT(lv_image_set_src);
RTM_EXPORT(lv_image_set_inner_align);
RTM_EXPORT(lv_image_cache_drop);
RTM_EXPORT(lv_image_header_cache_drop);
RTM_EXPORT(lv_label_create);
RTM_EXPORT(lv_label_class);
RTM_EXPORT(lv_label_get_text);
RTM_EXPORT(lv_label_set_text);
RTM_EXPORT(lv_label_set_text_static);
RTM_EXPORT(lv_label_set_text_fmt);
#if LV_USE_SPAN
RTM_EXPORT(lv_spangroup_create);
RTM_EXPORT(lv_spangroup_add_span);
RTM_EXPORT(lv_spangroup_refresh);
RTM_EXPORT(lv_spangroup_get_span_by_point);
RTM_EXPORT(lv_span_set_text);
RTM_EXPORT(lv_span_get_text);
RTM_EXPORT(lv_span_get_style);
RTM_EXPORT(lv_style_set_text_font);
RTM_EXPORT(lv_style_set_text_color);
RTM_EXPORT(lv_style_set_text_decor);
#endif
RTM_EXPORT(lv_obj_set_pos);
RTM_EXPORT(lv_obj_set_size);
RTM_EXPORT(lv_pct);
RTM_EXPORT(lv_obj_add_event_cb);
RTM_EXPORT(lv_obj_remove_style_all);
RTM_EXPORT(lv_obj_add_flag);
RTM_EXPORT(lv_obj_remove_flag);
RTM_EXPORT(lv_obj_set_style_text_font);
RTM_EXPORT(lv_obj_set_style_text_color);
RTM_EXPORT(lv_obj_set_style_bg_color);
RTM_EXPORT(lv_obj_set_style_bg_opa);
RTM_EXPORT(lv_obj_set_style_border_width);
RTM_EXPORT(lv_obj_set_style_outline_width);
RTM_EXPORT(lv_obj_set_style_border_color);
RTM_EXPORT(lv_obj_set_style_border_opa);
RTM_EXPORT(lv_obj_align);
RTM_EXPORT(lv_obj_center);
RTM_EXPORT(lv_event_get_user_data);
RTM_EXPORT(lv_obj_delete);
RTM_EXPORT(lv_obj_clean);
RTM_EXPORT(lv_timer_create);
RTM_EXPORT(lv_timer_delete);
RTM_EXPORT(lv_timer_pause);
RTM_EXPORT(lv_timer_resume);
RTM_EXPORT(lv_timer_reset);
RTM_EXPORT(lv_timer_set_period);
RTM_EXPORT(lv_timer_get_user_data);
RTM_EXPORT(lv_canvas_get_draw_buf);
RTM_EXPORT(lv_color_hex);
RTM_EXPORT(lv_event_get_target_obj);
RTM_EXPORT(lv_group_add_obj);
RTM_EXPORT(lv_group_create);
RTM_EXPORT(lv_group_delete);
RTM_EXPORT(lv_group_focus_obj);
RTM_EXPORT(lv_group_get_focused);
RTM_EXPORT(lv_group_remove_all_objs);
RTM_EXPORT(lv_obj_get_screen);
RTM_EXPORT(lv_obj_set_flex_align);
RTM_EXPORT(lv_obj_set_flex_flow);
RTM_EXPORT(lv_obj_set_style_pad_bottom);
RTM_EXPORT(lv_obj_set_style_pad_left);
RTM_EXPORT(lv_obj_set_style_pad_right);
RTM_EXPORT(lv_obj_set_style_pad_row);
RTM_EXPORT(lv_obj_set_style_pad_column);
RTM_EXPORT(lv_obj_set_style_pad_top);
RTM_EXPORT(lv_obj_set_style_radius);
RTM_EXPORT(lv_obj_set_style_clip_corner);
RTM_EXPORT(lv_obj_set_style_text_align);
RTM_EXPORT(lv_qrcode_create);
RTM_EXPORT(lv_qrcode_set_dark_color);
RTM_EXPORT(lv_qrcode_set_light_color);
RTM_EXPORT(lv_qrcode_set_quiet_zone);
RTM_EXPORT(lv_qrcode_set_size);
RTM_EXPORT(lv_qrcode_update);
RTM_EXPORT(lv_refr_now);
RTM_EXPORT(lv_tick_elaps);
RTM_EXPORT(lv_tick_get);
RTM_EXPORT(lv_display_trigger_activity);
RTM_EXPORT(lv_display_get_default);
RTM_EXPORT(lv_display_add_event_cb);
RTM_EXPORT(lv_display_remove_event_cb_with_user_data);

void epd_app_status(epd_app_status_t *out)
{
    out->battery_percent = ui_battery_percent(status.battery_percent);
    rt_strncpy(out->clock, status.clock_text[0] ? status.clock_text : "--:--", sizeof(out->clock) - 1);
    out->clock[sizeof(out->clock) - 1] = 0;
}
void epd_app_set_recent_reading(const char *title, int percent)
{
    ui_app_set_recent_reading(title, percent);
}
void epd_app_request_full_refresh(void) { epd_refresh_request_full(); }
bool epd_app_open_text_settings(void) { return ui_navigation_open(UI_PAGE_TEXT_SETTINGS); }
bool epd_app_open_font_install(void) { return reading_fonts_open_install(); }
bool epd_app_open_reading_fonts(void) { return ui_navigation_open(UI_PAGE_READING_FONTS); }
RTM_EXPORT(epd_app_open_font_install);
RTM_EXPORT(epd_app_open_reading_fonts);
RTM_EXPORT(ui_settings_set);
RTM_EXPORT(reading_font_list);
RTM_EXPORT(reading_font_select);
RTM_EXPORT(reading_font_job_status);
RTM_EXPORT(frontlight_get);
RTM_EXPORT(frontlight_commit);
bool epd_app_reading_font_begin(void) { return reading_font_begin(); }
uint32_t epd_app_reading_font_id(void) { return reading_font_selected(); }
uint32_t epd_app_reading_font_revision(void) { return reading_font_revision(); }
RTM_EXPORT(epd_app_reading_font_begin);
RTM_EXPORT(epd_app_reading_font_id);
RTM_EXPORT(epd_app_reading_font_revision);
lv_font_t *epd_app_font_create(unsigned family, unsigned size, unsigned weight,
                               uint32_t *identity, lv_font_t **fallback)
{
    if (family) { if (identity) *identity = 0; if (fallback) *fallback = NULL; return NULL; }
    return ui_font_reader_create(family, size, weight, identity, fallback);
}
lv_font_t *epd_app_reading_font_create(unsigned family, unsigned size, unsigned weight,
                                       uint32_t *identity, lv_font_t **fallback)
{ return ui_font_reading_create(family, size, weight, identity, fallback); }
RTM_EXPORT(epd_app_reading_font_create);
bool epd_app_reading_font_can_create(unsigned count)
{ return ui_reader_font_remote_can_create(count); }
RTM_EXPORT(epd_app_reading_font_can_create);
void epd_app_font_destroy(lv_font_t *font) { ui_font_reader_destroy(font); }
bool epd_app_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent)
{
    if (ui_reader_font_remote_owns(font)) return ui_reader_font_remote_ready(font, letter, 0, true) >= 0;
    return ui_reader_font_prepare(font, letter, adjacent);
}
RTM_EXPORT(epd_app_font_prepare);
int epd_app_font_status(const lv_font_t *font) { return ui_reader_font_remote_status(font); }
int epd_app_font_metrics(const lv_font_t *font, epd_app_font_metrics_t *out)
{ return ui_reader_font_remote_metrics(font, out); }
int epd_app_font_glyph_acquire(const lv_font_t *font, uint32_t letter, uint32_t next,
                              bool pixels, epd_app_font_glyph_t *out)
{ return ui_reader_font_remote_acquire(font, letter, next, pixels, out); }
void epd_app_font_glyph_release(epd_app_font_glyph_t *glyph) { ui_reader_font_remote_release(glyph); }
int epd_app_font_glyph_ready(const lv_font_t *font, uint32_t letter, uint32_t next, bool pixels)
{ return ui_reader_font_remote_ready(font, letter, next, pixels); }
int epd_app_font_prepare_text(const lv_font_t *font, const char *text, size_t length, bool pixels)
{ return ui_reader_font_remote_prepare(font, text, length, pixels); }
int epd_app_font_prepare_text_preview(const lv_font_t *font, const char *text, size_t length)
{ return ui_reader_font_remote_prepare_preview(font, text, length); }
lv_font_t *epd_app_font_oblique(const lv_font_t *font) { return ui_reader_font_remote_oblique(font); }
bool epd_app_font_ref(const lv_font_t *font) { return ui_reader_font_remote_ref(font); }
void epd_app_font_cancel(const lv_font_t *font) { ui_reader_font_remote_cancel(font); }
int epd_app_font_set_purpose(const lv_font_t *font, unsigned purpose)
{ return ui_reader_font_remote_purpose(font, purpose); }
epd_app_font_page_t epd_app_font_page_create(void) { return ui_reader_font_remote_page_create(); }
int epd_app_font_page_pin(epd_app_font_page_t page, const lv_font_t *font,
                         uint32_t letter, uint32_t next, bool pixels)
{ return ui_reader_font_remote_page_pin(page, font, letter, next, pixels); }
void epd_app_font_page_release(epd_app_font_page_t page) { ui_reader_font_remote_page_release(page); }
size_t epd_app_font_reclaim_pixels(size_t min_available)
{ return ui_reader_font_remote_reclaim_pixels(min_available); }
RTM_EXPORT(epd_app_font_status);
RTM_EXPORT(epd_app_font_metrics);
RTM_EXPORT(epd_app_font_glyph_acquire);
RTM_EXPORT(epd_app_font_glyph_release);
RTM_EXPORT(epd_app_font_glyph_ready);
RTM_EXPORT(epd_app_font_prepare_text);
RTM_EXPORT(epd_app_font_prepare_text_preview);
RTM_EXPORT(epd_app_font_oblique);
RTM_EXPORT(epd_app_font_ref);
RTM_EXPORT(epd_app_font_cancel);
RTM_EXPORT(epd_app_font_set_purpose);
RTM_EXPORT(epd_app_font_page_create);
RTM_EXPORT(epd_app_font_page_pin);
RTM_EXPORT(epd_app_font_page_release);
RTM_EXPORT(epd_app_font_reclaim_pixels);
void epd_app_font_cache_stats(epd_app_font_cache_stats_t *out)
{ ui_reader_font_cache_stats(out); ui_reader_font_remote_draw_stats(out); }
RTM_EXPORT(epd_app_font_cache_stats);
void epd_app_font_remote_stats(epd_app_font_remote_stats_t *out) { ui_reader_font_remote_stats(out); }
RTM_EXPORT(epd_app_font_remote_stats);
const lv_draw_buf_t *epd_app_font_derived_acquire(const lv_font_t *font, uint32_t glyph,
                                                uint32_t style, lv_cache_entry_t **entry)
{ return ui_reader_font_derived_acquire(font, glyph, style, entry); }
const lv_draw_buf_t *epd_app_font_derived_store(const lv_font_t *font, uint32_t glyph,
                                              uint32_t style, const lv_draw_buf_t *bitmap,
                                              lv_cache_entry_t **entry)
{ return ui_reader_font_derived_store(font, glyph, style, bitmap, entry); }
void epd_app_font_derived_release(lv_cache_entry_t *entry)
{ ui_reader_font_derived_release(entry); }
void epd_app_font_derived_drop(const lv_font_t *font)
{ ui_reader_font_derived_drop(font); }
RTM_EXPORT(epd_app_font_derived_acquire);
RTM_EXPORT(epd_app_font_derived_store);
RTM_EXPORT(epd_app_font_derived_release);
RTM_EXPORT(epd_app_font_derived_drop);
RTM_EXPORT(lv_draw_wait_for_finish);
RTM_EXPORT(rt_work_init);
RTM_EXPORT(rt_workqueue_sysq);
RTM_EXPORT(rt_workqueue_submit_work);
RTM_EXPORT(rt_workqueue_cancel_work);
RTM_EXPORT(rt_workqueue_cancel_work_sync);
RTM_EXPORT(epd_app_status);
RTM_EXPORT(epd_app_set_recent_reading);
RTM_EXPORT(epd_app_request_full_refresh);
RTM_EXPORT(epd_app_open_text_settings);
RTM_EXPORT(epd_app_font_create);
RTM_EXPORT(epd_app_font_destroy);
RTM_EXPORT(ui_settings_enabled);
RTM_EXPORT(ui_settings_cycle);
RTM_EXPORT(ui_settings_value);
RTM_EXPORT(ui_settings_index);
RTM_EXPORT(ui_icon_bookshelf);
RTM_EXPORT(gui_app_exec_now);
RTM_EXPORT(gui_app_is_page_present);
RTM_EXPORT(lv_color_eq);
RTM_EXPORT(lv_event_get_indev);
RTM_EXPORT(lv_indev_get_point);
RTM_EXPORT(lv_label_set_long_mode);
RTM_EXPORT(lv_log_add);
RTM_EXPORT(lv_malloc);
RTM_EXPORT(lv_malloc_zeroed);
RTM_EXPORT(lv_realloc);
RTM_EXPORT(lv_free);
RTM_EXPORT(lv_obj_add_state);
RTM_EXPORT(lv_obj_remove_state);
RTM_EXPORT(lv_obj_get_child);
RTM_EXPORT(lv_obj_get_child_count);
RTM_EXPORT(lv_obj_get_parent);
RTM_EXPORT(lv_obj_set_parent);
RTM_EXPORT(lv_obj_move_to_index);
RTM_EXPORT(lv_obj_check_type);
RTM_EXPORT(lv_obj_update_layout);
RTM_EXPORT(lv_obj_get_style_prop);
RTM_EXPORT(lv_obj_get_width);
RTM_EXPORT(lv_obj_get_height);
RTM_EXPORT(lv_obj_has_flag);
RTM_EXPORT(lv_obj_has_state);
RTM_EXPORT(lv_obj_set_height);
RTM_EXPORT(lv_obj_set_width);
RTM_EXPORT(lv_obj_set_x);
RTM_EXPORT(lv_obj_set_y);
RTM_EXPORT(lv_obj_set_style_text_letter_space);
RTM_EXPORT(lv_obj_set_style_text_line_space);
RTM_EXPORT(lv_text_attributes_init);
RTM_EXPORT(lv_text_get_next_line);
RTM_EXPORT(lv_font_get_glyph_dsc);
RTM_EXPORT(lv_font_get_line_height);
RTM_EXPORT(lv_font_get_glyph_bitmap);
RTM_EXPORT(lv_font_glyph_release_draw_data);
RTM_EXPORT(lv_obj_set_style_text_decor);
RTM_EXPORT(app_service_start);
RTM_EXPORT(app_service_stop);
RTM_EXPORT(app_service_running);
RTM_EXPORT(lv_event_get_code);
RTM_EXPORT(lv_event_get_param);
RTM_EXPORT(lv_keyboard_create);
RTM_EXPORT(lv_keyboard_set_mode);
RTM_EXPORT(lv_keyboard_set_popovers);
RTM_EXPORT(lv_keyboard_set_textarea);
RTM_EXPORT(lv_keyboard_get_textarea);
RTM_EXPORT(lv_keyboard_get_map_array);
RTM_EXPORT(lv_keyboard_get_mode);
RTM_EXPORT(lv_buttonmatrix_get_selected_button);
RTM_EXPORT(lv_buttonmatrix_get_button_text);
RTM_EXPORT(lv_buttonmatrix_set_button_ctrl);
RTM_EXPORT(lv_buttonmatrix_clear_button_ctrl);
RTM_EXPORT(lv_textarea_create);
RTM_EXPORT(lv_textarea_get_text);
RTM_EXPORT(lv_textarea_get_one_line);
RTM_EXPORT(lv_textarea_set_text);
RTM_EXPORT(lv_textarea_set_accepted_chars);
RTM_EXPORT(lv_textarea_set_max_length);
RTM_EXPORT(lv_textarea_set_one_line);
RTM_EXPORT(lv_textarea_set_placeholder_text);
RTM_EXPORT(lv_textarea_set_insert_replace);
RTM_EXPORT(lv_textarea_set_cursor_pos);
RTM_EXPORT(lv_textarea_get_cursor_pos);
RTM_EXPORT(lv_obj_get_y);
RTM_EXPORT(lv_obj_get_scroll_y);
RTM_EXPORT(lv_obj_get_scroll_bottom);
RTM_EXPORT(lv_obj_scroll_to_y);
RTM_EXPORT(lv_obj_scroll_to_view);
RTM_EXPORT(lv_obj_set_scroll_dir);
RTM_EXPORT(lv_obj_set_scrollbar_mode);
RTM_EXPORT(lv_obj_set_flex_grow);
RTM_EXPORT(lv_obj_align_to);
RTM_EXPORT(lv_indev_active);
RTM_EXPORT(lv_indev_wait_release);
RTM_EXPORT(lv_event_stop_processing);
RTM_EXPORT(lv_event_stop_bubbling);
RTM_EXPORT(lv_draw_triangle_dsc_init);
RTM_EXPORT(lv_draw_triangle);
RTM_EXPORT(lv_text_get_size);
RTM_EXPORT(lv_obj_set_style_anim_duration);
RTM_EXPORT(lv_obj_set_style_shadow_width);
RTM_EXPORT(lv_obj_set_style_border_side);
RTM_EXPORT(lv_slider_create);
RTM_EXPORT(lv_slider_get_value);
RTM_EXPORT(lv_slider_set_range);
RTM_EXPORT(lv_slider_set_value);
