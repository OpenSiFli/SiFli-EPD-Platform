#include "epd_refresh.h"
#include "gui_app_int.h"
#include "epd_waveform.h"
#include "epd_display_chunks.h"
#include "app_service_internal.h"
#include "app_module.h"
#include "epd_power.h"
#include "ui_shell.h"
#ifdef LCD_USING_EPD_E0470A03_V10_57
#include "epd_panel_pm.h"
#endif
#ifdef EPD_LCD_COMPLETION_EVENT
#include <rtdevice.h>
#include "mem_section.h"
#endif
#include "src/display/lv_display_private.h"
#include "src/core/lv_refr_private.h"
#include <string.h>
#include <rthw.h>
#include <rtm.h>

/* SDK lv_lcd.c checks the LCD queue, framebuffer and GPU completion. */
extern bool lv_refreshing_done(void);
static lv_display_t *refresh_display;

#ifdef EPD_LCD_COMPLETION_EVENT
typedef rt_err_t (*lcd_tx_complete_t)(rt_device_t device, void *buffer);
L2_RET_BSS_SECT_BEGIN(lcd_completion)
static struct rt_work lcd_completion_work L2_RET_BSS_SECT(lcd_completion);
static lcd_tx_complete_t lcd_tx_complete L2_RET_BSS_SECT(lcd_completion);
static volatile bool lcd_completion_pending L2_RET_BSS_SECT(lcd_completion);
L2_RET_BSS_SECT_END

static void notify_lcd_completion(struct rt_work *work, void *data)
{
    (void)work;
    rt_device_t lcd = data;
    bool busy = false;
    rt_device_control(lcd, RTGRAPHIC_CTRL_GET_BUSY, &busy);
    if (busy)
    {
        /* TX completion runs with the SDK driver locked. A synchronous info
         * request waits behind queued commands and returns after unlock. */
        struct rt_device_graphic_info info;
        rt_device_control(lcd, RTGRAPHIC_CTRL_GET_INFO, &info);
    }
    lcd_completion_pending = true;
    epd_power_notify();
}

static rt_err_t display_tx_complete(rt_device_t lcd, void *buffer)
{
    rt_err_t result = lcd_tx_complete(lcd, buffer);
    rt_work_submit(&lcd_completion_work, 0);
    return result;
}

rt_err_t __real_rt_device_set_tx_complete(rt_device_t device, lcd_tx_complete_t callback);

rt_err_t __wrap_rt_device_set_tx_complete(rt_device_t device, lcd_tx_complete_t callback)
{
    /* Keep the SDK's LVGL callback and the board's prepared-frame callback. */
    if (refresh_display && callback && device == lcd_completion_work.work_data)
    {
        lcd_tx_complete = callback;
        callback = display_tx_complete;
    }
    return __real_rt_device_set_tx_complete(device, callback);
}
#endif

typedef enum
{
    CHANGE_NONE, CHANGE_STATUS, CHANGE_INPUT, CHANGE_INPUT_STATUS, CHANGE_CONTENT,
    CHANGE_FULL, CHANGE_APP_FULL, CHANGE_CLEAN_FULL
} change_t;

#ifdef EPD_DISPLAY_CHUNKS
typedef struct { unsigned count, references; uint8_t phases[]; } screen_lut_t;
L2_RET_BSS_SECT_BEGIN(epd_lut_submission)
static screen_lut_t *submitted_lut L2_RET_BSS_SECT(epd_lut_submission);
L2_RET_BSS_SECT_END

static void lut_release(screen_lut_t *lut)
{
    if (lut && --lut->references == 0) lv_free(lut);
}
#endif

typedef struct screen_region
{
    struct screen_region *next;
    lv_obj_t *screen;
    lv_area_t area;
    bool has_status;
#ifdef EPD_DISPLAY_CHUNKS
    screen_lut_t *lut;
#endif
} screen_region_t;

static lv_display_flush_cb_t lcd_flush;
static lv_timer_t *handoff_timer;
static screen_region_t *screen_regions;
static lv_obj_t *foreground_screen;
static char foreground_app[GUI_APP_ID_MAX_LEN], submitted_app[GUI_APP_ID_MAX_LEN];
static lv_obj_t *current_screen;
static uint32_t page_generation;
static change_t pending_change, submitted_change;
static bool redraw_pending;
static bool locked, lock_hold, maintenance, interaction;
static bool completion_pending, completed_once;
static uint32_t dirty_generation, submitted_generation, retry_generation;
static bool retry_used, retry_pending;
static bool refresh_queued;
static uint32_t completed_at;
static uint32_t submission;
static bool synthetic_invalidate;
static unsigned status_update_depth;
#ifdef EPD_DISPLAY_CHUNKS
static bool prepare_enabled, prepared, prepared_interaction;
static change_t prepared_change;
static uint32_t prepared_generation;
#endif
typedef struct input_region {
    struct input_region *next;
    lv_obj_t *keyboard, *editor;
} input_region_t;
static input_region_t *input_regions;

void epd_refresh_input_objects(lv_obj_t *keyboard, lv_obj_t *editor, bool enabled)
{
    input_region_t **link = &input_regions;
    while (*link && (*link)->keyboard != keyboard) link = &(*link)->next;
    if (!enabled) {
        if (*link) { input_region_t *old = *link; *link = old->next; lv_free(old); }
        return;
    }
    if (!*link) {
        *link = lv_malloc(sizeof(**link));
        if (!*link) return;
        (*link)->keyboard = keyboard;
        (*link)->next = NULL;
    }
    (*link)->editor = editor;
}

static bool input_area(const lv_area_t *area)
{
    for (input_region_t *item = input_regions; item; item = item->next) {
        if (!lv_obj_is_visible(item->keyboard)) continue;
        lv_obj_t *objects[] = {item->keyboard, item->editor};
        for (unsigned i = 0; i < 2; ++i) {
            if (!objects[i] || !lv_obj_is_visible(objects[i])) continue;
            lv_area_t bounds;
            lv_obj_get_coords(objects[i], &bounds);
            /* Invalidation includes the border and cursor's draw extent. */
            lv_area_increase(&bounds, 2, 2);
            if (area->x1 >= bounds.x1 && area->y1 >= bounds.y1 &&
                area->x2 <= bounds.x2 && area->y2 <= bounds.y2) return true;
        }
    }
    return false;
}

static lv_obj_t *first_frame_screen;
static uint32_t first_frame_started;
static bool first_frame_waiting;
/* Intermediate native-slider values share one display budget. Input, hardware
 * preview and release callbacks still run at their normal rate. */
#define SLIDER_FRAME_INTERVAL_MS 100u
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
    uint32_t dirty_generation;
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
    if (elapsed < 100u) return 100u - elapsed;
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
    if (locked || !slider_dragging || !slider_submitted || pending_change >= CHANGE_FULL ||
        lv_display_get_screen_active(refresh_display) != current_screen) return 0;
    uint32_t elapsed = lv_tick_elaps(slider_submitted_at);
    return elapsed < SLIDER_FRAME_INTERVAL_MS ? SLIDER_FRAME_INTERVAL_MS - elapsed : 0;
}

static change_t merge_change(change_t left, change_t right)
{
    if ((left == CHANGE_INPUT && right == CHANGE_STATUS) ||
        (left == CHANGE_STATUS && right == CHANGE_INPUT))
        return CHANGE_INPUT_STATUS;
    return left > right ? left : right;
}

#ifdef EPD_DISPLAY_CHUNKS
static void discard_prepared(bool redraw)
{
    if (!prepared) return;
    epd_display_discard_prepared();
    prepared = false;
    if (redraw)
    {
        pending_change = merge_change(pending_change, prepared_change);
        redraw_pending = true;
    }
}
#endif

static void forget_screen(lv_obj_t *screen)
{
    if (first_frame_screen == screen) {
        first_frame_screen = NULL;
        first_frame_waiting = false;
    }
    if (foreground_screen == screen) foreground_screen = NULL;
    if (current_screen == screen)
    {
#ifdef EPD_DISPLAY_CHUNKS
        discard_prepared(false);
#endif
        current_screen = NULL;
        ++page_generation;
        ++dirty_generation;
        retry_pending = false;
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
#ifdef EPD_DISPLAY_CHUNKS
    lut_release(region->lut);
#endif
    lv_free(region);
}

static screen_region_t *screen_region(lv_obj_t *screen)
{
    if (!screen) return NULL;
    for (screen_region_t *region = screen_regions; region; region = region->next)
        if (region->screen == screen) return region;
    screen_region_t *region = lv_malloc_zeroed(sizeof(*region));
    LV_ASSERT_MALLOC(region);
    if (!region) return NULL;
    region->screen = screen;
    region->has_status = false;
    if (!lv_obj_add_event_cb(screen, screen_deleted, LV_EVENT_DELETE, region))
    { lv_free(region); return NULL; }
    region->next = screen_regions;
    screen_regions = region;
    return region;
}

rt_err_t epd_app_lut_load(lv_obj_t *screen, const void *data, size_t size)
{
#ifdef EPD_DISPLAY_CHUNKS
    if (rt_thread_self() != rt_thread_find("main") || !screen || screen != lv_screen_active())
        return -RT_EINVAL;
    const subpage_node_t *page = app_schedule_get_this_page();
    if (!page || page->scr != (screen_t)screen) return -RT_EINVAL;
    epd_memory_owner_t account = app_module_owner_acquire(page->parent->id);
    if (account == EPD_MEMORY_OWNER_INVALID) return -RT_EINVAL;
    bool owner = epd_memory_owner_is_current_account(account);
    epd_memory_owner_release_trusted(account);
    if (!owner) return -RT_EINVAL;
    unsigned count = 0;
    screen_lut_t *copy = NULL;
    if (data)
    {
        const uint8_t *bytes = data;
        if (size < 8u || memcmp(bytes, "EL57", 4) || bytes[4] != 1u || bytes[6] || bytes[7])
            return -RT_EINVAL;
        count = bytes[5];
        if (!count || count > EPD_LUT_PHASES_MAX || size != 8u + count * EPD_LUT_PHASE_BYTES)
            return -RT_EINVAL;
        for (unsigned phase = 0; phase < count; ++phase)
        {
            const uint8_t *codes = bytes + 8u + phase * EPD_LUT_PHASE_BYTES;
            for (unsigned i = 0; i < 256; ++i)
            {
                unsigned code = (codes[i / 4u] >> (2u * (i % 4u))) & 3u;
                if (code && (code == 3u || i % 17u == 0u || phase == count - 1u)) return -RT_EINVAL;
            }
        }
        copy = lv_malloc(sizeof(*copy) + size - 8u);
        if (!copy) return -RT_ENOMEM;
        copy->count = count;
        copy->references = 1;
        memcpy(copy->phases, bytes + 8u, size - 8u);
    }
    else if (size) return -RT_EINVAL;
    screen_region_t *region = screen_region(screen);
    if (!region) { lv_free(copy); return -RT_ENOMEM; }
    if (!region->lut && !copy) return RT_EOK;
    lut_release(region->lut);
    region->lut = copy;
    epd_refresh_request_full();
    return RT_EOK;
#else
    (void)screen; (void)data; (void)size;
    return -RT_ENOSYS;
#endif
}
RTM_EXPORT(epd_app_lut_load);

void epd_refresh_status_area(lv_obj_t *screen, const lv_area_t *area)
{
    screen_region_t *region = screen_region(screen);
    if (!region || !area) return;
    region->area = *area;
    region->has_status = true;
}

void epd_refresh_status_begin(void) { ++status_update_depth; }
void epd_refresh_status_end(void) { --status_update_depth; }

static void select_screen(void)
{
    lv_obj_t *screen = lv_display_get_screen_active(refresh_display);
    if (screen == current_screen) return;
#ifdef EPD_DISPLAY_CHUNKS
    discard_prepared(false);
#endif
    screen_region(screen);
    current_screen = screen;
    slider_dragging = false;
    slider_pointer = NULL;
    ++page_generation;
    ++dirty_generation;
    retry_pending = false;
    /* Discard classifications belonging to the previous page, including when
     * LVGL or app_fwk reuses a deleted screen's address. */
    pending_change = screen ? CHANGE_CONTENT : CHANGE_NONE;
    redraw_pending = screen != NULL;
    deferred.count = 0;
}

static bool panel_frame_succeeded(void)
{
#ifdef LCD_USING_EPD_E0470A03_V10_57
    return epd_panel_last_frame_succeeded();
#else
    return true;
#endif
}

#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
static struct {
    lv_obj_t *screen;
    uint32_t start, submission;
    const char *reason;
    unsigned changes;
    bool submitted, checking_prepare;
} reader_timing;

void epd_reader_timing_wait_page(lv_obj_t *screen)
{
    reader_timing.screen = screen;
    reader_timing.start = lv_tick_get();
    reader_timing.reason = NULL;
    reader_timing.changes = 0;
    reader_timing.submission = 0;
    reader_timing.submitted = false;
    rt_kprintf("b45_refresh tick=%lu event=wait_mark screen=%p\n",
               (unsigned long)rt_tick_get(), (void *)screen);
}

static bool reader_timing_active(void)
{
    return reader_timing.screen &&
           lv_tick_elaps(reader_timing.start) < 5000u;
}

static bool reader_timing_gate(bool allowed, const char *reason)
{
    if (allowed) reason = "allow";
    if (reader_timing.checking_prepare && reader_timing_active() &&
        current_screen == reader_timing.screen && reader_timing.changes < 24u &&
        (!reader_timing.reason || strcmp(reader_timing.reason, reason))) {
        reader_timing.reason = reason;
        ++reader_timing.changes;
        rt_kprintf("b45_refresh tick=%lu event=gate reason=%s pending=%u\n",
                   (unsigned long)rt_tick_get(), reason, (unsigned)completion_pending);
    }
    return allowed;
}
#define TRACE_GATE(value, reason) reader_timing_gate((value), (reason))
#else
#define TRACE_GATE(value, reason) (value)
#endif

static void observe_completion(void)
{
    /* With a framebuffer, flushing clears when the draw-buffer copy finishes.
     * The SDK display path must also be idle before starting the lock interval.
     * Observe completion in the UI thread; FLUSH_FINISH is only submission. */
    if (completion_pending && !refresh_display->flushing &&
#ifdef EPD_DISPLAY_CHUNKS
        !epd_display_prepared_in_flight() &&
#endif
        lv_refreshing_done())
    {
        completion_pending = false;
#ifdef EPD_DISPLAY_CHUNKS
        lut_release(submitted_lut);
        submitted_lut = NULL;
#endif
        const bool succeeded = panel_frame_succeeded();
        app_service_ui_signal(EPD_SERVICE_UI_DISPLAY);
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
        if (reader_timing_active() && reader_timing.submitted &&
            reader_timing.submission == submission) {
            rt_kprintf("b45_refresh tick=%lu event=wait_complete submission=%lu ok=%u\n",
                       (unsigned long)rt_tick_get(), (unsigned long)submission, (unsigned)succeeded);
            reader_timing.submitted = false;
        }
#endif
        if (succeeded)
        {
            completed_once = true;
            completed_at = lv_tick_get();
        }
        else if (!maintenance && current_screen &&
                 current_screen == lv_display_get_screen_active(refresh_display) &&
                 submitted_generation == dirty_generation &&
                 (!retry_used || retry_generation != submitted_generation))
        {
            /* One recovery redraw per external dirty generation. Synthetic
             * invalidation does not replenish the retry budget. Newer dirty
             * content already owns its next request and must not be discarded. */
            retry_used = true;
            retry_generation = submitted_generation;
            retry_pending = true;
#ifdef EPD_DISPLAY_CHUNKS
            discard_prepared(false);
#endif
            pending_change = submitted_change >= CHANGE_FULL ? submitted_change : CHANGE_FULL;
            redraw_pending = true;
        }
    }
}

static bool lock_status_deferred(void)
{
    return locked && lock_hold && !redraw_pending && pending_change == CHANGE_STATUS;
}

bool epd_refresh_update_due(void)
{
    if (!refresh_display || maintenance ||
        !lv_display_get_screen_active(refresh_display)) return TRACE_GATE(false, "display");
    observe_completion();
    /* Physical submission waits for the preceding panel refresh. */
    if (completion_pending) return TRACE_GATE(false, "panel_pending");
    if (lock_status_deferred()) return TRACE_GATE(false, "status_hold");
    if (first_frame_delay()) return TRACE_GATE(false, "first_frame");
    if (slider_delay()) return TRACE_GATE(false, "slider");
    if (!locked) return TRACE_GATE(true, "allow");
    if (lv_display_get_screen_active(refresh_display) != foreground_screen) return TRACE_GATE(false, "foreground_screen");
    return TRACE_GATE(retry_pending || interaction || !completed_once ||
                      lv_tick_elaps(completed_at) >= EPD_REFRESH_LOCK_INTERVAL_MS, "lock_interval");
}

static bool refresh_can_prepare(void)
{
#ifdef EPD_DISPLAY_CHUNKS
    if (prepare_enabled)
    {
        observe_completion();
        if (prepared) return TRACE_GATE(false, "prepared");
        if (!epd_display_can_prepare()) return TRACE_GATE(false, "display_prepare");
        if (completion_pending) {
            if (locked) return TRACE_GATE(false, "locked");
            if (maintenance || !current_screen) return TRACE_GATE(false, "display");
            if (first_frame_delay()) return TRACE_GATE(false, "first_frame");
            if (slider_delay()) return TRACE_GATE(false, "slider");
            return TRACE_GATE(true, "allow");
        }
    }
#endif
    return epd_refresh_update_due();
}

bool epd_refresh_can_prepare(void)
{
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    reader_timing.checking_prepare = true;
#endif
    bool allowed = refresh_can_prepare();
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    reader_timing.checking_prepare = false;
#endif
    return allowed;
}

#undef TRACE_GATE

static EpdDrawMode draw_mode(change_t change)
{
    if (change >= CHANGE_FULL) return EPD_DRAW_MODE_FULL;
    if (change == CHANGE_INPUT) return EPD_DRAW_MODE_INPUT;
    /* INPUT quantizes every dirty span. Mixed status/input frames retain gray
     * targets with PARTIAL while remaining outside the AUTO full cadence. */
    return change == CHANGE_STATUS || change == CHANGE_INPUT_STATUS ?
           EPD_DRAW_MODE_PARTIAL : EPD_DRAW_MODE_AUTO;
}

static void record_submission(bool used_interaction, uint32_t generation, change_t change)
{
    submitted_change = change;
#ifdef EPD_DISPLAY_CHUNKS
    screen_region_t *region = NULL;
    if (change == CHANGE_CONTENT && current_screen == foreground_screen && foreground_app[0] &&
        !locked && !lock_hold && !maintenance && !ui_shell_modal_visible())
        region = screen_region(current_screen);
    RT_ASSERT(!submitted_lut);
    submitted_lut = region ? region->lut : NULL;
    if (submitted_lut) ++submitted_lut->references;
    epd_frame_transition_t transition = EPD_FRAME_NORMAL;
    if (change == CHANGE_CLEAN_FULL) transition = EPD_FRAME_CLEAR_FIRST;
    else if (change == CHANGE_APP_FULL && !locked) transition = EPD_FRAME_APP_SWITCH;
    epd_display_stage_frame(submitted_lut ? submitted_lut->phases : NULL,
                            submitted_lut ? submitted_lut->count : 0, transition);
#endif
    if (current_screen == foreground_screen && foreground_app[0])
        memcpy(submitted_app, foreground_app, sizeof(submitted_app));
    if (used_interaction) interaction = false;
    completion_pending = true;
    submitted_generation = generation;
    retry_pending = false;
    ++submission;
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
    if (reader_timing_active() && current_screen == reader_timing.screen &&
        !reader_timing.submitted && !reader_timing.submission) {
        reader_timing.submission = submission;
        reader_timing.submitted = true;
        rt_kprintf("b45_refresh tick=%lu event=wait_submit submission=%lu\n",
                   (unsigned long)rt_tick_get(), (unsigned long)submission);
    }
#endif
    if (slider_dragging)
    {
        slider_submitted = true;
        slider_submitted_at = lv_tick_get();
    }
}

#ifdef EPD_DISPLAY_CHUNKS
static void submit_prepared(void)
{
    if (!prepared || !epd_refresh_update_due()) return;
    prepared = false;
    record_submission(prepared_interaction, prepared_generation, prepared_change);
    epd_display_submit_prepared(draw_mode(prepared_change));
}
#endif

bool epd_refresh_completed_once(void)
{
    if (!refresh_display) return false;
#ifdef EPD_LCD_COMPLETION_EVENT
    rt_base_t level = rt_hw_interrupt_disable();
    bool completed = lcd_completion_pending;
    lcd_completion_pending = false;
    rt_hw_interrupt_enable(level);
    if (completed) epd_refresh_process();
    else
#endif
        observe_completion();
    return completed_once;
}

uint32_t epd_refresh_submission(void) { return submission; }

bool epd_refresh_idle(void)
{
    if (!refresh_display) return false;
    observe_completion();
    return !completion_pending && (!retry_pending || maintenance) && !refresh_display->flushing &&
#ifdef EPD_DISPLAY_CHUNKS
           !prepared &&
#endif
           lv_refreshing_done();
}

static void schedule_handoff(void)
{
    if (!handoff_timer) return;
#ifdef EPD_DISPLAY_CHUNKS
    bool eligible = !locked || (current_screen == foreground_screen &&
        (retry_pending || interaction || !completed_once || (!completion_pending &&
         lv_tick_elaps(completed_at) >= EPD_REFRESH_LOCK_INTERVAL_MS)));
    epd_display_set_pending(!maintenance && current_screen && eligible && !lock_status_deferred() && !first_frame_delay() &&
        (pending_change != CHANGE_NONE || redraw_pending || deferred.count || prepared));
#endif
    uint32_t delay;
    if (completion_pending) {
#ifdef EPD_LCD_COMPLETION_EVENT
        if (lcd_completion_work.work_data)
        {
            lv_timer_pause(handoff_timer);
            return;
        }
#endif
        delay = 1;
    }
    else if (maintenance || lock_status_deferred() || !lv_display_get_screen_active(refresh_display) ||
             (locked && lv_display_get_screen_active(refresh_display) != foreground_screen) ||
             (pending_change == CHANGE_NONE && !redraw_pending
#ifdef EPD_DISPLAY_CHUNKS
              && !prepared
#endif
             ))
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

static void arm_refresh_timer(void)
{
    lv_timer_t *timer = lv_display_get_refr_timer(refresh_display);
    /* Arm once until the callback consumes the request, even if it defers. */
    if (!refresh_queued)
    {
        refresh_queued = true;
        lv_timer_ready(timer);
    }
    lv_timer_resume(timer);
}

void epd_refresh_process(void)
{
    if (!refresh_display) return;
    observe_completion();
    select_screen();
    restore_deferred();
#ifdef EPD_DISPLAY_CHUNKS
    if (prepared && prepared_change == CHANGE_STATUS && pending_change >= CHANGE_CONTENT)
        discard_prepared(true);
    submit_prepared();
#endif
    if (epd_refresh_can_prepare())
    {
        redraw_current();
        if (refresh_display->inv_p) arm_refresh_timer();
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
    if (!epd_refresh_can_prepare())
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
    if (!enabled) lock_hold = false;
#ifdef EPD_DISPLAY_CHUNKS
    if (enabled) discard_prepared(true);
#endif
    schedule_handoff();
}

void epd_refresh_set_lock_hold(bool enabled)
{
    lock_hold = enabled;
    schedule_handoff();
}

void epd_refresh_set_maintenance(bool enabled)
{
    maintenance = enabled;
    if (enabled)
    {
        interaction = false;
#ifdef EPD_DISPLAY_CHUNKS
        discard_prepared(true);
#endif
    }
    schedule_handoff();
}

void epd_refresh_request_interaction(void)
{
    first_frame_waiting = false;
    interaction = true;
    schedule_handoff();
}

static void request_full(change_t change)
{
    if (!refresh_display) return;
    select_screen();
#ifdef EPD_DISPLAY_CHUNKS
    discard_prepared(true);
#endif
    if (pending_change < change) pending_change = change;
    redraw_pending = true;
    ++dirty_generation;
    retry_pending = false;
    schedule_handoff();
}

void epd_refresh_request_full(void) { request_full(CHANGE_FULL); }
void epd_refresh_request_clean_full(void) { request_full(CHANGE_CLEAN_FULL); }

static void page_lifecycle(const screen_t screen, gui_app_msg_type_t message)
{
    lv_obj_t *object = (lv_obj_t *)screen;
    if (message == GUI_APP_MSG_ONRESUME)
    {
        screen_region(object);
        foreground_screen = object;
        const gui_runing_app_t *app = gui_app_get_actived();
        foreground_app[0] = 0;
        if (app) {
            strncpy(foreground_app, app->id, sizeof(foreground_app) - 1);
            foreground_app[sizeof(foreground_app) - 1] = 0;
        }
        /* A transient screen can resume during a switch without reaching the panel. */
        if (object == lv_display_get_screen_active(refresh_display) &&
            submitted_app[0] && foreground_app[0] && strcmp(submitted_app, foreground_app))
            request_full(CHANGE_APP_FULL);
        else
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
    ++dirty_generation;
    screen_region_t *region = screen_region(current_screen);
    change_t change = input_area(area) ? CHANGE_INPUT : CHANGE_CONTENT;
    if (status_update_depth ||
        (current_screen == foreground_screen && region && region->has_status &&
        area->x1 >= region->area.x1 && area->y1 >= region->area.y1 &&
         area->x2 <= region->area.x2 && area->y2 <= region->area.y2))
        change = CHANGE_STATUS;
    pending_change = merge_change(pending_change, change);
}

static void display_event(lv_event_t *event)
{
    switch (lv_event_get_code(event))
    {
    case LV_EVENT_REFR_REQUEST:
        /* An eligible invalidation needs no handoff tick before rendering.
         * refresh_timer_cb rechecks the gates if state changes meanwhile. */
        if (epd_refresh_can_prepare()) arm_refresh_timer();
        else lv_timer_pause(lv_display_get_refr_timer(refresh_display));
        schedule_handoff();
        break;
    case LV_EVENT_REFR_START:
        /* A synchronous lv_refr_now also consumes an armed timer request. */
        refresh_queued = false;
        observe_completion();
        select_screen();
        restore_deferred();
        frame.allowed = epd_refresh_can_prepare();
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
        frame.dirty_generation = dirty_generation;
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
#ifdef EPD_DISPLAY_CHUNKS
    if (prepare_enabled)
    {
        const lv_layer_t *layer = display->layer_head;
        uint32_t stride = layer->draw_buf->header.stride;
        unsigned x = area->x1 - display->offset_x - layer->buf_area.x1;
        unsigned y = area->y1 - display->offset_y - layer->buf_area.y1;
        epd_display_prepare_chunk(pixels + y * stride + x * 2u, stride,
                                  area->x1, area->y1, area->x2, area->y2);
        lv_display_flush_ready(display);
        if (last && frame.page_generation != page_generation)
            epd_display_discard_prepared();
        else if (last)
        {
            prepared = true;
            prepared_change = frame.change;
            prepared_generation = frame.dirty_generation;
            prepared_interaction = frame.interaction;
            submit_prepared();
        }
        return;
    }
#endif
    if (last)
    {
        epd_wave_submit_frame(draw_mode(frame.change));
        record_submission(frame.interaction, frame.dirty_generation, frame.change);
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
#ifdef EPD_DISPLAY_CHUNKS
    submitted_lut = NULL;
#endif
    lv_display_t *display = lv_display_get_default();
    if (!display || !display->flush_cb) return;
    handoff_timer = lv_timer_create(handoff_timer_cb, 1, NULL);
    if (!handoff_timer) return;
    lv_timer_pause(handoff_timer);
#ifdef EPD_LCD_COMPLETION_EVENT
    if (rt_workqueue_sysq())
        rt_work_init(&lcd_completion_work, notify_lcd_completion, rt_device_find("lcd"));
#endif
    refresh_display = display;
#ifdef EPD_DISPLAY_CHUNKS
    if (lv_display_get_color_format(display) == LV_COLOR_FORMAT_RGB565)
        prepare_enabled = epd_display_prepare_init();
#endif
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && lv_indev_get_display(indev) == display)
            lv_indev_add_event_cb(indev, pointer_event, LV_EVENT_ALL, NULL);
    /* LVGL has no getter for its installed callback. Use the same private
     * display interface as this SDK's lv_lcd.c, preserving its wait callback,
     * driver data and asynchronous hardware completion path. */
    lcd_flush = display->flush_cb;
    lv_display_set_flush_cb(display, display_flush);
    lv_timer_set_cb(lv_display_get_refr_timer(display), refresh_timer_cb);
    lv_timer_set_period(lv_display_get_refr_timer(display), 100);
    app_schedule_set_anim_hook(page_lifecycle);
    lv_display_add_event_cb(display, invalidated,
        (lv_event_code_t)(LV_EVENT_INVALIDATE_AREA | LV_EVENT_PREPROCESS), NULL);
    lv_display_add_event_cb(display, display_event, LV_EVENT_REFR_REQUEST, NULL);
    lv_display_add_event_cb(display, display_event, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(display, display_event, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
    lv_display_add_event_cb(display, display_event, LV_EVENT_REFR_READY, NULL);
}
