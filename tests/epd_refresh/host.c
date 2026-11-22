#include "epd_refresh.h"
#include "epd_waveform.h"
#ifdef LCD_USING_EPD_E0470A03_V10_57
#include "epd_display_chunks.h"
#endif
#include "gui_app_int.h"
#include "src/display/lv_display_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s (tick=%u, frames=%u)\n", __FILE__, __LINE__, \
            #condition, (unsigned)lv_tick_get(), frames); exit(1); } } while (0)

enum { WIDTH = 80, HEIGHT = 80, ROWS = 40 };
static lv_display_t *display;
static lv_obj_t *screen, *slider;
static lv_indev_t *pointer;
static host_lifecycle_t lifecycle;
static gui_runing_app_t active_app = {"Main"};
static unsigned frames, ready_calls, refresh_starts, chunks;
static bool panel_busy, pointer_down, queued_last;
static bool panel_succeeded = true;
static bool prepare_enabled;
static unsigned pending_chunks, discarded_frames;
static EpdDrawMode last_mode;
static uint32_t submitted_at;
_Alignas(32) static uint8_t buffer_a[WIDTH * ROWS * 2];
_Alignas(32) static uint8_t buffer_b[WIDTH * ROWS * 2];

void app_schedule_set_anim_hook(host_lifecycle_t hook) { lifecycle = hook; }
gui_runing_app_t *gui_app_get_actived(void) { return &active_app; }
bool lv_refreshing_done(void) { return !panel_busy; }
bool epd_panel_last_frame_succeeded(void) { CHECK(!panel_busy); return panel_succeeded; }
void epd_wave_submit_frame(EpdDrawMode mode) { last_mode = mode; }
void epd_display_set_pending(bool pending) {
#ifdef LCD_USING_EPD_E0470A03
    CHECK(0);
#endif
    (void)pending;
}
void epd_display_queue_chunk(const void *pixels, uint32_t stride, bool first, bool last)
{
#ifdef LCD_USING_EPD_E0470A03
    CHECK(0);
#endif
    CHECK(pixels && stride);
    (void)first;
    queued_last = last;
    ++chunks;
}
bool epd_display_prepare_init(void) { return prepare_enabled; }
bool epd_display_can_prepare(void) { return prepare_enabled; }
bool epd_display_prepared_in_flight(void) { return false; }
void epd_display_prepare_chunk(const uint8_t *pixels, uint32_t stride,
                               uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    CHECK(prepare_enabled && pixels && stride && x0 <= x1 && y0 <= y1);
    ++pending_chunks;
    ++chunks;
}
void epd_display_discard_prepared(void)
{
    CHECK(prepare_enabled && pending_chunks);
    pending_chunks = 0;
    ++discarded_frames;
}
void epd_display_submit_prepared(EpdDrawMode mode)
{
    CHECK(prepare_enabled && pending_chunks && !panel_busy);
    pending_chunks = 0;
    panel_busy = true;
    last_mode = mode;
    submitted_at = lv_tick_get();
    ++frames;
}
void host_timer_ready(lv_timer_t *timer)
{
    ++ready_calls;
    lv_timer_ready(timer);
}
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    CHECK(!panel_busy);
#ifdef LCD_USING_EPD_E0470A03
    queued_last = lv_display_flush_is_last(disp);
    ++chunks;
#endif
    CHECK(queued_last == (bool)lv_display_flush_is_last(disp));
    if (queued_last) {
        panel_busy = true;
        submitted_at = lv_tick_get();
        ++frames;
    } else lv_display_flush_ready(disp);
}
static void wait_flush(lv_display_t *disp)
{
    (void)disp;
    /* A normal gated refresh must never wait for a held final chunk. */
    CHECK(!panel_busy);
}
static void refresh_event(lv_event_t *event)
{
    (void)event;
    ++refresh_starts;
}
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = (lv_point_t){30, 12};
    data->state = pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void tick(unsigned milliseconds)
{
    for (unsigned i = 0; i < milliseconds; ++i) {
        lv_tick_inc(1);
        (void)lv_timer_handler();
    }
}
static void complete(void)
{
    panel_busy = false;
    lv_display_flush_ready(display);
    epd_refresh_process();
}
static void fail_complete(void)
{
    panel_succeeded = false;
    panel_busy = false;
    lv_display_flush_ready(display);
    /* Leave observation and retry scheduling to the production timers. */
}
static void dirty(void)
{
    const lv_area_t area = {4, 45, 30, 60};
    lv_obj_invalidate_area(screen, &area);
}
static void setup(void)
{
    lv_init();
    lv_tick_inc(1000);
    display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_buffers(display, buffer_a, buffer_b, sizeof(buffer_a), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_flush_wait_cb(display, wait_flush);
    screen = lv_display_get_screen_active(display);
    slider = lv_slider_create(screen);
    lv_obj_set_pos(slider, 5, 5);
    lv_obj_set_size(slider, 65, 15);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_timer_pause(lv_indev_get_read_timer(pointer));
    epd_refresh_init();
    CHECK(lifecycle);
    lifecycle(screen, GUI_APP_MSG_ONRESUME);
    lv_display_add_event_cb(display, refresh_event, LV_EVENT_REFR_START, NULL);
    epd_refresh_process();
    for (unsigned i = 0; i < 201 && !frames; ++i) tick(1);
    CHECK(frames == 1 && chunks >= 2);
    complete();
    tick(1);
    CHECK(epd_refresh_idle());
    CHECK(last_mode == EPD_DRAW_MODE_AUTO);
    /* Start from a recent actual submission, not an already expired timer. */
    dirty();
    lv_refr_now(display);
    CHECK(frames == 2);
    complete();
    tick(1);
    ready_calls = refresh_starts = 0;
}
static void expect_prompt(unsigned previous)
{
    uint32_t start = lv_tick_get();
    tick(2);
    if (frames == previous) {
        while (frames == previous && lv_tick_elaps(start) < 250) tick(1);
        fprintf(stderr, "Pending frame waited %u ms after becoming eligible\n",
                (unsigned)lv_tick_elaps(start));
    }
    CHECK(frames == previous + 1);
    CHECK(lv_tick_elaps(start) <= 2);
    CHECK(last_mode == EPD_DRAW_MODE_AUTO);
}
static void begin_slider_frame(void)
{
    pointer_down = true;
    lv_indev_read(pointer);
    dirty();
    lv_refr_now(display);
    CHECK(frames == 3);
    complete();
    tick(1);
    ready_calls = 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *name = argv[1];
    prepare_enabled = strstr(name, "prepared") != NULL;
    setup();
    if (!strcmp(name, "native_frame") || !strcmp(name, "chunk_frame")) {
        dirty(); lv_refr_now(display);
        CHECK(frames == 3);
        CHECK(last_mode == EPD_DRAW_MODE_AUTO); complete();
        epd_refresh_request_full();
        lv_refr_now(display);
        CHECK(frames == 4 && last_mode == EPD_DRAW_MODE_FULL); complete();
    } else if (!strcmp(name, "app_switch") || !strcmp(name, "app_switch_prepared")) {
        lv_obj_t *weather = lv_obj_create(NULL);
        lv_obj_t *detail = lv_obj_create(NULL);
        strcpy(active_app.id, "weather");
        lv_screen_load(weather);
        lifecycle(weather, GUI_APP_MSG_ONRESUME);
        lv_refr_now(display);
        CHECK(frames == 3 && last_mode == EPD_DRAW_MODE_FULL);
        complete();
        lv_screen_load(detail);
        lifecycle(detail, GUI_APP_MSG_ONRESUME);
        lv_refr_now(display);
        CHECK(frames == 4 && last_mode == EPD_DRAW_MODE_AUTO);
        /* A temporary return to Main while the previous frame is busy must
         * not consume the full waveform reserved for the new destination. */
        strcpy(active_app.id, "Main");
        lv_screen_load(screen);
        lifecycle(screen, GUI_APP_MSG_ONRESUME);
        if (prepare_enabled) {
            lv_refr_now(display);
            CHECK(frames == 4 && pending_chunks);
        }
        strcpy(active_app.id, "calendr");
        lv_screen_load(weather);
        lifecycle(weather, GUI_APP_MSG_ONRESUME);
        if (prepare_enabled) {
            CHECK(discarded_frames == 1 && !pending_chunks);
            lv_refr_now(display);
            CHECK(frames == 4 && pending_chunks);
        }
        complete(); lv_refr_now(display);
        CHECK(frames == 5 && last_mode == EPD_DRAW_MODE_FULL);
        complete();
        strcpy(active_app.id, "Main");
        lv_screen_load(screen);
        lifecycle(screen, GUI_APP_MSG_ONRESUME);
        lv_refr_now(display);
        CHECK(frames == 6 && last_mode == EPD_DRAW_MODE_FULL);
        complete();
        dirty(); lv_refr_now(display);
        CHECK(frames == 7 && last_mode == EPD_DRAW_MODE_AUTO);
    } else if (!strncmp(name, "retry_", 6)) {
        bool locked_case = strstr(name, "locked") != NULL;
        if (locked_case) { epd_refresh_set_locked(true); epd_refresh_request_interaction(); }
        dirty(); lv_refr_now(display);
        CHECK(frames == 3);
        if (!strcmp(name, "retry_newer_prepared") || !strcmp(name, "retry_newer_dirty")) {
            dirty();
            if (prepare_enabled) { lv_refr_now(display); CHECK(pending_chunks); }
            CHECK(frames == 3);
            fail_complete(); tick(2);
            CHECK(frames == 4 && !pending_chunks && !discarded_frames);
            fail_complete(); tick(2);
            CHECK(frames == 5 && last_mode == EPD_DRAW_MODE_FULL);
            fail_complete(); tick(1000);
            CHECK(frames == 5 && epd_refresh_idle());
        } else if (!strcmp(name, "retry_maintenance")) {
            epd_refresh_set_maintenance(true);
            fail_complete(); tick(1000);
            CHECK(frames == 3 && epd_refresh_idle() && !panel_succeeded);
            epd_refresh_set_maintenance(false);
            tick(1000); CHECK(frames == 3);
            dirty(); tick(2); CHECK(frames == 4);
        } else {
            fail_complete();
            CHECK(!display->inv_p);
            if (!strcmp(name, "retry_maintenance_queued")) {
                CHECK(!epd_refresh_idle());
                epd_refresh_set_maintenance(true);
                tick(1000); CHECK(frames == 3 && epd_refresh_idle());
                epd_refresh_set_maintenance(false);
            }
            tick(2);
            CHECK(frames == 4 && last_mode == EPD_DRAW_MODE_FULL && panel_busy);
            if (strstr(name, "external_during_retry")) {
                dirty();
                if (prepare_enabled) { lv_refr_now(display); CHECK(pending_chunks); }
                fail_complete(); tick(2); CHECK(frames == 5);
                fail_complete(); tick(2);
                CHECK(frames == 6 && last_mode == EPD_DRAW_MODE_FULL);
                fail_complete(); tick(1000);
                CHECK(frames == 6 && epd_refresh_idle());
            } else if (strstr(name, "success")) {
                panel_succeeded = true; complete(); tick(1000);
                CHECK(frames == 4 && epd_refresh_idle());
            } else {
                fail_complete(); tick(1000);
                CHECK(frames == 4 && epd_refresh_idle());
                unsigned stopped_starts = refresh_starts;
                tick(EPD_REFRESH_LOCK_INTERVAL_MS + 10);
                CHECK(frames == 4 && refresh_starts == stopped_starts);
                /* An explicit interaction without dirty must not restart recovery. */
                epd_refresh_request_interaction(); tick(2); CHECK(frames == 4);
                dirty(); tick(2); CHECK(frames == 5);
                fail_complete(); tick(2);
                CHECK(frames == 6 && last_mode == EPD_DRAW_MODE_FULL);
                fail_complete(); tick(1000);
                CHECK(frames == 6 && epd_refresh_idle());
            }
        }
    } else if (!strcmp(name, "idle_dirty")) {
        dirty();
        expect_prompt(2);
    } else if (!strcmp(name, "busy_release") || !strcmp(name, "flushing_gate") ||
               !strcmp(name, "sdk_busy_gate")) {
        dirty();
        lv_refr_now(display);
        CHECK(frames == 3);
        ready_calls = 0;
        dirty();
        epd_refresh_request_interaction();
        if (!strcmp(name, "flushing_gate")) panel_busy = false;
        if (!strcmp(name, "sdk_busy_gate")) lv_display_flush_ready(display);
        tick(30);
        CHECK(frames == 3 && ready_calls == 0 && !epd_refresh_update_due());
        /* Hardware completion is observed by the normal handoff timer. */
        panel_busy = false;
        lv_display_flush_ready(display);
        expect_prompt(3);
    } else if (!strcmp(name, "maintenance") || !strcmp(name, "maintenance_rearm") ||
               !strcmp(name, "maintenance_paused")) {
        dirty();
        if (strcmp(name, "maintenance")) epd_refresh_process();
        epd_refresh_set_maintenance(true);
        if (!strcmp(name, "maintenance_paused")) lv_obj_invalidate(screen);
        epd_refresh_request_interaction();
        unsigned ready = ready_calls;
        tick(30);
        CHECK(frames == 2 && ready_calls == ready);
        epd_refresh_set_maintenance(false);
        expect_prompt(2);
    } else if (!strcmp(name, "lock_deadline")) {
        epd_refresh_set_locked(true);
        dirty();
        tick(EPD_REFRESH_LOCK_INTERVAL_MS - 2);
        CHECK(frames == 2 && ready_calls == 0);
        expect_prompt(2);
    } else if (!strcmp(name, "lock_interaction")) {
        epd_refresh_set_locked(true);
        dirty();
        tick(30);
        CHECK(frames == 2 && ready_calls == 0);
        epd_refresh_request_interaction();
        expect_prompt(2);
        complete();
        dirty();
        tick(30);
        CHECK(frames == 3);
    } else if (!strcmp(name, "lock_hold")) {
        const lv_area_t status = {0, 0, WIDTH - 1, 25};
        epd_refresh_status_area(screen, &status);
        epd_refresh_set_locked(true);
        lv_obj_invalidate_area(screen, &status);
        epd_refresh_request_interaction();
        epd_refresh_set_lock_hold(true);
        tick(1500);
        CHECK(frames == 2 && !epd_refresh_update_due());
        lv_refr_now(display);
        CHECK(frames == 2);
        epd_refresh_set_lock_hold(false);
        tick(2);
        CHECK(frames == 3 && last_mode == EPD_DRAW_MODE_PARTIAL);
        complete();

        epd_refresh_set_lock_hold(true);
        lv_obj_invalidate_area(screen, &status);
        epd_refresh_request_interaction();
        tick(30);
        CHECK(frames == 3);
        lv_obj_t *previous = lv_obj_create(NULL);
        lv_screen_load(previous);
        lifecycle(previous, GUI_APP_MSG_ONRESUME);
        epd_refresh_set_locked(false);
        epd_refresh_request_interaction();
        epd_refresh_process();
        expect_prompt(3);
    } else if (!strcmp(name, "lock_background")) {
        epd_refresh_set_locked(true);
        lifecycle(screen, GUI_APP_MSG_ONSTOP);
        dirty();
        epd_refresh_request_interaction();
        tick(30);
        CHECK(frames == 2 && ready_calls == 0);
        lifecycle(screen, GUI_APP_MSG_ONRESUME);
        expect_prompt(2);
    } else if (!strcmp(name, "full_locked")) {
        epd_refresh_set_locked(true);
        epd_refresh_request_full();
        tick(30);
        CHECK(frames == 2 && ready_calls == 0);
        epd_refresh_request_interaction();
        tick(2);
        CHECK(frames == 3 && last_mode == EPD_DRAW_MODE_FULL);
    } else if (!strcmp(name, "slider_deadline") || !strcmp(name, "slider_release")) {
        begin_slider_frame();
        dirty();
        tick(30);
        CHECK(frames == 3 && ready_calls == 0);
        if (!strcmp(name, "slider_release")) {
            pointer_down = false;
            lv_indev_read(pointer);
        } else {
            while (lv_tick_elaps(submitted_at) < 99) tick(1);
            CHECK(frames == 3);
        }
        expect_prompt(3);
    } else if (!strcmp(name, "one_shot")) {
        dirty();
        for (unsigned i = 0; i < 20; ++i) epd_refresh_process();
        CHECK(ready_calls == 1);
        expect_prompt(2);
        CHECK(ready_calls == 1);
        complete();
        unsigned starts = refresh_starts;
        for (unsigned i = 0; i < 50; ++i) { epd_refresh_process(); tick(1); }
        CHECK(frames == 3 && ready_calls == 1 && refresh_starts == starts);
        CHECK(lv_timer_handler() != 0);
    } else if (!strcmp(name, "synchronous_rearm")) {
        dirty();
        epd_refresh_process();
        CHECK(ready_calls == 1);
        lv_refr_now(display);
        CHECK(frames == 3);
        dirty();
        tick(10);
        CHECK(frames == 3 && ready_calls == 1);
        complete();
        expect_prompt(3);
        CHECK(ready_calls == 2);
    } else if (!strcmp(name, "empty_idle")) {
        epd_refresh_request_interaction();
        for (unsigned i = 0; i < 50; ++i) { epd_refresh_process(); tick(1); }
        CHECK(frames == 2 && ready_calls == 0 && refresh_starts == 0);
        CHECK(lv_timer_handler() != 0);
    } else if (!strcmp(name, "deferred")) {
        epd_refresh_set_maintenance(true);
        dirty();
        lv_refr_now(display);
        CHECK(frames == 2);
        epd_refresh_set_maintenance(false);
        expect_prompt(2);
    } else if (!strcmp(name, "input_regions")) {
        const lv_area_t key = {8, 8, 25, 12};
        epd_refresh_input_objects(slider, NULL, true);
        lv_obj_invalidate_area(screen, &key);
        tick(2);
        CHECK(frames == 3 && last_mode == EPD_DRAW_MODE_INPUT);
        complete();
        lv_obj_invalidate_area(screen, &key);
        dirty();
        tick(2);
        CHECK(frames == 4 && last_mode == EPD_DRAW_MODE_AUTO);
        complete();
        epd_refresh_status_begin();
        dirty();
        epd_refresh_status_end();
        lv_obj_invalidate_area(screen, &key);
        tick(2);
        CHECK(frames == 5 && last_mode == EPD_DRAW_MODE_PARTIAL);
        complete();
        lv_obj_invalidate_area(screen, &key);
        epd_refresh_status_begin();
        dirty();
        epd_refresh_status_end();
        tick(2);
        CHECK(frames == 6 && last_mode == EPD_DRAW_MODE_PARTIAL);
        complete();
        epd_refresh_input_objects(slider, NULL, false);
        lv_obj_invalidate_area(screen, &key);
        tick(2);
        CHECK(frames == 7 && last_mode == EPD_DRAW_MODE_AUTO);
    } else if (!strcmp(name, "input_status") || !strcmp(name, "input_status_prepared")) {
        const lv_area_t key = {8, 8, 25, 12};
        const char *changes[] = {"SI", "IS", "SIS", "ISI", "SIC", "CSI", "ICS", "I"};
        epd_refresh_input_objects(slider, NULL, true);
        for (unsigned i = 0; i < sizeof changes / sizeof changes[0]; ++i) {
            unsigned previous = frames;
            for (const char *change = changes[i]; *change; ++change) {
                if (*change == 'I') lv_obj_invalidate_area(screen, &key);
                else {
                    if (*change == 'S') epd_refresh_status_begin();
                    dirty();
                    if (*change == 'S') epd_refresh_status_end();
                }
            }
            tick(2);
            CHECK(frames == previous + 1);
            CHECK(last_mode == (strchr(changes[i], 'C') ? EPD_DRAW_MODE_AUTO :
                               strchr(changes[i], 'S') ? EPD_DRAW_MODE_PARTIAL :
                               EPD_DRAW_MODE_INPUT));
            complete();
        }
        /* Retain the mixed classification while a preceding frame is busy,
         * including a rendered prepared frame. */
        dirty(); lv_refr_now(display);
        unsigned previous = frames;
        CHECK(panel_busy && last_mode == EPD_DRAW_MODE_AUTO);
        epd_refresh_status_begin(); dirty(); epd_refresh_status_end();
        lv_obj_invalidate_area(screen, &key);
        if (prepare_enabled) lv_refr_now(display);
        else tick(2);
        CHECK(frames == previous);
        if (prepare_enabled) CHECK(pending_chunks);
        complete(); tick(2);
        CHECK(frames == previous + 1 && last_mode == EPD_DRAW_MODE_PARTIAL);
        complete();

        /* A synchronous render blocked by maintenance must restore both types. */
        epd_refresh_set_maintenance(true);
        epd_refresh_status_begin(); dirty(); epd_refresh_status_end();
        lv_obj_invalidate_area(screen, &key);
        previous = frames;
        lv_refr_now(display);
        CHECK(frames == previous);
        epd_refresh_set_maintenance(false);
        tick(2);
        CHECK(frames == previous + 1 && last_mode == EPD_DRAW_MODE_PARTIAL);
        complete();

        /* Holding background status updates must not hold keyboard input. */
        epd_refresh_set_locked(true);
        epd_refresh_set_lock_hold(true);
        epd_refresh_status_begin(); dirty(); epd_refresh_status_end();
        epd_refresh_request_interaction();
        previous = frames;
        tick(2); CHECK(frames == previous);
        lv_obj_invalidate_area(screen, &key);
        tick(2);
        CHECK(frames == previous + 1 && last_mode == EPD_DRAW_MODE_PARTIAL);
        complete();
        epd_refresh_set_locked(false);

        epd_refresh_status_begin(); dirty(); epd_refresh_status_end();
        lv_obj_invalidate_area(screen, &key);
        epd_refresh_request_full();
        tick(2);
        CHECK(frames == previous + 2 && last_mode == EPD_DRAW_MODE_FULL);
        complete();
        epd_refresh_input_objects(slider, NULL, false);
    } else if (!strcmp(name, "first_frame_deadline")) {
        epd_refresh_defer_first_frame(screen, true);
        dirty();
        uint32_t started = lv_tick_get();
        tick(98);
        CHECK(frames == 2);
        tick(3);
        CHECK(frames == 3 && lv_tick_elaps(started) <= 101);
    } else if (!strcmp(name, "first_frame_interaction")) {
        epd_refresh_defer_first_frame(screen, true);
        dirty();
        tick(10);
        CHECK(frames == 2);
        epd_refresh_request_interaction();
        expect_prompt(2);
    } else if (!strcmp(name, "modes")) {
        const lv_area_t status = {0, 0, WIDTH - 1, 25};
        epd_refresh_status_area(screen, &status);
        lv_obj_invalidate_area(screen, &status);
        tick(2);
        CHECK(frames == 3 && last_mode == EPD_DRAW_MODE_PARTIAL);
        complete();
        epd_refresh_request_full();
        tick(2);
        CHECK(frames == 4 && last_mode == EPD_DRAW_MODE_FULL);
    } else CHECK(false);
    printf("PASS %s\n", name);
    return 0;
}
