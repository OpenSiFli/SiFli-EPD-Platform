/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_memory.h"
#include "platform/epd_app_font.h"
#include "platform/epd_reader_session.h"
#include "epd_reader_view.h"
#include "epd_app.h"
#include "epd_refresh.h"
#include "ui_settings.h"
#include "reader/reader_service.h"
#include "reader/reader_flow.h"
#include "reader/reader_view_ui.h"
#include "../../../modules/common/reader/reader_font.h"
#include <rtm.h>
#include <string.h>

typedef struct reader_label {
    struct reader_label *next;
    lv_obj_t *object;
} reader_label_t;
typedef struct {
    lv_font_t *normal[3], *fallback[3], *oblique[3];
    unsigned references, size, weight;
    uint32_t family, revision;
} reader_fonts_t;
typedef struct {
    epd_reader_page_t page;
    lv_obj_t *root;
    reader_label_t *labels, *cursor, *tail;
    reader_fonts_t *fonts;
    epd_read_anchor_t position;
    unsigned prepared;
    bool cleaning;
} reader_surface_t;
typedef struct {
    reader_flow_t flow;
    reader_surface_t surface[2];
    lv_obj_t *container;
    lv_display_t *display;
    reader_fonts_t *fonts;
    unsigned front;
    uint32_t submission, staged_generation, generation;
    uint32_t attempted_font_id, attempted_font_revision;
    unsigned attempted_size, attempted_weight;
    bool font_attempted;
    epd_read_anchor_t visible_position;
    bool staging, awaiting, drawn, visible, visible_end, deleting;
    int error;
} reader_ui_t;
static reader_ui_t *view;
/* Session IDs remain unique across reader-service restarts. A container may
 * detach and reattach while its reading session stays open. */
static epd_read_session_t font_session;

static void drawn(lv_event_t *event)
{
    reader_ui_t *v = lv_event_get_user_data(event);
    if (v->awaiting && lv_event_get_target_obj(event) == v->surface[v->front].root) v->drawn = true;
}
static void submitted(lv_event_t *event)
{
    reader_ui_t *v = lv_event_get_user_data(event);
    if (v->awaiting && v->drawn && epd_refresh_submission() != v->submission) reader_service_notify();
}

static const lv_font_t *font_for(reader_fonts_t *fonts, uint32_t style)
{
    unsigned i = style & BOOK_STYLE_HEADING ? 2u : style & BOOK_STYLE_BOLD ? 1u : 0u;
    return style & BOOK_STYLE_ITALIC ? fonts->oblique[i] : fonts->normal[i];
}
static uint32_t height(void *user, uint32_t style)
{ return font_for(((reader_ui_t *)user)->fonts, style)->line_height; }
static uint32_t advance(void *user, uint32_t style, uint32_t letter)
{
    lv_font_glyph_dsc_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    lv_font_get_glyph_dsc(font_for(((reader_ui_t *)user)->fonts, style), &glyph, letter, 0);
    return glyph.adv_w;
}
static void fonts_free(reader_fonts_t *fonts)
{
    if (!fonts || --fonts->references) return;
    lv_draw_wait_for_finish();
    for (unsigned i = 0; i < 3; ++i)
    {
        document_font_free(fonts->oblique[i]);
        epd_app_font_destroy(fonts->normal[i]); epd_app_font_destroy(fonts->fallback[i]);
    }
    epd_app_free(fonts);
}
static reader_fonts_t *fonts_create(void)
{
    static const unsigned sizes[] = {28, 30, 32, 34, 36, 40, 44, 48};
    unsigned index = ui_settings_index(UI_SETTING_FONT_SIZE);
    if (index >= sizeof(sizes) / sizeof(sizes[0])) return NULL;
    reader_fonts_t *fonts = epd_app_alloc(sizeof(*fonts), EPD_APP_PSRAM);
    if (!fonts) return NULL;
    memset(fonts, 0, sizeof(*fonts)); fonts->references = 1;
    fonts->family = epd_app_reading_font_id();
    fonts->revision = epd_app_reading_font_revision();
    fonts->size = index; fonts->weight = ui_settings_index(UI_SETTING_FONT_WEIGHT);
    for (unsigned i = 0; i < 3; ++i)
    {
        uint32_t identity;
        fonts->normal[i] = epd_app_font_create(fonts->family, sizes[index] + (i == 2 ? 6 : 0),
            i ? 1 : fonts->weight, &identity, &fonts->fallback[i]);
        if (!fonts->normal[i]) { fonts_free(fonts); return NULL; }
        fonts->normal[i]->kerning = LV_FONT_KERNING_NONE;
        if (fonts->fallback[i]) fonts->fallback[i]->kerning = LV_FONT_KERNING_NONE;
        fonts->oblique[i] = document_font_oblique(fonts->normal[i]);
        if (!fonts->oblique[i]) { fonts_free(fonts); return NULL; }
    }
    return fonts;
}
static bool fonts_changed(const reader_fonts_t *fonts)
{
    return fonts->family != epd_app_reading_font_id() ||
        fonts->revision != epd_app_reading_font_revision() ||
        fonts->size != ui_settings_index(UI_SETTING_FONT_SIZE) ||
        fonts->weight != ui_settings_index(UI_SETTING_FONT_WEIGHT);
}
static bool fonts_retry_pending(const reader_ui_t *v)
{
    return fonts_changed(v->fonts) && (!v->font_attempted ||
        v->attempted_font_id != epd_app_reading_font_id() ||
        v->attempted_font_revision != epd_app_reading_font_revision() ||
        v->attempted_size != ui_settings_index(UI_SETTING_FONT_SIZE) ||
        v->attempted_weight != ui_settings_index(UI_SETTING_FONT_WEIGHT));
}
static void unbind(reader_ui_t *v)
{
    reader_sessions_bind(&v->flow.core->sessions, v->flow.session, false);
    v->container = NULL; v->staging = v->awaiting = false;
    reader_flow_close(&v->flow);
}
static void container_deleted(lv_event_t *event)
{
    reader_ui_t *v = lv_event_get_user_data(event);
    if (v->deleting) return;
    /* Child widgets are deleted by LVGL after the parent's DELETE event. */
    v->surface[0].root = v->surface[1].root = NULL;
    unbind(v); reader_service_notify();
}
static void surfaces_free(reader_ui_t *v)
{
    v->deleting = true;
    if (v->container) lv_obj_remove_event_cb_with_user_data(v->container, container_deleted, v);
    lv_draw_wait_for_finish();
    for (unsigned i = 0; i < 2; ++i)
    {
        if (v->surface[i].root) lv_obj_delete(v->surface[i].root);
        reader_label_t *label = v->surface[i].labels;
        while (label) { reader_label_t *next = label->next; epd_app_free(label); label = next; }
        v->surface[i].root = NULL; v->surface[i].labels = v->surface[i].tail = NULL;
        fonts_free(v->surface[i].fonts); v->surface[i].fonts = NULL;
    }
}
int epd_read_view_attach(epd_read_session_t id, lv_obj_t *container)
{
    reader_core_t *core = reader_service_core();
    if (!core || !container) return EPD_READ_INVALID;
    if (view) return view->flow.session == id && view->container == container ? EPD_READ_OK : EPD_READ_BUSY;
    const reader_session_slot_t *s = reader_sessions_get(&core->sessions, id);
    if (!s || s->phase != READER_SESSION_ACTIVE) return EPD_READ_WAIT;
    lv_obj_update_layout(container);
    int32_t width = lv_obj_get_content_width(container), viewport_height = lv_obj_get_content_height(container);
    if (width <= 0 || viewport_height <= 0 || width > UINT16_MAX || viewport_height > UINT16_MAX) return EPD_READ_INVALID;
    if (font_session != id)
    {
        if (!epd_app_reading_font_begin()) return EPD_READ_NO_MEMORY;
        font_session = id;
    }
    reader_ui_t *v = epd_app_alloc(sizeof(*v), EPD_APP_PSRAM);
    if (!v) return EPD_READ_NO_MEMORY;
    memset(v, 0, sizeof(*v)); v->container = container; v->display = lv_obj_get_display(container);
    v->fonts = fonts_create();
    if (!v->fonts) { epd_app_free(v); return EPD_READ_NO_MEMORY; }
    reader_page_metrics_t metrics = {(uint32_t)width, (uint32_t)viewport_height,
        v->fonts->normal[0]->line_height * ui_settings_index(UI_SETTING_LINE_SPACING) / 5u,
        v->fonts->normal[0]->line_height, EPD_READER_BATCH, v, height, advance};
    int result = reader_flow_init(&v->flow, core, id, &metrics);
    if (!result) result = reader_sessions_bind(&core->sessions, id, true);
    if (result) { fonts_free(v->fonts); epd_app_free(v); return result; }
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_t *root = lv_obj_create(container); v->surface[i].root = root;
        if (!root)
        {
            surfaces_free(v); unbind(v); fonts_free(v->fonts); epd_app_free(v);
            return EPD_READ_NO_MEMORY;
        }
        epd_app_object_init(root);
        lv_obj_set_style_pad_all(root, 0, 0); lv_obj_set_style_border_width(root, 0, 0);
        lv_obj_set_style_bg_color(root, lv_color_white(), 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_size(root, width, viewport_height); lv_obj_set_pos(root, 0, 0);
        lv_obj_add_event_cb(root, drawn, LV_EVENT_DRAW_POST_END, v);
    }
    if (s->visible.version)
    {
        result = reader_core_seek(core, id, &s->visible);
        if (result)
        { surfaces_free(v); unbind(v); fonts_free(v->fonts); epd_app_free(v); return result; }
    }
    lv_obj_add_event_cb(container, container_deleted, LV_EVENT_DELETE, v);
    lv_display_add_event_cb(v->display, submitted, LV_EVENT_REFR_READY, v);
    view = v; reader_service_notify(); return EPD_READ_OK;
}
RTM_EXPORT(epd_read_view_attach);
void epd_read_view_detach(epd_read_session_t id)
{
    reader_ui_t *v = view;
    if (!v || v->flow.session != id) return;
    surfaces_free(v); unbind(v); reader_service_notify();
}
RTM_EXPORT(epd_read_view_detach);
static bool bind_step(reader_ui_t *v)
{
    reader_surface_t *surface = &v->surface[1u - v->front];
    unsigned budget = 8;
    if (surface->cleaning)
    {
        while (budget && surface->cursor)
        {
            --budget;
            lv_obj_set_style_text_font(surface->cursor->object, epd_app_font(), 0);
            surface->cursor = surface->cursor->next;
        }
        if (surface->cursor) return true;
        fonts_free(surface->fonts);
        surface->fonts = v->fonts; ++surface->fonts->references;
        surface->cursor = surface->labels; surface->cleaning = false;
    }
    while (budget && surface->prepared < surface->page.count)
    {
        --budget;
        const epd_reader_run_t *run = &surface->page.runs[surface->prepared];
        if (run->image) { v->error = EPD_READ_UNSUPPORTED; return false; }
        reader_label_t *label = surface->cursor;
        if (!label)
        {
            label = epd_app_alloc(sizeof(*label), EPD_APP_PSRAM);
            if (!label) { v->error = EPD_READ_NO_MEMORY; return false; }
            label->object = lv_label_create(surface->root); label->next = NULL;
            if (!label->object) { epd_app_free(label); v->error = EPD_READ_NO_MEMORY; return false; }
            if (surface->tail) surface->tail->next = label;
            else surface->labels = label;
            surface->tail = label;
            lv_obj_remove_flag(label->object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_add_flag(label->object, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        lv_obj_t *object = label->object;
        lv_obj_set_style_text_font(object, font_for(surface->fonts, run->style), 0);
        lv_obj_set_style_text_color(object, lv_color_black(), 0);
        lv_obj_set_style_text_line_space(object, 0, 0); lv_obj_set_style_text_letter_space(object, 0, 0);
        lv_obj_set_style_text_decor(object, run->link ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE, 0);
        lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(object, run->width + 2, run->height); lv_obj_set_pos(object, run->x, run->y);
        lv_label_set_text_static(object, surface->page.text + run->text);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        surface->cursor = label->next; ++surface->prepared;
    }
    if (surface->prepared < surface->page.count) return true;
    while (budget && surface->cursor)
    {
        --budget;
        lv_obj_add_flag(surface->cursor->object, LV_OBJ_FLAG_HIDDEN);
        surface->cursor = surface->cursor->next;
    }
    if (surface->cursor) return true;
    lv_obj_add_flag(v->surface[v->front].root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(surface->root, LV_OBJ_FLAG_HIDDEN);
    v->front = 1u - v->front; v->staging = false; v->awaiting = true; v->drawn = false;
    v->submission = epd_refresh_submission();
    return false;
}
bool reader_view_ui_process(reader_core_t *core, bool stopping)
{
    reader_ui_t *v = view;
    if (!v) return false;
    const reader_session_slot_t *s = reader_sessions_get(&core->sessions, v->flow.session);
    if (stopping || !s || s->phase == READER_SESSION_CLOSING || s->phase == READER_SESSION_SAVING)
        epd_read_view_detach(v->flow.session);
    if (v->container && s && s->phase == READER_SESSION_ACTIVE && fonts_retry_pending(v))
    {
        if (lv_obj_get_screen(v->container) != lv_screen_active()) return false;
        epd_read_view_reflow(v->flow.session);
    }
    bool again = reader_flow_step(&v->flow);
    if (reader_flow_closed(&v->flow))
    {
        lv_display_remove_event_cb_with_user_data(v->display, submitted, v);
        surfaces_free(v); fonts_free(v->fonts); view = NULL; epd_app_free(v); return false;
    }
    if (!v->container || !s || s->phase != READER_SESSION_ACTIVE) return false;
    if (v->generation != s->generation)
    { v->generation = s->generation; v->error = 0; v->staging = v->awaiting = false; }
    if (v->error || v->flow.error) return false;
    if (lv_obj_get_screen(v->container) != lv_screen_active()) return false;
    if ((v->staging || v->awaiting) && v->staged_generation != s->generation)
    { v->staging = v->awaiting = false; }
    if (v->awaiting)
    {
        if (!v->drawn || epd_refresh_submission() == v->submission) return false;
        int result = reader_flow_present(&v->flow);
        if (result) { v->error = result; return false; }
        v->awaiting = false; v->visible = true;
        v->visible_position = v->surface[v->front].position;
        v->visible_end = v->surface[v->front].page.eof != 0;
    }
    if (v->staging) return bind_step(v);
    const epd_reader_page_t *page = reader_flow_page(&v->flow);
    if (page && !v->flow.presented)
    {
        reader_surface_t *surface = &v->surface[1u - v->front];
        lv_draw_wait_for_finish();
        surface->page = *page; surface->position = v->flow.page.start;
        surface->prepared = 0; surface->cursor = surface->labels;
        surface->cleaning = surface->fonts != v->fonts;
        lv_obj_set_size(surface->root, (int32_t)v->flow.metrics.width, (int32_t)v->flow.metrics.height);
        v->staged_generation = s->generation; v->staging = true;
        return true;
    }
    return again;
}
int epd_read_view_next(epd_read_session_t id)
{
    if (!view || view->flow.session != id || !view->container) return EPD_READ_STALE;
    int result = reader_flow_next(&view->flow);
    if (!result) reader_service_notify();
    return result;
}
RTM_EXPORT(epd_read_view_next);
int epd_read_view_reflow(epd_read_session_t id)
{
    reader_ui_t *v = view;
    if (!v || v->flow.session != id || !v->container) return EPD_READ_STALE;
    const reader_session_slot_t *s = reader_sessions_get(&v->flow.core->sessions, id);
    if (!s || (s->phase != READER_SESSION_ACTIVE && s->phase != READER_SESSION_PAUSED)) return EPD_READ_STALE;
    lv_obj_update_layout(v->container);
    int32_t width = lv_obj_get_content_width(v->container), viewport_height = lv_obj_get_content_height(v->container);
    if (width <= 0 || viewport_height <= 0 || width > UINT16_MAX || viewport_height > UINT16_MAX) return EPD_READ_INVALID;
    bool font_changed = fonts_changed(v->fonts);
    /* Retry automatically only when the requested font configuration changes.
     * An explicit reflow still retries after memory or session recovery. */
    v->font_attempted = true;
    v->attempted_font_id = epd_app_reading_font_id();
    v->attempted_font_revision = epd_app_reading_font_revision();
    v->attempted_size = ui_settings_index(UI_SETTING_FONT_SIZE);
    v->attempted_weight = ui_settings_index(UI_SETTING_FONT_WEIGHT);
    uint32_t spacing = v->fonts->normal[0]->line_height * ui_settings_index(UI_SETTING_LINE_SPACING) / 5u;
    if (!font_changed && v->flow.metrics.line_space == spacing &&
        v->flow.metrics.width == (uint32_t)width && v->flow.metrics.height == (uint32_t)viewport_height) return EPD_READ_OK;
    reader_fonts_t *fonts = font_changed ? fonts_create() : v->fonts;
    if (!fonts) return EPD_READ_NO_MEMORY;
    int result = EPD_READ_OK;
    if (v->visible) result = reader_flow_seek(&v->flow, &v->visible_position);
    else if (v->flow.page.start.version && v->flow.generation == s->generation)
        result = reader_flow_seek(&v->flow, &v->flow.page.start);
    if (result) { if (font_changed) fonts_free(fonts); return result; }
    if (font_changed) { fonts_free(v->fonts); v->fonts = fonts; }
    v->flow.metrics.width = (uint32_t)width; v->flow.metrics.height = (uint32_t)viewport_height;
    v->flow.metrics.base_height = fonts->normal[0]->line_height;
    v->flow.metrics.line_space = fonts->normal[0]->line_height * ui_settings_index(UI_SETTING_LINE_SPACING) / 5u;
    /* Before the first source record, no seek anchor exists and no glyphs
     * have been measured. Refresh the empty batch without moving the source. */
    if (v->flow.job && !v->flow.page.start.version && v->flow.generation == s->generation)
        reader_page_init(&v->flow.page, reader_compute_data(v->flow.job),
                         &v->flow.metrics, v->flow.generation);
    v->staging = v->awaiting = false;
    reader_service_notify(); return EPD_READ_OK;
}
RTM_EXPORT(epd_read_view_reflow);
int epd_read_view_status(epd_read_session_t id, epd_read_view_status_t *status)
{
    if (!status) return EPD_READ_INVALID;
    memset(status, 0, sizeof(*status));
    if (!view || view->flow.session != id || !view->container) return EPD_READ_STALE;
    status->visible = view->visible;
    status->error = view->error ? view->error : view->flow.error;
    status->preparing = !status->error && (!view->flow.ready || view->staging || view->awaiting);
    if (view->visible)
    { status->position = view->visible_position; status->at_end = view->visible_end; }
    return EPD_READ_OK;
}
RTM_EXPORT(epd_read_view_status);
