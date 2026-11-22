/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_memory_manager.h"
#include "epd_resource.h"
#include "app_background.h"
#include "epd_input_test.h"
#include <rtthread.h>
#include <string.h>

#ifdef RT_USING_FINSH
#if EPD_INPUT_TEST_ENABLED && defined(RT_USING_MEMHEAP) && !defined(RT_USING_MEMHEAP2)
static int mem_topology(int argc, char **argv)
{
    struct heap_sample {
        char name[RT_NAME_MAX + 1];
        rt_uint32_t used, free, largest, available, max_used, items;
        int valid;
    } samples[8] = {0};
    unsigned count = 0, omitted = 0, threads = 0;
    const rt_size_t header = RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    struct rt_object_information *info;
    struct rt_list_node *node;
    (void)argv;
    if (argc != 1) return -RT_EINVAL;

    /* One-shot diagnostics: keep heap objects alive, skip locked heaps, and
     * print only after scheduling and allocation can proceed normally. */
    rt_enter_critical();
    info = rt_object_get_information(RT_Object_Class_MemHeap);
    rt_list_for_each(node, &info->object_list) {
        struct rt_memheap *heap = rt_list_entry(node, struct rt_memheap, parent.list);
        if (count == sizeof(samples) / sizeof(samples[0])) { ++omitted; continue; }
        struct heap_sample *sample = &samples[count++];
        rt_memcpy(sample->name, heap->parent.name, RT_NAME_MAX);
        if (rt_sem_take(&heap->lock, 0) != RT_EOK) continue;
        uintptr_t tail = (uintptr_t)heap->start_addr + heap->pool_size - header;
        struct rt_memheap_item *item = heap->block_list;
        sample->available = heap->available_size;
        sample->max_used = heap->max_used_size;
        while ((uintptr_t)item < tail) {
            uintptr_t current = (uintptr_t)item;
            if (current < (uintptr_t)heap->start_addr ||
                current % RT_ALIGN_SIZE ||
                (item->magic & 0xfffffffeu) != 0x1ea01ea0u ||
                (uintptr_t)item->next < current + header || (uintptr_t)item->next > tail)
                break;
            if (item->magic & 1u) ++sample->used;
            else {
                rt_size_t size = (uintptr_t)item->next - current - header;
                ++sample->free;
                if (size > sample->largest) sample->largest = size;
            }
            item = item->next;
        }
        sample->valid = (uintptr_t)item == tail;
        /* Include the terminal block and free-list header embedded in heap. */
        sample->items = sample->used + sample->free + 2;
        rt_sem_release(&heap->lock);
    }
    info = rt_object_get_information(RT_Object_Class_Thread);
    rt_list_for_each(node, &info->object_list) ++threads;
    rt_exit_critical();

    rt_kprintf("mem_topology heaps=%u omitted=%u threads=%u item_bytes=%u thread_bytes=%u\n",
        count, omitted, threads, (unsigned)header, (unsigned)sizeof(struct rt_thread));
    for (unsigned i = 0; i < count; ++i) {
        const struct heap_sample *sample = &samples[i];
        rt_kprintf("topology name=%s valid=%d used=%lu free_blocks=%lu items=%lu available=%lu largest=%lu max_used=%lu\n",
            sample->name, sample->valid, (unsigned long)sample->used,
            (unsigned long)sample->free, (unsigned long)sample->items,
            (unsigned long)sample->available, (unsigned long)sample->largest,
            (unsigned long)sample->max_used);
    }
    return RT_EOK;
}
MSH_CMD_EXPORT(mem_topology, One-shot heap topology for memory regression);

#ifdef RT_USING_MEMHEAP_OWNER
#define MEM_BLOCK_BATCH 16u
struct mem_block_sample {
    uintptr_t address;
    rt_uint32_t size, physical, owner, ret_addr;
};

/* Never carry an item pointer across unlock: free/coalescing may invalidate
 * it. Re-find the address cursor from the live list on each batch. Object
 * lookup and the heap walk cannot race detach; printing is outside both locks.
 * This is a rolling observation, not an atomic snapshot of all three heaps. */
static const char *mem_block_batch(const void *key, uintptr_t *cursor,
    struct mem_block_sample *samples, unsigned *count)
{
    const uintptr_t header = RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    struct rt_object_information *info;
    struct rt_list_node *node;
    const char *status = "gone";
    *count = 0;
    rt_enter_critical();
    info = rt_object_get_information(RT_Object_Class_MemHeap);
    if (!info) goto done;
    rt_list_for_each(node, &info->object_list) {
        struct rt_memheap *heap = rt_list_entry(node, struct rt_memheap, parent.list);
        if (heap != key) continue;
        status = "busy";
        if (rt_sem_take(&heap->lock, 0) != RT_EOK) break;
        uintptr_t start = (uintptr_t)heap->start_addr;
        uintptr_t current = (uintptr_t)heap->block_list;
        uintptr_t tail = start + heap->pool_size - header;
        status = "invalid";
        if (heap->pool_size < 2 * header || tail < start) goto unlock;
        while (current != tail) {
            if (current < start || current > tail || tail - current < header ||
                current % RT_ALIGN_SIZE) goto unlock;
            const struct rt_memheap_item *item = (const struct rt_memheap_item *)current;
            uintptr_t next = (uintptr_t)item->next;
            if ((item->magic & 0xfffffffeu) != 0x1ea01ea0u || item->pool_ptr != heap ||
                next < current + header || next > tail || next % RT_ALIGN_SIZE)
                goto unlock;
            if (current >= *cursor) {
                if (item->magic & 1u) {
                    struct mem_block_sample *sample = &samples[(*count)++];
                    sample->address = current + header;
                    sample->size = item->size;
                    sample->physical = (rt_uint32_t)(next - current);
                    sample->owner = item->owner;
#ifdef RT_USING_MEMTRACE
                    sample->ret_addr = item->ret_addr;
#else
                    sample->ret_addr = 0;
#endif
                }
                *cursor = next;
                if (*count == MEM_BLOCK_BATCH && next != tail) {
                    status = "more";
                    goto unlock;
                }
            }
            current = next;
        }
        status = "complete";
unlock:
        rt_sem_release(&heap->lock);
        break;
    }
done:
    rt_exit_critical();
    return status;
}

static int mem_block_matches(epd_memory_owner_t token, epd_memory_owner_t target,
                             bool children, unsigned capacity)
{
    if (token == target) return 1;
    if (!children) return 0;
    for (unsigned depth = 0; depth < capacity; ++depth) {
        epd_memory_owner_snapshot_t owner;
        if (!epd_memory_owner_get(token, &owner)) return -1;
        if (owner.parent == target) return 1;
        if (owner.parent == token || owner.parent == EPD_MEMORY_OWNER_INVALID) return 0;
        token = owner.parent;
    }
    return -1;
}

static int mem_blocks(int argc, char **argv)
{
    epd_memory_owner_t target = 0;
    epd_memory_owner_snapshot_t owner;
    epd_memory_metadata_snapshot_t metadata;
    unsigned heaps = 0, incomplete = 0;
    bool children = argc != 3;
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "self")) || !argv[1][0])
        goto usage;
    for (const char *p = argv[1]; *p; ++p) {
        if (*p < '0' || *p > '9' || target > (UINT32_MAX - (unsigned)(*p - '0')) / 10u)
            goto usage;
        target = target * 10u + (unsigned)(*p - '0');
    }
    if (!epd_memory_owner_get(target, &owner)) {
        rt_kprintf("mem_blocks owner=%lu not_found\n", (unsigned long)target);
        return -RT_EINVAL;
    }
    epd_memory_metadata_snapshot(&metadata);
    rt_kprintf("mem_blocks owner=%lu children=%u rolling=1 trace=%u\n",
        (unsigned long)target, children,
#ifdef RT_USING_MEMTRACE
        1u
#else
        0u
#endif
    );
    for (unsigned h = 0; h < EPD_MEMORY_BACKEND_COUNT; ++h) {
        epd_memory_backend_snapshot_t backend;
        struct mem_block_sample samples[MEM_BLOCK_BATCH];
        uintptr_t cursor = 0;
        unsigned blocks = 0, unknown = 0;
        unsigned long bytes = 0;
        const char *status;
        if (!epd_memory_backend_get((epd_memory_backend_t)h, &backend)) continue;
        ++heaps;
        do {
            unsigned count;
            status = mem_block_batch(backend.key, &cursor, samples, &count);
            for (unsigned i = 0; i < count; ++i) {
                const struct mem_block_sample *sample = &samples[i];
                int matches = mem_block_matches(sample->owner, target, children, metadata.descriptors_capacity);
                if (matches < 0) { ++unknown; continue; }
                if (!matches) continue;
                ++blocks;
                bytes += sample->physical;
                rt_kprintf("mem_block heap=%u name=%s addr=%p size=%lu physical=%lu owner=%lu ret_addr=0x%08lx\n",
                    h, backend.name, (void *)sample->address, (unsigned long)sample->size,
                    (unsigned long)sample->physical, (unsigned long)sample->owner,
                    (unsigned long)sample->ret_addr);
            }
        } while (!strcmp(status, "more"));
        if (strcmp(status, "complete") || unknown) ++incomplete;
        rt_kprintf("mem_blocks_heap heap=%u name=%s status=%s blocks=%u physical=%lu unknown=%u\n",
            h, backend.name, status, blocks, bytes, unknown);
    }
    rt_kprintf("mem_blocks_done heaps=%u incomplete=%u\n", heaps, incomplete);
    return incomplete ? -RT_ERROR : RT_EOK;
usage:
    rt_kprintf("usage: mem_blocks <owner> [self]\n");
    return -RT_EINVAL;
}
MSH_CMD_EXPORT(mem_blocks, Inspect allocated heap blocks for an owner and its descendants);
#endif
#endif

static void print_usage(const epd_memory_usage_t *usage)
{
    rt_kprintf(" reclaim=%lu active=%lu inflight=%lu pending_free=%lu unused=%lu allocating=%lu blocks=%lu\n",
        (unsigned long)usage->physical[EPD_MEMORY_RECLAIMABLE],
        (unsigned long)usage->physical[EPD_MEMORY_ACTIVE],
        (unsigned long)usage->physical[EPD_MEMORY_INFLIGHT],
        (unsigned long)usage->physical[EPD_MEMORY_PENDING_FREE],
        (unsigned long)usage->unused, (unsigned long)usage->allocating,
        (unsigned long)usage->allocations);
}

static int mem_state(int argc, char **argv)
{
    bool scopes = argc == 2 && !strcmp(argv[1], "scopes");
    epd_memory_metadata_snapshot_t metadata;
    epd_resource_stats_t resources;
    if (argc > 2 || (argc == 2 && !scopes))
    {
        rt_kprintf("usage: mem_state [scopes]\n");
        return -RT_EINVAL;
    }
    epd_memory_metadata_snapshot(&metadata);
    rt_kprintf("mem_state initialized=%u metadata=%lu sram=%lu psram=%lu owners=%lu/%lu retired=%lu errors=%lu transactions=%lu/%lu\n",
        metadata.initialized, (unsigned long)metadata.fixed_bytes,
        (unsigned long)metadata.sram_bytes, (unsigned long)metadata.retained_bytes,
        (unsigned long)metadata.descriptors_used, (unsigned long)metadata.descriptors_capacity,
        (unsigned long)metadata.retired_slots, (unsigned long)metadata.protocol_errors,
        (unsigned long)metadata.transactions_used, (unsigned long)metadata.transactions_capacity);
    for (unsigned i = 0; i < EPD_MEMORY_BACKEND_COUNT; ++i)
    {
        epd_memory_backend_snapshot_t heap;
        if (!epd_memory_backend_get((epd_memory_backend_t)i, &heap)) continue;
        rt_kprintf("mem_heap id=%u name=%s region=%u parent=%u capacity=%lu unavailable=%lu free=%lu known=%u",
            i, heap.name, heap.config.region, heap.config.parent,
            (unsigned long)heap.config.capacity, (unsigned long)heap.config.unavailable,
            (unsigned long)heap.available, heap.available_known);
        print_usage(&heap.usage);
    }
    for (unsigned i = 0; i < metadata.descriptors_capacity; ++i)
    {
        epd_memory_owner_snapshot_t owner;
        epd_memory_account_snapshot_t account;
        if (!epd_memory_owner_at(i, &owner)) continue;
        bool root = owner.owner == owner.account;
        if (!scopes && !root) continue;
        rt_kprintf("mem_owner id=%lu account=%lu parent=%lu kind=%u app=%s closed=%u refs=%lu blocks=%lu allocating=%lu\n",
            (unsigned long)owner.owner, (unsigned long)owner.account, (unsigned long)owner.parent,
            owner.kind, owner.appid, owner.closed, (unsigned long)owner.references,
            (unsigned long)owner.allocations, (unsigned long)owner.allocating);
        /* Each root is reported once. Child scope views overlap this account. */
        if (root && epd_memory_account_snapshot(owner.owner, &account))
            for (unsigned h = 0; h < EPD_MEMORY_BACKEND_COUNT; ++h)
            {
                const epd_memory_usage_t *usage = &account.backend[h];
                if (!usage->allocations && !usage->unused && !usage->allocating) continue;
                rt_kprintf("mem_account id=%lu heap=%u", (unsigned long)owner.owner, h);
                print_usage(usage);
            }
        if (root && owner.owner != EPD_MEMORY_OWNER_SYSTEM)
        {
            epd_app_background_snapshot_t background;
            if (app_background_snapshot(owner.owner, &background) == EPD_MEMORY_OK)
                rt_kprintf("mem_background id=%lu requested=%u effective=%u actual=%u foreground=%u tasks=%lu revision=%lu applied=%lu dirty=%lu saved=%lu result=%u\n",
                    (unsigned long)owner.owner, background.requested.mode, background.effective,
                    background.actual, background.foreground, (unsigned long)background.tasks,
                    (unsigned long)background.revision, (unsigned long)background.applied_revision,
                    (unsigned long)background.dirty_revision, (unsigned long)background.saved_revision,
                    background.last_result);
        }
    }
    epd_resource_stats(&resources);
    rt_kprintf("mem_resources activities=%lu providers=%lu resources=%lu refs=%lu physical=%lu reclaimed=%lu loads=%lu failed=%lu\n",
        (unsigned long)resources.activities, (unsigned long)resources.providers,
        (unsigned long)resources.resources, (unsigned long)resources.references,
        (unsigned long)resources.physical_bytes, (unsigned long)resources.reclaimed_bytes,
        (unsigned long)resources.loads, (unsigned long)resources.load_failures);
    return RT_EOK;
}
MSH_CMD_EXPORT(mem_state, Inspect application memory ownership and resource accounting);
#endif
