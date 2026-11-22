/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lvgl.h"
#include "src/misc/lv_execution_context.h"
#include "ui/platform/epd_input.h"
#include "ui/platform/epd_app.h"
#include "platform/epd_memory_manager.h"
#include "boards/controls/buttons.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum { UI_SETTING_TOUCH };
static bool epd_keyboard_back(lv_obj_t *owner);
static bool ui_navigation_cancel(void) { return false; }
static bool ui_shell_locked(void) { return false; }
static void ui_shell_lock(void) {}
static void ui_shell_power_menu(void) {}
static void frontlight_wake(void) {}
static bool ui_settings_enabled(int setting) { (void)setting; return true; }
static bool epd_power_sleeping(void) { return false; }
static bool epd_power_key_wakeup(void) { return false; }
static void epd_power_notify(void) {}
static void epd_power_activity(void) {}
static void epd_refresh_request_interaction(void) {}
static void gui_app_exec_now(void) {}
static void gui_app_goback(void) {}
static rt_tick_t rt_tick_get(void) { return lv_tick_get(); }
static rt_tick_t rt_tick_from_millisecond(int ms) { return (rt_tick_t)ms; }
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
void buttons_init(buttons_callback_t callback) { (void)callback; }
bool buttons_pressed(void) { return false; }

static uint32_t current, refs[16], parents[16];
static uint32_t captured_owner;
static bool closed[16], fail_enter, close_on_restore;
static unsigned depth, callbacks;
static uint32_t stack[128];
epd_memory_owner_t epd_memory_owner_current(void) { return current == 12 && captured_owner ? captured_owner : current; }
bool epd_memory_owner_retain(epd_memory_owner_t owner)
{
    if (owner >= 16 || (owner && !refs[owner])) return false;
    if (owner) ++refs[owner];
    return true;
}
void epd_memory_owner_release(epd_memory_owner_t owner)
{
    CHECK(owner < 16);
    if (owner) { CHECK(refs[owner] > 0); --refs[owner]; }
}
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner)
{
    if (fail_enter || owner >= 16 || (owner && !refs[owner])) return EPD_MEMORY_OWNER_INVALID;
    CHECK(epd_memory_owner_retain(current));
    CHECK(depth < 128);
    stack[depth++] = current;
    uint32_t previous = current;
    current = owner;
    return previous;
}
void epd_memory_owner_leave(epd_memory_owner_t previous)
{
    CHECK(depth && stack[--depth] == previous);
    CHECK(!previous || refs[previous]);
    current = previous;
    epd_memory_owner_release(previous);
    if (close_on_restore && previous == 2) {
        close_on_restore = false;
        closed[1] = true;
    }
}
bool epd_memory_owner_get(epd_memory_owner_t owner, epd_memory_owner_snapshot_t *out)
{
    if (owner >= 16 || (owner && !refs[owner])) return false;
    *out = (epd_memory_owner_snapshot_t){0};
    out->owner = owner; out->parent = parents[owner];
    out->closed = closed[owner]; out->references = refs[owner];
    return true;
}

/* Compile the complete production input implementation; only hardware includes
 * are replaced. All LVGL objects, events and groups use the real host library. */
#include "input_impl.inc"

#if LV_USE_EXECUTION_CONTEXT
lv_execution_context_t lv_execution_context_capture(void) { return epd_memory_owner_current(); }
bool lv_execution_context_is_open(lv_execution_context_t t) { return context_open(t); }
bool lv_execution_context_matches(lv_execution_context_t t, lv_execution_context_t root)
{
    if (t >= 16 || root >= 16) return false;
    do { if (t == root) return true; t = parents[t]; } while (t);
    return false;
}
bool lv_execution_context_retain(lv_execution_context_t t) { return epd_memory_owner_retain(t); }
void lv_execution_context_release(lv_execution_context_t t) { epd_memory_owner_release(t); }
lv_execution_context_t lv_execution_context_enter(lv_execution_context_t t) { return epd_memory_owner_enter(t); }
void lv_execution_context_leave(lv_execution_context_t t) { epd_memory_owner_leave(t); }
#endif

static bool keyboard_deletes;
static bool epd_keyboard_back(lv_obj_t *owner)
{
    if (keyboard_deletes) { keyboard_deletes = false; lv_obj_delete(owner); }
    return false;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{ (void)area; (void)data; lv_display_flush_ready(display); }
static uint32_t pixels[160 * 120];
static void check_balanced(void)
{
    CHECK(!scopes && !press_scope && !depth && current == 0);
    for (unsigned i = 1; i < 16; ++i) {
        if (refs[i] != 1) fprintf(stderr, "owner %u refs=%u\n", i, refs[i]);
        CHECK(refs[i] == 1); closed[i] = false;
    }
}
static void dispatch(lv_obj_t *object)
{
    press_scope = scope_for_owner(object); held_key = BUTTONS_KEY2;
    dispatch_application(EPD_APP_KEY_CLICKED);
}
static void unlisten_self(epd_app_key_t key, epd_app_key_event_t event, void *data)
{
    CHECK(current == 2 && key == EPD_APP_KEY2 && event == EPD_APP_KEY_CLICKED);
    epd_app_input_unlisten(data);
    CHECK(refs[2] > 1); ++callbacks;
}
static void close_self(epd_app_key_t key, epd_app_key_event_t event, void *data)
{
    (void)key; (void)event; (void)data;
    CHECK(current == 2); closed[1] = true;
    epd_input_cleanup_closed(); CHECK(!scopes && refs[2] > 1); ++callbacks;
}
static void noop(epd_app_key_t key, epd_app_key_event_t event, void *data)
{ (void)key; (void)event; (void)data; ++callbacks; }
static bool ops_key(uint32_t key) { CHECK(key == LV_KEY_ESC); ++callbacks; return false; }
static const epd_input_ops_t plain_ops = {NULL, NULL, ops_key};
static lv_group_t *test_group;
static bool replace_ops(uint32_t key)
{
    CHECK(key == LV_KEY_ESC && current == 3);
    epd_input_set_ops(test_group, &plain_ops);
    CHECK(refs[3] > 1); ++callbacks; return true;
}
static const epd_input_ops_t replacing_ops = {replace_ops, NULL, ops_key};

int main(void)
{
    for (unsigned i = 1; i < 16; ++i) refs[i] = 1;
    parents[2] = parents[3] = 1;
    lv_init(); lv_display_t *display = lv_display_create(160, 120);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, pixels, NULL, sizeof pixels, LV_DISPLAY_RENDER_MODE_FULL);
    lv_obj_t *object = lv_obj_create(lv_screen_active());
    current = 2; CHECK(epd_app_input_listen(object, unlisten_self, object));
    current = 0; dispatch(object); CHECK(callbacks == 1);
    lv_obj_delete(object); check_balanced();

    object = lv_obj_create(lv_screen_active());
    current = 2; CHECK(epd_app_input_listen(object, close_self, NULL));
    current = 0; dispatch(object); CHECK(callbacks == 2);
    CHECK(lv_obj_is_valid(object)); lv_obj_delete(object); check_balanced();

    /* Root closure also removes descendant callbacks on a system-owned scope. */
    object = lv_obj_create(lv_screen_active());
    test_group = epd_app_input_group(object);
    current = 2; CHECK(epd_app_input_listen(object, noop, NULL));
    current = 3; epd_input_set_ops(test_group, &plain_ops);
    current = 0; closed[1] = true; dispatch(object);
    epd_input_cleanup_closed();
    CHECK(callbacks == 2 && !scope_for_owner(object)->callback && !scope_for_owner(object)->ops);
    CHECK(refs[2] == 1 && refs[3] == 1);
    lv_obj_delete(object); check_balanced();

    object = lv_obj_create(lv_screen_active());
    current = 2; CHECK(epd_app_input_listen(object, noop, NULL));
    lv_obj_t *back = lv_obj_create(object); epd_app_input_back(object, back);
    current = 0; closed[1] = true;
#if LV_USE_EXECUTION_CONTEXT
    lv_execution_context_cancel(1);
    CHECK(scope_for_owner(object)->back == NULL);
#else
    lv_obj_delete(back);
#endif
    epd_input_cleanup_closed(); CHECK(!scopes && refs[2] == 1);
    epd_input_cleanup_closed(); lv_obj_delete(object); check_balanced();

    /* A callback replacement cancels the rest of the same back gesture. */
    object = lv_obj_create(lv_screen_active());
    test_group = epd_app_input_group(object);
    current = 3; epd_input_set_ops(test_group, &replacing_ops);
    current = 0; epd_input_back(); CHECK(callbacks == 3);
    keyboard_deletes = true; epd_input_back(); CHECK(callbacks == 3);
    check_balanced();

    object = lv_obj_create(lv_screen_active());
    current = 2; fail_enter = true;
    CHECK(!epd_app_input_group(object)); CHECK(refs[2] == 1);
    fail_enter = false; CHECK(epd_app_input_listen(object, noop, NULL));
    current = 0; fail_enter = true; dispatch(object);
    fail_enter = false; CHECK(callbacks == 3); lv_obj_delete(object); check_balanced();

    /* Closure during registration must not return the group reclaimed by publish. */
    object = lv_obj_create(lv_screen_active());
    current = 2; close_on_restore = true;
    CHECK(!epd_app_input_group(object));
    current = 0; CHECK(!scopes && refs[2] == 1);
    lv_obj_delete(object); check_balanced();

    object = lv_obj_create(lv_screen_active());
    current = 2; CHECK(epd_app_input_listen(object, noop, NULL));
    current = 0; physical_post(BUTTONS_KEY2, true); physical_post(BUTTONS_KEY2, false);
    closed[1] = true; epd_input_process(); CHECK(callbacks == 3 && !scopes);
    lv_obj_delete(object); check_balanced();
    object = lv_obj_create(lv_screen_active());
    current = 12; captured_owner = 2;
    CHECK(epd_app_input_listen(object, noop, NULL));
    CHECK(current == 12 && scope_for_owner(object)->scope_owner == 2);
    dispatch(object); CHECK(current == 12 && callbacks == 4);
    epd_app_input_unlisten(object); lv_obj_delete(object);
    CHECK(current == 12); captured_owner = 0; current = 0; check_balanced();
    lv_display_delete(display); lv_deinit(); check_balanced();
    printf("input: close, ancestors, self-unsubscribe, executing references, replacement, keyboard deletion, failed enter, queued keys PASS (LV context=%d)\n", LV_USE_EXECUTION_CONTEXT);
    return 0;
}
