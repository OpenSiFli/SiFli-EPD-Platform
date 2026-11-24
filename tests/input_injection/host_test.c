/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "epd_input_test.h"
#include "rtthread.h"
#include "ui_nav_gesture.h"
#include "ui_shell_model.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ExitProcess(1); } } while (0)
enum { WIDTH = 684, HEIGHT = 1216, UI_SETTING_TOUCH, UI_PAGE_LOCK, NAV_IDLE, NAV_BACK };
static DWORD ui_thread;
static CRITICAL_SECTION irq;
static __declspec(thread) unsigned irq_depth;
static uint32_t now = 10;
static lv_indev_t *pointer, *other;
static lv_obj_t *content;
static unsigned starts, ends, messages, clicks, long_presses, gestures, actions, unlocks;
static unsigned keys_pending, keys_delivered, key_clears, key_polls;
static unsigned long start_ids[512], end_ids[512];
static char end_results[512][32];
static lv_point_t clicked_point;
static lv_dir_t gesture_dir;
static ui_nav_action_t nav_action;
static volatile LONG wakes, notifications;
static bool locked, refresh_hold;
static struct { bool power, recent, nav_error, nav_visible, transient_status;
    uint32_t hide_status_at, navigation_scope, navigation_popup; ui_shell_gesture_t gesture; } shell;
typedef struct { int id; bool foreground, popup; void *feature; } page_t;
static page_t page, *current = &page;
static int pending_page = NAV_IDLE;

static void on_ui(void) { CHECK(GetCurrentThreadId() == ui_thread && !irq_depth); }
rt_base_t rt_hw_interrupt_disable(void) { EnterCriticalSection(&irq); return irq_depth++; }
void rt_hw_interrupt_enable(rt_base_t level) { CHECK(irq_depth == level + 1); --irq_depth; LeaveCriticalSection(&irq); }
bool epd_power_key_wakeup(void) { CHECK(!irq_depth); InterlockedIncrement(&wakes); return true; }
void epd_power_notify(void) { CHECK(!irq_depth); InterlockedIncrement(&notifications); }
void epd_input_clear(void) { on_ui(); keys_pending = 0; ++key_clears; }
void epd_input_process(void) { on_ui(); keys_delivered += keys_pending; keys_pending = 0; ++key_polls; }
int rt_kprintf(const char *format, ...)
{
    CHECK(!irq_depth);
    char line[256];
    va_list args; va_start(args, format); int result = vsnprintf(line, sizeof(line), format, args); va_end(args);
    if (strstr(line, "ui_pointer start ")) {
        on_ui(); CHECK(starts < 512); CHECK(sscanf(line, "ui_pointer start id=%lu", &start_ids[starts]) == 1); ++starts;
    } else if (strstr(line, "ui_pointer end ")) {
        on_ui(); CHECK(ends < starts);
        CHECK(sscanf(line, "ui_pointer end id=%lu result=%31s", &end_ids[ends], end_results[ends]) == 2);
        CHECK(end_ids[ends] == start_ids[ends]); ++ends;
    } else ++messages;
    return result;
}
static bool ui_settings_enabled(int value) { (void)value; return true; }
static bool ui_shell_locked(void) { return locked; }
static bool ui_control_center_visible(void) { return false; }
static bool ui_voice_input_visible(void) { return false; }
static bool page_popup_visible(void) { return false; }
static bool board_power_pending(void) { return false; }
static bool system_reset_blocks_apps(void) { return false; }
static bool storage_changing(void) { return false; }
static bool storage_release_requested(void) { return false; }
static bool epd_power_sleeping(void) { return false; }
static bool gesture_navigation(void) { return true; }
static bool immersive(void) { return false; }
static unsigned epd_ui_scale_percent(void) { return 100; }
static int epd_ui_px(int value) { return value; }
static unsigned epd_input_scope_id(void) { return 1; }
static unsigned navigation_popup(void) { return 0; }
static bool ui_shell_modal_visible(void) { return false; }
static void epd_refresh_set_lock_hold(bool active) { on_ui(); refresh_hold = active; }
static bool epd_lock_unlock(void) { on_ui(); CHECK(lv_indev_active() == pointer); ++unlocks; return true; }
static bool epd_lock_active(void) { return locked; }
#include "actual_handlers.inc"

typedef struct { lv_point_t point; bool down; } sample_t;
static struct { sample_t value, buffer[64]; unsigned count, cursor, reads; } devices[2];
static uint32_t tick(void) { return now; }
static void driver(lv_indev_t *input, lv_indev_data_t *data)
{
    on_ui(); unsigned index = input == pointer ? 0 : 1;
    if (devices[index].cursor < devices[index].count) devices[index].value = devices[index].buffer[devices[index].cursor++];
    data->point = devices[index].value.point;
    data->state = devices[index].value.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = devices[index].cursor < devices[index].count;
    ++devices[index].reads;
}
static void event(lv_event_t *ev)
{
    on_ui(); lv_indev_t *input = lv_event_get_indev(ev);
    switch (lv_event_get_code(ev)) {
    case LV_EVENT_CLICKED: ++clicks; lv_indev_get_point(input, &clicked_point); break;
    case LV_EVENT_LONG_PRESSED: ++long_presses; break;
    case LV_EVENT_GESTURE: ++gestures; gesture_dir = lv_indev_get_gesture_dir(input); break;
    default: break;
    }
}
static void dispatch(ui_nav_action_t action) { on_ui(); ++actions; nav_action = action; }
static void physical(unsigned index, int x, int y, bool down)
{
    devices[index].value.point.x = x; devices[index].value.point.y = y;
    devices[index].value.down = down; devices[index].count = devices[index].cursor = 0;
}
static void step(uint32_t elapsed)
{
    now += elapsed;
    navigation_update();
    lv_obj_update_layout(lv_screen_active()); lv_obj_update_layout(lv_layer_top());
    if (pointer) lv_indev_read(pointer);
    if (other) lv_indev_read(other);
    epd_input_test_process_keys();
}
static void reset(void)
{
    physical(0, 340, 500, false); physical(1, 340, 500, false);
    step(10); step(10);
    CHECK(starts == ends);
    locked = false; page.feature = NULL; pending_page = NAV_IDLE;
    clicks = long_presses = gestures = actions = unlocks = 0;
    keys_delivered = keys_pending = key_polls = key_clears = 0;
    nav_action = UI_NAV_NONE; shell.gesture = UI_SHELL_NONE;
    ui_nav_gesture_cancel(); navigation_update();
}
static epd_input_test_request_t request(epd_input_test_kind_t kind, unsigned x, unsigned y,
                                      unsigned end_x, unsigned end_y, uint32_t ms)
{
    epd_input_test_request_t value = {kind, (uint16_t)x, (uint16_t)y, (uint16_t)end_x, (uint16_t)end_y, ms, 0};
    return value;
}
static uint32_t submit(epd_input_test_request_t value)
{
    uint32_t id = 0; CHECK(epd_input_test_submit(&value, &id) == RT_EOK && id); return id;
}
static void wait_for(uint32_t id)
{
    unsigned limit = 0;
    while (!ends || end_ids[ends - 1] != id) { CHECK(++limit < 5000); step(10); }
}
static void run(epd_input_test_request_t value) { wait_for(submit(value)); CHECK(!strcmp(end_results[ends - 1], "ok")); }
static void setup(void)
{
    lv_init(); lv_tick_set_cb(tick);
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    CHECK(display);
    content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(content); lv_obj_set_size(content, WIDTH, HEIGHT);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(content, event, LV_EVENT_ALL, NULL);
    other = lv_indev_create(); pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER); lv_indev_set_type(other, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, driver); lv_indev_set_read_cb(other, driver);
    lv_timer_pause(lv_indev_get_read_timer(pointer)); lv_timer_pause(lv_indev_get_read_timer(other));
    /* The first enumerated pointer is the injection target. */
    CHECK(lv_indev_get_next(NULL) == pointer);
    CHECK(epd_input_test_init()); CHECK(epd_input_test_init());
    edge_pointer.read = lv_indev_get_read_cb(pointer); lv_indev_set_read_cb(pointer, edge_pointer_read);
    ui_nav_gesture_init(dispatch); reset();
}

static void tap_hold_cases(void)
{
    reset(); run(request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80));
    CHECK(clicks == 1 && !long_presses && !gestures && !actions);
    CHECK(clicked_point.x == 340 && clicked_point.y == 500);
    CHECK(key_clears && !key_polls); step(10); CHECK(key_polls == 1);
    reset(); run(request(EPD_INPUT_TEST_HOLD, 340, 500, 340, 500, 700));
    CHECK(clicks == 1 && long_presses == 1 && !actions);
    puts("PASS real LVGL tap, long press, final release and key restoration");
}
static void lock_cases(void)
{
    reset(); locked = true;
    lock_state_t lock = {0}; lock.unlock = lv_obj_create(content);
    lv_obj_set_pos(lock.unlock, 240, 400); lv_obj_set_size(lock.unlock, 200, 120);
    lv_obj_remove_flag(lock.unlock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(lock.unlock, lock_unlock_event, LV_EVENT_ALL, NULL);
    page.id = UI_PAGE_LOCK; page.foreground = true; page.feature = &lock;
    run(request(EPD_INPUT_TEST_TAP, 340, 460, 340, 460, 80)); CHECK(!unlocks && !refresh_hold);
    run(request(EPD_INPUT_TEST_HOLD, 340, 460, 340, 460, 700)); CHECK(unlocks == 1 && !refresh_hold);
    run(request(EPD_INPUT_TEST_SWIPE, 340, 460, 500, 460, 700)); CHECK(unlocks == 1 && !refresh_hold);
    lv_obj_delete(lock.unlock); page.feature = NULL; locked = false;
    puts("PASS production lock callback: short tap rejected, long hold unlocks, drag rejected");
}
static void swipe_cases(void)
{
    reset(); run(request(EPD_INPUT_TEST_SWIPE, 200, 500, 480, 500, 160));
    CHECK(gestures == 1 && gesture_dir == LV_DIR_RIGHT && !actions);
    reset(); run(request(EPD_INPUT_TEST_SWIPE, 4, 500, 110, 500, 160));
    CHECK(actions == 1 && nav_action == UI_NAV_BACK && !clicks && !gestures);
    reset(); run(request(EPD_INPUT_TEST_SWIPE, 340, 1208, 340, 1090, 160));
    CHECK(actions == 1 && nav_action == UI_NAV_HOME && !clicks);
    reset(); epd_input_test_request_t recent = request(EPD_INPUT_TEST_SWIPE, 340, 1208, 340, 1090, 160);
    recent.end_hold_ms = 500; run(recent);
    CHECK(actions == 1 && nav_action == UI_NAV_RECENT && !clicks);
    reset(); run(request(EPD_INPUT_TEST_SWIPE, 500, 8, 500, 160, 160));
    CHECK(shell.gesture == UI_SHELL_CONTROLS && !clicks);
    puts("PASS LVGL gesture and production side/back, bottom/home/recent, top/control-center observers");
}
static void isolation_cases(void)
{
    reset(); uint32_t id = submit(request(EPD_INPUT_TEST_HOLD, 340, 500, 340, 500, 700)); step(10);
    physical(0, 540, 700, true); physical(1, 100, 800, true);
    while (!ends || end_ids[ends - 1] != id) { keys_pending = 1; step(10); CHECK(!keys_delivered); }
    CHECK(clicks == 1 && clicked_point.x == 340 && clicked_point.y == 500);
    for (unsigned i = 0; i < 20; ++i) step(10);
    CHECK(clicks == 1 && lv_indev_get_state(pointer) == LV_INDEV_STATE_RELEASED && lv_indev_get_state(other) == LV_INDEV_STATE_RELEASED);
    physical(0, 540, 700, false); physical(1, 100, 800, false); step(10);
    CHECK(clicks == 1);
    physical(0, 540, 700, true); step(10); physical(0, 540, 700, false); step(10);
    CHECK(clicks == 2 && clicked_point.x == 540); keys_pending = 1; step(10); CHECK(keys_delivered == 1);
    reset(); id = submit(request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80)); step(10);
    devices[0].count = 40; devices[0].cursor = 0;
    for (unsigned i = 0; i < 40; ++i) devices[0].buffer[i] = (sample_t){{100, 800}, (i & 1u) == 0};
    unsigned before = devices[0].reads; step(10); CHECK(devices[0].reads - before == 8);
    wait_for(id); step(10); CHECK(clicks == 1 && devices[0].cursor == 40);
    reset(); id = submit(request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80)); step(10);
    uint32_t rejected = submit(request(EPD_INPUT_TEST_TAP, 65535, 500, 65535, 500, 80));
    physical(0, 540, 700, true); wait_for(id); wait_for(rejected);
    CHECK(!strcmp(end_results[ends - 1], "bounds") && lv_indev_get_state(pointer) == LV_INDEV_STATE_RELEASED);
    physical(0, 540, 700, false); step(10); CHECK(clicks == 1);
    puts("PASS concurrent touch and keys excluded, second pointer isolated, held touch quarantined, bounded FIFO drain");
}
static void queue_cases(void)
{
    reset(); uint32_t ids[EPD_INPUT_TEST_CAPACITY], unchanged = 123456;
    epd_input_test_request_t value = request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80);
    unsigned before = starts;
    ids[0] = submit(value); step(10);
    for (unsigned i = 1; i < EPD_INPUT_TEST_CAPACITY; ++i) ids[i] = submit(value);
    CHECK(epd_input_test_submit(&value, &unchanged) == -RT_EFULL && unchanged == 123456);
    wait_for(ids[EPD_INPUT_TEST_CAPACITY - 1]); CHECK(clicks == EPD_INPUT_TEST_CAPACITY);
    for (unsigned i = 0; i < EPD_INPUT_TEST_CAPACITY; ++i) CHECK(start_ids[before + i] == ids[i] && end_ids[before + i] == ids[i]);
    puts("PASS four total slots including active request, FIFO order, full queue preserves requests and output id");
}
int host_shell(int argc, char **argv);
static int command(const char *text)
{
    char buffer[180]; strcpy(buffer, text); char *argv[12]; int argc = 0;
    for (char *part = strtok(buffer, " "); part; part = strtok(NULL, " ")) { CHECK(argc < 12); argv[argc++] = part; }
    return host_shell(argc, argv);
}
static void parser_cases(void)
{
    reset();
    const char *invalid[] = {"ui_pointer", "ui_pointer tap 1", "ui_pointer hold 1 2", "ui_pointer drag 1 2 3",
        "ui_pointer tap -1 2", "ui_pointer tap 1x 2", "ui_pointer tap 4294967297 2", "ui_pointer tap 1 2 0",
        "ui_pointer hold 1 2 10001", "ui_pointer swipe 1 2 3 4 5", "ui_pointer swipe 1 2 3 4 80 extra",
        "ui_pointer swipe 1 2 3 4 10000 1", "ui_pointer swipe 1 2 3 4 80 999999999999"};
    unsigned before = starts;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) CHECK(command(invalid[i]) == -RT_EINVAL);
    CHECK(starts == before);
    CHECK(command("ui_pointer tap 340 500") == RT_EOK);
    for (unsigned i = 0; i < 14; ++i) step(10);
    CHECK(starts == before + 1 && starts == ends && clicks == 1);
    CHECK(command("ui_pointer hold 340 500 600") == RT_EOK);
    for (unsigned i = 0; i < 65; ++i) step(10);
    CHECK(long_presses == 1 && starts == ends);
    CHECK(command("ui_pointer swipe 200 500 480 500 160") == RT_EOK);
    for (unsigned i = 0; i < 20; ++i) step(10);
    CHECK(gestures == 1 && starts == ends);
    CHECK(command("ui_pointer swipe 340 1208 340 1090 160 500") == RT_EOK);
    for (unsigned i = 0; i < 70; ++i) step(10);
    CHECK(nav_action == UI_NAV_RECENT && starts == ends);
    puts("PASS real FinSH command parser, numeric/arity/duration rejection and three command forms");
}
static void bounds_busy_rotation_time(void)
{
    reset(); wait_for(submit(request(EPD_INPUT_TEST_TAP, 65535, 500, 65535, 500, 80)));
    CHECK(!strcmp(end_results[ends - 1], "bounds") && !clicks);
    reset(); physical(0, 340, 500, true); step(10);
    wait_for(submit(request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80)));
    CHECK(!strcmp(end_results[ends - 1], "busy"));
    physical(0, 340, 500, false); step(10); CHECK(clicks == 1);
    for (unsigned i = 0; i < 4; ++i) {
        reset(); lv_display_set_rotation(NULL, (lv_display_rotation_t)i);
        lv_obj_set_size(content, lv_display_get_horizontal_resolution(NULL), lv_display_get_vertical_resolution(NULL));
        run(request(EPD_INPUT_TEST_TAP, 300, 400, 300, 400, 80));
        CHECK(clicks == 1 && clicked_point.x == 300 && clicked_point.y == 400);
    }
    lv_display_set_rotation(NULL, LV_DISPLAY_ROTATION_0); lv_obj_set_size(content, WIDTH, HEIGHT);
    reset(); now = UINT32_MAX - 40u;
    run(request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80)); CHECK(clicks == 1);
    reset(); uint32_t id = submit(request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80));
    step(10); step(5000); CHECK(lv_indev_get_state(pointer) == LV_INDEV_STATE_PRESSED);
    step(10); CHECK(clicks == 1 && end_ids[ends - 1] != id);
    step(10); CHECK(end_ids[ends - 1] == id && clicks == 1);
    puts("PASS out-of-bounds/busy rejection, four rotations, tick wrap and delayed UI release before completion");
}
static volatile LONG producer_done, producer_count;
static DWORD WINAPI producer(LPVOID unused)
{
    (void)unused;
    epd_input_test_request_t value = request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 40);
    for (unsigned i = 0; i < 80;) {
        int result = epd_input_test_submit(&value, NULL);
        if (result == RT_EOK) { ++i; InterlockedIncrement(&producer_count); }
        else CHECK(result == -RT_EFULL);
        SwitchToThread();
    }
    InterlockedExchange(&producer_done, 1); return 0;
}
static void concurrent_case(void)
{
    reset(); unsigned before = ends;
    HANDLE thread = CreateThread(NULL, 0, producer, NULL, 0, NULL); CHECK(thread);
    DWORD start = GetTickCount();
    while (!producer_done || ends != before + 80) {
        CHECK(GetTickCount() - start < 10000); step(10); SwitchToThread();
    }
    CHECK(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0); CloseHandle(thread);
    CHECK(producer_count == 80 && starts == ends && clicks == 80);
    puts("PASS concurrent shell producer/UI consumer: 80 FIFO requests, once-only completion and UI-only callbacks");
}
static void delete_case(void)
{
    reset(); unsigned before = ends;
    uint32_t id = submit(request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80)); step(10);
    lv_indev_delete(other); other = NULL; wait_for(id);
    CHECK(clicks == 1 && !strcmp(end_results[ends - 1], "ok")); before = ends;
    submit(request(EPD_INPUT_TEST_HOLD, 340, 500, 340, 500, 700)); step(10);
    submit(request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80));
    lv_indev_delete(pointer); pointer = NULL;
    CHECK(ends == before + 2 && starts == ends);
    CHECK(!strcmp(end_results[ends - 1], "device_deleted") && !strcmp(end_results[ends - 2], "device_deleted"));
    epd_input_test_request_t value = request(EPD_INPUT_TEST_TAP, 340, 500, 340, 500, 80);
    CHECK(epd_input_test_submit(&value, NULL) == -RT_ENOSYS);
    epd_input_test_process_keys(); keys_pending = 1; epd_input_test_process_keys(); CHECK(keys_delivered == 1);
    puts("PASS input deletion completes active/queued requests and restores physical-key processing");
}
int main(void)
{
    ui_thread = GetCurrentThreadId(); InitializeCriticalSection(&irq);
    epd_input_test_request_t value = request(EPD_INPUT_TEST_TAP, 10, 10, 10, 10, 80);
    CHECK(epd_input_test_submit(&value, NULL) == -RT_ENOSYS);
    CHECK(epd_input_test_submit(NULL, NULL) == -RT_EINVAL);
    setup(); tap_hold_cases(); lock_cases(); swipe_cases(); isolation_cases(); queue_cases(); parser_cases();
    bounds_busy_rotation_time(); concurrent_case(); delete_case();
    CHECK(starts == ends && starts > 100 && wakes == notifications && !irq_depth);
    printf("PASS %u accepted requests: exactly %u start and %u end logs, no sample logs\n", starts, starts, ends);
    return 0;
}
