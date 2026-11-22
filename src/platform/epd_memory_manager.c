/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_memory_manager.h"
#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#ifndef EPD_MEMORY_HOST
#include "mem_section.h"
#include "bf0_hal.h"
#endif

#ifndef EPD_MEMORY_DESCRIPTOR_COUNT
#define EPD_MEMORY_DESCRIPTOR_COUNT 128u
#endif
#ifndef EPD_MEMORY_MAX_BYPASSES
#define EPD_MEMORY_MAX_BYPASSES 3u
#endif
#ifndef EPD_MEMORY_FAIR_WAIT_MS
#define EPD_MEMORY_FAIR_WAIT_MS 250u
#endif
#ifndef EPD_MEMORY_GENERATION_MAX
#define EPD_MEMORY_GENERATION_MAX UINT32_C(0xffffff)
#endif
#ifndef EPD_MEMORY_TRANSACTION_COUNT
#define EPD_MEMORY_TRANSACTION_COUNT 8u
#endif
#define MAX_DEPTH 16u
#define INVALID EPD_MEMORY_OWNER_INVALID
#if EPD_MEMORY_DESCRIPTOR_COUNT > 255 || EPD_MEMORY_DESCRIPTOR_COUNT < 2
#error "Descriptor count must be between 2 and 255"
#endif

typedef struct {
    size_t physical, sized, unused, pending;
    uint32_t blocks;
} local_usage_t;

typedef struct {
    uint32_t generation;
    bool used, retired, closed, cleanup_grace;
    epd_memory_scope_kind_t kind;
    epd_memory_class_t cls;
    epd_memory_owner_t parent, account;
    uint32_t refs, blocks, pending_calls, mask, depth;
    char appid[EPD_MEMORY_APPID_SIZE];
    size_t limit[EPD_MEMORY_BACKEND_COUNT];
    local_usage_t local[EPD_MEMORY_BACKEND_COUNT];
    epd_memory_usage_t total[EPD_MEMORY_BACKEND_COUNT];
    epd_memory_credit_state_t state;
    epd_memory_result_t result;
    epd_memory_priority_t priority;
    size_t want[EPD_MEMORY_BACKEND_COUNT];
    uint32_t queued_at, deadline, bypasses;
    uint64_t sequence;
    unsigned transaction; /* Fixed transaction slot + 1, or zero. */
} descriptor_t;

typedef struct {
    bool used, replace;
    unsigned count;
    epd_memory_backend_t backend;
    epd_memory_credit_t sources[EPD_MEMORY_CREDIT_MAX_SOURCES];
    size_t takes[EPD_MEMORY_CREDIT_MAX_SOURCES];
    size_t replacement[EPD_MEMORY_BACKEND_COUNT];
    size_t target[EPD_MEMORY_BACKEND_COUNT];
} transaction_t;

typedef struct {
    bool ready, reclaim_running, descriptors_ready, descriptors_initializing;
    bool capacity_pending;
    bool diagnostic_enabled;
    uint8_t diagnostic_depth, diagnostic_path;
    uint32_t diagnostic_started;
    uint32_t capacity_epoch;
    epd_memory_capacity_notify_t capacity_notify;
    uint32_t used, retired, errors, last_tick, now_ms, tick_remainder;
    uint64_t sequence;
    unsigned wait_count;
    descriptor_t system;
    epd_memory_backend_snapshot_t backend[EPD_MEMORY_BACKEND_COUNT];
} manager_t;

typedef struct {
    epd_memory_owner_t waits[EPD_MEMORY_DESCRIPTOR_COUNT];
    descriptor_t desc[EPD_MEMORY_DESCRIPTOR_COUNT - 1u];
    size_t associated[EPD_MEMORY_DESCRIPTOR_COUNT - 1u][EPD_MEMORY_BACKEND_COUNT];
    /* Commit scratch is shared only within the single-HCPU IRQ guard. */
    size_t settlement[EPD_MEMORY_CREDIT_MAX_SOURCES];
    epd_memory_diagnostic_snapshot_t diagnostic;
    epd_memory_rejection_snapshot_t rejection;
    transaction_t transactions[EPD_MEMORY_TRANSACTION_COUNT];
} descriptors_t;

/* SYSTEM settlement and heap identities work before PSRAM is accessible.
 * Only explicitly initialized application metadata resides in retained PSRAM. */
static manager_t manager;
#ifndef EPD_MEMORY_HOST
L2_RET_BSS_SECT_BEGIN(epd_memory_manager)
static descriptors_t descriptors L2_RET_BSS_SECT(epd_memory_manager);
L2_RET_BSS_SECT_END
#else
static descriptors_t descriptors;
/* The host harness poisons this region to detect any pre-init access. */
void epd_memory_host_retained_storage(void **address, size_t *size)
{
    *address = &descriptors;
    *size = sizeof(descriptors);
}
#endif

static void init_locked(void)
{
    unsigned b;
    descriptor_t *d;
    if (manager.ready) return;
    d = &manager.system;
    d->used = true;
    d->mask = EPD_MEMORY_ALL_BACKENDS;
    d->cls = EPD_MEMORY_ACTIVE;
    d->refs = 1;
    memcpy(d->appid, "system", 7);
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) d->limit[b] = SIZE_MAX;
    manager.used = 1;
    manager.ready = true;
}

static uint32_t diagnostic_cycles(void)
{
#if !defined(EPD_MEMORY_HOST)
    return HAL_DBG_DWT_GetCycles();
#elif defined(EPD_MEMORY_DIAGNOSTIC_HOST)
    extern uint32_t epd_memory_test_cycles(void);
    return epd_memory_test_cycles();
#else
    return 0;
#endif
}

static bool diagnostic_clock_start(uint32_t *frequency)
{
#ifndef EPD_MEMORY_HOST
    if (!HAL_DBG_DWT_IsInit()) HAL_DBG_DWT_Init();
    *frequency = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
    return HAL_DBG_DWT_IsInit() && *frequency;
#elif defined(EPD_MEMORY_DIAGNOSTIC_HOST)
    *frequency = 1000000;
    return true;
#else
    (void)frequency;
    return false;
#endif
}

static rt_base_t lock_manager_path(epd_memory_diagnostic_path_t path)
{
    rt_base_t level = rt_hw_interrupt_disable();
    init_locked();
    if (manager.diagnostic_enabled) {
        epd_memory_diagnostic_snapshot_t *s = &descriptors.diagnostic;
        if (!manager.diagnostic_depth) {
            if ((int32_t)((uint32_t)rt_tick_get() - s->deadline_tick) >= 0) {
                manager.diagnostic_enabled = false;
                s->active = false; s->timed_out = true;
            } else {
                manager.diagnostic_started = diagnostic_cycles();
                manager.diagnostic_path = (uint8_t)path;
                manager.diagnostic_depth = 1;
                if (level) ++s->path[path].entered_irq_off;
            }
        } else {
            ++manager.diagnostic_depth;
            ++s->nested_calls;
        }
    }
    return level;
}

static rt_base_t lock_manager(void)
{ return lock_manager_path(EPD_MEMORY_DIAG_CONTROL); }

static void unlock_manager(rt_base_t level)
{
    if (manager.diagnostic_enabled && !--manager.diagnostic_depth) {
        epd_memory_diagnostic_snapshot_t *s = &descriptors.diagnostic;
        epd_memory_diagnostic_path_snapshot_t *p = &s->path[manager.diagnostic_path];
        uint32_t elapsed;
        ++p->samples;
        if (!--s->remaining) {
            s->active = false; s->exhausted = true;
            manager.diagnostic_enabled = false;
        }
        elapsed = diagnostic_cycles() - manager.diagnostic_started;
        if (elapsed > p->max_cycles) p->max_cycles = elapsed;
    }
    rt_hw_interrupt_enable(level);
}

epd_memory_result_t epd_memory_diagnostic_start(uint32_t samples, uint32_t duration_ms)
{
    uint32_t frequency;
    rt_base_t level;
    if (!samples || samples > 1000000u || !duration_ms || duration_ms > 60000u)
        return EPD_MEMORY_INVALID;
    level = rt_hw_interrupt_disable();
    init_locked();
    if (level || rt_interrupt_get_nest() || rt_critical_level() || !manager.descriptors_ready) {
        rt_hw_interrupt_enable(level); return EPD_MEMORY_BUSY;
    }
    manager.diagnostic_enabled = false;
    rt_hw_interrupt_enable(level);
    if (!diagnostic_clock_start(&frequency)) return EPD_MEMORY_INVALID;
    level = rt_hw_interrupt_disable();
    memset(&descriptors.diagnostic, 0, sizeof(descriptors.diagnostic));
    descriptors.diagnostic.active = true;
    descriptors.diagnostic.remaining = samples;
    descriptors.diagnostic.duration_ms = duration_ms;
    descriptors.diagnostic.frequency_hz = frequency;
    descriptors.diagnostic.deadline_tick = (uint32_t)rt_tick_get() +
        (uint32_t)(((uint64_t)duration_ms * RT_TICK_PER_SECOND + 999u) / 1000u);
    manager.diagnostic_depth = 0;
    manager.diagnostic_enabled = true;
    rt_hw_interrupt_enable(level);
    return EPD_MEMORY_OK;
}

void epd_memory_diagnostic_stop(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (manager.descriptors_ready && !manager.diagnostic_depth) {
        manager.diagnostic_enabled = false;
        descriptors.diagnostic.active = false;
    }
    rt_hw_interrupt_enable(level);
}

bool epd_memory_diagnostic_read(epd_memory_diagnostic_snapshot_t *out)
{
    bool ready;
    rt_base_t level;
    if (!out) return false;
    level = rt_hw_interrupt_disable();
    ready = manager.descriptors_ready;
    if (ready) {
        if (manager.diagnostic_enabled && !manager.diagnostic_depth &&
            (int32_t)((uint32_t)rt_tick_get() - descriptors.diagnostic.deadline_tick) >= 0) {
            manager.diagnostic_enabled = false;
            descriptors.diagnostic.active = false;
            descriptors.diagnostic.timed_out = true;
        }
        *out = descriptors.diagnostic;
    }
    rt_hw_interrupt_enable(level);
    return ready;
}

static void capacity_changed(void)
{
    ++manager.capacity_epoch;
    manager.capacity_pending = true;
}

void epd_memory_dispatch_changes(void)
{
    epd_memory_capacity_notify_t notify = NULL;
    rt_base_t level = lock_manager();
    if (!level && !rt_interrupt_get_nest() && !rt_critical_level() &&
        manager.capacity_pending && manager.capacity_notify) {
        manager.capacity_pending = false;
        notify = manager.capacity_notify;
    }
    unlock_manager(level);
    if (notify) notify();
}

void epd_memory_set_capacity_notifier(epd_memory_capacity_notify_t notify)
{
    rt_base_t level = lock_manager();
    manager.capacity_notify = notify;
    manager.capacity_pending = true;
    unlock_manager(level);
    epd_memory_dispatch_changes();
}

uint32_t epd_memory_capacity_epoch(void)
{
    rt_base_t level = lock_manager();
    uint32_t epoch = manager.capacity_epoch;
    unlock_manager(level);
    return epoch;
}

epd_memory_result_t epd_memory_init(void)
{
    rt_base_t level = lock_manager();
    if (manager.descriptors_ready) {
        unlock_manager(level); return EPD_MEMORY_OK;
    }
    if (level || rt_interrupt_get_nest() || manager.descriptors_initializing) {
        unlock_manager(level); return EPD_MEMORY_BUSY;
    }
    manager.descriptors_initializing = true;
    unlock_manager(level);
    /* Retained sections are not in the startup zero table. Keep this large
     * cold-start clear outside the IRQ guard. Until publication, every owner
     * other than SYSTEM is rejected without accessing descriptors. */
    memset(&descriptors, 0, sizeof(descriptors));
    level = lock_manager();
    manager.descriptors_ready = true;
    manager.descriptors_initializing = false;
    unlock_manager(level);
    return EPD_MEMORY_OK;
}

#ifndef EPD_MEMORY_HOST
static int memory_descriptors_init(void)
{
    return epd_memory_init() == EPD_MEMORY_OK ? RT_EOK : -RT_ERROR;
}
INIT_PREV_EXPORT(memory_descriptors_init);
#endif

static epd_memory_owner_t token_of(const descriptor_t *d)
{
    if (d == &manager.system) return 0;
    return (d->generation << 8) | ((unsigned)(d - descriptors.desc) + 1u);
}

static descriptor_t *lookup(epd_memory_owner_t token)
{
    unsigned i = token & 255u;
    descriptor_t *d;
    if (!token) return &manager.system;
    if (!manager.descriptors_ready || !i || i >= EPD_MEMORY_DESCRIPTOR_COUNT || token == INVALID) return NULL;
    d = &descriptors.desc[i - 1u];
    return d->used && token_of(d) == token ? d : NULL;
}

static descriptor_t *parent_of(const descriptor_t *d)
{
    return d == &manager.system ? NULL : lookup(d->parent);
}

static bool below(const descriptor_t *d, const descriptor_t *ancestor)
{
    while (d) {
        if (d == ancestor) return true;
        d = parent_of(d);
    }
    return false;
}

static uint32_t common_ancestor_depth(const descriptor_t *a, const descriptor_t *b)
{
    while (a->depth > b->depth) a = parent_of(a);
    while (b->depth > a->depth) b = parent_of(b);
    while (a != b) { a = parent_of(a); b = parent_of(b); }
    return a->depth;
}

static bool open_path(const descriptor_t *d)
{
    while (d) {
        if (d->closed) return false;
        d = parent_of(d);
    }
    return true;
}

static uint32_t now_locked(void)
{
    uint32_t tick = (uint32_t)rt_tick_get();
    uint32_t elapsed = tick - manager.last_tick;
    uint64_t scaled = (uint64_t)elapsed * 1000u + manager.tick_remainder;
    manager.last_tick = tick;
    manager.now_ms += (uint32_t)(scaled / RT_TICK_PER_SECOND);
    manager.tick_remainder = (uint32_t)(scaled % RT_TICK_PER_SECOND);
    return manager.now_ms;
}

uint32_t epd_memory_now_ms(void)
{
    rt_base_t level = lock_manager();
    uint32_t now = now_locked();
    unlock_manager(level);
    return now;
}

static size_t physical_total(const epd_memory_usage_t *u)
{
    unsigned c;
    size_t bytes = 0;
    for (c = 0; c < EPD_MEMORY_CLASS_COUNT; ++c) bytes += u->physical[c];
    return bytes;
}

static size_t committed(const epd_memory_usage_t *u)
{
    return physical_total(u) + u->unused + u->allocating;
}

/* SYSTEM owns account 0, but is also the scope ancestor of every APP root.
 * Its account ledger excludes other accounts; the existing backend ledger is
 * the global subtree view used by scope constraints and diagnostics. */
static const epd_memory_usage_t *subtree_usage(const descriptor_t *d, unsigned b)
{
    return d == &manager.system ? &manager.backend[b].usage : &d->total[b];
}

static size_t associated(const descriptor_t *d, unsigned b)
{
    return d == &manager.system ? 0 : descriptors.associated[d - descriptors.desc][b];
}

static size_t effective_limit(const descriptor_t *d, unsigned b)
{
    size_t limit = d->limit[b];
    if (d != &manager.system && d->kind == EPD_MEMORY_SCOPE_APP) {
        const epd_memory_backend_snapshot_t *heap = &manager.backend[b];
        size_t capacity = heap->key ? heap->config.capacity - heap->config.unavailable : 0;
        if (capacity < limit) limit = capacity;
    }
    return limit;
}

static bool budget_fits(const descriptor_t *d, unsigned b, size_t addition)
{
    size_t shared = associated(d, b), limit = effective_limit(d, b);
    if (shared > limit || addition > limit - shared) return false;
    return committed(subtree_usage(d, b)) <= limit - shared - addition;
}

/* A representation change of an existing promise is not new admission. */
static bool association_fits(const descriptor_t *d, unsigned b, size_t old_bytes,
                             size_t new_bytes, size_t returned)
{
    size_t growth = new_bytes > old_bytes ? new_bytes - old_bytes : 0;
    return returned >= growth || budget_fits(d, b, growth - returned);
}

enum field { PHYSICAL, SIZED, UNUSED, ALLOCATING, BLOCKS };

static void adjust_usage(epd_memory_usage_t *u, enum field f,
                         epd_memory_class_t cls, size_t bytes, bool add)
{
    size_t *p = NULL;
    switch (f) {
    case PHYSICAL: p = &u->physical[cls]; break;
    case SIZED: p = &u->sized; break;
    case UNUSED: p = &u->unused; break;
    case ALLOCATING: p = &u->allocating; break;
    case BLOCKS:
        if (add) u->allocations += (uint32_t)bytes;
        else u->allocations -= (uint32_t)bytes;
        return;
    }
    if (add) *p += bytes;
    else *p -= bytes;
    if (add && f != SIZED) {
        size_t current = committed(u);
        if (current > u->peak_committed) u->peak_committed = current;
    }
}

/* No backend traversal or application scan on settlement. */
static void adjust(descriptor_t *d, unsigned b, enum field f, size_t bytes, bool add)
{
    descriptor_t *p;
    size_t *value = NULL;
    local_usage_t *u = &d->local[b];
    if (!bytes) return;
    switch (f) {
    case PHYSICAL: value = &u->physical; break;
    case SIZED: value = &u->sized; break;
    case UNUSED: value = &u->unused; break;
    case ALLOCATING: value = &u->pending; break;
    case BLOCKS:
        if (add) { u->blocks += (uint32_t)bytes; d->blocks += (uint32_t)bytes; }
        else { u->blocks -= (uint32_t)bytes; d->blocks -= (uint32_t)bytes; }
        break;
    }
    if (value) {
        if (add) *value += bytes;
        else *value -= bytes;
    }
    for (p = d; p && (p != &manager.system || d->account == EPD_MEMORY_OWNER_SYSTEM); p = parent_of(p))
        adjust_usage(&p->total[b], f, d->cls, bytes, add);
    adjust_usage(&manager.backend[b].usage, f, d->cls, bytes, add);
}

static bool hold(descriptor_t *d)
{
    if (!d) return false;
    if (d == &manager.system) return true;
    if (d->refs == UINT32_MAX) return false;
    ++d->refs;
    return true;
}

static bool has_unused(const descriptor_t *d)
{
    unsigned b;
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b)
        if (d->local[b].unused || associated(d, b)) return true;
    return false;
}

static void collect(descriptor_t *d)
{
    while (d && d != &manager.system && !d->refs && !d->blocks &&
           !d->pending_calls && !has_unused(d)) {
        descriptor_t *p = parent_of(d);
        d->used = false;
        --manager.used;
        if (d->generation == EPD_MEMORY_GENERATION_MAX) {
            d->retired = true;
            ++manager.retired;
        }
        if (p && p != &manager.system) {
            if (!p->refs) { ++manager.errors; return; }
            --p->refs;
        }
        d = p;
    }
}

static void finish_credit(descriptor_t *d, epd_memory_credit_state_t state,
                          epd_memory_result_t result);

static void drop(descriptor_t *d)
{
    if (!d || d == &manager.system) return;
    if (!d->refs) { ++manager.errors; return; }
    --d->refs;
    if (!d->refs && d->kind == EPD_MEMORY_SCOPE_CREDIT)
        finish_credit(d, EPD_MEMORY_CREDIT_CANCELLED, EPD_MEMORY_CANCELLED);
    collect(d);
}

static descriptor_t *new_descriptor(descriptor_t *parent, epd_memory_scope_kind_t kind)
{
    unsigned i, b;
    descriptor_t *d;
    if (!manager.descriptors_ready || parent->depth >= MAX_DEPTH || parent->refs == UINT32_MAX) return NULL;
    for (i = 1; i < EPD_MEMORY_DESCRIPTOR_COUNT; ++i) {
        uint32_t generation;
        d = &descriptors.desc[i - 1u];
        if (d->used || d->retired) continue;
        generation = d->generation + 1u;
        memset(d, 0, sizeof(*d));
        d->generation = generation;
        d->used = true;
        d->refs = 1;
        d->parent = token_of(parent);
        d->account = parent->account;
        d->depth = parent->depth + 1u;
        d->kind = kind;
        d->cls = EPD_MEMORY_ACTIVE;
        d->mask = EPD_MEMORY_ALL_BACKENDS;
        for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) d->limit[b] = SIZE_MAX;
        (void)hold(parent);
        ++manager.used;
        return d;
    }
    return NULL;
}

epd_memory_owner_t epd_memory_owner_create(const char *appid)
{
    descriptor_t *d;
    size_t n = 0;
    epd_memory_owner_t token = INVALID;
    rt_base_t level;
    if (!appid) return INVALID;
    while (n < EPD_MEMORY_APPID_SIZE && appid[n]) ++n;
    if (!n || n == EPD_MEMORY_APPID_SIZE) return INVALID;
    level = lock_manager();
    d = new_descriptor(&manager.system, EPD_MEMORY_SCOPE_APP);
    if (d) {
        token = token_of(d);
        d->account = token;
        memcpy(d->appid, appid, n + 1u);
    }
    unlock_manager(level);
    return token;
}

static epd_memory_owner_t current_token(void);

static bool current_account(const descriptor_t *target)
{
    const descriptor_t *current = lookup(current_token());
    return target && current && target->account == current->account;
}

bool epd_memory_owner_is_current_account(epd_memory_owner_t token)
{
    rt_base_t level = lock_manager();
    bool ok = current_account(lookup(token));
    unlock_manager(level);
    return ok;
}

static epd_memory_owner_t scope_create(epd_memory_owner_t parent,
                                        epd_memory_scope_kind_t kind, bool trusted)
{
    epd_memory_owner_t token = INVALID;
    rt_base_t level = lock_manager();
    descriptor_t *p = lookup(parent), *d;
    if (p && (trusted || current_account(p)) && open_path(p) && p->kind != EPD_MEMORY_SCOPE_CREDIT &&
        kind > EPD_MEMORY_SCOPE_APP && kind < EPD_MEMORY_SCOPE_CREDIT) {
        d = new_descriptor(p, kind);
        if (d) token = token_of(d);
    }
    unlock_manager(level);
    return token;
}

static bool owner_retain(epd_memory_owner_t token, bool trusted)
{
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(token);
    bool ok = (trusted || current_account(d)) && hold(d);
    unlock_manager(level);
    return ok;
}

static void owner_release(epd_memory_owner_t token, bool trusted)
{
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(token);
    if (!d) ++manager.errors;
    else if (trusted || current_account(d)) drop(d);
    unlock_manager(level);
    epd_memory_dispatch_changes();
}

static epd_memory_owner_t current_token(void)
{
#ifdef RT_USING_THREAD_MEM_CONTEXT
    return rt_thread_mem_context_get(RT_NULL);
#else
    return 0;
#endif
}

epd_memory_result_t epd_memory_rejection_start(uint32_t count, uint32_t duration_ms)
{
    rt_base_t level;
    if (!count || count > 1024u || !duration_ms || duration_ms > 60000u)
        return EPD_MEMORY_INVALID;
    level = rt_hw_interrupt_disable();
    if (level || !rt_thread_self() || rt_interrupt_get_nest() || rt_critical_level() ||
        !manager.descriptors_ready) {
        rt_hw_interrupt_enable(level); return EPD_MEMORY_BUSY;
    }
    memset(&descriptors.rejection, 0, sizeof(descriptors.rejection));
    descriptors.rejection.remaining = count;
    descriptors.rejection.deadline_tick = (uint32_t)rt_tick_get() +
        (uint32_t)(((uint64_t)duration_ms * RT_TICK_PER_SECOND + 999u) / 1000u);
    descriptors.rejection.active = true;
    rt_hw_interrupt_enable(level);
    return EPD_MEMORY_OK;
}

static bool rejection_active(void)
{
    epd_memory_rejection_snapshot_t *s;
    if (!manager.descriptors_ready) return false;
    s = &descriptors.rejection;
    if (s->active && (int32_t)((uint32_t)rt_tick_get() - s->deadline_tick) >= 0) {
        s->active = false;
        s->timed_out = true;
    }
    return s->active;
}

void epd_memory_rejection_stop(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (manager.descriptors_ready) descriptors.rejection.active = false;
    rt_hw_interrupt_enable(level);
}

bool epd_memory_rejection_read(epd_memory_rejection_snapshot_t *out)
{
    bool ready;
    rt_base_t level;
    if (!out) return false;
    level = rt_hw_interrupt_disable();
    ready = manager.descriptors_ready;
    if (ready) {
        (void)rejection_active();
        *out = descriptors.rejection;
    }
    rt_hw_interrupt_enable(level);
    return ready;
}

static void rejection_record(epd_memory_owner_t token, const epd_memory_change_t *c,
                             descriptor_t *d, epd_memory_result_t result,
                             epd_memory_rejection_reason_t reason)
{
    epd_memory_rejection_snapshot_t *s;
    if (!rejection_active()) return;
    s = &descriptors.rejection;
    s->token = token; s->current_token = current_token();
    s->account = d ? d->account : INVALID;
    s->scope = d ? d->kind : (epd_memory_scope_kind_t)255;
    s->credit_state = d && d->kind == EPD_MEMORY_SCOPE_CREDIT ? d->state : (epd_memory_credit_state_t)255;
    s->closed = d && d->closed;
    s->backend = c ? c->backend : EPD_MEMORY_BACKEND_NONE;
    s->kind = c ? c->kind : (epd_memory_change_kind_t)255;
    s->size = c ? c->size : 0;
    s->physical = c ? c->physical : 0;
    s->old_physical = c ? c->old_physical : 0;
    s->delta = c && c->kind == EPD_MEMORY_CHANGE_REALLOC_INPLACE ?
        (c->physical > c->old_physical ? c->physical - c->old_physical : 0) : s->physical;
    s->unused = d && s->backend < EPD_MEMORY_BACKEND_COUNT ? d->local[s->backend].unused : 0;
    s->reserved = s->backend < EPD_MEMORY_BACKEND_COUNT ? manager.backend[s->backend].usage.unused : 0;
    s->available_after = c ? c->available_after : 0;
    s->result = result; s->reason = reason;
    s->tick = (uint32_t)rt_tick_get(); s->valid = true;
    ++s->captured;
    if (!--s->remaining) { s->active = false; s->exhausted = true; }
}

void epd_memory_rejection_adapter(epd_memory_owner_t token, const epd_memory_change_t *change,
                                 epd_memory_rejection_reason_t reason)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (rejection_active()) rejection_record(token, change, lookup(token), EPD_MEMORY_INVALID, reason);
    rt_hw_interrupt_enable(level);
}

epd_memory_owner_t epd_memory_owner_current(void)
{
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(current_token());
    epd_memory_owner_t token = INVALID;
    if (d) token = d->kind == EPD_MEMORY_SCOPE_CREDIT ? d->parent : token_of(d);
    unlock_manager(level);
    return token;
}

epd_memory_credit_t epd_memory_credit_current(void)
{
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(current_token());
    epd_memory_credit_t token = d && d->kind == EPD_MEMORY_SCOPE_CREDIT ? token_of(d) : INVALID;
    unlock_manager(level);
    return token;
}

static epd_memory_owner_t owner_enter(epd_memory_owner_t token, bool trusted)
{
    epd_memory_owner_t previous = INVALID;
    rt_base_t level = lock_manager();
#ifdef RT_USING_THREAD_MEM_CONTEXT
    descriptor_t *d = lookup(token), *old = lookup(current_token());
    /* Reserve room for the thread hook as well as the separate old guard. */
    if (d && (trusted || current_account(d)) && d->refs < UINT32_MAX - 1u && old && hold(old)) {
        previous = token_of(old);
        if (rt_thread_mem_context_set(RT_NULL, token) != RT_EOK) {
            drop(old);
            previous = INVALID;
        }
    }
#else
    (void)token;
    (void)trusted;
#endif
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return previous;
}

static void owner_leave(epd_memory_owner_t previous, bool trusted)
{
    rt_base_t level = lock_manager();
#ifdef RT_USING_THREAD_MEM_CONTEXT
    descriptor_t *d = lookup(previous);
    if (!d) ++manager.errors;
    else if (trusted || current_account(d)) {
        if (rt_thread_mem_context_set(RT_NULL, previous) == RT_EOK) drop(d);
        else ++manager.errors;
    }
#else
    (void)previous;
    (void)trusted;
#endif
    unlock_manager(level);
    epd_memory_dispatch_changes();
}

static void release_transaction(descriptor_t *d)
{
    transaction_t *t;
    unsigned i;
    if (!d->transaction) return;
    t = &descriptors.transactions[d->transaction - 1u];
    d->transaction = 0;
    t->used = false;
    /* Sources were LIVE when pinned, so they cannot contain preparations. */
    for (i = 0; i < t->count; ++i) drop(lookup(t->sources[i]));
    t->count = 0;
}

static void finish_credit(descriptor_t *d, epd_memory_credit_state_t state,
                          epd_memory_result_t result)
{
    unsigned b;
    if (d->state != EPD_MEMORY_CREDIT_WAITING && d->state != EPD_MEMORY_CREDIT_LIVE &&
        d->state != EPD_MEMORY_CREDIT_PREPARED &&
        d->state != EPD_MEMORY_CREDIT_SUSPENDED) return;
    if (d->state == EPD_MEMORY_CREDIT_WAITING) {
        unsigned i;
        for (i = 0; i < manager.wait_count; ++i) {
            if (descriptors.waits[i] == token_of(d)) {
                --manager.wait_count;
                memmove(&descriptors.waits[i], &descriptors.waits[i + 1u],
                        (manager.wait_count - i) * sizeof(descriptors.waits[0]));
                break;
            }
        }
    }
    d->state = state;
    d->result = result;
    d->closed = true;
    d->cleanup_grace = false;
    capacity_changed();
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b)
        adjust(d, b, UNUSED, d->local[b].unused, false);
    release_transaction(d);
}

static bool owner_close(epd_memory_owner_t token, bool trusted)
{
    unsigned i;
    rt_base_t level = lock_manager();
    descriptor_t *root = lookup(token);
    if (!root || !token || (!trusted && !current_account(root))) { unlock_manager(level); return false; }
    /* A repeated close does not revoke already accepted cleanup. */
    for (i = 1; i < EPD_MEMORY_DESCRIPTOR_COUNT; ++i) {
        descriptor_t *d = &descriptors.desc[i - 1u], *p;
        bool grace = false;
        if (!d->used || !below(d, root)) continue;
        if (d->kind == EPD_MEMORY_SCOPE_CREDIT && d->state == EPD_MEMORY_CREDIT_LIVE) {
            if (root->kind != EPD_MEMORY_SCOPE_CLEANUP && root->kind != EPD_MEMORY_SCOPE_CREDIT) {
                for (p = parent_of(d); p && p != root; p = parent_of(p))
                    if (p->kind == EPD_MEMORY_SCOPE_CLEANUP) grace = true;
            }
            if (grace) d->cleanup_grace = true;
            else finish_credit(d, EPD_MEMORY_CREDIT_CANCELLED, EPD_MEMORY_CLOSED);
        } else if (d->kind == EPD_MEMORY_SCOPE_CREDIT) {
            finish_credit(d, EPD_MEMORY_CREDIT_CANCELLED, EPD_MEMORY_CLOSED);
        }
        d->closed = true;
    }
    capacity_changed();
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return true;
}

epd_memory_owner_t epd_memory_scope_create(epd_memory_owner_t parent, epd_memory_scope_kind_t kind)
{ return scope_create(parent, kind, false); }
bool epd_memory_owner_retain(epd_memory_owner_t owner)
{ return owner_retain(owner, false); }
void epd_memory_owner_release(epd_memory_owner_t owner)
{ owner_release(owner, false); }
epd_memory_owner_t epd_memory_owner_enter(epd_memory_owner_t owner)
{ return owner_enter(owner, false); }
void epd_memory_owner_leave(epd_memory_owner_t previous)
{ owner_leave(previous, false); }
bool epd_memory_owner_close(epd_memory_owner_t owner)
{ return owner_close(owner, false); }

epd_memory_owner_t epd_memory_scope_create_trusted(epd_memory_owner_t parent, epd_memory_scope_kind_t kind)
{ return scope_create(parent, kind, true); }
bool epd_memory_owner_retain_trusted(epd_memory_owner_t owner)
{ return owner_retain(owner, true); }
void epd_memory_owner_release_trusted(epd_memory_owner_t owner)
{ owner_release(owner, true); }
epd_memory_owner_t epd_memory_owner_enter_trusted(epd_memory_owner_t owner)
{ return owner_enter(owner, true); }
void epd_memory_owner_leave_trusted(epd_memory_owner_t previous)
{ owner_leave(previous, true); }
bool epd_memory_owner_close_trusted(epd_memory_owner_t owner)
{ return owner_close(owner, true); }

static void owner_snapshot(const descriptor_t *d, epd_memory_owner_snapshot_t *out)
{
    descriptor_t *account;
    memset(out, 0, sizeof(*out));
    out->owner = token_of(d);
    out->parent = d->parent;
    out->account = d->account;
    out->kind = d->kind;
    out->closed = d->closed;
    out->references = d->refs;
    out->allocations = d->blocks;
    out->allocating = d->pending_calls;
    account = lookup(d->account);
    if (account) memcpy(out->appid, account->appid, sizeof(out->appid));
}

bool epd_memory_owner_get(epd_memory_owner_t token, epd_memory_owner_snapshot_t *out)
{
    rt_base_t level;
    descriptor_t *d;
    if (!out) return false;
    level = lock_manager();
    d = lookup(token);
    if (d) owner_snapshot(d, out);
    unlock_manager(level);
    return d != NULL;
}

bool epd_memory_owner_at(unsigned slot, epd_memory_owner_snapshot_t *out)
{
    rt_base_t level;
    descriptor_t *d = NULL;
    if (!out || slot >= EPD_MEMORY_DESCRIPTOR_COUNT) return false;
    level = lock_manager();
    if (!slot) d = &manager.system;
    else if (manager.descriptors_ready && descriptors.desc[slot - 1u].used)
        d = &descriptors.desc[slot - 1u];
    if (d) owner_snapshot(d, out);
    unlock_manager(level);
    return d != NULL;
}

static bool snapshot(epd_memory_owner_t token, epd_memory_account_snapshot_t *out, bool root)
{
    rt_base_t level;
    descriptor_t *d;
    if (!out) return false;
    level = lock_manager();
    d = lookup(token);
    if (d && root) d = lookup(d->account);
    if (!d) { unlock_manager(level); return false; }
    out->account = token_of(d);
    for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        out->backend[b] = root ? d->total[b] : *subtree_usage(d, b);
        out->associated[b] = associated(d, b);
    }
    unlock_manager(level);
    return true;
}

bool epd_memory_account_snapshot(epd_memory_owner_t token, epd_memory_account_snapshot_t *out)
{ return snapshot(token, out, true); }

bool epd_memory_scope_snapshot(epd_memory_owner_t token, epd_memory_account_snapshot_t *out)
{ return snapshot(token, out, false); }

epd_memory_result_t epd_memory_owner_set_limit(epd_memory_owner_t token,
                                              epd_memory_backend_t backend, size_t bytes)
{
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(token);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (d && backend < EPD_MEMORY_BACKEND_COUNT) {
        if (d->limit[backend] != bytes) capacity_changed();
        d->limit[backend] = bytes;
        result = EPD_MEMORY_OK;
    }
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return result;
}

bool epd_memory_owner_budget(epd_memory_owner_t token, epd_memory_backend_t b,
                            epd_memory_budget_snapshot_t *out)
{
    rt_base_t level;
    descriptor_t *d;
    if (!out || b >= EPD_MEMORY_BACKEND_COUNT) return false;
    level = lock_manager();
    d = lookup(token);
    if (d) d = lookup(d->account);
    if (!d || !manager.backend[b].key) { unlock_manager(level); return false; }
    memset(out, 0, sizeof(*out));
    out->account = token_of(d);
    out->capacity = manager.backend[b].config.capacity - manager.backend[b].config.unavailable;
    out->configured_limit = d->limit[b];
    out->effective_limit = effective_limit(d, b);
    out->private_committed = committed(&d->total[b]);
    out->associated = associated(d, b);
    out->application_limit = d != &manager.system;
    out->above_limit = !budget_fits(d, b, 0);
    unlock_manager(level);
    return true;
}

epd_memory_result_t epd_memory_owner_set_backends(epd_memory_owner_t token, uint32_t mask)
{
    unsigned b;
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(token);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (d && !(mask & ~EPD_MEMORY_ALL_BACKENDS)) {
        result = EPD_MEMORY_OK;
        for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b)
            if (committed(subtree_usage(d, b)) || associated(d, b)) result = EPD_MEMORY_BUSY;
        if (result == EPD_MEMORY_OK) {
            if (d->mask != mask) capacity_changed();
            d->mask = mask;
        }
    }
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return result;
}

epd_memory_result_t epd_memory_owner_set_class(epd_memory_owner_t token, epd_memory_class_t cls)
{
    unsigned b;
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(token), *p;
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (d && cls >= EPD_MEMORY_RECLAIMABLE && cls < EPD_MEMORY_CLASS_COUNT) {
        for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
            size_t bytes = d->local[b].physical;
            for (p = d; p && (p != &manager.system || d->account == EPD_MEMORY_OWNER_SYSTEM); p = parent_of(p)) {
                p->total[b].physical[d->cls] -= bytes;
                p->total[b].physical[cls] += bytes;
            }
            manager.backend[b].usage.physical[d->cls] -= bytes;
            manager.backend[b].usage.physical[cls] += bytes;
        }
        d->cls = cls;
        result = EPD_MEMORY_OK;
    }
    unlock_manager(level);
    return result;
}

epd_memory_result_t epd_memory_backend_register(const void *key,
                          const epd_memory_backend_config_t *config,
                          epd_memory_backend_t *out)
{
    unsigned b, free_slot = EPD_MEMORY_BACKEND_COUNT, p, steps = 0;
    rt_base_t level;
    if (!key || !config || !out || config->unavailable > config->capacity ||
        config->region < EPD_MEMORY_REGION_UNKNOWN || config->region > EPD_MEMORY_REGION_ACPU)
        return EPD_MEMORY_INVALID;
    level = lock_manager();
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        if (manager.backend[b].key == key) break;
        if (!manager.backend[b].key && free_slot == EPD_MEMORY_BACKEND_COUNT) free_slot = b;
    }
    if (b == EPD_MEMORY_BACKEND_COUNT) b = free_slot;
    if (b == EPD_MEMORY_BACKEND_COUNT) {
        unlock_manager(level);
        return EPD_MEMORY_NO_METADATA;
    }
    for (p = config->parent; p != EPD_MEMORY_BACKEND_NONE;
         p = manager.backend[p].config.parent) {
        if (p >= EPD_MEMORY_BACKEND_COUNT || p == b || !manager.backend[p].key ||
            ++steps >= EPD_MEMORY_BACKEND_COUNT) {
            unlock_manager(level);
            return EPD_MEMORY_INVALID;
        }
    }
    /* Compare semantic fields: padding is not a capacity change. */
    const epd_memory_backend_config_t *previous = &manager.backend[b].config;
    if (manager.backend[b].key == key && previous->region == config->region &&
        previous->capacity == config->capacity && previous->unavailable == config->unavailable &&
        previous->parent == config->parent) {
        *out = (epd_memory_backend_t)b;
        unlock_manager(level);
        return EPD_MEMORY_OK;
    }
    manager.backend[b].key = key;
    manager.backend[b].config = *config;
    capacity_changed();
    *out = (epd_memory_backend_t)b;
    unlock_manager(level);
    return EPD_MEMORY_OK;
}

bool epd_memory_backend_find(const void *key, epd_memory_backend_t *out)
{
    unsigned b;
    rt_base_t level;
    bool ok = false;
    if (!key || !out) return false;
    level = lock_manager();
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        if (manager.backend[b].key == key) { *out = (epd_memory_backend_t)b; ok = true; break; }
    }
    unlock_manager(level);
    return ok;
}

bool epd_memory_backend_get(epd_memory_backend_t b, epd_memory_backend_snapshot_t *out)
{
    rt_base_t level;
    if (!out || b >= EPD_MEMORY_BACKEND_COUNT) return false;
    level = lock_manager();
    if (!manager.backend[b].key) { unlock_manager(level); return false; }
    *out = manager.backend[b];
    unlock_manager(level);
    return true;
}

bool epd_memory_backend_set_name(epd_memory_backend_t b, const char *name)
{
    size_t n = 0;
    rt_base_t level;
    if (!name || b >= EPD_MEMORY_BACKEND_COUNT) return false;
    while (n + 1 < EPD_MEMORY_BACKEND_NAME_SIZE && name[n]) ++n;
    level = lock_manager();
    if (!manager.backend[b].key) { unlock_manager(level); return false; }
    memcpy(manager.backend[b].name, name, n);
    manager.backend[b].name[n] = 0;
    unlock_manager(level);
    return true;
}

epd_memory_result_t epd_memory_region_snapshot(epd_memory_region_t region,
                          size_t *capacity, epd_memory_usage_t *usage)
{
    unsigned b, c;
    size_t total = 0;
    epd_memory_usage_t sum = {{0}, 0, 0, 0, 0, 0};
    rt_base_t level;
    if (!capacity || !usage || region < EPD_MEMORY_REGION_UNKNOWN || region > EPD_MEMORY_REGION_ACPU)
        return EPD_MEMORY_INVALID;
    level = lock_manager();
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        const epd_memory_backend_snapshot_t *heap = &manager.backend[b];
        size_t usable;
        if (!heap->key || heap->config.region != region || heap->config.parent != EPD_MEMORY_BACKEND_NONE)
            continue;
        usable = heap->config.capacity - heap->config.unavailable;
        if (SIZE_MAX - total < usable || UINT32_MAX - sum.allocations < heap->usage.allocations) {
            unlock_manager(level); return EPD_MEMORY_OVERFLOW;
        }
        total += usable;
        /* Configuration may have reduced a live heap below its current usage. */
        for (c = 0; c < EPD_MEMORY_CLASS_COUNT; ++c) {
            if (SIZE_MAX - sum.physical[c] < heap->usage.physical[c]) {
                unlock_manager(level); return EPD_MEMORY_OVERFLOW;
            }
            sum.physical[c] += heap->usage.physical[c];
        }
        if (SIZE_MAX - sum.sized < heap->usage.sized || SIZE_MAX - sum.unused < heap->usage.unused ||
            SIZE_MAX - sum.allocating < heap->usage.allocating ||
            SIZE_MAX - sum.peak_committed < heap->usage.peak_committed) {
            unlock_manager(level); return EPD_MEMORY_OVERFLOW;
        }
        sum.sized += heap->usage.sized;
        sum.unused += heap->usage.unused;
        sum.allocating += heap->usage.allocating;
        sum.peak_committed += heap->usage.peak_committed;
        sum.allocations += heap->usage.allocations;
    }
    *capacity = total;
    *usage = sum;
    unlock_manager(level);
    return EPD_MEMORY_OK;
}

static bool room(size_t used, size_t addition, size_t limit)
{ return addition <= limit && used <= limit - addition; }

static bool allowed_backend(const descriptor_t *d, unsigned b)
{
    while (d) {
        if (!(d->mask & (UINT32_C(1) << b))) return false;
        d = parent_of(d);
    }
    return true;
}

static bool budget_room(const descriptor_t *d, unsigned b, size_t addition)
{
    while (d) {
        if (!budget_fits(d, b, addition)) return false;
        d = parent_of(d);
    }
    return true;
}

epd_memory_result_t epd_memory_association_change(epd_memory_owner_t token,
                          epd_memory_backend_t b, size_t old_bytes, size_t new_bytes)
{
    rt_base_t level = lock_manager();
    descriptor_t *owner = lookup(token), *account = owner ? lookup(owner->account) : NULL;
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    size_t *amount;
    if (!account || b >= EPD_MEMORY_BACKEND_COUNT || !manager.backend[b].key) goto done;
    if (account == &manager.system) { result = EPD_MEMORY_OK; goto done; }
    amount = &descriptors.associated[account - descriptors.desc][b];
    if (old_bytes > *amount) goto done;
    if (new_bytes > old_bytes) {
        size_t delta = new_bytes - old_bytes;
        if (!open_path(owner)) { result = EPD_MEMORY_CLOSED; goto done; }
        if (!allowed_backend(owner, b)) { result = EPD_MEMORY_REGION; goto done; }
        if (!budget_fits(account, b, delta)) { result = EPD_MEMORY_LIMIT; goto done; }
        *amount += delta;
    } else if (new_bytes < old_bytes) {
        *amount -= old_bytes - new_bytes;
        capacity_changed();
        collect(account);
    }
    result = EPD_MEMORY_OK;
done:
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return result;
}

epd_memory_result_t epd_memory_association_batch(epd_memory_backend_t b,
                          const epd_memory_association_t *changes, unsigned count)
{
    rt_base_t level = lock_manager();
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    bool reduced = false;
    unsigned i;
    if (b >= EPD_MEMORY_BACKEND_COUNT || !manager.backend[b].key ||
        count > EPD_MEMORY_CREDIT_MAX_SOURCES || (count && !changes)) goto done;
    for (i = 0; i < count; ++i) {
        descriptor_t *d = lookup(changes[i].account);
        if (!d || d == &manager.system || d->account != changes[i].account ||
            changes[i].old_bytes > associated(d, b)) goto done;
        for (unsigned j = 0; j < i; ++j)
            if (changes[i].account == changes[j].account) goto done;
        if (changes[i].new_bytes > changes[i].old_bytes) {
            if (!open_path(d)) { result = EPD_MEMORY_CLOSED; goto done; }
            if (!allowed_backend(d, b)) { result = EPD_MEMORY_REGION; goto done; }
            if (!budget_fits(d, b, changes[i].new_bytes - changes[i].old_bytes)) {
                result = EPD_MEMORY_LIMIT; goto done;
            }
        }
    }
    for (i = 0; i < count; ++i) {
        descriptor_t *d = lookup(changes[i].account);
        size_t *amount = &descriptors.associated[d - descriptors.desc][b];
        *amount = *amount - changes[i].old_bytes + changes[i].new_bytes;
        reduced |= changes[i].new_bytes < changes[i].old_bytes;
        collect(d);
    }
    if (reduced) capacity_changed();
    result = EPD_MEMORY_OK;
done:
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return result;
}

epd_memory_result_t epd_memory_credit_associate(epd_memory_credit_t credit,
                          epd_memory_backend_t b, size_t consumed,
                          const epd_memory_association_t *changes, unsigned count)
{
    rt_base_t level = lock_manager();
    descriptor_t *source = lookup(credit);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    size_t returned;
    unsigned i;
    bool changed = false;
    if (!source || source->kind != EPD_MEMORY_SCOPE_CREDIT || b >= EPD_MEMORY_BACKEND_COUNT ||
        !manager.backend[b].key || count > EPD_MEMORY_CREDIT_MAX_SOURCES || (count && !changes)) goto done;
    if (source->state != EPD_MEMORY_CREDIT_LIVE || !open_path(source)) {
        result = EPD_MEMORY_CLOSED; goto done;
    }
    returned = consumed < source->local[b].unused ? consumed : source->local[b].unused;
    for (i = 0; i < count; ++i) {
        descriptor_t *d = lookup(changes[i].account);
        size_t old;
        if (!d || d == &manager.system || d->account != changes[i].account) goto done;
        for (unsigned j = 0; j < i; ++j)
            if (changes[i].account == changes[j].account) goto done;
        old = associated(d, b);
        if (changes[i].old_bytes > old || SIZE_MAX - (old - changes[i].old_bytes) < changes[i].new_bytes)
            goto done;
        if (changes[i].new_bytes <= changes[i].old_bytes) continue;
        if (!open_path(d)) { result = EPD_MEMORY_CLOSED; goto done; }
        if (!allowed_backend(d, b)) { result = EPD_MEMORY_REGION; goto done; }
        if (!association_fits(d, b, changes[i].old_bytes, changes[i].new_bytes,
                              changes[i].account == source->account ? returned : 0)) {
            result = EPD_MEMORY_LIMIT; goto done;
        }
    }
    adjust(source, b, UNUSED, returned, false);
    changed = returned != 0;
    for (i = 0; i < count; ++i) {
        descriptor_t *d = lookup(changes[i].account);
        size_t *amount = &descriptors.associated[d - descriptors.desc][b];
        *amount = *amount - changes[i].old_bytes + changes[i].new_bytes;
        changed |= changes[i].new_bytes < changes[i].old_bytes;
        collect(d);
    }
    if (changed) capacity_changed();
    result = EPD_MEMORY_OK;
done:
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return result;
}

static bool due(uint32_t now, uint32_t deadline)
{ return (int32_t)(now - deadline) >= 0; }

static uint32_t observe_now(uint32_t sampled)
{
    uint32_t now = now_locked();
    /* The caller may have sampled before a preemption. Keep tick elapsed time
     * and never roll the shared clock back to that stale sample. */
    if ((int32_t)(sampled - now) > 0) manager.now_ms = sampled;
    return manager.now_ms;
}

static size_t admission_headroom(const epd_memory_backend_snapshot_t *heap,
                                 epd_memory_priority_t priority)
{
    return priority >= EPD_MEMORY_PRIORITY_NEARBY ? heap->headroom : 0;
}

static size_t after_headroom(size_t bytes, size_t headroom)
{ return bytes > headroom ? bytes - headroom : 0; }

bool epd_memory_backend_set_headroom(epd_memory_backend_t b, size_t bytes)
{
    rt_base_t level;
    if (b >= EPD_MEMORY_BACKEND_COUNT) return false;
    level = lock_manager();
    if (!manager.backend[b].key) { unlock_manager(level); return false; }
    if (manager.backend[b].headroom != bytes) capacity_changed();
    manager.backend[b].headroom = bytes;
    unlock_manager(level);
    return true;
}

static bool competes(const descriptor_t *waiting, const descriptor_t *owner,
                     const size_t *want)
{
    unsigned b;
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        const epd_memory_backend_snapshot_t *heap = &manager.backend[b];
        const descriptor_t *shared;
        size_t together;
        if (!waiting->want[b] || !want[b]) continue;
        if (want[b] > SIZE_MAX - waiting->want[b]) return true;
        together = want[b] + waiting->want[b];
        size_t floor = admission_headroom(heap, waiting->priority);
        if (!room(committed(&heap->usage), together,
                  after_headroom(heap->config.capacity - heap->config.unavailable, floor))) return true;
        if (heap->available_known &&
            !room(heap->usage.unused + heap->usage.allocating, together,
                  after_headroom(heap->available, floor))) return true;
        /* A request waiting on another heap or a private budget must not stop
         * allocations that leave its shared capacity available. */
        for (shared = parent_of(waiting); shared; shared = parent_of(shared))
            if (below(owner, shared) && !budget_fits(shared, b, together)) return true;
    }
    return false;
}

static bool fairness_blocks(const descriptor_t *owner, const size_t *want, epd_memory_priority_t priority,
                            uint64_t sequence, const descriptor_t *except)
{
    unsigned i;
    for (i = 0; i < manager.wait_count; ++i) {
        descriptor_t *w = lookup(descriptors.waits[i]);
        if (!w || w == except || w->sequence >= sequence || w->priority > priority ||
            !competes(w, owner, want) || due(manager.now_ms, w->deadline)) continue;
        if (w->bypasses >= EPD_MEMORY_MAX_BYPASSES ||
            manager.now_ms - w->queued_at >= EPD_MEMORY_FAIR_WAIT_MS) return true;
    }
    return false;
}

static void note_bypass(const descriptor_t *owner, const size_t *want, epd_memory_priority_t priority,
                         uint64_t sequence, const descriptor_t *except)
{
    unsigned i;
    for (i = 0; i < manager.wait_count; ++i) {
        descriptor_t *w = lookup(descriptors.waits[i]);
        if (w && w != except && w->sequence < sequence && w->priority <= priority &&
            !due(manager.now_ms, w->deadline) && competes(w, owner, want) &&
            w->bypasses < UINT32_MAX) ++w->bypasses;
    }
}

static epd_memory_result_t fits(descriptor_t *owner, const size_t *want,
                                epd_memory_priority_t priority, uint64_t sequence,
                                descriptor_t *except)
{
    unsigned b;
    if (!open_path(owner)) return EPD_MEMORY_CLOSED;
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        epd_memory_backend_snapshot_t *heap = &manager.backend[b];
        size_t capacity, floor = admission_headroom(heap, priority);
        if (!want[b]) continue;
        if (!heap->key || !allowed_backend(owner, b)) return EPD_MEMORY_REGION;
        capacity = after_headroom(heap->config.capacity - heap->config.unavailable, floor);
        if (!room(committed(&heap->usage), want[b], capacity)) return EPD_MEMORY_NO_CAPACITY;
        if (heap->available_known &&
            !room(heap->usage.unused + heap->usage.allocating, want[b], after_headroom(heap->available, floor)))
            return EPD_MEMORY_NO_CAPACITY;
        if (!budget_room(owner, b, want[b])) return EPD_MEMORY_LIMIT;
    }
    return fairness_blocks(owner, want, priority, sequence, except) ? EPD_MEMORY_WAITING : EPD_MEMORY_OK;
}

static void grant(descriptor_t *d)
{
    unsigned b, i;
    note_bypass(d, d->want, d->priority, d->sequence, d);
    for (i = 0; i < manager.wait_count; ++i) {
        if (descriptors.waits[i] == token_of(d)) {
            --manager.wait_count;
            memmove(&descriptors.waits[i], &descriptors.waits[i + 1u],
                    (manager.wait_count - i) * sizeof(descriptors.waits[0]));
            break;
        }
    }
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) adjust(d, b, UNUSED, d->want[b], true);
    d->state = d->transaction ? EPD_MEMORY_CREDIT_PREPARED : EPD_MEMORY_CREDIT_LIVE;
    d->result = EPD_MEMORY_OK;
}

epd_memory_result_t epd_memory_credit_request(epd_memory_owner_t owner,
                          const epd_memory_credit_spec_t *spec, epd_memory_credit_t *out)
{
    unsigned b;
    bool any = false;
    rt_base_t level;
    descriptor_t *p, *d;
    epd_memory_result_t result;
    if (out) *out = INVALID;
    if (!spec || !out || spec->timeout_ms > INT32_MAX ||
        spec->priority < EPD_MEMORY_PRIORITY_INTERACTIVE ||
        spec->priority > EPD_MEMORY_PRIORITY_OPTIONAL) return EPD_MEMORY_INVALID;
    level = lock_manager();
    (void)now_locked();
    p = lookup(owner);
    if (!p || p->kind == EPD_MEMORY_SCOPE_CREDIT) { result = EPD_MEMORY_INVALID; goto done; }
    if (!open_path(p)) { result = EPD_MEMORY_CLOSED; goto done; }
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        descriptor_t *a;
        epd_memory_backend_snapshot_t *heap = &manager.backend[b];
        if (!spec->bytes[b]) continue;
        any = true;
        if (!heap->key || !allowed_backend(p, b)) { result = EPD_MEMORY_REGION; goto done; }
        if (spec->bytes[b] > after_headroom(heap->config.capacity - heap->config.unavailable,
                                           admission_headroom(heap, spec->priority))) {
            result = EPD_MEMORY_NO_CAPACITY; goto done;
        }
        for (a = p; a; a = parent_of(a)) {
            if (spec->bytes[b] > effective_limit(a, b)) { result = EPD_MEMORY_LIMIT; goto done; }
        }
    }
    if (!any) { result = EPD_MEMORY_INVALID; goto done; }
    if (manager.sequence == UINT64_MAX) { result = EPD_MEMORY_OVERFLOW; goto done; }
    result = fits(p, spec->bytes, spec->priority, manager.sequence + 1u, NULL);
    if (result != EPD_MEMORY_OK && !spec->timeout_ms) goto done;
    d = new_descriptor(p, EPD_MEMORY_SCOPE_CREDIT);
    if (!d) { result = EPD_MEMORY_NO_METADATA; goto done; }
    d->priority = spec->priority;
    d->sequence = ++manager.sequence;
    d->queued_at = manager.now_ms;
    d->deadline = manager.now_ms + spec->timeout_ms;
    memcpy(d->want, spec->bytes, sizeof(d->want));
    d->state = EPD_MEMORY_CREDIT_WAITING;
    d->result = EPD_MEMORY_WAITING;
    *out = token_of(d);
    if (result == EPD_MEMORY_OK) grant(d);
    else {
        descriptors.waits[manager.wait_count++] = *out;
        result = EPD_MEMORY_WAITING;
    }
done:
    unlock_manager(level);
    return result;
}

bool epd_memory_credit_get(epd_memory_credit_t token, epd_memory_credit_snapshot_t *out)
{
    unsigned b;
    rt_base_t level;
    descriptor_t *d;
    if (!out) return false;
    level = lock_manager();
    d = lookup(token);
    if (!d || d->kind != EPD_MEMORY_SCOPE_CREDIT) { unlock_manager(level); return false; }
    memset(out, 0, sizeof(*out));
    out->credit = token;
    out->owner = d->parent;
    out->state = d->state;
    out->result = d->result;
    out->priority = d->priority;
    out->queued_at_ms = d->queued_at;
    out->deadline_ms = d->deadline;
    out->bypasses = d->bypasses;
    if (d->transaction)
        memcpy(out->prepared_bytes, descriptors.transactions[d->transaction - 1u].target,
               sizeof(out->prepared_bytes));
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        out->unused[b] = d->local[b].unused;
        out->allocating[b] = d->local[b].pending;
        out->physical[b] = d->local[b].physical;
    }
    unlock_manager(level);
    return true;
}

static bool end_credit(epd_memory_credit_t token, bool cancel)
{
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(token);
    if (!d || d->kind != EPD_MEMORY_SCOPE_CREDIT) { unlock_manager(level); return false; }
    finish_credit(d, cancel ? EPD_MEMORY_CREDIT_CANCELLED : EPD_MEMORY_CREDIT_ENDED,
                   cancel ? EPD_MEMORY_CANCELLED : EPD_MEMORY_OK);
    collect(d);
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return true;
}

bool epd_memory_credit_end(epd_memory_credit_t token) { return end_credit(token, false); }
bool epd_memory_credit_cancel(epd_memory_credit_t token) { return end_credit(token, true); }

/* Control path only: fixed transaction pool, at most 8 * 64 source tokens. */
static bool credit_transaction_busy(const descriptor_t *a, const descriptor_t *b)
{
    unsigned i, j;
    epd_memory_owner_t at = token_of(a), bt = b ? token_of(b) : INVALID;
    if (a->transaction || (b && b->transaction)) return true;
    for (i = 0; i < EPD_MEMORY_TRANSACTION_COUNT; ++i) {
        const transaction_t *t = &descriptors.transactions[i];
        if (!t->used) continue;
        for (j = 0; j < t->count; ++j)
            if (t->sources[j] == at || t->sources[j] == bt) return true;
    }
    return false;
}

epd_memory_result_t epd_memory_credit_suspend(epd_memory_credit_t token)
{
    unsigned b;
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(token);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (!d || d->kind != EPD_MEMORY_SCOPE_CREDIT) goto done;
    if (!open_path(d)) { result = EPD_MEMORY_CLOSED; goto done; }
    if (d->pending_calls || credit_transaction_busy(d, NULL)) {
        result = EPD_MEMORY_BUSY; goto done;
    }
    if (d->state == EPD_MEMORY_CREDIT_SUSPENDED) { result = EPD_MEMORY_OK; goto done; }
    if (d->state != EPD_MEMORY_CREDIT_LIVE) { result = EPD_MEMORY_CLOSED; goto done; }
    d->state = EPD_MEMORY_CREDIT_SUSPENDED;
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b)
        adjust(d, b, UNUSED, d->local[b].unused, false);
    capacity_changed();
    result = EPD_MEMORY_OK;
done:
    unlock_manager(level);
    return result;
}

epd_memory_result_t epd_memory_credit_resume_commit(epd_memory_credit_t token,
                                                    epd_memory_credit_t grant_token)
{
    unsigned b;
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(token), *g = grant_token ? lookup(grant_token) : NULL;
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (!d || d->kind != EPD_MEMORY_SCOPE_CREDIT ||
        (grant_token && (!g || g == d || g->kind != EPD_MEMORY_SCOPE_CREDIT))) goto done;
    if (d->state != EPD_MEMORY_CREDIT_SUSPENDED || !open_path(d) || (g && !open_path(g))) {
        result = EPD_MEMORY_CLOSED; goto done;
    }
    if (g && (g->parent != d->parent || g->account != d->account)) goto done;
    if (d->pending_calls || (g && (g->pending_calls || g->blocks)) || credit_transaction_busy(d, g)) {
        result = EPD_MEMORY_BUSY; goto done;
    }
    if (g && g->state != EPD_MEMORY_CREDIT_LIVE) {
        result = g->state == EPD_MEMORY_CREDIT_WAITING ? EPD_MEMORY_BUSY : EPD_MEMORY_CLOSED;
        goto done;
    }
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        size_t bytes = g ? g->local[b].unused : 0;
        if (d->local[b].pending || (g && (g->local[b].physical || g->local[b].pending))) {
            result = EPD_MEMORY_BUSY; goto done;
        }
        if (d->local[b].unused) goto done;
        if (!bytes) continue;
        if (!allowed_backend(d, b)) { result = EPD_MEMORY_REGION; goto done; }
        if (!room(committed(&d->total[b]), bytes, SIZE_MAX)) {
            result = EPD_MEMORY_OVERFLOW; goto done;
        }
        /* Same parent: every ancestor/backend already holds this grant. */
        if (!budget_fits(d, b, bytes)) { result = EPD_MEMORY_LIMIT; goto done; }
    }
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        size_t bytes = g ? g->local[b].unused : 0;
        if (bytes) {
            adjust(g, b, UNUSED, bytes, false);
            adjust(d, b, UNUSED, bytes, true);
        }
    }
    d->state = EPD_MEMORY_CREDIT_LIVE;
    d->result = EPD_MEMORY_OK;
    if (g) {
        d->priority = g->priority;
        finish_credit(g, EPD_MEMORY_CREDIT_ENDED, EPD_MEMORY_OK);
    }
    capacity_changed();
    result = EPD_MEMORY_OK;
done:
    unlock_manager(level);
    return result;
}

epd_memory_result_t epd_memory_credit_trim(epd_memory_credit_t token,
                          epd_memory_backend_t b, size_t bytes)
{
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(token);
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (d && d->kind == EPD_MEMORY_SCOPE_CREDIT && b < EPD_MEMORY_BACKEND_COUNT &&
        d->state == EPD_MEMORY_CREDIT_LIVE && bytes <= d->local[b].unused) {
        adjust(d, b, UNUSED, bytes, false);
        if (bytes) capacity_changed();
        result = EPD_MEMORY_OK;
    }
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return result;
}

epd_memory_result_t epd_memory_credit_expand(epd_memory_credit_t token,
                          const size_t additional[EPD_MEMORY_BACKEND_COUNT])
{
    unsigned b;
    rt_base_t level;
    descriptor_t *d;
    epd_memory_result_t result;
    if (!additional) return EPD_MEMORY_INVALID;
    level = lock_manager();
    (void)now_locked();
    d = lookup(token);
    if (!d || d->kind != EPD_MEMORY_SCOPE_CREDIT) result = EPD_MEMORY_INVALID;
    else if (d->state != EPD_MEMORY_CREDIT_LIVE || !open_path(d)) result = EPD_MEMORY_CLOSED;
    else {
        result = fits(d, additional, d->priority, UINT64_MAX, NULL);
        if (result == EPD_MEMORY_OK) {
            note_bypass(d, additional, d->priority, UINT64_MAX, NULL);
            for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b)
                adjust(d, b, UNUSED, additional[b], true);
        }
    }
    unlock_manager(level);
    return result;
}

epd_memory_recovery_result_t epd_memory_recovery_admit(epd_memory_recovery_t *s,
    bool failed, bool available_known, size_t available, bool *reclaim)
{
    epd_memory_recovery_result_t action = EPD_MEMORY_RECOVERY_DENIED;
    rt_base_t level;
    descriptor_t *d;
    epd_memory_backend_snapshot_t *heap;
    size_t addition, reserved, capacity, occupied, floor = 0, want[EPD_MEMORY_BACKEND_COUNT] = {0};
    bool credit;
    if (!s || !reclaim) return action;
    *reclaim = false;
    s->result = EPD_MEMORY_INVALID; s->reason = EPD_MEMORY_REJECT_CHANGE;
    s->min_available = 0;
    level = lock_manager();
    d = lookup(s->owner);
    if (!d || current_token() != s->owner) { s->reason = EPD_MEMORY_REJECT_CONTEXT; goto done; }
    if (s->backend >= EPD_MEMORY_BACKEND_COUNT || !manager.backend[s->backend].key ||
        !allowed_backend(d, s->backend)) {
        s->result = EPD_MEMORY_REGION; s->reason = EPD_MEMORY_REJECT_BACKEND; goto done;
    }
    if (!s->physical || s->physical == SIZE_MAX) goto done;
    credit = d->kind == EPD_MEMORY_SCOPE_CREDIT;
    if (!open_path(d) && !(credit && d->state == EPD_MEMORY_CREDIT_LIVE && d->cleanup_grace)) {
        s->result = EPD_MEMORY_CLOSED; s->reason = EPD_MEMORY_REJECT_CLOSED_PATH; goto done;
    }
    if (credit && d->state != EPD_MEMORY_CREDIT_LIVE) {
        s->result = EPD_MEMORY_CLOSED; s->reason = EPD_MEMORY_REJECT_CREDIT_STATE; goto done;
    }
    heap = &manager.backend[s->backend];
    if (available_known) { heap->available = available; heap->available_known = true; }
    capacity = heap->config.capacity - heap->config.unavailable;
    occupied = physical_total(&heap->usage);
    reserved = heap->usage.unused + heap->usage.allocating;
    addition = credit ? (s->physical > d->local[s->backend].unused ?
                         s->physical - d->local[s->backend].unused : 0) : s->physical;
    if (credit && addition) floor = admission_headroom(heap, d->priority);
    capacity = after_headroom(capacity, floor);
    if (addition) {
        if (!open_path(d)) {
            s->result = EPD_MEMORY_CLOSED; s->reason = EPD_MEMORY_REJECT_CLOSED_PATH; goto done;
        }
        if (s->physical > capacity) {
            s->result = EPD_MEMORY_NO_CAPACITY; s->reason = EPD_MEMORY_REJECT_CAPACITY; goto done;
        }
        /* Releasing another account cannot repair this owner's hard limit. */
        if (!budget_room(d, s->backend, addition)) {
            s->result = EPD_MEMORY_LIMIT; s->reason = EPD_MEMORY_REJECT_LIMIT; goto done;
        }
        want[s->backend] = addition;
        (void)now_locked();
        if (fairness_blocks(d, want, credit ? d->priority : EPD_MEMORY_PRIORITY_NORMAL, UINT64_MAX, NULL)) {
            s->result = EPD_MEMORY_WAITING; s->reason = EPD_MEMORY_REJECT_FAIRNESS;
            action = EPD_MEMORY_RECOVERY_PENDING; goto done;
        }
        /* Reservations alone would still block after all physical reclamation. */
        if (!room(reserved, addition, capacity)) {
            s->result = EPD_MEMORY_WAITING; s->reason = EPD_MEMORY_REJECT_RESERVED;
            action = EPD_MEMORY_RECOVERY_PENDING; goto done;
        }
    }
    if (!room(reserved, addition, SIZE_MAX - floor)) {
        s->result = EPD_MEMORY_OVERFLOW; s->reason = EPD_MEMORY_REJECT_OVERFLOW; goto done;
    }
    s->min_available = reserved + addition + floor;
    if ((addition && !room(committed(&heap->usage), addition, capacity)) ||
        (heap->available_known && s->min_available > heap->available)) {
        s->result = EPD_MEMORY_NO_CAPACITY;
        s->reason = occupied ? EPD_MEMORY_REJECT_CAPACITY : EPD_MEMORY_REJECT_RESERVED;
        *reclaim = occupied != 0;
        action = EPD_MEMORY_RECOVERY_PENDING; goto done;
    }
    if (credit && addition) {
        /* Public expansion preserves all other reservations and fair waiting.
         * It has no callback/dispatch, so the nested core guard is safe. */
        s->result = epd_memory_credit_expand(s->owner, want);
        s->reason = EPD_MEMORY_REJECT_CREDIT_UNUSED;
        if (s->result != EPD_MEMORY_OK) goto done;
    }
    if (failed) {
        /* Admission fits, but an actual fresh allocation failed: physical
         * fragmentation can improve only while some live physical bytes exist. */
        s->result = EPD_MEMORY_NO_CAPACITY; s->reason = EPD_MEMORY_REJECT_CAPACITY;
        *reclaim = occupied != 0;
        if (*reclaim) action = EPD_MEMORY_RECOVERY_PENDING;
        goto done;
    }
    s->result = EPD_MEMORY_OK;
    action = EPD_MEMORY_RECOVERY_READY;
done:
    unlock_manager(level);
    return action;
}

static transaction_t *new_transaction(descriptor_t *d)
{
    unsigned i;
    for (i = 0; i < EPD_MEMORY_TRANSACTION_COUNT; ++i) {
        transaction_t *t = &descriptors.transactions[i];
        if (t->used) continue;
        memset(t, 0, sizeof(*t));
        t->used = true;
        d->transaction = i + 1u;
        return t;
    }
    return NULL;
}

epd_memory_result_t epd_memory_credit_replace_prepare(epd_memory_credit_t previous,
                          epd_memory_owner_t owner, const epd_memory_credit_spec_t *spec,
                          epd_memory_credit_t *prepared)
{
    size_t delta[EPD_MEMORY_BACKEND_COUNT];
    descriptor_t *src, *p, *d, *a;
    transaction_t *t;
    epd_memory_result_t result;
    unsigned b;
    rt_base_t level;
    if (prepared) *prepared = INVALID;
    if (!prepared || !spec || spec->timeout_ms > INT32_MAX ||
        spec->priority < EPD_MEMORY_PRIORITY_INTERACTIVE ||
        spec->priority > EPD_MEMORY_PRIORITY_OPTIONAL) return EPD_MEMORY_INVALID;
    level = lock_manager();
    (void)now_locked();
    src = lookup(previous); p = lookup(owner);
    if (!src || !p || src->kind != EPD_MEMORY_SCOPE_CREDIT || src->parent != owner) {
        result = EPD_MEMORY_INVALID; goto done;
    }
    if (src->state != EPD_MEMORY_CREDIT_LIVE || !open_path(src)) {
        result = EPD_MEMORY_CLOSED; goto done;
    }
    if (src->refs == UINT32_MAX || manager.sequence == UINT64_MAX) {
        result = EPD_MEMORY_OVERFLOW; goto done;
    }
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        epd_memory_backend_snapshot_t *heap = &manager.backend[b];
        delta[b] = spec->bytes[b] > src->local[b].unused ? spec->bytes[b] - src->local[b].unused : 0;
        if (!spec->bytes[b]) continue;
        if (!heap->key || !allowed_backend(p, b)) { result = EPD_MEMORY_REGION; goto done; }
        if (!delta[b]) continue; /* Existing promise survives a lowered ceiling. */
        if (spec->bytes[b] > after_headroom(heap->config.capacity - heap->config.unavailable,
                                           admission_headroom(heap, spec->priority))) {
            result = EPD_MEMORY_NO_CAPACITY; goto done;
        }
        for (a = p; a; a = parent_of(a))
            if (spec->bytes[b] > effective_limit(a, b)) { result = EPD_MEMORY_LIMIT; goto done; }
    }
    result = fits(p, delta, spec->priority, manager.sequence + 1u, NULL);
    if (result != EPD_MEMORY_OK && !spec->timeout_ms) goto done;
    d = new_descriptor(p, EPD_MEMORY_SCOPE_CREDIT);
    if (!d) { result = EPD_MEMORY_NO_METADATA; goto done; }
    t = new_transaction(d);
    if (!t) { drop(d); result = EPD_MEMORY_NO_METADATA; goto done; }
    t->replace = true;
    t->sources[0] = previous; t->count = 1;
    (void)hold(src);
    memcpy(t->target, spec->bytes, sizeof(t->target));
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b)
        t->replacement[b] = spec->bytes[b] - delta[b];
    memcpy(d->want, delta, sizeof(delta));
    d->priority = spec->priority;
    d->sequence = ++manager.sequence;
    d->queued_at = manager.now_ms;
    d->deadline = manager.now_ms + spec->timeout_ms;
    d->state = EPD_MEMORY_CREDIT_WAITING;
    d->result = EPD_MEMORY_WAITING;
    *prepared = token_of(d);
    if (result == EPD_MEMORY_OK) grant(d);
    else { descriptors.waits[manager.wait_count++] = *prepared; result = EPD_MEMORY_WAITING; }
done:
    unlock_manager(level);
    return result;
}

epd_memory_result_t epd_memory_credit_share_prepare(const epd_memory_credit_t *sources,
                          size_t count, epd_memory_owner_t shared_owner,
                          epd_memory_backend_t backend, size_t bytes,
                          epd_memory_credit_t *prepared)
{
    descriptor_t *p, *d, *src;
    transaction_t *t;
    size_t i, remaining = bytes;
    uint8_t seen[32] = {0};
    rt_base_t level;
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (prepared) *prepared = INVALID;
    if (!prepared || !sources || !count || count > EPD_MEMORY_CREDIT_MAX_SOURCES ||
        !bytes || backend >= EPD_MEMORY_BACKEND_COUNT) return result;
    level = lock_manager();
    p = lookup(shared_owner);
    if (!p || (shared_owner && (p->kind != EPD_MEMORY_SCOPE_SHARED || p->account))) goto done;
    if (!open_path(p)) { result = EPD_MEMORY_CLOSED; goto done; }
    if (!manager.backend[backend].key || !allowed_backend(p, backend)) {
        result = EPD_MEMORY_REGION; goto done;
    }
    for (i = 0; i < count; ++i) {
        unsigned slot = sources[i] & 255u;
        src = lookup(sources[i]);
        if (!src || src->kind != EPD_MEMORY_SCOPE_CREDIT || (seen[slot / 8] & (1u << (slot % 8))))
            goto done;
        seen[slot / 8] |= (uint8_t)(1u << (slot % 8));
        if (src->state != EPD_MEMORY_CREDIT_LIVE || !open_path(src)) {
            result = EPD_MEMORY_CLOSED; goto done;
        }
        if (src->refs == UINT32_MAX) { result = EPD_MEMORY_OVERFLOW; goto done; }
        remaining -= src->local[backend].unused < remaining ? src->local[backend].unused : remaining;
    }
    if (remaining) { result = EPD_MEMORY_NO_CAPACITY; goto done; }
    d = new_descriptor(p, EPD_MEMORY_SCOPE_CREDIT);
    if (!d) { result = EPD_MEMORY_NO_METADATA; goto done; }
    t = new_transaction(d);
    if (!t) { drop(d); result = EPD_MEMORY_NO_METADATA; goto done; }
    t->backend = backend;
    t->target[backend] = bytes;
    d->priority = EPD_MEMORY_PRIORITY_OPTIONAL;
    remaining = bytes;
    for (i = 0; i < count; ++i) {
        size_t take;
        src = lookup(sources[i]);
        take = src->local[backend].unused < remaining ? src->local[backend].unused : remaining;
        /* Only contributors may raise the shared grant's admission priority. */
        if (take && src->priority < d->priority) d->priority = src->priority;
        t->sources[t->count] = sources[i];
        t->takes[t->count++] = take;
        (void)hold(src);
        remaining -= take;
    }
    d->state = EPD_MEMORY_CREDIT_PREPARED;
    d->result = EPD_MEMORY_OK;
    *prepared = token_of(d);
    result = EPD_MEMORY_OK;
done:
    unlock_manager(level);
    return result;
}

static epd_memory_result_t credit_commit(epd_memory_credit_t prepared,
                          const epd_memory_association_t *associations, unsigned count, bool share)
{
    /* Low byte: shared ancestor depth; high byte: stable source account slot. */
    uint16_t source_path[EPD_MEMORY_CREDIT_MAX_SOURCES];
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(prepared), *src, *p;
    transaction_t *t;
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    unsigned i, b, first_backend, backend_end;
    if (count > EPD_MEMORY_CREDIT_MAX_SOURCES || (count && !associations)) goto done;
    if (!d || !d->transaction) goto done;
    if (d->state == EPD_MEMORY_CREDIT_WAITING) { result = EPD_MEMORY_WAITING; goto done; }
    if (d->state != EPD_MEMORY_CREDIT_PREPARED || !open_path(d)) {
        result = EPD_MEMORY_CLOSED; goto done;
    }
    t = &descriptors.transactions[d->transaction - 1u];
    if (share && t->replace) goto done;
    first_backend = t->replace ? 0 : t->backend;
    backend_end = t->replace ? EPD_MEMORY_BACKEND_COUNT : t->backend + 1u;
    for (i = 0; i < t->count; ++i) {
        src = lookup(t->sources[i]);
        if (!src || src->state != EPD_MEMORY_CREDIT_LIVE || !open_path(src)) {
            result = EPD_MEMORY_CLOSED; goto done;
        }
        source_path[i] = (uint16_t)(common_ancestor_depth(src, d) | ((src->account & 255u) << 8));
        descriptors.settlement[i] = t->takes[i];
        for (b = first_backend; b < backend_end; ++b) {
            size_t take = t->replace ? t->replacement[b] : t->takes[i];
            if (src->local[b].unused < take) { result = EPD_MEMORY_BUSY; goto done; }
        }
        if (share) {
            for (unsigned j = 0; j < count; ++j) {
                const epd_memory_association_t *change = &associations[j];
                if (change->account == src->account && change->new_bytes > change->old_bytes) {
                    size_t returned = change->new_bytes - change->old_bytes;
                    if (returned > src->local[t->backend].unused) returned = src->local[t->backend].unused;
                    if (returned > descriptors.settlement[i]) descriptors.settlement[i] = returned;
                }
            }
        }
    }
    for (i = 0; i < count; ++i) {
        const epd_memory_association_t *change = &associations[i];
        descriptor_t *account = lookup(change->account);
        size_t demand, outgoing = 0;
        if (!account || account == &manager.system || account->account != change->account) goto done;
        for (unsigned j = 0; j < i; ++j)
            if (associations[j].account == change->account) goto done;
        demand = associated(account, t->backend);
        if (change->old_bytes > demand || SIZE_MAX - (demand - change->old_bytes) < change->new_bytes)
            goto done;
        if (change->new_bytes <= change->old_bytes) continue;
        if (!open_path(account)) { result = EPD_MEMORY_CLOSED; goto done; }
        if (!allowed_backend(account, t->backend)) { result = EPD_MEMORY_REGION; goto done; }
        for (unsigned j = 0; j < t->count; ++j)
            if ((unsigned)(source_path[j] >> 8) == (change->account & 255u)) outgoing += descriptors.settlement[j];
        if (!association_fits(account, t->backend, change->old_bytes, change->new_bytes, outgoing)) {
            result = EPD_MEMORY_LIMIT; goto done;
        }
    }
    for (b = first_backend; b < backend_end; ++b) {
        if (!t->target[b]) continue;
        if (!allowed_backend(d, b)) { result = EPD_MEMORY_REGION; goto done; }
        /* Only incoming bytes from outside each destination ancestor increase
         * its commitment. Common ancestors already account for the promise. */
        for (p = d; p; p = parent_of(p)) {
            size_t incoming = 0;
            for (i = 0; i < t->count; ++i) {
                if (p->depth > (source_path[i] & 255u))
                    incoming += t->replace ? t->replacement[b] : t->takes[i];
            }
            if (incoming && !budget_fits(p, b, incoming)) {
                result = EPD_MEMORY_LIMIT; goto done;
            }
        }
    }
    for (i = 0; i < t->count; ++i) {
        src = lookup(t->sources[i]);
        for (b = first_backend; b < backend_end; ++b) {
            size_t take = t->replace ? t->replacement[b] : descriptors.settlement[i];
            adjust(src, b, UNUSED, take, false);
        }
        if (t->replace) finish_credit(src, EPD_MEMORY_CREDIT_ENDED, EPD_MEMORY_OK);
    }
    /* Settle the destination once per backend, after every validation passed. */
    for (b = first_backend; b < backend_end; ++b)
        adjust(d, b, UNUSED, t->replace ? t->replacement[b] : t->target[b], true);
    for (i = 0; i < count; ++i) {
        descriptor_t *account = lookup(associations[i].account);
        size_t *demand = &descriptors.associated[account - descriptors.desc][t->backend];
        *demand = *demand - associations[i].old_bytes + associations[i].new_bytes;
        collect(account);
    }
    d->state = EPD_MEMORY_CREDIT_LIVE;
    capacity_changed();
    release_transaction(d);
    result = EPD_MEMORY_OK;
done:
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return result;
}

epd_memory_result_t epd_memory_credit_commit(epd_memory_credit_t prepared)
{ return credit_commit(prepared, NULL, 0, false); }

epd_memory_result_t epd_memory_credit_share_commit(epd_memory_credit_t prepared,
                          const epd_memory_association_t *associations, unsigned count)
{ return credit_commit(prepared, associations, count, true); }

bool epd_memory_credit_abort(epd_memory_credit_t prepared)
{
    rt_base_t level = lock_manager();
    descriptor_t *d = lookup(prepared);
    bool active = d && d->transaction;
    if (active) finish_credit(d, EPD_MEMORY_CREDIT_CANCELLED, EPD_MEMORY_CANCELLED);
    unlock_manager(level);
    epd_memory_dispatch_changes();
    return active;
}

static epd_memory_result_t wait_feasible(descriptor_t *d)
{
    const size_t *target = d->want;
    unsigned b, i;
    if (!open_path(d)) return EPD_MEMORY_CLOSED;
    if (d->transaction) {
        transaction_t *t = &descriptors.transactions[d->transaction - 1u];
        target = t->target;
        for (i = 0; i < t->count; ++i) {
            descriptor_t *src = lookup(t->sources[i]);
            if (!src || src->state != EPD_MEMORY_CREDIT_LIVE || !open_path(src))
                return EPD_MEMORY_CLOSED;
        }
    }
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        descriptor_t *p;
        epd_memory_backend_snapshot_t *heap = &manager.backend[b];
        if (!target[b]) continue;
        if (!heap->key || !allowed_backend(d, b)) return EPD_MEMORY_REGION;
        /* Replacement waits reuse old promises on zero-growth backends. */
        if (target[b] > after_headroom(heap->config.capacity - heap->config.unavailable,
                                      d->want[b] ? admission_headroom(heap, d->priority) : 0))
            return EPD_MEMORY_NO_CAPACITY;
        /* Current occupancy can drain; a demand larger than the absolute
         * ceiling cannot. Replacement waits require the complete new target. */
        for (p = d; p; p = parent_of(p))
            if (target[b] > effective_limit(p, b)) return EPD_MEMORY_LIMIT;
    }
    return EPD_MEMORY_OK;
}

bool epd_memory_credit_reclaim_need(epd_memory_credit_t credit,
                                   size_t bytes[EPD_MEMORY_BACKEND_COUNT])
{
    rt_base_t level;
    descriptor_t *d;
    bool any = false;
    unsigned b;
    if (!bytes) return false;
    memset(bytes, 0, sizeof(size_t) * EPD_MEMORY_BACKEND_COUNT);
    level = lock_manager();
    d = lookup(credit);
    if (!d || d->kind != EPD_MEMORY_SCOPE_CREDIT || d->state != EPD_MEMORY_CREDIT_WAITING ||
        wait_feasible(d) != EPD_MEMORY_OK || due(now_locked(), d->deadline) ||
        fairness_blocks(d, d->want, d->priority, d->sequence, d)) goto done;
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        epd_memory_backend_snapshot_t *heap = &manager.backend[b];
        size_t reserved = heap->usage.unused + heap->usage.allocating;
        size_t capacity = heap->config.capacity - heap->config.unavailable;
        if (!d->want[b]) continue;
        if (!room(reserved, d->want[b], after_headroom(capacity, admission_headroom(heap, d->priority)))) goto done;
        /* The default APP ceiling is the heap capacity, so occupied cache
         * below that ceiling must be allowed to drain. A narrower scope
         * budget cannot be relieved by evicting unrelated owners. */
        for (descriptor_t *p = d; p; p = parent_of(p))
            if (effective_limit(p, b) < capacity && !budget_fits(p, b, d->want[b])) goto done;
    }
    for (b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        epd_memory_backend_snapshot_t *heap = &manager.backend[b];
        size_t reserved = heap->usage.unused + heap->usage.allocating;
        size_t floor = admission_headroom(heap, d->priority);
        size_t capacity = after_headroom(heap->config.capacity - heap->config.unavailable, floor);
        size_t used = committed(&heap->usage), need = 0;
        if (!d->want[b]) continue;
        if (!room(used, d->want[b], capacity))
            need = used > capacity ? used - capacity + d->want[b] : d->want[b] - (capacity - used);
        if (heap->available_known && reserved + d->want[b] + floor > heap->available) {
            size_t physical_need = reserved + d->want[b] + floor - heap->available;
            if (physical_need > need) need = physical_need;
        }
        bytes[b] = need;
        if (need) any = true;
    }
done:
    unlock_manager(level);
    return any;
}

void epd_memory_poll(uint32_t now_ms)
{
    unsigned step;
    /* Inspect one candidate per guard. Queue snapshots contain generation
     * tokens, so cancellation/reuse between inspections cannot change owners. */
    for (step = 0; step < EPD_MEMORY_DESCRIPTOR_COUNT; ++step) {
        unsigned i, count;
        epd_memory_owner_t candidates[EPD_MEMORY_DESCRIPTOR_COUNT];
        epd_memory_owner_t best = INVALID;
        epd_memory_priority_t best_priority = EPD_MEMORY_PRIORITY_OPTIONAL;
        uint64_t best_sequence = UINT64_MAX;
        bool granted = false;
        rt_base_t level = lock_manager();
        now_ms = observe_now(now_ms);
        count = manager.wait_count;
        if (count) memcpy(candidates, descriptors.waits, count * sizeof(candidates[0]));
        unlock_manager(level);
        for (i = 0; i < count; ++i) {
            descriptor_t *d;
            epd_memory_result_t result;
            level = lock_manager();
            now_ms = now_locked();
            d = lookup(candidates[i]);
            if (!d || d->state != EPD_MEMORY_CREDIT_WAITING) {
                unlock_manager(level); continue;
            }
            if (due(now_ms, d->deadline)) {
                finish_credit(d, EPD_MEMORY_CREDIT_TIMED_OUT, EPD_MEMORY_TIMEOUT);
                collect(d);
                unlock_manager(level);
                continue;
            }
            result = wait_feasible(d);
            if (result != EPD_MEMORY_OK) {
                finish_credit(d, EPD_MEMORY_CREDIT_REJECTED, result);
                collect(d);
                unlock_manager(level);
                continue;
            }
            result = fits(d, d->want, d->priority, d->sequence, d);
            if (result == EPD_MEMORY_OK && (best == INVALID || d->priority < best_priority ||
                (d->priority == best_priority && d->sequence < best_sequence))) {
                best = token_of(d);
                best_priority = d->priority;
                best_sequence = d->sequence;
            }
            unlock_manager(level);
        }
        if (best == INVALID) break;
        level = lock_manager();
        now_ms = now_locked();
        {
            descriptor_t *d = lookup(best);
            if (d && d->state == EPD_MEMORY_CREDIT_WAITING && !due(now_ms, d->deadline) &&
                wait_feasible(d) == EPD_MEMORY_OK &&
                fits(d, d->want, d->priority, d->sequence, d) == EPD_MEMORY_OK) {
                grant(d);
                granted = true;
            }
        }
        unlock_manager(level);
        if (!granted) break;
    }
}

bool epd_memory_next_deadline(uint32_t now_ms, uint32_t *delay_ms)
{
    unsigned i;
    uint32_t delay = UINT32_MAX;
    rt_base_t level;
    if (!delay_ms) return false;
    level = lock_manager();
    now_ms = observe_now(now_ms);
    for (i = 0; i < manager.wait_count; ++i) {
        descriptor_t *d = lookup(descriptors.waits[i]);
        uint32_t candidate;
        if (!d) continue;
        candidate = due(now_ms, d->deadline) ? 0 : d->deadline - now_ms;
        if (candidate < delay) delay = candidate;
    }
    unlock_manager(level);
    if (delay == UINT32_MAX) return false;
    *delay_ms = delay;
    return true;
}

static bool valid_change(const epd_memory_change_t *c)
{
    if (!c || c->backend >= EPD_MEMORY_BACKEND_COUNT ||
        !manager.backend[c->backend].key || c->kind < EPD_MEMORY_CHANGE_ALLOC ||
        c->kind > EPD_MEMORY_CHANGE_FREE) return false;
    if (c->kind != EPD_MEMORY_CHANGE_FREE && (!c->physical || c->size > c->physical))
        return false;
    if ((c->kind == EPD_MEMORY_CHANGE_REALLOC_INPLACE || c->kind == EPD_MEMORY_CHANGE_FREE) &&
        (!c->old_physical || c->old_size > c->old_physical)) return false;
    return true;
}

epd_memory_result_t epd_memory_try_change(const epd_memory_change_t *c)
{
    rt_base_t level = lock_manager_path(EPD_MEMORY_DIAG_ADMIT);
    descriptor_t *d = NULL;
    epd_memory_backend_snapshot_t *heap;
    size_t delta, wanted[EPD_MEMORY_BACKEND_COUNT] = {0};
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    epd_memory_rejection_reason_t reason = EPD_MEMORY_REJECT_CHANGE;
    bool inplace, live_credit;
    if (!valid_change(c) || c->kind == EPD_MEMORY_CHANGE_FREE) goto done;
    d = lookup(c->token);
    if (!d) { reason = EPD_MEMORY_REJECT_TOKEN; goto done; }
    heap = &manager.backend[c->backend];
    inplace = c->kind == EPD_MEMORY_CHANGE_REALLOC_INPLACE;
    if (inplace && (!d->local[c->backend].blocks ||
        d->local[c->backend].physical < c->old_physical ||
        d->local[c->backend].sized < c->old_size)) {
        reason = EPD_MEMORY_REJECT_ACCOUNTING; goto done;
    }
    if (d->pending_calls == UINT32_MAX || (!inplace &&
        (d->blocks == UINT32_MAX || heap->usage.allocations == UINT32_MAX))) {
        reason = EPD_MEMORY_REJECT_OVERFLOW; result = EPD_MEMORY_OVERFLOW; goto done;
    }
    /* Heap snapshots are observation only. A denied try leaves no commitment. */
    heap->available = c->available_before;
    heap->available_known = true;
    delta = inplace ? (c->physical > c->old_physical ? c->physical - c->old_physical : 0) : c->physical;
    live_credit = d->kind == EPD_MEMORY_SCOPE_CREDIT && d->state == EPD_MEMORY_CREDIT_LIVE;
    if (!inplace || delta || c->size > c->old_size) {
        if (!open_path(d) && !(live_credit && d->cleanup_grace)) {
            reason = EPD_MEMORY_REJECT_CLOSED_PATH; result = EPD_MEMORY_CLOSED; goto done;
        }
        if (d->kind == EPD_MEMORY_SCOPE_CREDIT && !live_credit) {
            reason = EPD_MEMORY_REJECT_CREDIT_STATE; result = EPD_MEMORY_CLOSED; goto done;
        }
        if (!allowed_backend(d, c->backend)) {
            reason = EPD_MEMORY_REJECT_BACKEND; result = EPD_MEMORY_REGION; goto done;
        }
    }
    if (delta) {
        if (live_credit) {
            if (d->local[c->backend].unused < delta) {
                reason = EPD_MEMORY_REJECT_CREDIT_UNUSED; result = EPD_MEMORY_NO_CAPACITY; goto done;
            }
        } else {
            if (!room(committed(&heap->usage), delta,
                      heap->config.capacity - heap->config.unavailable)) {
                reason = EPD_MEMORY_REJECT_CAPACITY; result = EPD_MEMORY_NO_CAPACITY; goto done;
            }
            if (!budget_room(d, c->backend, delta)) {
                reason = EPD_MEMORY_REJECT_LIMIT; result = EPD_MEMORY_LIMIT; goto done;
            }
            wanted[c->backend] = delta;
            (void)now_locked();
            /* Ordinary allocations are normal work; only explicit credits
             * classify speculative work as nearby or optional. */
            if (fairness_blocks(d, wanted, EPD_MEMORY_PRIORITY_NORMAL, UINT64_MAX, NULL)) {
                reason = EPD_MEMORY_REJECT_FAIRNESS; result = EPD_MEMORY_WAITING; goto done;
            }
        }
        /* Every standard hook preserves every other unconsumed reservation.
         * Actual unsplit charge is known here, before the heap is mutated. */
        if (heap->usage.unused - (live_credit ? delta : 0) > c->available_after) {
            reason = EPD_MEMORY_REJECT_RESERVED; result = EPD_MEMORY_NO_CAPACITY; goto done;
        }
        if (live_credit) adjust(d, c->backend, UNUSED, delta, false);
        else note_bypass(d, wanted, EPD_MEMORY_PRIORITY_NORMAL, UINT64_MAX, NULL);
        adjust(d, c->backend, ALLOCATING, delta, true);
    }
    ++d->pending_calls; /* Pins even zero-byte/shrinking in-place changes. */
    result = EPD_MEMORY_OK;
done:
    if (result != EPD_MEMORY_OK) rejection_record(c ? c->token : INVALID, c, d, result, reason);
    unlock_manager(level);
    return result;
}

void epd_memory_notify(const epd_memory_change_t *c)
{
    rt_base_t level = lock_manager_path(EPD_MEMORY_DIAG_SETTLE);
    descriptor_t *d;
    epd_memory_backend_snapshot_t *heap;
    bool inplace, release;
    size_t delta;
    if (!valid_change(c) || !(d = lookup(c->token))) goto bad;
    heap = &manager.backend[c->backend];
    inplace = c->kind == EPD_MEMORY_CHANGE_REALLOC_INPLACE;
    release = c->kind == EPD_MEMORY_CHANGE_FREE;
    if ((inplace || release) && (!d->local[c->backend].blocks ||
        d->local[c->backend].physical < c->old_physical ||
        d->local[c->backend].sized < c->old_size)) goto bad;
    delta = inplace ? (c->physical > c->old_physical ? c->physical - c->old_physical : 0) : c->physical;
    if (!release && (!d->pending_calls || d->local[c->backend].pending < delta)) goto bad;
    if (!release) {
        --d->pending_calls;
        adjust(d, c->backend, ALLOCATING, delta, false);
    }
    if (inplace || release) {
        adjust(d, c->backend, PHYSICAL, c->old_physical, false);
        adjust(d, c->backend, SIZED, c->old_size, false);
    }
    if (!release) {
        adjust(d, c->backend, PHYSICAL, c->physical, true);
        adjust(d, c->backend, SIZED, c->size, true);
    }
    if (!inplace) adjust(d, c->backend, BLOCKS, 1, !release);
    if (d->kind == EPD_MEMORY_SCOPE_CREDIT && d->state == EPD_MEMORY_CREDIT_LIVE) {
        size_t returned = release ? c->old_physical :
                          (inplace && c->old_physical > c->physical ? c->old_physical - c->physical : 0);
        adjust(d, c->backend, UNUSED, returned, true);
    }
    heap->available = c->available_after;
    heap->available_known = true;
    if (release || (inplace && c->physical < c->old_physical)) capacity_changed();
    collect(d);
    unlock_manager(level);
    return;
bad:
    /* A violated adapter contract is observable; never silently subtract a
     * different generation/account or publish fabricated free capacity. */
    ++manager.errors;
    unlock_manager(level);
}

epd_memory_result_t epd_memory_transfer(epd_memory_owner_t from,
                          epd_memory_owner_t to, epd_memory_backend_t b,
                          size_t size, size_t physical)
{
    rt_base_t level = lock_manager_path(EPD_MEMORY_DIAG_TRANSFER);
    descriptor_t *src = lookup(from), *dst = lookup(to), *p;
    epd_memory_result_t result = EPD_MEMORY_INVALID;
    if (!src || !dst || b >= EPD_MEMORY_BACKEND_COUNT || !physical || size > physical ||
        !src->local[b].blocks || src->local[b].physical < physical ||
        src->local[b].sized < size) goto done;
    if (src == dst) { result = EPD_MEMORY_OK; goto done; }
    /* Existing owned buffers may settle into a pre-created scope after their
     * account closes. This neither allocates nor moves liability off-account. */
    if (!open_path(dst) && src->account != dst->account) { result = EPD_MEMORY_CLOSED; goto done; }
    if (dst->kind == EPD_MEMORY_SCOPE_CREDIT) goto done;
    if (!allowed_backend(dst, b)) { result = EPD_MEMORY_REGION; goto done; }
    if (dst->blocks == UINT32_MAX) { result = EPD_MEMORY_OVERFLOW; goto done; }
    /* Common ancestors see no physical increase. */
    for (p = dst; p; p = parent_of(p)) {
        if (!below(src, p) && !budget_fits(p, b, physical)) {
            result = EPD_MEMORY_LIMIT; goto done;
        }
    }
    adjust(src, b, PHYSICAL, physical, false);
    adjust(src, b, SIZED, size, false);
    adjust(src, b, BLOCKS, 1, false);
    adjust(dst, b, PHYSICAL, physical, true);
    adjust(dst, b, SIZED, size, true);
    adjust(dst, b, BLOCKS, 1, true);
    capacity_changed();
    collect(src);
    result = EPD_MEMORY_OK;
done:
    unlock_manager(level);
    return result;
}

epd_memory_result_t epd_memory_reclaim_run(epd_memory_reclaim_fn fn, void *argument)
{
    rt_base_t level;
    if (!fn) return EPD_MEMORY_INVALID;
    level = lock_manager();
    if (level || rt_interrupt_get_nest() || rt_critical_level() || manager.reclaim_running) {
        unlock_manager(level); return EPD_MEMORY_BUSY;
    }
    manager.reclaim_running = true;
    unlock_manager(level);
    fn(argument);
    level = lock_manager();
    manager.reclaim_running = false;
    unlock_manager(level);
    return EPD_MEMORY_OK;
}

void epd_memory_metadata_snapshot(epd_memory_metadata_snapshot_t *out)
{
    unsigned i;
    rt_base_t level;
    if (!out) return;
    level = lock_manager();
    out->fixed_bytes = sizeof(manager) + sizeof(descriptors);
    out->sram_bytes = sizeof(manager);
    out->retained_bytes = sizeof(descriptors);
    out->initialized = manager.descriptors_ready;
    out->descriptors_used = manager.used;
    out->descriptors_capacity = EPD_MEMORY_DESCRIPTOR_COUNT;
    out->retired_slots = manager.retired;
    out->protocol_errors = manager.errors;
    out->transactions_used = 0;
    out->transactions_capacity = EPD_MEMORY_TRANSACTION_COUNT;
    for (i = 0; manager.descriptors_ready && i < EPD_MEMORY_TRANSACTION_COUNT; ++i)
        if (descriptors.transactions[i].used) ++out->transactions_used;
    out->reclaim_running = manager.reclaim_running;
    unlock_manager(level);
}

RTM_EXPORT(epd_memory_owner_budget);
RTM_EXPORT(epd_memory_capacity_epoch);
RTM_EXPORT(epd_memory_scope_create);
RTM_EXPORT(epd_memory_owner_current);
RTM_EXPORT(epd_memory_owner_retain);
RTM_EXPORT(epd_memory_owner_release);
RTM_EXPORT(epd_memory_owner_enter);
RTM_EXPORT(epd_memory_owner_leave);
RTM_EXPORT(epd_memory_owner_close);
RTM_EXPORT(epd_memory_owner_get);
RTM_EXPORT(epd_memory_owner_at);
RTM_EXPORT(epd_memory_account_snapshot);
RTM_EXPORT(epd_memory_scope_snapshot);
