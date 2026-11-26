/* A bound text page, one worker context, six occupied font slots and an offscreen LVGL display. */
#include "reader_types.h"
#include "ui_settings_internal.h"
#include "epd_reader_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define DOCUMENT_TITLE_MAX 64
#define EPD_APP_PSRAM 1
#define EPD_FONT_BODY 0
#define BOOK_STYLE_NOTEREF EPD_READER_NOTEREF
#define VIEW_BURST_TURN_MS 2000u
#define VIEW_HEIGHT (view.status.bottom - view.status.top)
typedef unsigned rt_tick_t;
typedef void *rt_sem_t, *rt_thread_t;
typedef int document_t, epd_activity_work_t, epd_activity_guard_t;
typedef epd_reader_cursor_t document_cursor_t;
typedef struct { unsigned unit, chapter, offset, skip_utf8; } document_anchor_t;
typedef struct { document_cursor_t cursor; document_anchor_t source; unsigned layout, page, pages, percent; } reader_position_t;
typedef bool (*reader_save_fn)(void *, const reader_position_t *);
typedef struct { int width, top, bottom, height; } reader_geometry_t;
typedef epd_reader_page_t view_page_t;
typedef epd_reader_run_t view_run_t;
typedef struct view_widget {
    struct view_widget *next; lv_obj_t *object; unsigned link; bool note;
    struct { bool moved, held; } touch;
} view_widget_t;
typedef struct view_frame view_frame_t;
typedef struct view_state {
    struct view_state *previous, *following;
    unsigned references, session, intent, layout, wanted, preview_page, progress;
    unsigned navigation_link, navigation_toc, published_intent, published_generation, window_state_charge;
    bool initialized, displayed, selected, parked, frame_published, boundary_target, target_pending;
    bool forward_step, numeric_target, initial_target_ready, preview_cache_active, display_pending, notified;
    int segment_turns, deferred_turns, turn_pending, turn_remaining;
    view_frame_t *rendering, *frames;
    view_page_t *building, *display;
    document_cursor_t initial_anchor, anchor;
    document_t *document; void *back_request;
    reader_view_t status; reader_geometry_t font_area;
    unsigned font_id, font_revision, font_identity, appearance, settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    lv_font_t *fonts[3], *oblique[3];
    /* Worker state invalidated by a new layout. */
    view_page_t *index, *prefetch;
    uint32_t display_layout;
    unsigned count, toc_count, toc_scanned, visited_count, visited_next, sparse_stride, sparse_pages;
    unsigned index_saved_count, numbered_page, cache_blocked, prefetch_number;
    bool font_metrics_ready, sparse_seek, toc_limited, count_loaded, count_saved;
    bool complete, index_loaded, index_loading, index_saved, from_cache, index_save_failed;
    bool preview, restoring, preview_clipped, preview_back_scanning, preview_back_ready;
    bool preview_back_seeking, preview_back_blocked, preview_back_failed;
    struct { unsigned wanted; bool window_dirty; } cache;
    reader_error_t failure;
    char link_target[1];
} view_state_t;
struct view_frame {
    view_frame_t *next, *backing; view_state_t *owner;
    unsigned references, number, intent, generation, layout, appearance;
    size_t window_charge;
    rt_tick_t preparation_started, requested_at, ready_at, completed_at;
    bool full_picture, complete;
    struct { uint8_t *data; } pixels; struct { void *first; } cold;
    void *renderer, *pins;
    lv_image_dsc_t image; reader_view_t info; reader_position_t position;
    char title[DOCUMENT_TITLE_MAX]; epd_reader_page_t page;
};
#include "channels.inc"

static view_state_t owner, *state, *parked_state;
#define view (*state)
static struct { view_state_t *owner; } window_slots[4];
static view_page_t original, ahead, building, index_page, prefetch;
static reader_geometry_t viewport = {80, 10, 58, 70};
static const unsigned antialiased = UI_READER_NEGATIVE | UI_READER_GUIDE_DOTTED | UI_READER_ANTIALIAS;
static lv_font_t shells[6];
static uint8_t selected[UI_SETTING_COUNT], stored[UI_SETTING_COUNT];
static unsigned depth, slot_checks, prepares, writes, saves, pagination_resets, window_clears;
static bool fail_activity, fail_widget, fail_setting, testing_resize;
static lv_obj_t *body, *old_root, *old_bitmap;
static view_frame_t *old_bound;
static char visible_chapter[64];

static void unexpected(void);
static const unsigned refresh_counts[] = {UI_FULL_REFRESH_COUNTS};
static const ui_setting_t appearance_setting = {.option_count=UI_READER_APPEARANCE_COUNT};
const ui_setting_t *ui_settings_item(ui_setting_id_t id) { CHECK(id == UI_SETTING_READER_APPEARANCE); return &appearance_setting; }
bool ui_settings_enabled(ui_setting_id_t id) { CHECK(id == UI_SETTING_BLUETOOTH); return false; }
unsigned ui_settings_refresh_count(void) { return refresh_counts[0]; }
#define reading_font_selected() 23u
#define system_reset_blocks_apps() false
#define storage_changing() false
#define board_power_pending() false
#define ui_scale_prepare(n) (unexpected(), false)
#define ui_scale_apply(n) unexpected()
#define btpan_enable(n) ((void)(n), unexpected(), -1)
#define wlan_service_enable(n) ((void)(n), unexpected(), -1)
#define epd_wave_set_part_times(n) ((void)(n), unexpected())
#define RT_EOK 0
static bool ui_settings_store_save(const uint8_t candidate[UI_SETTING_COUNT])
{
    CHECK(candidate[UI_SETTING_READER_APPEARANCE] == ui.ready->appearance);
    if (fail_setting) return false;
    memcpy(stored, candidate, sizeof(stored));
    return true;
}
#include "settings_set.inc"
static bool observed_settings_set(ui_setting_id_t id, unsigned value)
{
    CHECK(!depth && id == UI_SETTING_READER_APPEARANCE && ui.ready && value == ui.ready->appearance);
    CHECK(ui.root != old_root && lv_obj_is_valid(old_root) && ui.bound == old_bound);
    CHECK(lv_obj_get_parent(ui.root) == body && lv_image_get_src(ui.bitmap) == &ui.ready->image);
    CHECK(lv_obj_get_child_count(ui.root) == 2); /* New bitmap and link both exist before committing. */
    ++writes; return ui_settings_set(id, value);
}
#define ui_settings_set observed_settings_set
static bool save_position(void *user, const reader_position_t *p)
{
    (void)user;
    CHECK(p->page == 5 && p->pages == (view.preview ? 0u : 31u));
    CHECK(p->cursor.record == 7 && p->cursor.byte == 13);
    CHECK(p->source.unit == 1 && p->source.offset == 241);
    CHECK(ui_settings_index(UI_SETTING_READER_APPEARANCE) == ui.bound->appearance);
    CHECK(stored[UI_SETTING_READER_APPEARANCE] == ui.bound->appearance);
    ++saves; return true;
}
static bool epd_app_reading_font_can_create(unsigned count) { ++slot_checks; CHECK(count == 6); return false; }
static bool prepare_fonts(view_state_t *s) { (void)s; ++prepares; return false; }
static unsigned epd_app_reading_font_id(void) { return 23; }
static unsigned epd_app_reading_font_revision(void) { return 71; }
static reader_geometry_t reader_geometry(void) { return viewport; }
static const lv_font_t *epd_app_font_role(unsigned role) { CHECK(role == EPD_FONT_BODY); return LV_FONT_DEFAULT; }
static void *view_allocate(size_t size, int region)
{ CHECK(!depth && region == EPD_APP_PSRAM); return fail_widget && size == sizeof(view_widget_t) ? NULL : calloc(1, size); }
static void frame_release(view_frame_t *f)
{
    if (!f) return;
    CHECK(f->references && f->owner == &owner);
    if (--f->references) return;
    if (f->backing) frame_release(f->backing); else free(f->pixels.data);
    CHECK(owner.references); --owner.references; free(f);
}
static void page_reset(view_page_t *p, document_cursor_t anchor)
{ memset(p, 0, sizeof(*p)); p->start = p->cursor = anchor; p->collect = true; }
static epd_activity_work_t *view_activity_new(void) { return fail_activity ? NULL : calloc(1, sizeof(epd_activity_work_t)); }
static void unexpected(void) { CHECK(!"unexpected navigation, font or allocator path"); }
static void frame_publish(view_frame_t *f);
#define rt_enter_critical() (++depth)
#define rt_exit_critical() CHECK(depth-- > 0)
#define rt_tick_get() lv_tick_get()
#define rt_tick_from_millisecond(ms) (ms)
#define elapsed_ms(start) (lv_tick_get() - (start))
#define duration_ms(ticks) (ticks)
#define rt_kprintf(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define epd_app_free free
#define epd_activity_work_close free
#define state_dispose free
#define epd_app_object_init lv_obj_remove_style_all
#define reader_error_text(e) ((e) ? "failure" : "")
#define epd_activity_work_ready(a) ((a) != NULL)
#define epd_activity_work_enter(a,g) ((void)(g), (a) != NULL)
#define epd_activity_work_leave(g) ((void)(g))
#define epd_reader_font_begin() (unexpected(), -1)
#define epd_reader_note_close() ((void)0)
#define reader_note_ui_process() ((void)0)
#define reader_view_touch_input(e) ((void)(e), false)
#define reader_view_touch_event(...) unexpected()
#define reader_note_ui_open(...) unexpected()
#define epd_reader_follow_link(...) unexpected()
#define epd_reader_collect() ((void)0)
#define wake_view() ((void)0)
#define view_cancel() ((void)0)
#define turn_cancel() unexpected()
#define epd_reader_ui_wake() ((void)0)
#define first_font_cancel() unexpected()
#define first_font_ready(...) ((void)0)
#define link_clear() ((void)0)
#define navigation_clear() ((void)0)
#define document_set_foreground(...) unexpected()
#define document_current_title(d,r,s,n,i) ((void)snprintf(s,n,"chapter"))
#define document_source_percent(d,p,s) ((void)(d), (void)(s), *(p))
#define pagination_snapshot(s,i) ((void)(s), (void)(i))
#define document_record_cancel(r) CHECK(!(r))
#define cluster_retire(...) unexpected()
#define frame_turn_matches(...) false
#define frame_turn_cached(...) false
#define frame_handoff_note(...) unexpected()
#define pagination_seek(...) unexpected()
#define progress_seek(...) unexpected()
#define target_select(...) unexpected()
#define pagination_reset() do { CHECK(testing_resize); ++pagination_resets; } while (0)
#define window_clear() do { CHECK(testing_resize); ++window_clears; } while (0)
#define state_retire(owner) CHECK(!(owner))
#define compute_cancel() ((void)0)
#define images_cancel_pending() ((void)0)
#define images_prune(optional) CHECK(optional)
#define trace_start() ((void)0)
#define near_trace_finish(reason) ((void)(reason))
#define reader_page_cache_clear(c) memset(c, 0, sizeof(*(c)))
#define core_seek_cursor(...) unexpected()
#define core_cancel_pending() unexpected()
#define core_turn(...) unexpected()
#define chapter_select(...) (unexpected(), (view_state_t *)NULL)
#define follow_toc(...) unexpected()
#define follow_link(...) unexpected()
#define target_needed() (unexpected(), false)
#define frame_find(...) (unexpected(), (view_frame_t *)NULL)
#define window_charge(n) (unexpected(), false)
#define window_uncharge_bytes(n) CHECK(!(n))
#define fail(e) unexpected()
#include "appearance.inc"

/* Supply only completed pixels and the source anchor at the raster boundary. The worker
 * chooses the page, runs, appearance and page number; publication and binding are real. */
static void publish_pixels(const epd_reader_page_t *page)
{
    CHECK(page->done);
    unsigned number = view.preview_cache_active ? view.preview_page : view.wanted;
    view_frame_t *f = frame_create(page, number, view.intent);
    CHECK(f && !view.frames && !f->full_picture);
    f->pixels.data = calloc(1, f->image.data_size); CHECK(f->pixels.data);
    f->image.data = f->pixels.data; f->complete = true;
    f->position.cursor = f->page.start; f->position.source = (document_anchor_t){1, 0, 241, 0};
    view.frames = f; frame_publish(f);
    CHECK(mailbox.ready && mailbox.ready->backing == f);
}
static void check_page(const epd_reader_page_t *page)
{
    CHECK(cursor_equal(page->start, original.start) && cursor_equal(page->cursor, original.cursor));
    CHECK(page->count == original.count && page->bytes == original.bytes && page->collect && page->done);
    CHECK(!memcmp(page->runs, original.runs, original.count * sizeof(original.runs[0])));
    CHECK(!memcmp(page->text, original.text, original.bytes));
}
static void check_font_objects(void)
{
    CHECK(view.font_id == 23 && ui.font_id == 23 && view.font_revision == 71 && ui.font_revision == 71);
    for (unsigned i = 0; i < 3; ++i) CHECK(view.fonts[i] == shells + i && view.oblique[i] == shells + 3 + i);
    CHECK(view.status.font == shells && !slot_checks && !prepares && !font_request.context && !mailbox.configuration);
}
static void check_layout(void)
{
    CHECK(state == ui.session && state == &owner && view.layout == 91 && ui.bound->layout == view.layout);
    CHECK(ui.info.page == 5 && ui.bound->number == 4 && ui.info.pages == 31 && ui.info.percent == 16);
    check_font_objects(); check_page(&ui.bound->page);
}
static void open_bound_page(void)
{
    static document_t document;
    if (testing_resize) viewport = (reader_geometry_t){684, 54, 1168, 1216};
    lv_init(); CHECK(lv_display_create(viewport.width, viewport.height));
    body = lv_obj_create(lv_screen_active());
    state = ui.session = &owner; view.document = &document;
    view.selected = view.displayed = view.initialized = true;
    view.session = ui.id = mailbox.session = 2; view.intent = ui.intent = mailbox.intent = 9;
    view.font_id = epd_app_reading_font_id(); view.font_revision = epd_app_reading_font_revision();
    view.font_area = reader_geometry(); view.appearance = antialiased;
    selected[UI_SETTING_READER_APPEARANCE] = (uint8_t)antialiased;
    for (unsigned i = 0; i < 3; ++i) { view.fonts[i] = shells + i; view.oblique[i] = shells + 3 + i; }
    shells[0].line_height = 36;
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        view.settings[i - UI_SETTING_FONT_SIZE] = selected[i] = (uint8_t)((i - UI_SETTING_FONT_SIZE) % 3);
    view.layout = 91; view.wanted = 4; view.progress = 16; view.building = &building; view.display = &ahead;
    view.status = (reader_view_t){.font=shells, .ready=true, .page=5, .pages=31, .top=10, .bottom=58, .content_width=64, .margin=8};
    if (testing_resize) {
        view.font_identity = 17;
        view.settings[UI_SETTING_MARGIN - UI_SETTING_FONT_SIZE] = selected[UI_SETTING_MARGIN] = 4;
        layout_for_area(state, viewport);
        view.index = &index_page; view.prefetch = &prefetch;
    }
    view.display_layout = view.layout;
    memcpy(stored, selected, sizeof(stored));
    ui_configuration(&owner); ui.save = save_position;
    original.start = (document_cursor_t){7, 13}; original.cursor = (document_cursor_t){8, 5};
    original.runs[0] = (view_run_t){.x=1, .y=2, .width=12, .height=8, .bytes=4};
    original.runs[1] = (view_run_t){.x=5, .y=16, .width=20, .height=8, .text=4, .bytes=4, .link=17};
    original.count = 2; original.done = original.collect = 1; original.bytes = 8; memcpy(original.text, "Textbody", 8);
    publish_pixels(&original); epd_reader_step(8, true, true);
    if (testing_resize) {
        CHECK(ui.ready && !ui.bound && !epd_reader_resize());
        CHECK(!mailbox.command.kind && ui.intent == 9);
    }
    CHECK(epd_reader_bind(body));
    CHECK(epd_reader_bound_image(body) == &ui.bound->image && writes == 0 && saves == 1);
    if (!testing_resize) check_layout();
}
static void remember_binding(void) { old_root = ui.root; old_bitmap = ui.bitmap; old_bound = ui.bound; }
static void check_old(unsigned saved)
{
    CHECK(ui_settings_index(UI_SETTING_READER_APPEARANCE) == saved && ui.appearance == saved);
    CHECK(stored[UI_SETTING_READER_APPEARANCE] == saved);
    CHECK(ui.bound == old_bound && ui.root == old_root && ui.bitmap == old_bitmap);
    CHECK(lv_obj_is_valid(old_root) && lv_image_get_src(old_bitmap) == epd_reader_bound_image(body));
    CHECK(epd_reader_bound_image(body) == &old_bound->image && lv_obj_get_child_count(body) == 1);
    check_layout();
}
static void request(unsigned target)
{
    unsigned old = ui.appearance, serial = ui.intent, before = writes;
    remember_binding();
    CHECK(epd_reader_request_appearance(target) && ui.appearance_pending && ui.requested_appearance == target);
    CHECK(ui.intent == serial + 1 && mailbox.command.serial == ui.intent && mailbox.command.session == ui.id);
    CHECK(mailbox.command.kind == INTENT_REPAINT && mailbox.command.origin == old_bound);
    CHECK(mailbox.command.appearance == target && ui.pending && !epd_reader_settings_changed());
    CHECK(epd_reader_request_appearance(target) && ui.intent == serial + 1);
    CHECK(epd_reader_bind(body) && writes == before); check_old(old);
}
static void raster_and_receive(void)
{
    unsigned saved = ui.appearance, before = writes;
    view_command_t command = mailbox.command; mailbox.command = (view_command_t){0};
    /* The worker is ahead of the page still visible in LVGL. */
    ahead.start = ahead.cursor = (document_cursor_t){81, 4}; building = ahead;
    view.wanted = 14; view.display_pending = false; view.anchor = ahead.start;
    command_apply(command, &state); frame_release(command.origin);
    CHECK(view.wanted == ui.bound->number && cursor_equal(view.anchor, original.start));
    CHECK(view.appearance == command.appearance && view.display_pending && !view.frames);
    check_page(view.building); check_old(saved);
    publish_pixels(view.building);
    epd_reader_step(8, true, false); CHECK(!ui.ready && mailbox.ready && ui.pending);
    epd_reader_step(8, true, true); CHECK(ui.ready && !mailbox.ready && ui.pending);
    CHECK(ui.ready->intent == ui.intent && ui.ready->appearance == command.appearance && writes == before);
    check_old(saved);
}
static void roundtrip(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        unsigned target = ui.appearance ^ UI_READER_ANTIALIAS;
        request(target); raster_and_receive();
        view_frame_t *ready = ui.ready;
        CHECK(epd_reader_bind(body) && ui.bound == ready && !ui.pending && !ui.appearance_pending);
        CHECK(ui.appearance == target && ui_settings_index(UI_SETTING_READER_APPEARANCE) == target);
        CHECK(stored[UI_SETTING_READER_APPEARANCE] == target);
        CHECK(writes == i + 1 && saves == i + 2 && mailbox.ack == ready);
        CHECK(!lv_obj_is_valid(old_root) && lv_obj_get_child_count(body) == 1);
        CHECK(lv_image_get_src(ui.bitmap) == &ready->image && epd_reader_bound_image(body) == &ready->image);
        CHECK(epd_reader_bind(body) && writes == i + 1 && !epd_reader_settings_changed()); check_layout();
    }
    CHECK(ui.appearance == antialiased);
}
static void failure(const char *which)
{
    unsigned target = antialiased ^ UI_READER_ANTIALIAS;
    if (!strcmp(which, "enqueue-failure")) {
        remember_binding(); fail_activity = true;
        unsigned serial = ui.intent;
        CHECK(!epd_reader_request_appearance(target) && ui.intent == serial && !mailbox.command.origin);
        CHECK(ui.info.error_code == READER_ERROR_LAYOUT_MEMORY);
    } else {
        request(target);
        if (!strcmp(which, "render-failure")) {
            mailbox.result_intent = ui.intent; mailbox.rejected = true; mailbox.error = READER_ERROR_IMAGE_MEMORY;
            epd_reader_step(8, true, true); CHECK(!epd_reader_bind(body) && ui.info.error_code == READER_ERROR_IMAGE_MEMORY);
        } else {
            raster_and_receive();
            fail_widget = !strcmp(which, "widget-failure"); fail_setting = !strcmp(which, "settings-failure");
            CHECK(fail_widget || fail_setting);
            CHECK(!epd_reader_bind(body) && !ui.ready);
            CHECK(ui.info.error_code == (fail_setting ? READER_ERROR_IO : READER_ERROR_WIDGET_MEMORY));
        }
    }
    CHECK(!ui.appearance_pending && !ui.pending && writes == (unsigned)fail_setting && saves == 1);
    check_old(antialiased);
}
static void resize_bound_page(void)
{
    remember_binding();
    document_cursor_t anchor = old_bound->position.cursor;
    unsigned old_layout = old_bound->layout, serial = ui.intent;
    CHECK(old_bound->complete && old_bound->image.header.w == 648 && old_bound->image.header.h == 1114);
    viewport = (reader_geometry_t){1216, 54, 636, 684};
    lv_display_set_resolution(lv_display_get_default(), viewport.width, viewport.height);
    CHECK(epd_reader_resize() && ui.intent == serial + 1 && ui.pending && !epd_reader_settings_changed());
    view_command_t command = mailbox.command; mailbox.command = (view_command_t){0};
    CHECK(command.kind == INTENT_RESIZE && command.origin == old_bound && command.geometry.width == 1216);
    CHECK(ui.bound == old_bound && epd_reader_bound_image(body) == &old_bound->image);

    /* A worker page ahead of the bound page must not become the resize anchor. */
    document_cursor_t ahead_cursor = {81, 4};
    ahead.start = ahead.cursor = ahead_cursor; building = ahead;
    view.anchor = ahead_cursor; view.count = 31; view.visited_count = 1;
    command_apply(command, &state); frame_release(command.origin);
    CHECK(pagination_resets == 1 && window_clears == 1 && !view.count && !view.visited_count);
    CHECK(!view.frames && !mailbox.ready && !ui.ready && ui.bound == old_bound && ui.root == old_root);
    CHECK(state == &owner && view.preview && view.restoring && core_waiting() && !view.display_pending);
    CHECK(cursor_equal(view.display->start, ahead_cursor) && !cursor_equal(ahead_cursor, anchor));
    CHECK(cursor_equal(view.anchor, anchor) && cursor_equal(view.building->start, anchor) && !view.building->done);
    CHECK(view.layout != old_layout && view.status.margin == 18 && width_available() == 1180 && VIEW_HEIGHT == 582);
    check_font_objects();
    epd_reader_detach();

    /* The bounded layout job has consumed input, but has not completed the page. */
    static epd_reader_batch_t batch;
    static const char record[] = "Earlier text Textbody";
    memcpy(batch.data, record, sizeof(record));
    batch.kind = EPD_READER_TEXT_RECORD; batch.length = sizeof(record) - 1; batch.next_record = anchor.record + 1;
    batch.width = width_available(); batch.height = VIEW_HEIGHT;
    batch.base_height = batch.font_height = batch.font_size = view.fonts[0]->line_height;
    batch.line_space = view.status.line_space; batch.limit = 1; batch.page = *view.building;
    for (unsigned i = 0; i < batch.length; ++i) batch.glyphs[i] = (epd_reader_glyph_t){record[i], 18, 1};
    CHECK(epd_reader_layout(&batch) == 0 && batch.events == 1 && !batch.page.done);
    CHECK(batch.page.cursor.record == anchor.record && batch.page.cursor.byte == anchor.byte + 1);
    *view.building = batch.page;
    /* A late result for the old intent must not satisfy the pending resize. */
    frame_ref(old_bound); mailbox.ready = old_bound;
    CHECK(epd_reader_step(8, true, true) == READER_WORK_TARGET && !epd_reader_bind(body));
    CHECK(!epd_reader_resize() && ui.intent == serial + 1 && !mailbox.command.kind);
    CHECK(!ui.ready && !ui.root && ui.bound == old_bound && ui.pending && saves == 1);

    batch.limit = EPD_READER_BATCH;
    CHECK(epd_reader_layout(&batch) == 0 && batch.page.cursor.record == anchor.record + 1);
    batch.kind = EPD_READER_EOF; batch.length = 0;
    CHECK(epd_reader_layout(&batch) == 0 && batch.page.done && batch.page.eof);
    CHECK(batch.page.count == 1 && batch.page.runs[0].bytes == 8);
    CHECK(!memcmp(batch.page.text + batch.page.runs[0].text, "Textbody", 8));
    *view.building = batch.page; publish_pixels(view.building);
    epd_reader_step(8, true, true);
    view_frame_t *ready = ui.ready;
    CHECK(ready && ready->layout == view.layout && cursor_equal(ready->position.cursor, anchor));
    CHECK(epd_reader_bind(body) && ui.bound == ready && !ui.pending && saves == 2 && !writes);
    CHECK(epd_reader_bound_image(body) == &ready->image && ready->image.header.w == 1180 && ready->image.header.h == 582);
    lv_obj_update_layout(lv_screen_active());
    CHECK(lv_obj_get_x(body) == 18 && lv_obj_get_y(body) == 54);
    CHECK(lv_obj_get_width(body) == 1180 && lv_obj_get_height(body) == 582);
    check_font_objects();
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); testing_resize = !strcmp(argv[1], "resize"); open_bound_page();
    if (testing_resize) resize_bound_page();
    else if (!strcmp(argv[1], "roundtrip")) roundtrip(); else failure(argv[1]);
    epd_reader_detach(); frame_release(ui.bound); frame_release(mailbox.ack); frame_release(mailbox.command.origin);
    frame_cache_clear(); free(mailbox.activity); CHECK(!depth && !owner.references && !ui.widgets);
    lv_deinit();
    printf("PASS %s: slot_checks=%u prepare_fonts=%u settings_set=%u position_saves=%u\n",
           argv[1], slot_checks, prepares, writes, saves);
    return 0;
}
