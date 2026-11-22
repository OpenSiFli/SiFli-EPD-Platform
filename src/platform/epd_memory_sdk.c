/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_memory_manager.h"
#include "app_memory_pressure.h"
#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include "mem_map.h"

extern struct rt_memheap app_image_psram_memheap;

bool epd_memory_app_backend(epd_memory_region_t region, epd_memory_backend_t *out)
{
    return out && region == EPD_MEMORY_REGION_HCPU_PSRAM &&
           epd_memory_backend_find(&app_image_psram_memheap, out);
}
RTM_EXPORT(epd_memory_app_backend);

size_t epd_app_allocation_peak(size_t payload)
{
    const size_t header_size = RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    /* memheap.c splits only when the remainder holds a header and 12 bytes.
     * Subtract one alignment unit for the largest unsplittable remainder. */
    const size_t overhead = 2 * header_size + 12 - RT_ALIGN_SIZE;
    size_t rounded;
    if (!payload) return 0;
    if (payload > SIZE_MAX - header_size - 31) return SIZE_MAX;
    rounded = RT_ALIGN(payload, RT_ALIGN_SIZE);
#if defined(SOLUTION)
    /* The SDK's heap/psram_memheap names select cache-line rounding. Cover
     * either policy without exposing a heap address or reading live heaps. */
    size_t cached = RT_ALIGN(payload + sizeof(struct rt_memheap_item), 32) -
                    sizeof(struct rt_memheap_item);
    if (cached > rounded) rounded = cached;
#endif
    if (rounded < 12) rounded = 12;
    return rounded > SIZE_MAX - overhead ? SIZE_MAX : rounded + overhead;
}
RTM_EXPORT(epd_app_allocation_peak);

static bool recovery_fragmented(size_t physical)
{
    const struct rt_memheap_item *sentinel = app_image_psram_memheap.free_list;
    const struct rt_memheap_item *item = sentinel->next_free;
    /* Recovery after a real allocation failure, outside the IRQ guard. Bound
     * the additional walk and skip pressure if a fitting block has appeared.
     * An incomplete scan does not disprove the failure: allow bounded reclaim. */
    for (unsigned i = 0; i < 16 && item != sentinel; ++i, item = item->next_free) {
        size_t span = (size_t)((const unsigned char *)item->next - (const unsigned char *)item);
        if (span >= physical) return false;
    }
    return true;
}

static epd_memory_recovery_result_t recovery_step(epd_memory_recovery_t *s, bool failed, size_t contiguous)
{
    epd_memory_backend_t app_backend;
    bool reclaim = false;
    if (!contiguous) contiguous = s->physical;
    bool app_heap = epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &app_backend) &&
                    app_backend == s->backend;
    epd_memory_recovery_result_t result;
    if (app_heap) {
        if (rt_sem_take(&app_image_psram_memheap.lock, 0) != RT_EOK) {
            s->result = EPD_MEMORY_BUSY; s->reason = EPD_MEMORY_REJECT_ADAPTER;
            return s->state = EPD_MEMORY_RECOVERY_PENDING;
        }
        bool fragmented = failed && recovery_fragmented(contiguous);
        result = epd_memory_recovery_admit(s, fragmented, true,
            app_image_psram_memheap.available_size, &reclaim);
        rt_sem_release(&app_image_psram_memheap.lock);
    } else result = epd_memory_recovery_admit(s, failed, false, 0, &reclaim);
    if (result == EPD_MEMORY_RECOVERY_DENIED) return s->state = result;
    if (s->pressure_requested) {
        if (s->generation) {
            app_memory_pressure_status_t pressure = app_memory_pressure_query();
            if (pressure.generation != s->generation) {
                s->result = EPD_MEMORY_CANCELLED;
                return s->state = EPD_MEMORY_RECOVERY_DENIED;
            }
            if (pressure.pending) return s->state = EPD_MEMORY_RECOVERY_PENDING;
            if (pressure.result != APP_MEMORY_PRESSURE_SATISFIED) {
                s->result = pressure.result == APP_MEMORY_PRESSURE_TIMED_OUT ?
                            EPD_MEMORY_TIMEOUT : EPD_MEMORY_NO_CAPACITY;
                return s->state = EPD_MEMORY_RECOVERY_DENIED;
            }
        }
        if (result == EPD_MEMORY_RECOVERY_PENDING && reclaim) {
            /* Reclaiming enough total bytes does not prove a contiguous span
             * exists. Upgrade once after a real allocator failure, preserving
             * the captured context and the original recovery deadline. */
            if (app_heap && failed && !s->fragmentation_requested) {
                s->fragmentation_requested = true;
                s->generation = app_memory_pressure_allocation_failed_block(s->min_available, contiguous);
                return s->state = EPD_MEMORY_RECOVERY_PENDING;
            }
            s->result = EPD_MEMORY_NO_CAPACITY;
            return s->state = EPD_MEMORY_RECOVERY_DENIED;
        }
    } else if (result == EPD_MEMORY_RECOVERY_PENDING && reclaim && app_heap) {
        /* A single application-PSRAM pressure request, after releasing all
         * allocator/core locks. Other backend shortages never reclaim this pool. */
        s->pressure_requested = true;
        s->fragmentation_requested = failed;
        s->generation = failed ? app_memory_pressure_allocation_failed_block(s->min_available, contiguous) :
                                 app_memory_pressure_request(s->min_available);
    }
    return s->state = result;
}

static size_t recovery_allocation_span(epd_memory_backend_t backend, size_t physical, size_t payload)
{
    epd_memory_backend_t app;
    if (!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &app) || backend != app) return 0;
    size_t span = app_memory_pressure_allocation_span(payload);
    return span <= physical ? span : 0;
}

static epd_memory_recovery_result_t recovery_begin(epd_memory_recovery_t *s,
    epd_memory_backend_t backend, size_t physical, bool failed, size_t contiguous)
{
    if (!s || s->used) return EPD_MEMORY_RECOVERY_DENIED;
    memset(s, 0, sizeof(*s));
    s->used = true; s->backend = backend; s->physical = physical;
    s->started = (uint32_t)rt_tick_get();
#ifdef RT_USING_THREAD_MEM_CONTEXT
    s->owner = rt_thread_mem_context_get(RT_NULL);
#else
    s->owner = EPD_MEMORY_OWNER_SYSTEM;
#endif
    return recovery_step(s, failed, contiguous);
}
epd_memory_recovery_result_t epd_memory_recovery_begin(epd_memory_recovery_t *s,
    epd_memory_backend_t backend, size_t physical, bool failed)
{
    return recovery_begin(s, backend, physical, failed, 0);
}
RTM_EXPORT(epd_memory_recovery_begin);

epd_memory_recovery_result_t epd_memory_recovery_begin_allocation(epd_memory_recovery_t *s,
    epd_memory_backend_t backend, size_t physical, size_t payload, bool failed)
{
    size_t span = recovery_allocation_span(backend, physical, payload);
    if (!span) return EPD_MEMORY_RECOVERY_DENIED;
    return recovery_begin(s, backend, physical, failed, span);
}
RTM_EXPORT(epd_memory_recovery_begin_allocation);

epd_memory_recovery_result_t epd_memory_recovery_poll(epd_memory_recovery_t *s, bool cancelled)
{
    const uint32_t timeout = (uint32_t)(((uint64_t)APP_MEMORY_PRESSURE_TIMEOUT_MS * RT_TICK_PER_SECOND + 999u) / 1000u);
    if (!s || !s->used || s->state == EPD_MEMORY_RECOVERY_DENIED) return EPD_MEMORY_RECOVERY_DENIED;
    if (cancelled || (uint32_t)((uint32_t)rt_tick_get() - s->started) >= timeout) {
        s->result = cancelled ? EPD_MEMORY_CANCELLED : EPD_MEMORY_TIMEOUT;
        return s->state = EPD_MEMORY_RECOVERY_DENIED;
    }
    return recovery_step(s, false, 0);
}
RTM_EXPORT(epd_memory_recovery_poll);

epd_memory_recovery_result_t epd_memory_recovery_continue(epd_memory_recovery_t *next,
    epd_memory_recovery_t *previous)
{
    if (!next || !previous || next == previous || next->used || !previous->used ||
        previous->state == EPD_MEMORY_RECOVERY_DENIED) return EPD_MEMORY_RECOVERY_DENIED;
#ifdef RT_USING_THREAD_MEM_CONTEXT
    epd_memory_owner_t owner = rt_thread_mem_context_get(RT_NULL);
#else
    epd_memory_owner_t owner = EPD_MEMORY_OWNER_SYSTEM;
#endif
    if (owner == previous->owner) return EPD_MEMORY_RECOVERY_DENIED;
    *next = *previous;
    previous->result = EPD_MEMORY_CANCELLED;
    previous->state = EPD_MEMORY_RECOVERY_DENIED;
    next->owner = owner;
    return epd_memory_recovery_poll(next, false);
}
RTM_EXPORT(epd_memory_recovery_continue);

static epd_memory_recovery_result_t recovery_failed(epd_memory_recovery_t *s, size_t contiguous)
{
    const uint32_t timeout = (uint32_t)(((uint64_t)APP_MEMORY_PRESSURE_TIMEOUT_MS * RT_TICK_PER_SECOND + 999u) / 1000u);
    if (!s || !s->used || s->state != EPD_MEMORY_RECOVERY_READY) return EPD_MEMORY_RECOVERY_DENIED;
    if ((uint32_t)((uint32_t)rt_tick_get() - s->started) >= timeout) {
        s->result = EPD_MEMORY_TIMEOUT;
        return s->state = EPD_MEMORY_RECOVERY_DENIED;
    }
    return recovery_step(s, true, contiguous);
}
epd_memory_recovery_result_t epd_memory_recovery_failed(epd_memory_recovery_t *s)
{
    return recovery_failed(s, 0);
}
RTM_EXPORT(epd_memory_recovery_failed);

epd_memory_recovery_result_t epd_memory_recovery_failed_allocation(epd_memory_recovery_t *s, size_t payload)
{
    size_t span = s ? recovery_allocation_span(s->backend, s->physical, payload) : 0;
    if (!span) return EPD_MEMORY_RECOVERY_DENIED;
    return recovery_failed(s, span);
}
RTM_EXPORT(epd_memory_recovery_failed_allocation);


epd_memory_result_t epd_memory_block_get(const void *ptr, epd_memory_block_snapshot_t *out)
{
#ifdef RT_USING_MEMHEAP_OWNER
    const size_t header_size = RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    const struct rt_memheap_item *item;
    struct rt_memheap *heap;
    epd_memory_owner_snapshot_t owner;
    epd_memory_block_snapshot_t result = {0};
    bool valid;
    if (!ptr || !out) return EPD_MEMORY_INVALID;
    item = (const struct rt_memheap_item *)((const unsigned char *)ptr - header_size);
    heap = item->pool_ptr;
    if (rt_sem_take(&heap->lock, 0) != RT_EOK) return EPD_MEMORY_BUSY;
    valid = epd_memory_backend_find(heap, &result.backend) && epd_memory_owner_get(item->owner, &owner);
    if (valid) {
        result.owner = item->owner;
        result.account = owner.account;
        result.size = item->size;
        result.physical = (size_t)((const unsigned char *)item->next - (const unsigned char *)item);
    }
    rt_sem_release(&heap->lock);
    if (!valid) return EPD_MEMORY_INVALID;
    *out = result;
    return EPD_MEMORY_OK;
#else
    (void)ptr; (void)out;
    return EPD_MEMORY_INVALID;
#endif
}
RTM_EXPORT(epd_memory_block_get);

epd_memory_result_t epd_memory_sdk_register_heap(const void *heap,
                          const epd_memory_backend_config_t *config,
                          epd_memory_backend_t *out)
{
    epd_memory_result_t result = epd_memory_backend_register(heap, config, out);
    epd_memory_dispatch_changes();
    return result;
}

static bool range_inside(uintptr_t outer, size_t length, uintptr_t inner, size_t inner_length)
{
    return inner >= outer && inner - outer <= length && inner_length <= length - (inner - outer);
}

static epd_memory_region_t heap_region(const struct rt_memheap *heap)
{
    uintptr_t address = (uintptr_t)heap->start_addr;
#if defined(ACPU_RAM_DATA_START_ADDR) && defined(ACPU_RAM_DATA_SIZE)
    if (range_inside(ACPU_RAM_DATA_START_ADDR, ACPU_RAM_DATA_SIZE, address, heap->pool_size))
        return EPD_MEMORY_REGION_ACPU;
#endif
#if defined(HCPU_RAM_DATA_START_ADDR) && defined(HCPU_RAM_DATA_SIZE)
    if (range_inside(HCPU_RAM_DATA_START_ADDR, HCPU_RAM_DATA_SIZE, address, heap->pool_size))
#else
    if (range_inside(HPSYS_RAM0_BASE, HPSYS_RAM_SIZE, address, heap->pool_size))
#endif
        return EPD_MEMORY_REGION_SRAM;
    if (range_inside(PSRAM_DATA_START_ADDR, PSRAM_DATA_SIZE, address, heap->pool_size))
        return EPD_MEMORY_REGION_HCPU_PSRAM;
#if defined(ACPU_PSRAM_START_ADDR) && defined(ACPU_PSRAM_SIZE)
    if (range_inside(ACPU_PSRAM_START_ADDR, ACPU_PSRAM_SIZE, address, heap->pool_size))
        return EPD_MEMORY_REGION_ACPU;
#endif
    return EPD_MEMORY_REGION_UNKNOWN;
}

epd_memory_result_t epd_memory_sdk_init(void)
{
    epd_memory_result_t initialized = epd_memory_init();
    if (initialized != EPD_MEMORY_OK) return initialized;
    struct rt_memheap *heaps[EPD_MEMORY_BACKEND_COUNT];
    epd_memory_backend_t ids[EPD_MEMORY_BACKEND_COUNT];
    epd_memory_backend_config_t configs[EPD_MEMORY_BACKEND_COUNT];
    struct rt_object_information *objects;
    rt_list_t *node;
    unsigned count = 0, i, j;
    bool overflow = false;
    rt_base_t level = rt_hw_interrupt_disable();
    objects = rt_object_get_information(RT_Object_Class_MemHeap);
    if (objects) {
        for (node = objects->object_list.next; node != &objects->object_list; node = node->next) {
            if (count == EPD_MEMORY_BACKEND_COUNT) { overflow = true; break; }
            heaps[count++] = (struct rt_memheap *)rt_list_entry(node, struct rt_object, list);
        }
    }
    rt_hw_interrupt_enable(level);
    /* Startup/quiescent registration: heap identity/backing is immutable while
     * registered. No heap locks, allocation, logging or callbacks are needed. */
    for (i = 0; i < count; ++i) {
        epd_memory_result_t result;
        epd_memory_backend_snapshot_t existing;
        char name[EPD_MEMORY_BACKEND_NAME_SIZE];
        unsigned n = 0;
        configs[i].region = heap_region(heaps[i]);
        /* The terminal sentinel cannot belong to any allocation. Free-list
         * headers are already reflected by each event's exact available value. */
        configs[i].capacity = heaps[i]->pool_size - RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
        configs[i].unavailable = 0;
        configs[i].parent = EPD_MEMORY_BACKEND_NONE;
        if (epd_memory_backend_find(heaps[i], &ids[i]) && epd_memory_backend_get(ids[i], &existing)) {
            configs[i].unavailable = existing.config.unavailable;
            configs[i].parent = existing.config.parent;
        }
        result = epd_memory_sdk_register_heap(heaps[i], &configs[i], &ids[i]);
        if (result != EPD_MEMORY_OK) return result;
        while (n < RT_NAME_MAX && n + 1 < sizeof(name) && heaps[i]->parent.name[n]) {
            name[n] = heaps[i]->parent.name[n]; ++n;
        }
        name[n] = 0;
        (void)epd_memory_backend_set_name(ids[i], name);
        if (heaps[i] == &app_image_psram_memheap)
            (void)epd_memory_backend_set_headroom(ids[i], 1024u * 1024u);
    }
    for (i = 0; i < count; ++i) {
        size_t nearest = SIZE_MAX;
        for (j = 0; j < count; ++j) {
            if (i == j || heaps[j]->pool_size <= heaps[i]->pool_size || heaps[j]->pool_size >= nearest)
                continue;
            if (range_inside((uintptr_t)heaps[j]->start_addr, heaps[j]->pool_size,
                             (uintptr_t)heaps[i]->start_addr, heaps[i]->pool_size)) {
                configs[i].parent = ids[j];
                nearest = heaps[j]->pool_size;
            }
        }
        if (configs[i].parent != EPD_MEMORY_BACKEND_NONE) {
            epd_memory_result_t result = epd_memory_sdk_register_heap(heaps[i], &configs[i], &ids[i]);
            if (result != EPD_MEMORY_OK) return result;
        }
    }
    return overflow ? EPD_MEMORY_NO_METADATA : EPD_MEMORY_OK;
}

#ifdef RT_USING_MEMHEAP_OWNER
static bool heap_backend(struct rt_memheap *heap, epd_memory_backend_t *backend)
{
    epd_memory_backend_config_t config;
    if (!heap) return false;
    if (epd_memory_backend_find(heap, backend)) return true;
    /* Boot allocations are accounted before named heap policy is installed.
     * Startup refines this same key, preserving its identity and live usage. */
    config.region = EPD_MEMORY_REGION_UNKNOWN;
    config.capacity = heap->pool_size - RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    config.unavailable = 0;
    config.parent = EPD_MEMORY_BACKEND_NONE;
    return epd_memory_backend_register(heap, &config, backend) == EPD_MEMORY_OK;
}

static bool translate(const struct rt_memheap_owner_event *event, epd_memory_change_t *change)
{
    if (!event) {
        memset(change, 0, sizeof(*change));
        change->backend = EPD_MEMORY_BACKEND_NONE;
        change->token = EPD_MEMORY_OWNER_INVALID;
        change->kind = (epd_memory_change_kind_t)255;
        return false;
    }
    change->backend = EPD_MEMORY_BACKEND_NONE;
    change->token = event->owner;
    change->size = event->size;
    change->physical = event->physical;
    change->old_size = event->old_size;
    change->old_physical = event->old_physical;
    change->available_before = event->available_before;
    change->available_after = event->available_after;
    switch (event->operation) {
    case RT_MEMHEAP_OWNER_ALLOC: change->kind = EPD_MEMORY_CHANGE_ALLOC; break;
    case RT_MEMHEAP_OWNER_REALLOC_INPLACE: change->kind = EPD_MEMORY_CHANGE_REALLOC_INPLACE; break;
    case RT_MEMHEAP_OWNER_REALLOC_ALLOC: change->kind = EPD_MEMORY_CHANGE_REALLOC_ALLOC; break;
    case RT_MEMHEAP_OWNER_FREE: change->kind = EPD_MEMORY_CHANGE_FREE; break;
    default: change->kind = (epd_memory_change_kind_t)255; return false;
    }
    return heap_backend(event->heap, &change->backend);
}

rt_err_t rt_memheap_owner_resolve(rt_uint32_t *owner)
{
    epd_memory_owner_snapshot_t snapshot;
    rt_base_t level;
    uint32_t token = 0;
    if (!owner) return -RT_EINVAL;
    level = rt_hw_interrupt_disable();
#ifdef RT_USING_THREAD_MEM_CONTEXT
    token = rt_thread_mem_context_get(RT_NULL);
#endif
    if (!epd_memory_owner_get(token, &snapshot)) {
        epd_memory_rejection_adapter(token, NULL, EPD_MEMORY_REJECT_CONTEXT);
        rt_hw_interrupt_enable(level);
        return -RT_ENOMEM;
    }
    *owner = token;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

rt_err_t rt_memheap_owner_try_change(const struct rt_memheap_owner_event *event)
{
    epd_memory_change_t change;
    if (!translate(event, &change)) {
        epd_memory_rejection_adapter(change.token, &change, EPD_MEMORY_REJECT_ADAPTER);
        return -RT_ENOMEM;
    }
    return epd_memory_try_change(&change) == EPD_MEMORY_OK ? RT_EOK : -RT_ENOMEM;
}

void rt_memheap_owner_notify(const struct rt_memheap_owner_event *event)
{
    epd_memory_change_t change;
    /* A successful try already registered the backend and pinned its token.
     * FREE always refers to a previously committed allocation. */
    if (translate(event, &change)) epd_memory_notify(&change);
}

void rt_memheap_owner_post_change(void)
{
    epd_memory_dispatch_changes();
}

rt_err_t rt_memheap_owner_try_transfer(const struct rt_memheap_owner_event *event,
                                      rt_uint32_t new_owner)
{
    epd_memory_backend_t backend;
    if (!event || !heap_backend(event->heap, &backend)) return -RT_EINVAL;
    return epd_memory_transfer(event->owner, new_owner, backend, event->size, event->physical)
           == EPD_MEMORY_OK ? RT_EOK : -RT_ENOMEM;
}
#endif

#ifdef RT_USING_THREAD_MEM_CONTEXT
void rt_thread_mem_context_invoke(rt_thread_t thread, void (*callback)(rt_thread_t))
{
    epd_memory_owner_t previous;
    RT_ASSERT(thread && callback);
    if (!thread || !callback) return;
    previous = epd_memory_owner_enter_trusted(rt_thread_mem_context_get(thread));
    RT_ASSERT(previous != EPD_MEMORY_OWNER_INVALID);
    if (previous == EPD_MEMORY_OWNER_INVALID) return;
    callback(thread);
    epd_memory_owner_leave_trusted(previous);
}

void rt_thread_mem_context_post_change(void)
{
    epd_memory_dispatch_changes();
}

void rt_thread_mem_context_changed(rt_thread_t thread,
                                  enum rt_thread_mem_context_reason reason,
                                  rt_uint32_t old_context, rt_uint32_t new_context)
{
    (void)reason;
    /* SDK holds its interrupt guard. Acquire first: same-account switches and
     * inherited contexts must never expose a zero-reference reuse window. */
    if (!epd_memory_owner_retain_trusted(new_context) && thread)
        thread->mem_context = EPD_MEMORY_OWNER_INVALID;
    if (old_context != EPD_MEMORY_OWNER_INVALID) epd_memory_owner_release_trusted(old_context);
}
#endif

epd_memory_result_t epd_memory_block_transfer(void *ptr, epd_memory_owner_t to)
{
#ifdef RT_USING_MEMHEAP_OWNER
    if (!ptr || !epd_memory_owner_is_current_account(to)) return EPD_MEMORY_INVALID;
    return rt_memheap_owner_transfer(ptr, to) == RT_EOK ? EPD_MEMORY_OK : EPD_MEMORY_BUSY;
#else
    (void)ptr;
    (void)to;
    return EPD_MEMORY_INVALID;
#endif
}

RTM_EXPORT(epd_memory_block_transfer);

epd_memory_result_t epd_memory_block_transfer_try(void *ptr, epd_memory_owner_t to)
{
#ifdef RT_USING_MEMHEAP_OWNER
    if (!ptr || !epd_memory_owner_is_current_account(to)) return EPD_MEMORY_INVALID;
    return rt_memheap_owner_transfer_try(ptr, to) == RT_EOK ? EPD_MEMORY_OK : EPD_MEMORY_BUSY;
#else
    (void)ptr;
    (void)to;
    return EPD_MEMORY_INVALID;
#endif
}

RTM_EXPORT(epd_memory_block_transfer_try);

epd_memory_result_t epd_memory_block_transfer_try_trusted(void *ptr, epd_memory_owner_t to)
{
#ifdef RT_USING_MEMHEAP_OWNER
    if (!ptr || to == EPD_MEMORY_OWNER_INVALID) return EPD_MEMORY_INVALID;
    return rt_memheap_owner_transfer_try(ptr, to) == RT_EOK ? EPD_MEMORY_OK : EPD_MEMORY_BUSY;
#else
    (void)ptr; (void)to;
    return EPD_MEMORY_INVALID;
#endif
}

#if defined(RT_USING_FINSH) && !defined(EPD_MEMORY_HOST)
#include <finsh.h>
#include <stdlib.h>
#include <string.h>

static bool diagnostic_number(const char *text, uint32_t *out)
{
    char *end;
    unsigned long value;
    if (!text || !*text || *text == '-') return false;
    value = strtoul(text, &end, 0);
    if (*end || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static int mem_diag(int argc, char **argv)
{
    epd_memory_diagnostic_snapshot_t snapshot;
    static const char *const names[EPD_MEMORY_DIAG_COUNT] = {"control", "admit", "settle", "transfer"};
    if (argc > 1 && !strcmp(argv[1], "reject")) {
        epd_memory_rejection_snapshot_t r;
        static const char *const reasons[] = {
            "change", "token", "accounting", "overflow", "closed_path", "credit_state", "backend",
            "credit_unused", "capacity", "limit", "fairness", "reserved", "context", "adapter"
        };
        if (argc > 2 && !strcmp(argv[2], "on")) {
            uint32_t count = 32, ms = 10000;
            if (argc > 5 || (argc > 3 && !diagnostic_number(argv[3], &count)) ||
                (argc > 4 && !diagnostic_number(argv[4], &ms))) return -RT_EINVAL;
            epd_memory_result_t result = epd_memory_rejection_start(count, ms);
            rt_kprintf("mem_diag reject start=%u count=%u ms=%u\n", (unsigned)result, count, ms);
            return result == EPD_MEMORY_OK ? RT_EOK : -RT_ERROR;
        }
        if (argc == 3 && !strcmp(argv[2], "off")) epd_memory_rejection_stop();
        else if (argc != 2 && (argc != 3 || strcmp(argv[2], "show"))) return -RT_EINVAL;
        if (!epd_memory_rejection_read(&r)) return -RT_ERROR;
        rt_kprintf("reject active=%u valid=%u count=%u remaining=%u exhausted=%u timeout=%u\n",
            (unsigned)r.active, (unsigned)r.valid, r.captured, r.remaining,
            (unsigned)r.exhausted, (unsigned)r.timed_out);
        if (r.valid) {
            rt_kprintf("token=0x%08x current=0x%08x account=0x%08x backend=%u kind=%u reason=%s result=%u scope=%u state=%u closed=%u tick=%u\n",
                r.token, r.current_token, r.account, (unsigned)r.backend, (unsigned)r.kind,
                r.reason < sizeof(reasons) / sizeof(reasons[0]) ? reasons[r.reason] : "unknown",
                (unsigned)r.result, (unsigned)r.scope,
                (unsigned)r.credit_state, (unsigned)r.closed, r.tick);
            rt_kprintf("size=%u physical=%u old_physical=%u delta=%u unused=%u reserved=%u available_after=%u\n",
                (unsigned)r.size, (unsigned)r.physical, (unsigned)r.old_physical,
                (unsigned)r.delta, (unsigned)r.unused, (unsigned)r.reserved, (unsigned)r.available_after);
        }
        return RT_EOK;
    }
    if (argc > 1 && !strcmp(argv[1], "on")) {
        uint32_t samples = 10000, ms = 2000;
        if (argc > 4 || (argc > 2 && !diagnostic_number(argv[2], &samples)) ||
            (argc > 3 && !diagnostic_number(argv[3], &ms))) return -RT_EINVAL;
        epd_memory_result_t result = epd_memory_diagnostic_start(samples, ms);
        rt_kprintf("mem_diag start=%u samples=%u ms=%u\n", (unsigned)result, samples, ms);
        return result == EPD_MEMORY_OK ? RT_EOK : -RT_ERROR;
    }
    if (argc == 4 && !strcmp(argv[1], "budget")) {
        uint32_t token, backend;
        epd_memory_budget_snapshot_t budget;
        if (!diagnostic_number(argv[2], &token) || !diagnostic_number(argv[3], &backend) ||
            backend >= EPD_MEMORY_BACKEND_COUNT || !epd_memory_owner_budget(token, (epd_memory_backend_t)backend, &budget))
            return -RT_EINVAL;
        rt_kprintf("account=0x%08x backend=%u app_cap=%u capacity=%u configured=%u effective=%u private=%u associated=%u above=%u\n",
            budget.account, backend, (unsigned)budget.application_limit, (unsigned)budget.capacity,
            (unsigned)budget.configured_limit, (unsigned)budget.effective_limit,
            (unsigned)budget.private_committed, (unsigned)budget.associated, (unsigned)budget.above_limit);
        return RT_EOK;
    }
    if (argc == 2 && !strcmp(argv[1], "off")) epd_memory_diagnostic_stop();
    else if (argc > 1 && (argc != 2 || strcmp(argv[1], "show"))) {
        rt_kprintf("mem_diag [show|off|on [samples<=1000000] [ms<=60000]|budget owner backend|reject [show|off|on [count<=1024] [ms<=60000]]]\n");
        return -RT_EINVAL;
    }
    if (!epd_memory_diagnostic_read(&snapshot)) return -RT_ERROR;
    rt_kprintf("active=%u remaining=%u exhausted=%u timeout=%u start_hz=%u nested=%u; guarded core cycles\n",
        (unsigned)snapshot.active, snapshot.remaining, (unsigned)snapshot.exhausted,
        (unsigned)snapshot.timed_out, snapshot.frequency_hz, snapshot.nested_calls);
    for (unsigned i = 0; i < EPD_MEMORY_DIAG_COUNT; ++i)
        rt_kprintf("%s samples=%u max_cycles=%u entered_irq_off=%u\n", names[i], snapshot.path[i].samples,
            snapshot.path[i].max_cycles, snapshot.path[i].entered_irq_off);
    return RT_EOK;
}
MSH_CMD_EXPORT(mem_diag, bounded memory timing and account budget);
#endif
