#include "epd_refresh.h"
#include "gui_app_int.h"
#include "epd_waveform.h"
#include "epd_display_chunks.h"
#include "src/display/lv_display_private.h"
#include "src/core/lv_refr_private.h"

/* SDK lv_lcd.c checks the LCD queue, framebuffer and GPU completion. */
extern bool lv_refreshing_done(void);

typedef enum
{
    CHANGE_NONE, CHANGE_STATUS, CHANGE_CONTENT, CHANGE_FULL
} change_t;

typedef struct screen_region
{
    struct screen_region *next;
    lv_obj_t *screen;
    lv_area_t area;
    bool has_status;
} screen_region_t;

static lv_display_t *refresh_display;
static lv_display_flush_cb_t lcd_flush;
static lv_timer_t *handoff_timer;
static screen_region_t *screen_regions;
static lv_obj_t *foreground_screen;
static lv_obj_t *current_screen;
static uint32_t page_generation;
static change_t pending_change;
static bool redraw_pending;
static bool locked, maintenance, interaction;
static bool completion_pending, completed_once;
static bool refresh_queued;
static uint32_t completed_at;
static uint32_t submission;
static bool synthetic_invalidate;
static lv_obj_t *first_frame_screen;
static uint32_t first_frame_started;
static bool first_frame_waiting;
/* Intermediate native-slider values share one display budget. Input, hardware
 * preview and release callbacks still run at their normal rate. */
#define SLIDER_FRAME_INTERVAL_MS 200u
static bool slider_dragging, slider_submitted;
static uint32_t slider_submitted_at;
static lv_indev_t *slider_pointer;

/* Only synchronous refreshes consume LVGL's dirty list while submission is
 * blocked. Keep their joined regions until LVGL can take them back. */
static struct
{
    lv_area_t areas[LV_INV_BUF_SIZE];
    uint16_t count;
    uint32_t page_generation;
} deferred;

static struct
{
    uint32_t page_generation;
    change_t change;
    bool allowed;
    bool interaction;
    bool first_chunk;
} frame;

static void schedule_handoff(void);

static uint32_t first_frame_delay(void)
{
    if (first_frame_screen != lv_display_get_screen_active(refresh_display)) {
        first_frame_screen = NULL;
        first_frame_waiting = false;
    }
    if (!first_frame_waiting) return 0;
    uint32_t elapsed = lv_tick_elaps(first_frame_started);
    if (elapsed < 1000u) return 1000u - elapsed;
    first_frame_waiting = false;
    return 0;
}

static void pointer_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED)
    {
        first_frame_waiting = false;
        lv_obj_t *object = lv_event_get_param(event);
        slider_dragging = object && lv_obj_check_type(object, &lv_slider_class);
        slider_pointer = slider_dragging ? lv_event_get_target(event) : NULL;
        slider_submitted = false;
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_INDEV_RESET ||
             code == LV_EVENT_DELETE)
    {
        if (slider_pointer == lv_event_get_target(event))
        {
            slider_dragging = false;
            slider_pointer = NULL;
        }
    }
    else return;
    schedule_handoff();
}

static uint32_t slider_delay(void)
{
    /* Cancelled and disabled controls need not emit an indev RELEASED event.
     * The input state still reports the physical release. */
    if (slider_dragging && (!slider_pointer ||
        lv_indev_get_state(slider_pointer) != LV_INDEV_STATE_PRESSED))
    {
        slider_dragging = false;
        slider_pointer = NULL;
    }
    if (locked || !slider_dragging || !slider_submitted || pending_change == CHANGE_FULL ||
        lv_display_get_screen_active(refresh_display) != current_screen) return 0;
    uint32_t elapsed = lv_tick_elaps(slider_submitted_at);
    return elapsed < SLIDER_FRAME_INTERVAL_MS ? SLIDER_FRAME_INTERVAL_MS - elapsed : 0;
}

static change_t merge_change(change_t left, change_t right)
{
    return left > right ? left : right;
}

static void forget_screen(lv_obj_t *screen)
{
    if (first_frame_screen == screen) {
        first_frame_screen = NULL;
        first_frame_waiting = false;
    }
    if (foreground_screen == screen) foreground_screen = NULL;
    if (current_screen == screen)
    {
        current_screen = NULL;
        ++page_generation;
        pending_change = CHANGE_NONE;
        redraw_pending = true;
        deferred.count = 0;
    }
    schedule_handoff();
}

static void screen_deleted(lv_event_t *event)
{
    screen_region_t *region = lv_event_get_user_data(event);
    screen_region_t **link = &screen_regions;
    while (*link && *link != region) link = &(*link)->next;
    if (*link) *link = region->next;
    forget_screen(region->screen);
    lv_free(region);
}

static screen_region_t *screen_region(lv_obj_t *screen)
{
    if (!screen) return NULL;
    for (screen_region_t *region = screen_regions; region; region = region->next)
        if (region->screen == screen) return region;
    screen_region_t *region = lv_malloc(sizeof(*region));
    LV_ASSERT_MALLOC(region);
    if (!region) return NULL;
    region->screen = screen;
    region->has_status = false;
    region->next = screen_regions;
    screen_regions = region;
    lv_obj_add_event_cb(screen, screen_deleted, LV_EVENT_DELETE, region);
    return region;
}

void epd_refresh_status_area(lv_obj_t *screen, const lv_area_t *area)
{
    screen_region_t *region = screen_region(screen);
    if (!region || !area) return;
    region->area = *area;
    region->has_status = true;
}

static void select_screen(void)
{
    lv_obj_t *screen = lv_display_get_screen_active(refresh_display);
    if (screen == current_screen) return;
    screen_region(screen);
    current_screen = screen;
    slider_dragging = false;
    slider_pointer = NULL;
    ++page_generation;
    /* Discard classifications belonging to the previous page, including when
     * LVGL or app_fwk reuses a deleted screen's address. */
    pending_change = screen ? CHANGE_CONTENT : CHANGE_NONE;
    redraw_pending = screen != NULL;
    deferred.count = 0;
}

static void observe_completion(void)
{
    /* With a framebuffer, flushing clears when the draw-buffer copy finishes.
     * The SDK display path must also be idle before starting the lock interval.
     * Observe completion in the UI thread; FLUSH_FINISH is only submission. */
    if (completion_pending && !refresh_display->flushing && lv_refreshing_done())
    {
        completion_pending = false;
        completed_once = true;
        completed_at = lv_tick_get();
    }
}

bool epd_refresh_update_due(void)
{
    if (!refresh_display || maintenance ||
        !lv_display_get_screen_active(refresh_display)) return false;
    observe_completion();
    /* Keep the UI thread out of the framebuffer's wait-for-writable path.
     * Coalesce changes while the preceding panel refresh is still running. */
    if (completion_pending) return false;
    if (first_frame_delay()) return false;
    if (slider_delay()) return false;
    if (!locked) return true;
    if (lv_display_get_screen_active(refresh_display) != foreground_screen) return false;
    return interaction || !completed_once ||
           lv_tick_elaps(completed_at) >= EPD_REFRESH_LOCK_INTERVAL_MS;
}

bool epd_refresh_completed_once(void)
{
    if (!refresh_display) return false;
    observe_completion();
    return completed_once;
}

uint32_t epd_refresh_submission(void) { return submission; }

bool epd_refresh_idle(void)
{
    if (!refresh_display) return false;
    observe_completion();
    return !completion_pending && !refresh_display->flushing && lv_refreshing_done();
}

static void schedule_handoff(void)
{
    if (!handoff_timer) return;
#ifdef EPD_DISPLAY_CHUNKS
    bool eligible = !locked || (current_screen == foreground_screen &&
        (interaction || !completed_once || (!completion_pending &&
         lv_tick_elaps(completed_at) >= EPD_REFRESH_LOCK_INTERVAL_MS)));
    epd_display_set_pending(!maintenance && current_screen && eligible && !first_frame_delay() &&
        (pending_change != CHANGE_NONE || redraw_pending || deferred.count));
#endif
    uint32_t delay;
    if (completion_pending) delay = 1;
    else if (maintenance || !lv_display_get_screen_active(refresh_display) ||
             (locked && lv_display_get_screen_active(refresh_display) != foreground_screen) ||
             (pending_change == CHANGE_NONE && !redraw_pending))
    {
        lv_timer_pause(handoff_timer);
        return;
    }
    else if (first_frame_delay()) delay = first_frame_delay();
    else if (epd_refresh_update_due()) delay = 1;
    else if (!locked && slider_delay()) delay = slider_delay();
    else delay = EPD_REFRESH_LOCK_INTERVAL_MS - lv_tick_elaps(completed_at);
    lv_timer_set_period(handoff_timer, delay);
    lv_timer_reset(handoff_timer);
    lv_timer_resume(handoff_timer);
}

static void redraw_current(void)
{
    if (!current_screen || !redraw_pending ||
        !lv_display_is_invalidation_enabled(refresh_display)) return;
    const lv_area_t area = {0, 0,
        lv_display_get_horizontal_resolution(refresh_display) - 1,
        lv_display_get_vertical_resolution(refresh_display) - 1};
    redraw_pending = false;
    /* A display-wide invalidation redraws the active screen and bottom/top/sys
     * layers. Consume only this one event before other invalidation handlers:
     * a nested real invalidation must still enter the business classification. */
    synthetic_invalidate = true;
    lv_inv_area(refresh_display, &area);
    synthetic_invalidate = false;
}

static void restore_deferred(void)
{
    if (!deferred.count ||
        !lv_display_is_invalidation_enabled(refresh_display)) return;
    uint16_t count = deferred.count;
    uint32_t generation = deferred.page_generation;
    deferred.count = 0;
    for (uint16_t i = 0; i < count && generation == page_generation; ++i)
    {
        /* These aligned regions have already been classified. Let LVGL retain
         * and merge them with new changes, without reclassifying alignment. */
        synthetic_invalidate = true;
        lv_inv_area(refresh_display, &deferred.areas[i]);
        synthetic_invalidate = false;
    }
}

void epd_refresh_process(void)
{
    if (!refresh_display) return;
    observe_completion();
    select_screen();
    restore_deferred();
    if (epd_refresh_update_due())
    {
        redraw_current();
        if (refresh_display->inv_p)
        {
            lv_timer_t *timer = lv_display_get_refr_timer(refresh_display);
            /* Resume alone retains the 200 ms deadline. Arm once until the
             * callback consumes the request, including a rejected attempt. */
            if (!refresh_queued)
            {
                refresh_queued = true;
                lv_timer_ready(timer);
            }
            lv_timer_resume(timer);
        }
    }
    schedule_handoff();
}

static void handoff_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    epd_refresh_process();
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    refresh_queued = false;
    /* Recheck before LVGL renders or waits for the preceding draw buffer.
     * Invalidations during layout can rearm a timer inside an allowed frame. */
    if (!epd_refresh_update_due())
    {
        lv_timer_pause(timer);
        schedule_handoff();
        return;
    }
    lv_display_refr_timer(timer);
}

void epd_refresh_defer_first_frame(lv_obj_t *screen, bool defer)
{
    if (!refresh_display || !screen) return;
    if (!defer) {
        if (first_frame_screen != screen || !first_frame_waiting) return;
        first_frame_waiting = false;
        epd_refresh_process();
        return;
    }
    if (screen != lv_display_get_screen_active(refresh_display) || first_frame_screen == screen) return;
    first_frame_screen = screen;
    first_frame_started = lv_tick_get();
    first_frame_waiting = true;
    refresh_queued = false;
    lv_timer_pause(lv_display_get_refr_timer(refresh_display));
    schedule_handoff();
}

void epd_refresh_set_locked(bool enabled)
{
    locked = enabled;
    schedule_handoff();
}

void epd_refresh_set_maintenance(bool enabled)
{
    maintenance = enabled;
    if (enabled) interaction = false;
    schedule_handoff();
}

void epd_refresh_request_interaction(void)
{
    first_frame_waiting = false;
    interaction = true;
    schedule_handoff();
}

void epd_refresh_request_full(void)
{
    if (!refresh_display) return;
    select_screen();
    pending_change = CHANGE_FULL;
    redraw_pending = true;
    schedule_handoff();
}

static void page_lifecycle(const screen_t screen, gui_app_msg_type_t message)
{
    lv_obj_t *object = (lv_obj_t *)screen;
    if (message == GUI_APP_MSG_ONRESUME)
    {
        screen_region(object);
        foreground_screen = object;
        schedule_handoff();
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        forget_screen(object);
    }
}

static void invalidated(lv_event_t *event)
{
    if (synthetic_invalidate)
    {
        synthetic_invalidate = false;
        return;
    }
    /* get_max_row() sends INVALIDATE_AREA to probe block alignment while
     * rendering. Real lv_inv_area() is forbidden in that phase by LVGL. */
    if (refresh_display->rendering_in_progress) return;
    const lv_area_t *area = lv_event_get_param(event);
    if (!area || area->x1 > area->x2 || area->y1 > area->y2) return;
    select_screen();
    screen_region_t *region = screen_region(current_screen);
    change_t change = CHANGE_CONTENT;
    if (current_screen == foreground_screen && region && region->has_status &&
        area->x1 >= region->area.x1 && area->y1 >= region->area.y1 &&
        area->x2 <= region->area.x2 && area->y2 <= region->area.y2)
        change = CHANGE_STATUS;
    pending_change = merge_change(pending_change, change);
}

static void display_event(lv_event_t *event)
{
    switch (lv_event_get_code(event))
    {
    case LV_EVENT_REFR_REQUEST:
        /* The default LVGL handler has resumed its refresh timer. Coalesce
         * ordinary requests here and recheck at the timer's rendering entry. */
        if (!epd_refresh_update_due())
        {
            /* The timer has not rendered anything: LVGL still owns the areas. */
            lv_timer_pause(lv_display_get_refr_timer(refresh_display));
        }
        schedule_handoff();
        break;
    case LV_EVENT_REFR_START:
        /* A synchronous lv_refr_now also consumes an armed timer request. */
        refresh_queued = false;
        observe_completion();
        select_screen();
        restore_deferred();
        frame.allowed = epd_refresh_update_due();
        frame.interaction = interaction;
        frame.first_chunk = true;
        if (frame.allowed) redraw_current();
        break;
    case LV_EVENT_REFR_READY:
        /* LVGL has cleared the just-rendered list. Requeue only the regions
         * whose chunks were discarded, including top/sys layer changes. */
        select_screen();
        restore_deferred();
        observe_completion();
        schedule_handoff();
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        observe_completion();
        schedule_handoff();
        break;
    default:
        break;
    }
}

static void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    bool last = lv_display_flush_is_last(display);
#ifdef EPD_DISPLAY_CHUNKS
    bool first = frame.first_chunk;
#endif
    if (frame.first_chunk)
    {
        /* Layout and RENDER_START handlers can still invalidate real content.
         * Snapshot after those handlers, before the first actual chunk. */
        frame.first_chunk = false;
        frame.page_generation = page_generation;
        frame.change = pending_change;
        pending_change = CHANGE_NONE;
        if (!frame.allowed)
        {
            deferred.page_generation = page_generation;
            deferred.count = 0;
            for (uint16_t i = 0; i < display->inv_p; ++i)
                if (!display->inv_area_joined[i])
                    deferred.areas[deferred.count++] = display->inv_areas[i];
        }
    }
    if (!frame.allowed)
    {
        if (frame.page_generation == page_generation)
        {
            pending_change = merge_change(pending_change, frame.change);
        }
        /* Nothing was sent: release each LVGL chunk exactly once, including
         * the last chunk. Never retain a draw buffer or touch the mode queue. */
        lv_display_flush_ready(display);
        return;
    }
    if (last)
    {
        EpdDrawMode mode = frame.change == CHANGE_STATUS ?
                           EPD_DRAW_MODE_PARTIAL : EPD_DRAW_MODE_AUTO;
        if (frame.change == CHANGE_FULL)
            mode = EPD_DRAW_MODE_FULL;
        epd_wave_submit_frame(mode);
        if (frame.interaction) interaction = false;
        completion_pending = true;
        ++submission;
        if (slider_dragging)
        {
            slider_submitted = true;
            slider_submitted_at = lv_tick_get();
        }
    }
#ifdef EPD_DISPLAY_CHUNKS
    epd_display_queue_chunk(pixels, display->layer_head->draw_buf->header.stride,
                            first, last);
#endif
    lcd_flush(display, area, pixels);
    if (last) observe_completion();
}

void epd_refresh_init(void)
{
    if (refresh_display) return;
    lv_display_t *display = lv_display_get_default();
    if (!display || !display->flush_cb) return;
    handoff_timer = lv_timer_create(handoff_timer_cb, 1, NULL);
    if (!handoff_timer) return;
    lv_timer_pause(handoff_timer);
    refresh_display = display;
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && lv_indev_get_display(indev) == display)
            lv_indev_add_event_cb(indev, pointer_event, LV_EVENT_ALL, NULL);
    /* LVGL has no getter for its installed callback. Use the same private
     * display interface as this SDK's lv_lcd.c, preserving its wait callback,
     * driver data and asynchronous hardware completion path. */
    lcd_flush = display->flush_cb;
    lv_display_set_flush_cb(display, display_flush);
    lv_timer_set_cb(lv_display_get_refr_timer(display), refresh_timer_cb);
    lv_timer_set_period(lv_display_get_refr_timer(display), 200);
    app_schedule_set_anim_hook(page_lifecycle);
    lv_display_add_event_cb(display, invalidated,
        (lv_event_code_t)(LV_EVENT_INVALIDATE_AREA | LV_EVENT_PREPROCESS), NULL);
    lv_display_add_event_cb(display, display_event, LV_EVENT_REFR_REQUEST, NULL);
    lv_display_add_event_cb(display, display_event, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(display, display_event, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
    lv_display_add_event_cb(display, display_event, LV_EVENT_REFR_READY, NULL);
}
