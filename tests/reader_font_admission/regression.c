#define main remote_fixture_main
#include "host.c"
#undef main
#include "font_chain.inc"
#include "ui_settings.h"

/* The host filesystem advances this token with its path revision. */
uint32_t storage_volume_revision(storage_volume_t volume)
{ (void)volume; return (uint32_t)fixture_generation; }

typedef struct { int width, top, bottom; } reader_geometry_t;
typedef struct {
    lv_font_t *fonts[3], *fallbacks[3], *oblique[3], *retired[6], *retired_oblique[3];
    struct { lv_font_t *font; int margin, content_width, top, bottom, line_space; } status;
    uint32_t font_id, font_revision, failed_font_id, failed_font_revision, layout;
    reader_geometry_t font_area, failed_font_area;
    unsigned settings[5], failed_font_settings[5], cache, cache_blocked, prefetch_number;
    bool font_failed, font_metrics_ready;
} view_state_t;
static unsigned selected_size = 5, apply_errors, cache_clears;
static unsigned error_slot[2], error_attempt[2];
static char error_stage[2][16];
static reader_geometry_t reader_geometry(void) { return (reader_geometry_t){684, 54, 1168}; }
uint32_t epd_app_reading_font_id(void) { return 2; }
uint32_t epd_app_reading_font_revision(void) { return 2; }
unsigned ui_settings_index(ui_setting_id_t id)
{
    if (id == UI_SETTING_FONT_SIZE) return selected_size;
    if (id == UI_SETTING_LINE_SPACING) return 1;
    return 0;
}
lv_font_t *epd_app_reading_font_create(unsigned family, unsigned size, unsigned weight,
                                    uint32_t *identity, lv_font_t **fallback)
{ return ui_font_reading_create(family, size, weight, identity, fallback); }
static void reader_page_cache_clear(unsigned *cache) { (void)cache; ++cache_clears; }
static void capture_apply(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    va_list print; va_copy(print, ap); vprintf(format, print); va_end(print);
    (void)va_arg(ap, unsigned long); (void)va_arg(ap, unsigned long);
    unsigned slot = va_arg(ap, unsigned);
    const char *stage = va_arg(ap, const char *);
    unsigned attempt = va_arg(ap, unsigned);
    if (apply_errors < 2) {
        error_slot[apply_errors] = slot; error_attempt[apply_errors] = attempt;
        snprintf(error_stage[apply_errors], sizeof(error_stage[0]), "%s", stage);
    }
    ++apply_errors;
    va_end(ap);
}
#define view (*state)
#define VIEW_HEIGHT (view.status.bottom - view.status.top)
#define VIEW_LAYOUT_VERSION 3u
#define rt_kprintf capture_apply
#include "prepare_fonts.inc"
#undef rt_kprintf
#undef view

static void drop_set(view_state_t *s)
{
    for (unsigned i = 0; i < 3; ++i) {
        document_font_free(s->oblique[i]); document_font_free(s->retired_oblique[i]);
        epd_app_font_destroy(s->fonts[i]); epd_app_font_destroy(s->fallbacks[i]);
    }
    for (unsigned i = 0; i < 6; ++i) epd_app_font_destroy(s->retired[i]);
    memset(s, 0, sizeof(*s));
}

typedef struct { unsigned references; } view_frame_t;
typedef struct { unsigned record, byte; } document_cursor_t;
typedef struct {
    int kind, value; document_cursor_t cursor; unsigned session, serial; view_frame_t *origin;
} view_command_t;
enum { INTENT_NONE, INTENT_REFLOW, READER_ERROR_NONE = 0, READER_ERROR_FONT_MEMORY = 29 };
static struct {
    view_state_t *session;
    view_frame_t *bound, *ready;
    unsigned font_id, font_revision, settings[5], id, intent;
    rt_tick_t requested_at;
    reader_geometry_t area;
    bool configuration_failed, pending;
    struct { unsigned error_code; char error[32]; } info;
} ui;
static struct {
    view_state_t *configuration; view_frame_t *ready;
    view_command_t command; unsigned intent; rt_tick_t requested_at;
} mailbox;
static unsigned ui_cancels, ui_wakes, note_closes;
static void state_dispose(view_state_t *s) { drop_set(s); epd_app_free(s); }
static void frame_ref(view_frame_t *f) { if (f) ++f->references; }
static void frame_release(view_frame_t *f) { if (f) { assert(f->references); --f->references; } }
static void epd_reader_note_close(void) { ++note_closes; }
static void epd_reader_set_error_code(unsigned error) { ui.info.error_code = error; }
static void rt_enter_critical(void) { EnterCriticalSection(&interrupt_gate); }
static void rt_exit_critical(void) { LeaveCriticalSection(&interrupt_gate); }
static unsigned elapsed_ms(rt_tick_t at) { return rt_tick_get() - at; }
static void view_cancel(void) { ++ui_cancels; }
static void wake_view(void) { ++ui_wakes; }
#include "admission_bridge.inc"
/* The reader's log is independent of the font-worker cache-lock assertions. */
static void reflow_log(const char *format, ...) { (void)format; }
#define rt_kprintf reflow_log
#include "reflow.inc"
#undef rt_kprintf

static void bind_fixture(view_state_t *session, view_state_t *current, view_frame_t *bound)
{
    ui.session = session; ui.bound = bound; ui.id = 1; bound->references = 1;
    ui_configuration(current);
}
static void consume_configuration(view_state_t *current)
{
    view_state_t *configuration = mailbox.configuration; assert(configuration);
    mailbox.configuration = NULL;
    view_state_t old = *current; *current = *configuration; *configuration = old;
    state_dispose(configuration);
    frame_release(mailbox.command.origin); mailbox.command.origin = NULL;
}
static void finish_fixture(view_state_t *session, view_state_t *current, view_frame_t *bound)
{
    if (mailbox.configuration) consume_configuration(current);
    frame_release(ui.bound); assert(!bound->references);
    drop_set(current); drop_set(session);
}

static void check_set(view_state_t *s, unsigned px)
{
    idle();
    for (unsigned i = 0; i < 3; ++i) {
        assert(ui_reader_font_remote_status(s->fonts[i]) == 1);
        assert(ui_reader_font_remote_status(s->oblique[i]) == 1);
        assert(remote(s->fonts[i])->style.size_px == px + (i == 2 ? 6 : 0));
        assert(!remote(s->fonts[i])->style.oblique && remote(s->oblique[i])->style.oblique);
        assert(remote(s->oblique[i])->parent == remote(s->fonts[i]));
        assert(remote(s->oblique[i])->primary == remote(s->fonts[i])->primary);
        assert(!s->fallbacks[i]);
    }
}
static void two_live_sets(view_state_t *session, view_state_t *current)
{
    selected_size = 5; assert(prepare_fonts(session)); check_set(session, 40);
    selected_size = 6; assert(prepare_fonts(current)); check_set(current, 44);
    assert(font_count == 12 && resident_sources == 1 && leases == 12);
}
static void require_roundtrip(bool expect_failure)
{
    view_state_t session = {0}, current = {0}, next = {0};
    two_live_sets(&session, &current);
    selected_size = 5;
    /* Keep the worker from reclaiming the partial set between UI retries. */
    lock();
    bool ok = prepare_fonts(&next);
    printf("ROUNDTRIP success=%u shells=%u shell_bytes=%zu oom=%llu/%llu/%llu/%llu\n",
        ok, font_count, sizeof(remote_font_t), (unsigned long long)diagnostics.oom_budget,
        (unsigned long long)diagnostics.oom_reserve, (unsigned long long)diagnostics.oom_alloc,
        (unsigned long long)diagnostics.oom_postcheck);
    if (expect_failure) {
        assert(!ok && apply_errors == 2 && cache_clears == 1);
        assert(error_slot[0] == 2 && error_slot[1] == 0);
        assert(error_attempt[0] == 1 && error_attempt[1] == 2);
        assert(!strcmp(error_stage[0], "create") && !strcmp(error_stage[1], "create"));
    } else assert(ok && !apply_errors && font_count == 18);
    assert(!diagnostics.oom_budget && !diagnostics.oom_reserve &&
           !diagnostics.oom_alloc && !diagnostics.oom_postcheck);
    unlock(); idle();
    check_set(&session, 40); check_set(&current, 44);
    if (ok) check_set(&next, 40);
    else assert(font_count == 12);
    drop_set(&next); drop_set(&current); drop_set(&session);
}
static void roundtrip(void) { require_roundtrip(false); }
static void baseline_signature(void) { require_roundtrip(true); }
static void delayed_retirement(void)
{
    view_state_t session = {0}, current = {0}, next = {0}, final = {0};
    two_live_sets(&session, &current);
    selected_size = 5; assert(prepare_fonts(&next)); check_set(&next, 40);
    lock();
    drop_set(&current); /* Releasing a layout does not synchronously free its shells. */
    assert(font_count == 18);
    selected_size = 6;
    bool ok = prepare_fonts(&final);
    printf("DEFERRED success=%u shells=%u limit=%u\n", ok, font_count, FONT_LIMIT);
    assert(ok && font_count == 24 && !apply_errors);
    unlock(); idle(); assert(font_count == 18);
    check_set(&session, 40); check_set(&next, 40); check_set(&final, 44);
    drop_set(&session); drop_set(&next); drop_set(&final);
}
static void repeated_changes(void)
{
    view_state_t session = {0}, current = {0};
    two_live_sets(&session, &current);
    for (unsigned n = 0; n < 40; ++n) {
        view_state_t next = {0}; selected_size = n & 1 ? 6 : 5;
        lock(); assert(prepare_fonts(&next)); assert(font_count == 18);
        drop_set(&current); current = next; unlock();
        check_set(&current, selected_size == 5 ? 40 : 44);
        assert(font_count == 12 && resident_sources == 1 && leases == 12);
    }
    assert(!apply_errors); drop_set(&session); drop_set(&current);
}
static void held_page_and_draw(void)
{
    view_state_t session = {0}, current = {0}, next = {0};
    two_live_sets(&session, &current);
    lv_font_t *held = session.oblique[0];
    epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
    assert(ui_reader_font_remote_page_pin(page, held, 'A', 0, true) == 0); idle();
    assert(ui_reader_font_remote_page_pin(page, held, 'A', 0, true) == 1);
    lv_font_glyph_dsc_t d = descriptor(held, 'A');
    const void *bitmap = held->get_glyph_bitmap(&d, NULL); assert(bitmap);
    selected_size = 5; lock(); assert(prepare_fonts(&next)); unlock(); check_set(&next, 40);
    drop_set(&session); idle();
    assert(ui_reader_font_remote_ready(held, 'A', 0, true) == 1);
    assert(held->get_glyph_bitmap(&d, NULL) == bitmap);
    /* One callback descriptor owns one draw reference, even with repeated lookup. */
    ui_reader_font_remote_page_release(page); idle();
    assert(font_count >= 2 && remote(held)->draws);
    held->release_glyph(held, &d); idle();
    assert(font_count == 12);
    drop_set(&current); drop_set(&next);
}
static void reserve_rejection(void)
{
    view_state_t session = {0}, current = {0}, next = {0};
    two_live_sets(&session, &current);
    LONG64 available = available_bytes;
    lock(); InterlockedExchange64(&available_bytes, SHARED_RESERVE);
    selected_size = 5; assert(!prepare_fonts(&next));
    assert(next.font_failed && font_count >= 12 && font_count <= FONT_LIMIT && diagnostics.oom_reserve);
    InterlockedExchange64(&available_bytes, available); unlock();
    check_set(&session, 40); check_set(&current, 44);
    assert(font_count == 12);
    assert(prepare_fonts(&next)); check_set(&next, 40);
    drop_set(&session); drop_set(&current); drop_set(&next);
}
static void bounded_admission(void)
{
    lv_font_t *set[64] = {0}; uint32_t identity;
    assert(FONT_LIMIT < 64 && initialize()); lock();
    for (unsigned i = 0; i < FONT_LIMIT; ++i) {
        set[i] = ui_reader_font_remote_create(2, 40, 0, &identity); assert(set[i] && identity);
    }
    assert(!ui_reader_font_remote_create(2, 40, 0, &identity) && !identity);
    assert(font_count == FONT_LIMIT);
    for (unsigned i = 0; i < FONT_LIMIT; ++i) ui_reader_font_remote_destroy(set[i]);
    unlock();
}
static void rapid_changes(void)
{
    view_state_t session = {0}, current = {0}, pending = {0};
    two_live_sets(&session, &current);
    /* A busy font worker can leave two superseded mailbox configurations
     * queued for cleanup while the UI prepares the next configuration. */
    lock();
    for (unsigned n = 0; n < 8; ++n) {
        view_state_t next = {0}; selected_size = n & 1 ? 6 : 5;
        bool ok = prepare_fonts(&next);
        printf("RAPID change=%u success=%u shells=%u limit=%u\n", n + 1, ok, font_count, FONT_LIMIT);
        assert(ok);
        drop_set(&pending); pending = next;
    }
    unlock(); drop_set(&pending); drop_set(&current); drop_set(&session);
}
static void admission_rapid_busy(void)
{
    view_state_t session = {0}, current = {0}; view_frame_t bound = {0};
    two_live_sets(&session, &current); bind_fixture(&session, &current, &bound);
    lock();
    selected_size = 5; bool ok = epd_reader_reflow(); assert(ok);
    selected_size = 6; ok = epd_reader_reflow(); assert(ok && font_count == 24);
    view_state_t *pending = mailbox.configuration;
    unsigned original_generation = generation, cancels = ui_cancels, wakes = ui_wakes, closes = note_closes;
    LONG original_allocations = allocations;
    for (unsigned n = 0; n < 100; ++n) {
        selected_size = n & 1 ? 4 : 7;
        assert(epd_reader_settings_changed());
        ok = epd_reader_reflow();
        assert(!ok && !ui.configuration_failed && !ui.info.error_code);
        assert(epd_reader_settings_changed() && mailbox.configuration == pending);
        assert(font_count == 24 && generation == original_generation && allocations == original_allocations);
        assert(ui_cancels == cancels && ui_wakes == wakes && note_closes == closes);
    }
    selected_size = 7; unlock(); idle();
    assert(font_count == 18 && epd_reader_settings_changed());
    ok = epd_reader_reflow(); assert(ok && !epd_reader_settings_changed());
    assert(mailbox.configuration->settings[0] == 7 && !ui.configuration_failed && !apply_errors);
    consume_configuration(&current); check_set(&current, 48);
    assert(font_count == 12 && !ui.info.error_code);
    printf("BUSY 100 requests allocated=0; latest=48 applied; shells=12; error=0\n");
    finish_fixture(&session, &current, &bound);
}
static void admission_live_old_frame(void)
{
    view_state_t session = {0}, current = {0}, old = {0}; view_frame_t bound = {0};
    two_live_sets(&session, &current);
    selected_size = 7; assert(prepare_fonts(&old)); check_set(&old, 48);
    epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
    lv_font_t *held = old.oblique[2];
    for (unsigned i = 0; i < 3; ++i) {
        assert(ui_reader_font_remote_page_pin(page, old.fonts[i], 'A', 0, true) >= 0);
        assert(ui_reader_font_remote_page_pin(page, old.oblique[i], 'A', 0, true) >= 0);
    }
    idle(); drop_set(&old); idle(); assert(font_count == 18 && diagnostics.pins == 6);
    bind_fixture(&session, &current, &bound);
    selected_size = 5; bool ok = epd_reader_reflow(); assert(ok);
    idle(); assert(font_count == 24);
    selected_size = 4; ok = epd_reader_reflow();
    assert(!ok && !ui.info.error_code && epd_reader_settings_changed());
    /* rdview can consume an admitted configuration while the old display pins
     * remain live. Its superseded current fonts are then collectable. */
    consume_configuration(&current); idle(); assert(font_count == 18);
    assert(ui_reader_font_remote_ready(held, 'A', 0, true) == 1);
    ok = epd_reader_reflow(); assert(ok && !epd_reader_settings_changed());
    consume_configuration(&current); check_set(&current, 36);
    assert(diagnostics.pins == 6 && ui_reader_font_remote_ready(held, 'A', 0, true) == 1);
    ui_reader_font_remote_page_release(page); idle(); assert(font_count == 12);
    printf("BUSY old-frame pins=6 preserved; latest=36 applied before frame release\n");
    finish_fixture(&session, &current, &bound);
}
static void admission_real_failure(void)
{
    view_state_t session = {0}, current = {0}; view_frame_t bound = {0};
    two_live_sets(&session, &current); bind_fixture(&session, &current, &bound);
    assert(epd_app_reading_font_can_create(6));
    lock(); LONG64 available = available_bytes;
    InterlockedExchange64(&available_bytes, SHARED_RESERVE);
    selected_size = 5; bool ok = epd_reader_reflow();
    assert(!ok && ui.configuration_failed && ui.info.error_code == READER_ERROR_FONT_MEMORY);
    assert(!epd_reader_settings_changed() && !mailbox.configuration && apply_errors == 2);
    assert(diagnostics.oom_reserve && !ui_cancels && !ui_wakes);
    InterlockedExchange64(&available_bytes, available); unlock(); idle();
    assert(font_count == 12); check_set(&current, 44);
    finish_fixture(&session, &current, &bound);
}
static void admission_capacity_only(void)
{
    assert(epd_app_reading_font_can_create(6) && !initialized && !allocations);
    assert(!epd_app_reading_font_can_create(FONT_LIMIT + 1));
    assert(!initialized && !allocations && !raw_reads && !pipe_count);
}
typedef struct {
    unsigned page; lv_font_t *font; int margin, content_width, top, bottom, line_space;
} ui_reader_view_t;
static ui_reader_view_t reading_ui;
static struct {
    lv_obj_t *popup, *reader_body;
    unsigned reader_reflow_page;
    bool reader_reflow_pending, reader_error_reported;
} ui_page_fixture;
static unsigned ui_draw_waits, ui_font_rebinds;
static bool books_menu_font_pending(void) { return false; }
static const ui_reader_view_t *ui_reader_view(void) { return &reading_ui; }
static bool document_view_active(void) { return true; }
static void fixture_draw_wait(void) { ++ui_draw_waits; }
static void fixture_font_rebind(lv_obj_t *obj, const lv_font_t *font, unsigned selector)
{ (void)obj; (void)font; (void)selector; ++ui_font_rebinds; }
#define current (&ui_page_fixture)
#define ui_reader_settings_changed epd_reader_settings_changed
#define ui_reader_reflow epd_reader_reflow
#define lv_draw_wait_for_finish fixture_draw_wait
#define lv_obj_set_style_text_font fixture_font_rebind
#define lv_obj_has_flag(obj, flag) false
#define lv_obj_set_x(obj, value) ((void)0)
#define lv_obj_set_y(obj, value) ((void)0)
#define lv_obj_set_width(obj, value) ((void)0)
#define lv_obj_set_height(obj, value) ((void)0)
#define lv_obj_set_style_text_line_space(obj, value, selector) ((void)0)
#include "reader_resume.inc"
#undef current
#undef ui_reader_settings_changed
#undef ui_reader_reflow
#undef lv_draw_wait_for_finish
#undef lv_obj_set_style_text_font
#undef lv_obj_has_flag
#undef lv_obj_set_x
#undef lv_obj_set_y
#undef lv_obj_set_width
#undef lv_obj_set_height
#undef lv_obj_set_style_text_line_space
static void admission_ui_entry(void)
{
    view_state_t session = {0}, active = {0}, old_a = {0}, old_b = {0}; view_frame_t bound = {0};
    two_live_sets(&session, &active); bind_fixture(&session, &active, &bound);
    selected_size = 7; assert(prepare_fonts(&old_a)); assert(prepare_fonts(&old_b));
    idle(); assert(font_count == 24);
    reading_ui.font = active.fonts[0];
    selected_size = 5;
    LONG original_allocations = allocations;
    for (unsigned i = 0; i < 100; ++i) {
        reader_resume_prepare();
        assert(!ui_draw_waits && !ui_font_rebinds && !ui_cancels && !ui_wakes);
        assert(!ui.configuration_failed && !ui.info.error_code && epd_reader_settings_changed());
        assert(allocations == original_allocations && !mailbox.configuration);
    }
    drop_set(&old_a); idle(); assert(font_count == 18);
    reader_resume_prepare();
    assert(ui_draw_waits == 1 && ui_font_rebinds == 2 && mailbox.configuration);
    assert(ui_page_fixture.reader_reflow_pending && !epd_reader_settings_changed());
    consume_configuration(&active); check_set(&active, 40);
    drop_set(&old_b); finish_fixture(&session, &active, &bound);
    puts("UI BUSY 100 passes: draw_wait=0 font_rebind=0; available pass submits once");
}
static const test_t regression_cases[] = {
    {"roundtrip", roundtrip}, {"delayed_retirement", delayed_retirement},
    {"repeated_changes", repeated_changes}, {"held_page_and_draw", held_page_and_draw},
    {"reserve_rejection", reserve_rejection}, {"bounded_admission", bounded_admission},
    {"baseline_signature", baseline_signature}, {"rapid_changes", rapid_changes},
    {"admission_rapid_busy", admission_rapid_busy}, {"admission_live_old_frame", admission_live_old_frame},
    {"admission_real_failure", admission_real_failure}, {"admission_capacity_only", admission_capacity_only},
    {"admission_ui_entry", admission_ui_entry}
};
int main(int argc, char **argv)
{
    for (unsigned i = 0; argc == 2 && i < sizeof(regression_cases) / sizeof(*regression_cases); ++i) {
        if (strcmp(argv[1], regression_cases[i].name)) continue;
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        signal(SIGABRT, abort_without_dialog); setvbuf(stdout, NULL, _IONBF, 0);
        ui_thread = GetCurrentThreadId(); InitializeConditionVariable(&transition); InitializeCriticalSection(&interrupt_gate);
        io_entered = CreateEventA(NULL, TRUE, FALSE, NULL); io_continue = CreateEventA(NULL, TRUE, FALSE, NULL);
        regression_cases[i].run(); if (initialized) idle();
        assert(!fonts && !font_count && !pixel_bytes && !descriptor_bytes && !allocations && !leases && !pipe_count);
        assert(!sources && !resident_sources);
        printf("PASS %s; all font/source/lease/glyph allocations reclaimed\n", argv[1]); return 0;
    }
    return remote_fixture_main(argc, argv);
}
