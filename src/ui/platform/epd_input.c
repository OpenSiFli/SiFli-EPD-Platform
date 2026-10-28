#include "epd_input.h"
#include "epd_keyboard_internal.h"
#include "epd_app.h"
#include "epd_refresh.h"
#include "boards/controls/buttons.h"
#include "ui_shell.h"
#include "ui_settings.h"
#include "platform/epd_power.h"
#include "services/frontlight.h"
#include <rthw.h>
#include <rtm.h>

#define KEY_QUEUE_CAPACITY 32
#define KEY_HOLD_MS 3000
#define WAKE_RELEASE_MS 100
#define POINTER_READ_MS 8u

typedef struct input_scope
{
    struct input_scope *next;
    lv_obj_t *owner, *screen, *back;
    lv_group_t *group;
    const epd_input_ops_t *ops;
    epd_app_key_callback_t callback;
    void *callback_data;
    unsigned identity;
} input_scope_t;

static input_scope_t *scopes;
typedef struct
{
    buttons_key_t key;
    bool pressed;
    rt_tick_t tick;
    unsigned scope_identity;
} physical_event_t;
static physical_event_t key_queue[KEY_QUEUE_CAPACITY];
static unsigned key_head, key_count;
/* Queue, epoch and wake guard are shared with button/PM callbacks. */
static unsigned input_epoch, processed_epoch;
static bool consume_wake, release_observed;
static rt_tick_t released_at;
/* The active gesture and scope pointers belong only to the UI thread. */
static input_scope_t *press_scope;
static buttons_key_t held_key;
static rt_tick_t pressed_at;
static bool held, long_sent, press_locked;
static unsigned next_identity, published_identity;
static uint32_t pointer_read_at;
static bool pointer_initialized, pointer_present;
static lv_indev_read_cb_t muted_pointer_read;

static void pointer_read_muted(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* Drain the SDK queue while keeping disabled contacts out of LVGL. */
    if (muted_pointer_read) muted_pointer_read(indev, data);
    data->state = LV_INDEV_STATE_RELEASED;
}

bool epd_input_focusable(lv_obj_t *object)
{
    if (!object || lv_obj_has_state(object, LV_STATE_DISABLED)) return false;
    for (; object; object = lv_obj_get_parent(object))
        if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

static input_scope_t *scope_for_owner(lv_obj_t *owner)
{
    for (input_scope_t *scope = scopes; scope; scope = scope->next)
        if (scope->owner == owner) return scope;
    return NULL;
}

static bool object_above(lv_obj_t *left, lv_obj_t *right)
{
    lv_obj_t *a = left, *b = right;
    unsigned a_depth = 0, b_depth = 0;
    for (lv_obj_t *p = a; p; p = lv_obj_get_parent(p)) ++a_depth;
    for (lv_obj_t *p = b; p; p = lv_obj_get_parent(p)) ++b_depth;
    while (a_depth > b_depth) { a = lv_obj_get_parent(a); --a_depth; }
    while (b_depth > a_depth) { b = lv_obj_get_parent(b); --b_depth; }
    if (a == b) return a != left;
    while (lv_obj_get_parent(a) != lv_obj_get_parent(b))
    { a = lv_obj_get_parent(a); b = lv_obj_get_parent(b); }
    return lv_obj_get_index(a) > lv_obj_get_index(b);
}

static input_scope_t *active_scope(void)
{
    lv_obj_t *layers[] = {lv_layer_top(), lv_screen_active()};
    for (unsigned i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i)
    {
        input_scope_t *selected = NULL;
        for (input_scope_t *scope = scopes; scope; scope = scope->next)
        {
            if (scope->screen != layers[i] || !epd_input_focusable(scope->owner)) continue;
            if (!selected || object_above(scope->owner, selected->owner)) selected = scope;
        }
        if (selected) return selected;
    }
    return NULL;
}

static void publish_scope(void)
{
    input_scope_t *scope = active_scope();
    unsigned identity = scope ? scope->identity : 0;
    if (press_scope && press_scope != scope) press_scope = NULL;
    rt_base_t level = rt_hw_interrupt_disable();
    published_identity = identity;
    rt_hw_interrupt_enable(level);
}

static void back_deleted(lv_event_t *event)
{
    input_scope_t *scope = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) == scope->back) scope->back = NULL;
}

static void scope_deleted(lv_event_t *event)
{
    input_scope_t *scope = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) != scope->owner) return;
    input_scope_t **link = &scopes;
    while (*link != scope) link = &(*link)->next;
    *link = scope->next;
    publish_scope();
    if (scope->back) lv_obj_remove_event_cb_with_user_data(scope->back, back_deleted, scope);
    if (press_scope == scope) press_scope = NULL;
    lv_group_delete(scope->group);
    lv_free(scope);
}

lv_group_t *epd_app_input_group(lv_obj_t *owner)
{
    if (!owner) return NULL;
    input_scope_t *scope = scope_for_owner(owner);
    if (scope) return scope->group;
    scope = lv_malloc_zeroed(sizeof(*scope));
    LV_ASSERT_MALLOC(scope);
    if (!scope) return NULL;
    scope->group = lv_group_create();
    LV_ASSERT_MALLOC(scope->group);
    if (!scope->group)
    {
        lv_free(scope);
        return NULL;
    }
    scope->owner = owner;
    scope->identity = ++next_identity;
    if (!scope->identity) scope->identity = ++next_identity;
    scope->screen = lv_obj_get_screen(owner);
    scope->next = scopes;
    scopes = scope;
    lv_group_set_wrap(scope->group, true);
    lv_obj_add_event_cb(owner, scope_deleted, LV_EVENT_DELETE, scope);
    publish_scope();
    return scope->group;
}
RTM_EXPORT(epd_app_input_group);

bool epd_app_input_listen(lv_obj_t *owner, epd_app_key_callback_t callback, void *data)
{
    if (!owner || !callback || !epd_app_input_group(owner)) return false;
    input_scope_t *scope = scope_for_owner(owner);
    if (press_scope == scope) press_scope = NULL;
    scope->callback = callback;
    scope->callback_data = data;
    scope->identity = ++next_identity;
    if (!scope->identity) scope->identity = ++next_identity;
    publish_scope();
    return true;
}
RTM_EXPORT(epd_app_input_listen);

void epd_app_input_unlisten(lv_obj_t *owner)
{
    input_scope_t *scope = scope_for_owner(owner);
    if (!scope) return;
    if (press_scope == scope) press_scope = NULL;
    scope->callback = NULL;
    scope->callback_data = NULL;
    scope->identity = ++next_identity;
    if (!scope->identity) scope->identity = ++next_identity;
    publish_scope();
}
RTM_EXPORT(epd_app_input_unlisten);

void epd_input_add_object(lv_obj_t *object)
{
    for (lv_obj_t *owner = lv_obj_get_parent(object); owner; owner = lv_obj_get_parent(owner))
    {
        input_scope_t *scope = scope_for_owner(owner);
        if (scope)
        {
            lv_group_add_obj(scope->group, object);
            return;
        }
    }
    lv_group_add_obj(epd_app_input_group(lv_obj_get_screen(object)), object);
}

void epd_app_input_back(lv_obj_t *owner, lv_obj_t *button)
{
    input_scope_t *scope = scope_for_owner(owner);
    if (!scope) return;
    if (scope->back) lv_obj_remove_event_cb_with_user_data(scope->back, back_deleted, scope);
    scope->back = button;
    if (button) lv_obj_add_event_cb(button, back_deleted, LV_EVENT_DELETE, scope);
}
RTM_EXPORT(epd_app_input_back);

void epd_input_set_ops(lv_group_t *group, const epd_input_ops_t *ops)
{
    for (input_scope_t *scope = scopes; scope; scope = scope->next)
        if (scope->group == group)
        {
            scope->ops = ops;
            return;
        }
}

static void discard_queue(void)
{
    key_head = key_count = 0;
    ++input_epoch;
}

void epd_input_consume_wakeup(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    discard_queue();
    consume_wake = true;
    release_observed = false;
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

static void physical_post(buttons_key_t key, bool pressed)
{
    if (key < BUTTONS_KEY1 || key > BUTTONS_KEY3) return;
    /* PM arbitration and queue insertion share the interrupt boundary. */
    rt_base_t level = rt_hw_interrupt_disable();
    if (pressed && epd_power_key_wakeup())
    {
        rt_hw_interrupt_enable(level);
        return;
    }
    if (consume_wake)
    {
        /* SDK RELEASED is already debounced, including a wake whose PRESSED
         * was lost before ADC resumed. SDK CLICKED/LONG never enter this queue. */
        if (!pressed) consume_wake = release_observed = false;
    }
    else if (key_count < KEY_QUEUE_CAPACITY)
    {
        physical_event_t *event = &key_queue[(key_head + key_count) % KEY_QUEUE_CAPACITY];
        event->key = key;
        event->pressed = pressed;
        event->tick = rt_tick_get();
        event->scope_identity = published_identity;
        ++key_count;
    }
    else
    {
        /* A missing edge must never turn a later release into a click. */
        discard_queue();
        consume_wake = pressed || buttons_pressed();
        release_observed = false;
    }
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

static uint32_t remaining_ms(rt_tick_t started, int duration)
{
    rt_tick_t elapsed = rt_tick_get() - started;
    rt_tick_t interval = rt_tick_from_millisecond(duration);
    if (elapsed >= interval) return 0;
    return (uint32_t)(((uint64_t)(interval - elapsed) * 1000u + RT_TICK_PER_SECOND - 1u) /
                      RT_TICK_PER_SECOND);
}

static void wake_release_process(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (consume_wake)
    {
        if (buttons_pressed()) release_observed = false;
        else if (!release_observed)
        {
            release_observed = true;
            released_at = rt_tick_get();
        }
        else if (!remaining_ms(released_at, WAKE_RELEASE_MS))
            consume_wake = release_observed = false;
    }
    if (processed_epoch != input_epoch)
    {
        processed_epoch = input_epoch;
        held = false;
        press_scope = NULL;
    }
    rt_hw_interrupt_enable(level);
}

static void dispatch_application(epd_app_key_event_t event)
{
    if (held_key == BUTTONS_KEY1 || !press_scope) return;
    if (press_scope != active_scope())
    {
        press_scope = NULL;
        return;
    }
    /* Copy before entering module code: the callback may delete its owner or
     * replace/unregister itself. No scope pointer is dereferenced afterward. */
    epd_app_key_callback_t callback = press_scope->callback;
    void *data = press_scope->callback_data;
    if (callback)
    {
        epd_refresh_request_interaction();
        callback((epd_app_key_t)held_key, event, data);
    }
    gui_app_exec_now();
    publish_scope();
}

static void dispatch_long(void)
{
    if (long_sent) return;
    long_sent = true;
    if (held_key == BUTTONS_KEY1) ui_shell_power_menu();
    else dispatch_application(EPD_APP_KEY_LONG_PRESSED);
}

rt_err_t epd_input_init(void)
{
    buttons_init(physical_post);
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
        {
            pointer_present = true;
            lv_timer_t *timer = lv_indev_get_read_timer(indev);
            if (timer) lv_timer_pause(timer);
        }
    return RT_EOK;
}

void epd_input_read(void)
{
    if (pointer_initialized && lv_tick_elaps(pointer_read_at) < POINTER_READ_MS) return;
    pointer_initialized = true;
    pointer_read_at = lv_tick_get();
    pointer_present = false;
    /* Keep the SDK read callback and LVGL gesture processing on the UI thread.
     * Deferred application actions run after this call, before draw timers. */
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
        {
            pointer_present = true;
            lv_timer_t *timer = lv_indev_get_read_timer(indev);
            if (timer) lv_timer_pause(timer);
            bool enabled = ui_settings_enabled(UI_SETTING_TOUCH);
            lv_indev_read_cb_t read = lv_indev_get_read_cb(indev);
            if (!enabled)
            {
                lv_indev_reset(indev, NULL);
                muted_pointer_read = read;
                lv_indev_set_read_cb(indev, pointer_read_muted);
            }
            lv_indev_read(indev);
            if (!enabled)
            {
                lv_indev_set_read_cb(indev, read);
                muted_pointer_read = NULL;
                /* Re-enabling during a contact must not activate a control. */
                lv_indev_wait_release(indev);
            }
        }
}

void epd_input_clear(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    discard_queue();
    processed_epoch = input_epoch;
    rt_hw_interrupt_enable(level);
    held = false;
    press_scope = NULL;
    publish_scope();
}

void epd_input_process(void)
{
    wake_release_process();
    publish_scope();
    for (unsigned i = 0; i < KEY_QUEUE_CAPACITY; ++i)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        if (!key_count)
        {
            rt_hw_interrupt_enable(level);
            break;
        }
        physical_event_t event = key_queue[key_head];
        key_head = (key_head + 1) % KEY_QUEUE_CAPACITY;
        --key_count;
        unsigned epoch = input_epoch;
        rt_hw_interrupt_enable(level);
        wake_release_process();
        if (epoch != processed_epoch) continue;
        lv_display_trigger_activity(NULL);
        epd_power_activity();
        if (event.pressed)
        {
            if (held) continue;
            held = true;
            held_key = event.key;
            pressed_at = event.tick;
            long_sent = false;
            press_locked = ui_shell_locked();
            press_scope = event.key == BUTTONS_KEY1 ? NULL : active_scope();
            if (press_scope && press_scope->identity != event.scope_identity) press_scope = NULL;
            if (event.key == BUTTONS_KEY1)
            {
                if (press_locked) frontlight_wake();
            }
            else dispatch_application(EPD_APP_KEY_PRESSED);
        }
        else if (held && held_key == event.key)
        {
            if (!long_sent && (rt_tick_t)(event.tick - pressed_at) >=
                (rt_tick_t)rt_tick_from_millisecond(KEY_HOLD_MS)) dispatch_long();
            dispatch_application(EPD_APP_KEY_RELEASED);
            if (!long_sent)
            {
                if (held_key == BUTTONS_KEY1)
                {
                    if (!press_locked && !ui_shell_locked()) ui_shell_lock();
                }
                else dispatch_application(EPD_APP_KEY_CLICKED);
            }
            held = false;
            press_scope = NULL;
        }
    }
    wake_release_process();
    if (held && !long_sent && !remaining_ms(pressed_at, KEY_HOLD_MS) && buttons_pressed())
        dispatch_long();
}

uint32_t epd_input_next_delay(uint32_t default_ms)
{
    uint32_t elapsed = lv_tick_elaps(pointer_read_at);
    uint32_t pointer_delay = pointer_initialized && elapsed < POINTER_READ_MS ?
        POINTER_READ_MS - elapsed : 0;
    if (pointer_present && !epd_power_sleeping() && pointer_delay < default_ms) default_ms = pointer_delay;
    publish_scope();
    rt_base_t level = rt_hw_interrupt_disable();
    if (key_count || processed_epoch != input_epoch) default_ms = 0;
    else if (consume_wake)
    {
        /* Only an unidentified wake needs bounded PA34 release sampling. */
        uint32_t delay = release_observed ? remaining_ms(released_at, WAKE_RELEASE_MS) : 20;
        if (delay < default_ms) default_ms = delay;
    }
    rt_hw_interrupt_enable(level);
    if (held && !long_sent)
    {
        uint32_t delay = remaining_ms(pressed_at, KEY_HOLD_MS);
        /* A raw release may precede SDK's debounced edge. */
        if (!delay && !buttons_pressed()) delay = 20;
        if (delay < default_ms) default_ms = delay;
    }
    return default_ms;
}

RTM_EXPORT(epd_input_focusable);
RTM_EXPORT(epd_input_set_ops);

void epd_input_back(void)
{
    input_scope_t *scope = active_scope();
    if (!scope || (scope->ops && scope->ops->ready && !scope->ops->ready(LV_KEY_ESC))) return;
    epd_refresh_request_interaction();
    if (epd_keyboard_back(scope->owner)) return;
    if (scope->ops && scope->ops->key && scope->ops->key(LV_KEY_ESC)) return;
    if (scope->back)
    {
        if (epd_input_focusable(scope->back)) lv_obj_send_event(scope->back, LV_EVENT_CLICKED, NULL);
    }
    else if (scope->owner == scope->screen && scope->screen == lv_screen_active()) gui_app_goback();
    gui_app_exec_now();
}

unsigned epd_input_scope_id(void)
{
    input_scope_t *scope = active_scope();
    return scope ? scope->identity : 0;
}
