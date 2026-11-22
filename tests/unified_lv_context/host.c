/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lvgl.h"
#include "src/misc/lv_execution_context.h"
#include "src/misc/lv_timer_private.h"
#include "src/core/lv_obj_private.h"
#include "src/core/lv_obj_class_private.h"
#include "src/layouts/lv_layout.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while(0)
static uint32_t pixels[160 * 120];
static lv_display_t * display;
static unsigned callbacks;
static void flush(lv_display_t * d, const lv_area_t * area, uint8_t * data)
{ (void)area; (void)data; lv_display_flush_ready(d); }

#if LV_USE_EXECUTION_CONTEXT
static uint32_t current, refs[16], previous_stack[64];
static unsigned enters, leaves, depth;
static bool fail_enter;
static bool closed[16];
static uint32_t parents[16];
static uint32_t captured_owner;
lv_execution_context_t lv_execution_context_capture(void) { return current == 12 && captured_owner ? captured_owner : current; }
bool lv_execution_context_is_open(lv_execution_context_t t)
{
    if(t >= 16) return false;
    while(t) { if(closed[t]) return false; t = parents[t]; }
    return true;
}
bool lv_execution_context_matches(lv_execution_context_t t, lv_execution_context_t root)
{
    if(t >= 16 || root >= 16) return false;
    do { if(t == root) return true; t = parents[t]; } while(t);
    return false;
}
bool lv_execution_context_retain(lv_execution_context_t t)
{ if(t >= 16) return false; if(t) refs[t]++; return true; }
void lv_execution_context_release(lv_execution_context_t t)
{ CHECK(t < 16); if(t) { CHECK(refs[t]); refs[t]--; } }
lv_execution_context_t lv_execution_context_enter(lv_execution_context_t t)
{
    CHECK(t < 16 && (!t || refs[t]));
    CHECK(current < 16);
    if(fail_enter) return LV_EXECUTION_CONTEXT_INVALID;
    CHECK(lv_execution_context_retain(current));
    CHECK(depth < 64); previous_stack[depth++] = current;
    uint32_t previous = current; current = t; enters++; return previous;
}
void lv_execution_context_leave(lv_execution_context_t t)
{
    CHECK(depth && previous_stack[--depth] == t);
    CHECK(!t || refs[t]); CHECK(!current || refs[current]);
    current = t; leaves++;
    lv_execution_context_release(t);
}
static void select_owner(uint32_t t) { CHECK(depth == 0); current = t; }
static void in_owner(uint32_t t) { CHECK(current == t); CHECK(!t || refs[t]); callbacks++; }
static void no_refs(void)
{
    CHECK(!depth && enters == leaves);
    for(unsigned i = 1; i < 16; i++) {
        if(refs[i]) fprintf(stderr, "owner %u refs %u\n", i, refs[i]);
        CHECK(refs[i] == 0);
    }
}
static void noop_event(lv_event_t * e) { (void)e; }
static void nested_delete(lv_event_t * e)
{
    in_owner(3); lv_obj_delete(lv_event_get_target(e)); in_owner(3);
}
static void grow_and_nest(lv_event_t * e)
{
    in_owner(2); CHECK(lv_event_get_user_data(e) == (void *)(uintptr_t)0x1234);
    lv_obj_t * obj = lv_event_get_target(e);
    for(unsigned i = 0; i < 48; i++) CHECK(lv_obj_add_event_cb(obj, noop_event, LV_EVENT_REFRESH, NULL));
    CHECK(lv_obj_remove_event_cb(obj, grow_and_nest));
    CHECK(lv_obj_send_event(obj, LV_EVENT_READY, NULL) == LV_RESULT_INVALID);
    in_owner(2);
}
static void test_events(void)
{
    select_owner(1); lv_obj_t * obj = lv_obj_create(lv_screen_active()); CHECK(obj);
    select_owner(2); CHECK(lv_obj_add_event_cb(obj, grow_and_nest, LV_EVENT_CLICKED, (void *)(uintptr_t)0x1234));
    select_owner(3); CHECK(lv_obj_add_event_cb(obj, nested_delete, LV_EVENT_READY, NULL));
    select_owner(7); CHECK(lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL) == LV_RESULT_INVALID);
    CHECK(current == 7); no_refs(); puts("events: nesting, growth and target deletion PASS");
}
static void timer_delete_self(lv_timer_t * t)
{ in_owner(2); CHECK(lv_timer_get_user_data(t) == (void *)(uintptr_t)0x3456); lv_timer_delete(t); in_owner(2); }
static void timer_once(lv_timer_t * t) { (void)t; in_owner(3); }
static void async_run(void * p) { CHECK(p == (void *)(uintptr_t)0x789); in_owner(4); }
static void resume(void * p)
{ in_owner(5); CHECK(p == (void *)(uintptr_t)0x456); lv_timer_handler_set_resume_cb(NULL, NULL); in_owner(5); }
static void pump(void) { lv_tick_inc(10); lv_timer_handler(); CHECK(current == 7); }
static void test_timers(void)
{
    select_owner(2); CHECK(lv_timer_create(timer_delete_self, 1, (void *)(uintptr_t)0x3456));
    select_owner(7); pump(); no_refs();
    select_owner(2); lv_timer_t * t = lv_timer_create(NULL, 1, NULL);
    select_owner(3); lv_timer_set_cb(t, timer_once); lv_timer_set_repeat_count(t, 1); CHECK(refs[2] == 0);
    select_owner(7); pump(); no_refs();
    select_owner(4); CHECK(lv_async_call(async_run, (void *)(uintptr_t)0x789) == LV_RESULT_OK);
    select_owner(7); pump(); no_refs();
    select_owner(4); CHECK(lv_async_call(async_run, (void *)(uintptr_t)0x789) == LV_RESULT_OK);
    select_owner(7); CHECK(lv_async_call_cancel(async_run, (void *)(uintptr_t)0x789) == LV_RESULT_OK); no_refs();
    select_owner(5); lv_timer_handler_set_resume_cb(resume, (void *)(uintptr_t)0x456);
    select_owner(7); lv_timer_enable(true); no_refs();
    puts("timers: delete, replace, async, cancel and resume PASS");
}
static unsigned constructed, destroyed, drawn, laid_out;
static void constructor(const lv_obj_class_t * c, lv_obj_t * obj)
{ (void)c; (void)obj; in_owner(1); constructed++; }
static void destructor(const lv_obj_class_t * c, lv_obj_t * obj)
{ (void)c; (void)obj; in_owner(1); destroyed++; }
static void class_event(const lv_obj_class_t * c, lv_event_t * e)
{
    in_owner(1); if(lv_obj_event_base(c, e) != LV_RESULT_OK) return;
    if(lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) drawn++;
}
static const lv_obj_class_t owned_class = {
    .base_class = &lv_obj_class, .instance_size = sizeof(lv_obj_t),
    .constructor_cb = constructor, .destructor_cb = destructor, .event_cb = class_event
};
static void layout(lv_obj_t * obj, void * data)
{
    (void)obj; CHECK(data == (void *)(uintptr_t)0x111); in_owner(1); laid_out++;
    lv_timer_t * t = lv_timer_create(NULL, 100, NULL); CHECK(t && t->execution_context == 1); lv_timer_delete(t);
}
static void test_objects(void)
{
    select_owner(1); lv_obj_t * obj = lv_obj_class_create_obj(&owned_class, lv_screen_active()); CHECK(obj);
    select_owner(7); lv_obj_class_init_obj(obj); CHECK(constructed == 1);
    lv_obj_set_size(obj, 70, 50);
    select_owner(1); uint32_t id = lv_layout_register(layout, (void *)(uintptr_t)0x111);
    lv_obj_set_layout(obj, id); CHECK(lv_obj_create(obj));
    select_owner(7); lv_obj_update_layout(obj); lv_refr_now(display);
    CHECK(laid_out && drawn && current == 7);
    lv_obj_delete(obj); CHECK(destroyed == 1); no_refs();
    puts("objects: class, layout, draw and destructor PASS");
}
static unsigned anim_mode, anim_deleted, anim_completed;
static int anim_target;
static void anim_maybe_delete(unsigned mode)
{
    in_owner(3);
    if(anim_mode == mode) { lv_anim_delete(&anim_target, NULL); in_owner(3); }
}
static int32_t anim_get(lv_anim_t * a) { (void)a; anim_maybe_delete(1); return 0; }
static int32_t anim_path(const lv_anim_t * a) { (void)a; anim_maybe_delete(2); return 10; }
static void anim_begin(lv_anim_t * a) { (void)a; anim_maybe_delete(3); }
static void anim_exec(void * p, int32_t value) { (void)p; (void)value; anim_maybe_delete(4); }
static void anim_custom(lv_anim_t * a, int32_t v) { (void)a; (void)v; anim_maybe_delete(5); }
static void anim_done(lv_anim_t * a) { (void)a; anim_completed++; anim_maybe_delete(6); }
static void anim_del(lv_anim_t * a)
{ (void)a; in_owner(3); anim_deleted++; if(anim_mode == 7) lv_anim_delete_all(); in_owner(3); }
static void test_animations(void)
{
    for(unsigned early = 0; early < 2; early++) for(anim_mode = 0; anim_mode < 8; anim_mode++) {
        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, &anim_target);
        lv_anim_set_duration(&a, 1); lv_anim_set_early_apply(&a, early != 0);
        lv_anim_set_get_value_cb(&a, anim_get); lv_anim_set_path_cb(&a, anim_path);
        lv_anim_set_start_cb(&a, anim_begin); lv_anim_set_exec_cb(&a, anim_exec);
        lv_anim_set_custom_exec_cb(&a, anim_custom); lv_anim_set_completed_cb(&a, anim_done);
        lv_anim_set_deleted_cb(&a, anim_del);
        unsigned before = anim_deleted; select_owner(3); lv_anim_start(&a);
        select_owner(7); lv_tick_inc(10); lv_anim_refr_now(); lv_anim_delete_all();
        CHECK(anim_deleted == before + 1); CHECK(lv_anim_count_running() == 0); no_refs();
    }
    CHECK(anim_completed); puts("animations: all callback kinds, self deletion, early/delayed PASS");
}
static lv_anim_timeline_t * timeline;
static bool timeline_append, timeline_delete;
static unsigned timeline_calls;
static void timeline_exec(void * p, int32_t value)
{
    (void)value; uint32_t owner = (uint32_t)(uintptr_t)p; in_owner(owner); timeline_calls++;
    if(timeline_append) {
        timeline_append = false;
        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, p); lv_anim_set_exec_cb(&a, timeline_exec);
        lv_anim_timeline_add(timeline, 0, &a);
    }
    if(timeline_delete) { timeline_delete = false; lv_anim_timeline_delete(timeline); in_owner(owner); }
}
static void add_timeline_entry(lv_anim_timeline_t * t, uint32_t owner)
{
    select_owner(owner); lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, (void *)(uintptr_t)owner);
    lv_anim_set_exec_cb(&a, timeline_exec); lv_anim_timeline_add(t, 0, &a);
}
static void test_timelines(void)
{
    select_owner(1); lv_anim_timeline_t * src = lv_anim_timeline_create(); timeline = lv_anim_timeline_create();
    add_timeline_entry(src, 2); add_timeline_entry(src, 3);
    select_owner(7); lv_anim_timeline_merge(timeline, src, 0); lv_anim_timeline_delete(src);
    timeline_append = true; lv_anim_timeline_set_progress(timeline, LV_ANIM_TIMELINE_PROGRESS_MAX / 2);
    CHECK(timeline_calls >= 2); timeline_delete = true;
    lv_anim_timeline_set_progress(timeline, LV_ANIM_TIMELINE_PROGRESS_MAX); no_refs();
    select_owner(1); timeline = lv_anim_timeline_create(); add_timeline_entry(timeline, 4);
    select_owner(7); timeline_delete = true; lv_anim_timeline_start(timeline);
    CHECK(lv_anim_count_running() == 0); no_refs();
    puts("timelines: merged owners, append and self deletion PASS");
}
static void group_focus(lv_group_t * g) { in_owner(5); lv_group_delete(g); in_owner(5); }
static void group_edge(lv_group_t * g, bool forward)
{ (void)forward; in_owner(6); lv_group_set_edge_cb(g, NULL); in_owner(6); }
static void test_groups(void)
{
    select_owner(0); lv_obj_t * a = lv_obj_create(lv_screen_active()), * b = lv_obj_create(lv_screen_active());
    lv_group_t * g = lv_group_create(); lv_group_add_obj(g, a); lv_group_add_obj(g, b);
    select_owner(5); lv_group_set_focus_cb(g, group_focus);
    select_owner(7); lv_group_focus_obj(b); no_refs();
    select_owner(0); g = lv_group_create(); lv_group_add_obj(g, a); lv_group_set_wrap(g, false);
    select_owner(6); lv_group_set_edge_cb(g, group_edge);
    select_owner(7); lv_group_focus_next(g); lv_group_delete(g); lv_obj_delete(a); lv_obj_delete(b); no_refs();
    puts("groups: callback replacement and self deletion PASS");
}
static void test_failure(void)
{
    select_owner(99); CHECK(lv_obj_class_create_obj(&lv_obj_class, lv_screen_active()) == NULL);
    CHECK(lv_timer_create(NULL, 1, NULL) == NULL);
    CHECK(lv_async_call(async_run, NULL) == LV_RESULT_INVALID);
    lv_anim_t a; lv_anim_init(&a); CHECK(lv_anim_start(&a) == NULL);
    CHECK(lv_anim_timeline_create() == NULL);
    CHECK(lv_group_create() == NULL);
    CHECK(lv_obj_add_event_cb(lv_screen_active(), noop_event, LV_EVENT_CLICKED, NULL) == NULL);
    select_owner(7); lv_execution_context_guard_t guard; fail_enter = true;
    CHECK(!lv_execution_context_guard_enter(&guard, 2)); CHECK(current == 7);
    lv_execution_context_guard_leave(&guard); fail_enter = false; no_refs();
    puts("invalid capture and enter failure PASS");
}

static unsigned closed_deletes, closed_destructs, closed_anim_deletes;
typedef struct { lv_obj_t obj; lv_timer_t * timer; } close_widget_t;
static void forbidden_event(lv_event_t * e) { (void)e; CHECK(false); }
static void forbidden_timer(lv_timer_t * t) { (void)t; CHECK(false); }
static void forbidden_async(void * p) { (void)p; CHECK(false); }
static void forbidden_exec(void * p, int32_t v) { (void)p; (void)v; CHECK(false); }
static void forbidden_focus(lv_group_t * g) { (void)g; CHECK(false); }
static void forbidden_edge(lv_group_t * g, bool f) { (void)g; (void)f; CHECK(false); }
static void closed_delete(lv_event_t * e)
{
    (void)e; in_owner(2); CHECK(!lv_execution_context_is_open(current)); closed_deletes++;
    CHECK(lv_timer_create(NULL, 1, NULL) == NULL);
    CHECK(lv_async_call(forbidden_async, NULL) == LV_RESULT_INVALID);
    CHECK(lv_obj_add_event_cb(lv_screen_active(), forbidden_event, LV_EVENT_CLICKED, NULL) == NULL);
}
static void closed_destructor(const lv_obj_class_t * c, lv_obj_t * obj)
{
    (void)c; in_owner(2); closed_destructs++;
    lv_timer_delete(((close_widget_t *)obj)->timer);
}
static void closed_anim_deleted(lv_anim_t * a)
{
    (void)a; in_owner(3); closed_anim_deletes++;
    lv_anim_t next; lv_anim_init(&next); CHECK(lv_anim_start(&next) == NULL);
}
static const lv_obj_class_t close_widget_class = {
    .base_class = &lv_obj_class, .instance_size = sizeof(close_widget_t), .destructor_cb = closed_destructor
};
static void test_closed_tree(void)
{
    parents[2] = 1; parents[3] = 2; parents[4] = 1;
    select_owner(0);
    lv_obj_t * shared = lv_obj_create(lv_screen_active());
    lv_group_t * shared_group = lv_group_create();
    lv_group_add_obj(shared_group, shared); lv_group_set_wrap(shared_group, false);
    lv_anim_timeline_t * shared_timeline = lv_anim_timeline_create();
    select_owner(2);
    close_widget_t * widget = (close_widget_t *)lv_obj_class_create_obj(&close_widget_class, lv_screen_active());
    lv_obj_class_init_obj(&widget->obj);
    widget->timer = lv_timer_create(forbidden_timer, 1, NULL);
    CHECK(lv_obj_add_event_cb(&widget->obj, closed_delete, LV_EVENT_DELETE, NULL));
    CHECK(lv_obj_add_event_cb(shared, forbidden_event, LV_EVENT_CLICKED, NULL));
    CHECK(lv_obj_add_event_cb(&widget->obj, forbidden_event, LV_EVENT_CLICKED, NULL));
    lv_group_t * owned_group = lv_group_create(); lv_group_add_obj(owned_group, &widget->obj);
    lv_group_set_focus_cb(shared_group, forbidden_focus); lv_group_set_edge_cb(shared_group, forbidden_edge);
    select_owner(3);
    CHECK(lv_async_call(forbidden_async, NULL) == LV_RESULT_OK);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, (void *)0x100);
    lv_anim_set_early_apply(&a, false); lv_anim_set_exec_cb(&a, forbidden_exec);
    lv_anim_set_deleted_cb(&a, closed_anim_deleted); CHECK(lv_anim_start(&a));
    lv_anim_set_deleted_cb(&a, NULL); lv_anim_timeline_add(shared_timeline, 0, &a);
    select_owner(4); lv_anim_timeline_t * owned_timeline = lv_anim_timeline_create();
    lv_anim_timeline_add(owned_timeline, 0, &a);
    select_owner(8); lv_obj_t * survivor = lv_obj_create(lv_screen_active());
    lv_timer_t * survivor_timer = lv_timer_create(NULL, 1000, NULL);
    add_timeline_entry(shared_timeline, 8);
    closed[1] = true;
    select_owner(7);
    CHECK(lv_obj_send_event(shared, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    CHECK(lv_obj_send_event(&widget->obj, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    lv_group_focus_next(shared_group);
    pump(); lv_anim_refr_now();
    unsigned before = timeline_calls;
    lv_anim_timeline_set_progress(shared_timeline, LV_ANIM_TIMELINE_PROGRESS_MAX / 2);
    CHECK(timeline_calls == before + 1);
    lv_execution_context_cancel(1);
    CHECK(closed_deletes == 1 && closed_destructs == 1 && closed_anim_deletes == 1);
    CHECK(lv_obj_is_valid(survivor) && lv_obj_is_valid(shared));
    CHECK(lv_obj_get_event_count(shared) == 0);
    CHECK(lv_group_get_count() == 1 && !lv_group_get_focus_cb(shared_group) && !lv_group_get_edge_cb(shared_group));
    for(unsigned i = 1; i <= 4; i++) CHECK(refs[i] == 0);
    CHECK(refs[8] > 0);
    lv_execution_context_cancel(1); /*Repeated page/root cancellation is harmless.*/
    lv_anim_timeline_set_progress(shared_timeline, LV_ANIM_TIMELINE_PROGRESS_MAX);
    CHECK(timeline_calls == before + 2);
    lv_anim_timeline_delete(shared_timeline); lv_timer_delete(survivor_timer);
    lv_group_delete(shared_group); lv_obj_delete(shared); lv_obj_delete(survivor);
    closed[1] = false; parents[2] = parents[3] = parents[4] = 0;
    no_refs(); puts("closed tree: dispatch blocked, destructors allowed, descendants drained, unrelated owners preserved PASS");
}
static unsigned close_stage, close_hits;
static void close_step(unsigned stage)
{
    in_owner(3); CHECK(lv_execution_context_is_open(3));
    if(close_stage == stage) { closed[1] = true; close_hits++; }
}
static int32_t close_get(lv_anim_t * a) { (void)a; close_step(1); return 0; }
static int32_t close_path(const lv_anim_t * a) { close_step(2); return a->act_time + 10; }
static void close_start(lv_anim_t * a) { (void)a; close_step(3); }
static void close_exec(void * p, int32_t v) { (void)p; (void)v; close_step(4); }
static void close_custom(lv_anim_t * a, int32_t v) { (void)a; (void)v; close_step(5); }
static void close_complete(lv_anim_t * a) { (void)a; close_step(6); }
static void close_deleted(lv_anim_t * a) { (void)a; in_owner(3); }
static void close_animation(lv_anim_t * a)
{
    lv_anim_init(a); lv_anim_set_duration(a, 1);
    lv_anim_set_get_value_cb(a, close_get); lv_anim_set_path_cb(a, close_path);
    lv_anim_set_start_cb(a, close_start); lv_anim_set_exec_cb(a, close_exec);
    lv_anim_set_custom_exec_cb(a, close_custom); lv_anim_set_completed_cb(a, close_complete);
    lv_anim_set_deleted_cb(a, close_deleted);
}
static void test_close_in_callback(void)
{
    parents[3] = 1;
    for(unsigned early = 0; early < 2; early++) for(close_stage = 1; close_stage <= 6; close_stage++) {
        close_hits = 0; select_owner(3);
        lv_anim_t a; close_animation(&a); lv_anim_set_early_apply(&a, early != 0); lv_anim_start(&a);
        select_owner(7); lv_tick_inc(10); lv_anim_refr_now();
        CHECK(close_hits == 1); lv_execution_context_cancel(1); no_refs(); closed[1] = false;
    }
    for(close_stage = 2; close_stage <= 6; close_stage++) {
        close_hits = 0; select_owner(0); lv_anim_timeline_t * at = lv_anim_timeline_create();
        select_owner(3); lv_anim_t a; close_animation(&a); lv_anim_timeline_add(at, 0, &a);
        select_owner(7); lv_anim_timeline_start(at); lv_tick_inc(10); lv_anim_refr_now();
        CHECK(close_hits == 1); lv_execution_context_cancel(1); lv_anim_timeline_delete(at);
        no_refs(); closed[1] = false;
    }
    parents[3] = 0;
    puts("closing an ancestor inside animation/timeline callbacks blocks subsequent ordinary callbacks PASS");
}
static void test_teardown_enter_failure(void)
{
    select_owner(2); lv_obj_t * obj = lv_obj_create(lv_screen_active());
    CHECK(lv_obj_add_event_cb(obj, forbidden_event, LV_EVENT_CLICKED, NULL));
    lv_group_t * group = lv_group_create(); lv_group_add_obj(group, obj);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_early_apply(&a, false);
    lv_anim_set_exec_cb(&a, forbidden_exec); CHECK(lv_anim_start(&a));
    select_owner(7); fail_enter = true;
    CHECK(lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL) == LV_RESULT_INVALID);
    lv_obj_delete(obj); CHECK(lv_obj_is_valid(obj)); CHECK(refs[2] > 0);
    lv_group_delete(group); CHECK(lv_group_get_count() == 0);
    lv_anim_delete_all(); CHECK(lv_anim_count_running() == 0);
    fail_enter = false; lv_obj_delete(obj); no_refs();
    puts("enter failure: no foreign callback, live object retained, group/animation teardown terminates PASS");
}
static void test_previous_execution_token(void)
{
    select_owner(12); captured_owner = 4;
    lv_obj_t * obj = lv_obj_create(lv_screen_active());
    CHECK(obj && obj->execution_context == 4 && current == 12);
    lv_execution_context_guard_t outer, inner;
    CHECK(lv_execution_context_guard_enter(&outer, 2));
    CHECK(outer.previous == 12 && refs[12] && current == 2);
    CHECK(lv_execution_context_guard_enter(&inner, 3));
    CHECK(inner.previous == 2 && refs[2] && current == 3);
    lv_execution_context_guard_leave(&inner); CHECK(current == 2 && refs[12]);
    lv_execution_context_guard_leave(&outer); CHECK(current == 12 && refs[12] == 0);
    lv_obj_delete(obj); CHECK(current == 12); captured_owner = 0;
    no_refs(); puts("capture owner differs from exact previous execution token: nested restore PASS");
}
static unsigned identity_hits[16];
static void same_event(lv_event_t * e)
{
    uint32_t owner = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    in_owner(owner); identity_hits[owner]++;
    lv_obj_t * temporary = lv_obj_create(lv_screen_active());
    CHECK(temporary && temporary->execution_context == owner);
    lv_obj_delete(temporary);
}
static void same_timer(lv_timer_t * t)
{
    uint32_t owner = (uint32_t)(uintptr_t)lv_timer_get_user_data(t);
    in_owner(owner); identity_hits[owner]++;
    lv_timer_delete(t); in_owner(owner);
}
static void same_async(void * data)
{
    uint32_t owner = (uint32_t)(uintptr_t)data;
    in_owner(owner); identity_hits[owner]++;
    lv_timer_t * temporary = lv_timer_create(NULL, 100, NULL);
    CHECK(temporary && temporary->execution_context == owner);
    lv_timer_delete(temporary);
}
static void test_registration_identity(void)
{
    select_owner(0); lv_obj_t * shared = lv_obj_create(lv_screen_active());
    for(uint32_t owner = 2; owner <= 4; owner++) {
        select_owner(owner);
        CHECK(lv_obj_add_event_cb(shared, same_event, LV_EVENT_CLICKED, (void *)(uintptr_t)owner));
        CHECK(lv_timer_create(same_timer, 1, (void *)(uintptr_t)owner));
        CHECK(lv_async_call(same_async, (void *)(uintptr_t)owner) == LV_RESULT_OK);
    }
    select_owner(7); CHECK(lv_obj_send_event(shared, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    pump(); CHECK(current == 7);
    for(uint32_t owner = 2; owner <= 4; owner++) CHECK(identity_hits[owner] == 3);
    closed[3] = true;
    lv_execution_context_cancel(3);
    CHECK(lv_obj_get_event_count(shared) == 2 && refs[3] == 0);
    CHECK(lv_obj_send_event(shared, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    CHECK(identity_hits[2] == 4 && identity_hits[3] == 3 && identity_hits[4] == 4);
    closed[3] = false;
    lv_obj_delete(shared); no_refs();
    puts("same callback address: independent owners, deferred child ownership and selective cancellation PASS");
}
static void prepare_deinit(void)
{
    select_owner(4); CHECK(lv_async_call(async_run, (void *)(uintptr_t)0x789) == LV_RESULT_OK);
    CHECK(lv_timer_create(NULL, 100, NULL));
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_early_apply(&a, false); CHECK(lv_anim_start(&a));
    select_owner(0);
}
#else
static void event_off(lv_event_t * e) { callbacks++; lv_obj_delete(lv_event_get_target(e)); }
static void timer_off(lv_timer_t * t) { callbacks++; lv_timer_delete(t); }
#endif
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); lv_init(); display = lv_display_create(160, 120); CHECK(display);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
#if LV_USE_EXECUTION_CONTEXT
    test_events(); test_timers(); test_objects(); test_animations(); test_timelines(); test_groups(); test_failure();
    test_closed_tree(); test_close_in_callback(); test_teardown_enter_failure(); test_previous_execution_token();
    test_registration_identity();
    prepare_deinit(); lv_deinit(); no_refs();
    printf("context ON: %u callback checks, %u paired guards PASS\n", callbacks, enters);
#else
    lv_obj_t * obj = lv_obj_create(lv_screen_active()); lv_obj_add_event_cb(obj, event_off, LV_EVENT_CLICKED, NULL);
    CHECK(lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL) == LV_RESULT_INVALID);
    CHECK(lv_timer_create(timer_off, 1, NULL)); lv_tick_inc(10); lv_timer_handler(); CHECK(callbacks == 2);
    lv_deinit(); puts("context default OFF: no bridge symbols, callback behavior PASS");
#endif
    return 0;
}
