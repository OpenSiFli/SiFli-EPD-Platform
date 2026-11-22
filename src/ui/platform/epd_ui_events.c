#include "app_service_internal.h"
#include "epd_app.h"
#include "epd_power.h"
#include "platform/epd_memory_manager.h"
#include <rthw.h>
#include <rtm.h>

typedef struct ui_listener {
    struct ui_listener *next;
    lv_obj_t *screen;
    epd_memory_owner_t owner;
    uint32_t events;
    void (*notify)(uint32_t, void *);
    void *user;
} ui_listener_t;

static struct {
    ui_listener_t *listeners;
    uint16_t subscribed, pending;
} ui_events;

static void update_subscriptions(void)
{
    uint32_t mask = 0;
    for (ui_listener_t *listener = ui_events.listeners; listener; listener = listener->next)
        mask |= listener->events;
    rt_base_t level = rt_hw_interrupt_disable();
    ui_events.subscribed = mask;
    ui_events.pending &= mask;
    rt_hw_interrupt_enable(level);
}

static void remove_listener(ui_listener_t *listener)
{
    ui_listener_t **link = &ui_events.listeners;
    while (*link != listener) link = &(*link)->next;
    *link = listener->next;
    update_subscriptions();
    epd_memory_owner_release_trusted(listener->owner);
    lv_free(listener);
}

static void screen_deleted(lv_event_t *event)
{
    ui_listener_t *listener = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) == listener->screen) remove_listener(listener);
}

bool epd_app_ui_listen(lv_obj_t *screen, uint32_t events, void (*notify)(uint32_t, void *), void *user)
{
    if (!screen) return false;
    ui_listener_t *listener = ui_events.listeners;
    while (listener && listener->screen != screen) listener = listener->next;
    if (listener) {
        if (epd_memory_owner_current() != listener->owner) return false;
        if (notify) {
            listener->events = events;
            listener->notify = notify;
            listener->user = user;
            update_subscriptions();
        } else {
            lv_obj_remove_event_cb_with_user_data(screen, screen_deleted, listener);
            remove_listener(listener);
        }
        return true;
    }
    if (!notify) return true;
    epd_memory_owner_t owner = epd_memory_owner_current();
    if (!epd_memory_owner_retain_trusted(owner)) return false;
    listener = lv_malloc(sizeof(*listener));
    if (!listener || !lv_obj_add_event_cb(screen, screen_deleted, LV_EVENT_DELETE, listener)) {
        lv_free(listener);
        epd_memory_owner_release_trusted(owner);
        return false;
    }
    *listener = (ui_listener_t){ui_events.listeners, screen, owner, events, notify, user};
    ui_events.listeners = listener;
    update_subscriptions();
    return true;
}
RTM_EXPORT(epd_app_ui_listen);

void app_ui_signal(uint32_t events)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t added = events & ui_events.subscribed & ~ui_events.pending;
    ui_events.pending |= added;
    rt_hw_interrupt_enable(level);
    if (added) epd_power_notify();
}

void app_ui_process_events(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t events = ui_events.pending;
    ui_events.pending = 0;
    rt_hw_interrupt_enable(level);
    if (!events) return;
    lv_obj_t *screen = lv_screen_active();
    for (ui_listener_t *listener = ui_events.listeners; listener; listener = listener->next) {
        uint32_t matched = events & listener->events;
        if (!matched || listener->screen != screen) continue;
        epd_memory_owner_t owner = listener->owner;
        epd_memory_owner_snapshot_t snapshot;
        if (!epd_memory_owner_get(owner, &snapshot) || snapshot.closed ||
            !epd_memory_owner_retain_trusted(owner)) break;
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
        if (previous != EPD_MEMORY_OWNER_INVALID) {
            listener->notify(matched, listener->user);
            epd_memory_owner_leave_trusted(previous);
        }
        epd_memory_owner_release_trusted(owner);
        break;
    }
}
