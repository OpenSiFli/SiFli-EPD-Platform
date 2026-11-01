#include "sleep_guard.h"
#include "platform/epd_power.h"
#include <rthw.h>
#include <rtthread.h>

static sleep_guard_snapshot_t guard;
static sleep_guard_owner_t *holders;

static void changed(void)
{
    ++guard.revision;
    guard.changed_tick = (uint32_t)rt_tick_get();
}

static bool acquire(sleep_guard_owner_t *owner, bool draining)
{
    if (!owner) return false;
    bool notify = false;
    rt_base_t level = rt_hw_interrupt_disable();
    bool accepted = guard.state != SLEEP_GUARD_SLEEPING;
    if (accepted && !owner->held)
    {
        owner->held = true;
        owner->draining = draining;
        owner->next = holders;
        holders = owner;
        ++guard.held_count;
        if (draining) ++guard.draining_count;
        else
        {
            guard.state = SLEEP_GUARD_AWAKE;
            changed();
        }
        /* AWAKE drain work changes neither activity nor prepare eligibility. */
        notify = !draining || (guard.state == SLEEP_GUARD_PREPARING && guard.held_count == 1);
    }
    else if (accepted && owner->draining && !draining)
    {
        owner->draining = false;
        --guard.draining_count;
        guard.state = SLEEP_GUARD_AWAKE;
        changed();
        notify = true;
    }
    rt_hw_interrupt_enable(level);
    if (notify) epd_power_notify();
    return accepted;
}

bool sleep_guard_acquire(sleep_guard_owner_t *owner) { return acquire(owner, false); }
bool sleep_guard_acquire_drain(sleep_guard_owner_t *owner) { return acquire(owner, true); }

void sleep_guard_release(sleep_guard_owner_t *owner)
{
    if (!owner) return;
    rt_base_t level = rt_hw_interrupt_disable();
    bool notify = owner->held;
    if (owner->held)
    {
        owner->held = false;
        sleep_guard_owner_t **link = &holders;
        while (*link && *link != owner) link = &(*link)->next;
        if (*link) *link = owner->next;
        owner->next = NULL;
        --guard.held_count;
        if (owner->draining) --guard.draining_count;
        else changed();
        /* Only PREPARING can have a coordinator waiting for drain completion. */
        notify = !owner->draining || (guard.state == SLEEP_GUARD_PREPARING && !guard.held_count);
        owner->draining = false;
    }
    rt_hw_interrupt_enable(level);
    if (notify) epd_power_notify();
}

void sleep_guard_snapshot(sleep_guard_snapshot_t *snapshot)
{
    if (!snapshot) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *snapshot = guard;
    rt_hw_interrupt_enable(level);
}

size_t sleep_guard_holders(char (*ids)[SLEEP_GUARD_ID_MAX], size_t capacity,
                          sleep_guard_snapshot_t *snapshot)
{
    rt_base_t level = rt_hw_interrupt_disable();
    size_t total = guard.held_count, copied = 0;
    if (snapshot) *snapshot = guard;
    for (sleep_guard_owner_t *owner = holders; ids && owner && copied < capacity; owner = owner->next)
    {
        const char *id = owner->id ? owner->id : "platform";
        size_t n = 0;
        while (n + 1 < SLEEP_GUARD_ID_MAX && id[n]) { ids[copied][n] = id[n]; ++n; }
        ids[copied++][n] = 0;
    }
    rt_hw_interrupt_enable(level);
    return total;
}

bool sleep_guard_prepare(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool accepted = guard.held_count == guard.draining_count && guard.state != SLEEP_GUARD_SLEEPING;
    bool notify = accepted && guard.state != SLEEP_GUARD_PREPARING;
    if (notify)
    {
        guard.state = SLEEP_GUARD_PREPARING;
        changed();
    }
    rt_hw_interrupt_enable(level);
    if (notify) epd_power_notify();
    return accepted;
}

bool sleep_guard_commit(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool accepted = !guard.held_count && guard.state == SLEEP_GUARD_PREPARING;
    if (accepted)
    {
        guard.state = SLEEP_GUARD_SLEEPING;
        changed();
    }
    rt_hw_interrupt_enable(level);
    if (accepted) epd_power_notify();
    return accepted;
}

void sleep_guard_cancel(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool notify = guard.state != SLEEP_GUARD_AWAKE;
    if (notify)
    {
        guard.state = SLEEP_GUARD_AWAKE;
        changed();
    }
    rt_hw_interrupt_enable(level);
    if (notify) epd_power_notify();
}
