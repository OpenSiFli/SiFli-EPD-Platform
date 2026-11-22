/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_input_test.h"
#if EPD_INPUT_TEST_ENABLED
#include "lvgl.h"
#include "platform/epd_input.h"
#include "platform/epd_power.h"
#include <rthw.h>
#include <rtthread.h>
#include "mem_section.h"
#include <string.h>

enum { POINTER_CAPACITY = 4, DRAIN_BUDGET = 8 };
typedef struct {
    lv_indev_t *input;
    lv_indev_read_cb_t read;
    lv_point_t neutral;
    bool quarantine;
} pointer_t;
typedef struct { epd_input_test_request_t request; uint32_t id; } queued_t;
static unsigned pointer_count;
L2_RET_BSS_SECT_BEGIN(epd_input_test)
static pointer_t pointers[POINTER_CAPACITY] L2_RET_BSS_SECT(epd_input_test);
static queued_t queue[EPD_INPUT_TEST_CAPACITY] L2_RET_BSS_SECT(epd_input_test);
static queued_t current L2_RET_BSS_SECT(epd_input_test);
L2_RET_BSS_SECT_END
/* Only the queue/publication fields below are shared with the shell. */
static unsigned head, count;
static uint32_t next_id;
static bool ready;
static enum { IDLE, CONTACT, RELEASE, FINISH } phase;
static uint32_t started;
static lv_display_rotation_t rotation;
static int32_t raw_width, raw_height;
static bool settling;

static const char *kind_name(epd_input_test_kind_t kind)
{
    return kind == EPD_INPUT_TEST_TAP ? "tap" : kind == EPD_INPUT_TEST_HOLD ? "hold" : "swipe";
}

static void begin_log(void)
{
    const epd_input_test_request_t *r = &current.request;
    rt_kprintf("ui_pointer start id=%lu kind=%s from=%u,%u to=%u,%u ms=%lu hold=%lu\n",
        (unsigned long)current.id, kind_name(r->kind), (unsigned)r->x, (unsigned)r->y,
        (unsigned)r->end_x, (unsigned)r->end_y, (unsigned long)r->duration_ms, (unsigned long)r->end_hold_ms);
}

static void finish(const char *result)
{
    rt_kprintf("ui_pointer end id=%lu result=%s\n", (unsigned long)current.id, result);
    rt_base_t level = rt_hw_interrupt_disable();
    head = (head + 1u) % EPD_INPUT_TEST_CAPACITY;
    --count;
    rt_hw_interrupt_enable(level);
    phase = IDLE;
    settling = true;
}

static bool peek(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool available = count != 0;
    if (available) current = queue[head];
    rt_hw_interrupt_enable(level);
    return available;
}

static lv_point_t raw_point(int32_t x, int32_t y)
{
    lv_point_t point = {x, y};
    if (rotation == LV_DISPLAY_ROTATION_90) {
        point.x = y; point.y = raw_height - x - 1;
    } else if (rotation == LV_DISPLAY_ROTATION_180) {
        point.x = raw_width - x - 1; point.y = raw_height - y - 1;
    } else if (rotation == LV_DISPLAY_ROTATION_270) {
        point.x = raw_width - y - 1; point.y = x;
    }
    return point;
}

static void neutral_sample(pointer_t *pointer, lv_indev_data_t *data)
{
    data->point = pointer->neutral;
    data->state = LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    data->timestamp = lv_tick_get();
}

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    pointer_t *pointer = NULL;
    for (unsigned i = 0; i < pointer_count; ++i)
        if (pointers[i].input == input) { pointer = &pointers[i]; break; }
    if (!pointer) return;
    pointer->read(input, data);
    /* Drain a bounded number of real samples while they are excluded. Never
     * forward a buffered physical DOWN after the synthetic UP. */
    if (phase != IDLE || pointer->quarantine) {
        for (unsigned n = 1; data->continue_reading && n < DRAIN_BUDGET; ++n)
            pointer->read(input, data);
        pointer->quarantine = data->continue_reading || data->state == LV_INDEV_STATE_PRESSED;
    }
    if (pointer != &pointers[0]) {
        if (phase != IDLE || pointer->quarantine) neutral_sample(pointer, data);
        else pointer->neutral = data->point;
        return;
    }
    if (phase == FINISH) {
        /* The preceding UP has already passed through observers and LVGL. */
        finish("ok");
        neutral_sample(pointer, data);
        return;
    }
    if (phase == IDLE) {
        if (!peek()) {
            if (pointer->quarantine) neutral_sample(pointer, data);
            else pointer->neutral = data->point;
            return;
        }
        begin_log();
        lv_display_t *display = lv_indev_get_display(input);
        int32_t width = lv_display_get_horizontal_resolution(display);
        int32_t height = lv_display_get_vertical_resolution(display);
        const epd_input_test_request_t *r = &current.request;
        bool busy = pointer->quarantine || data->continue_reading || data->state == LV_INDEV_STATE_PRESSED;
        for (unsigned i = 0; i < pointer_count; ++i)
            if (pointers[i].input && lv_indev_get_state(pointers[i].input) == LV_INDEV_STATE_PRESSED) busy = true;
        if (r->x >= width || r->end_x >= width || r->y >= height || r->end_y >= height) {
            finish("bounds");
            if (pointer->quarantine) neutral_sample(pointer, data);
            return;
        }
        if (busy) {
            finish("busy");
            if (pointer->quarantine) neutral_sample(pointer, data);
            return;
        }
        rotation = lv_display_get_rotation(display);
        raw_width = lv_display_get_original_horizontal_resolution(display);
        raw_height = lv_display_get_original_vertical_resolution(display);
        started = lv_tick_get();
        phase = CONTACT;
    }
    const epd_input_test_request_t *r = &current.request;
    uint32_t elapsed = lv_tick_get() - started;
    bool completed = elapsed >= r->duration_ms + r->end_hold_ms;
    if (elapsed > r->duration_ms) elapsed = r->duration_ms;
    int32_t x = r->x + ((int32_t)r->end_x - r->x) * (int32_t)elapsed / (int32_t)r->duration_ms;
    int32_t y = r->y + ((int32_t)r->end_y - r->y) * (int32_t)elapsed / (int32_t)r->duration_ms;
    pointer->neutral = raw_point(x, y);
    neutral_sample(pointer, data);
    if (phase == RELEASE) phase = FINISH;
    else {
        data->state = LV_INDEV_STATE_PRESSED;
        if (completed) phase = RELEASE;
    }
}

static void pointer_deleted(lv_event_t *event)
{
    lv_indev_t *input = lv_event_get_target(event);
    if (input != pointers[0].input) {
        for (unsigned i = 1; i < pointer_count; ++i)
            if (pointers[i].input == input) pointers[i].input = NULL;
        return;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    ready = false;
    rt_hw_interrupt_enable(level);
    while (peek()) {
        if (phase == IDLE) begin_log();
        finish("device_deleted");
    }
    for (unsigned i = 0; i < pointer_count; ++i)
        if (pointers[i].input == input) pointers[i].input = NULL;
}

bool epd_input_test_init(void)
{
    if (pointer_count) return ready;
    memset(pointers, 0, sizeof(pointers));
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER || !lv_indev_get_read_cb(input)) continue;
        if (pointer_count == POINTER_CAPACITY) { pointer_count = 0; return false; }
        pointers[pointer_count].input = input;
        pointers[pointer_count++].read = lv_indev_get_read_cb(input);
    }
    if (!pointer_count) return false;
    for (unsigned i = 0; i < pointer_count; ++i) {
        uint32_t events = lv_indev_get_event_count(pointers[i].input);
        lv_indev_add_event_cb(pointers[i].input, pointer_deleted, LV_EVENT_DELETE, NULL);
        if (lv_indev_get_event_count(pointers[i].input) != events + 1u) {
            for (unsigned j = 0; j < i; ++j)
                lv_indev_remove_event_cb_with_user_data(pointers[j].input, pointer_deleted, NULL);
            pointer_count = 0;
            return false;
        }
    }
    for (unsigned i = 0; i < pointer_count; ++i)
        lv_indev_set_read_cb(pointers[i].input, pointer_read);
    rt_base_t level = rt_hw_interrupt_disable();
    ready = true;
    rt_hw_interrupt_enable(level);
    return true;
}

int epd_input_test_submit(const epd_input_test_request_t *request, uint32_t *id)
{
    if (!request || request->kind < EPD_INPUT_TEST_TAP || request->kind > EPD_INPUT_TEST_SWIPE ||
        request->duration_ms < 40 || request->duration_ms > 10000 ||
        request->end_hold_ms > 10000u - request->duration_ms ||
        (request->kind != EPD_INPUT_TEST_SWIPE &&
         (request->x != request->end_x || request->y != request->end_y || request->end_hold_ms)))
        return -RT_EINVAL;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!ready || count == EPD_INPUT_TEST_CAPACITY) {
        int result = ready ? -RT_EFULL : -RT_ENOSYS;
        rt_hw_interrupt_enable(level);
        return result;
    }
    queued_t *item = &queue[(head + count) % EPD_INPUT_TEST_CAPACITY];
    item->request = *request;
    if (!++next_id) ++next_id;
    item->id = next_id;
    if (id) *id = item->id;
    ++count;
    rt_hw_interrupt_enable(level);
    /* Use the existing power wake request; all subsequent input and page
     * callbacks still execute on the UI owner thread. */
    (void)epd_power_key_wakeup();
    epd_power_notify();
    return RT_EOK;
}

void epd_input_test_process_keys(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool blocked = count != 0;
    rt_hw_interrupt_enable(level);
    if (blocked || settling) epd_input_clear();
    else epd_input_process();
    settling = false;
}

#ifdef RT_USING_FINSH
#include <finsh.h>
static bool number(const char *text, uint32_t limit, uint32_t *value)
{
    if (!text || !*text) return false;
    uint32_t result = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (result > (limit - digit) / 10u) return false;
        result = result * 10u + digit;
    }
    *value = result;
    return true;
}

static int ui_pointer(int argc, char **argv)
{
    epd_input_test_request_t request = {0};
    uint32_t values[6] = {0, 0, 0, 0, 80, 0};
    bool swipe = argc > 1 && !strcmp(argv[1], "swipe");
    bool hold = argc > 1 && !strcmp(argv[1], "hold");
    bool tap = argc > 1 && !strcmp(argv[1], "tap");
    if ((!swipe && !hold && !tap) || (swipe && argc != 7 && argc != 8) ||
        (hold && argc != 5) || (tap && argc != 4 && argc != 5)) goto invalid;
    for (int i = 2; i < argc; ++i)
        if (!number(argv[i], (swipe ? i >= 6 : i == 4) ? 10000u : UINT16_MAX, &values[i - 2])) goto invalid;
    request.kind = swipe ? EPD_INPUT_TEST_SWIPE : hold ? EPD_INPUT_TEST_HOLD : EPD_INPUT_TEST_TAP;
    request.x = (uint16_t)values[0]; request.y = (uint16_t)values[1];
    request.end_x = swipe ? (uint16_t)values[2] : request.x;
    request.end_y = swipe ? (uint16_t)values[3] : request.y;
    request.duration_ms = swipe ? values[4] : argc == 5 ? values[2] : 80u;
    request.end_hold_ms = swipe ? values[5] : 0;
    int result = epd_input_test_submit(&request, NULL);
    if (result != RT_EOK) rt_kprintf("ui_pointer reject error=%d\n", result);
    return result;
invalid:
    rt_kprintf("ui_pointer tap x y [ms] | hold x y ms | swipe x1 y1 x2 y2 ms [end_hold_ms]\n");
    return -RT_EINVAL;
}
MSH_CMD_EXPORT(ui_pointer, Inject a pointer request through the UI input reader);
#endif
#else
typedef int epd_input_test_disabled_t;
#endif
