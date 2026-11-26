/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RT_USING_MEMHEAP_OWNER
/* Reuse the OS stubs and full account/heap audit with the real core bridge. */
#define main unified_existing_main
#include "host.c"
#undef main
#else
#define H RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE)
static unsigned char pool[32768] __attribute__((aligned(32)));
static struct rt_memheap heap;
static struct rt_memheap *heaps[] = {&heap};
static rt_err_t error_value;
static int heap_locks;

void rt_assert_handler(const char *expr, const char *func, rt_size_t line)
{ fprintf(stderr, "SDK ASSERT %s:%lu: %s\n", func, line, expr); abort(); }
void *rt_memset(void *p, int value, rt_ubase_t n) { return memset(p, value, n); }
void rt_set_errno(rt_err_t value) { error_value = value; }
rt_err_t rt_get_errno(void) { return error_value; }
rt_tick_t rt_tick_get(void) { return 0; }
rt_uint8_t rt_object_get_type(rt_object_t o) { return o->type & ~RT_Object_Class_Static; }
void rt_object_init(struct rt_object *o, enum rt_object_class_type type, const char *name)
{
    memset(o, 0, sizeof(*o)); o->type = type | RT_Object_Class_Static;
    strncpy(o->name, name, RT_NAME_MAX - 1); rt_list_init(&o->list);
}
rt_err_t rt_sem_init(rt_sem_t s, const char *name, rt_uint32_t value, rt_uint8_t flag)
{ (void)name; (void)flag; s->value = value; return RT_EOK; }
rt_err_t rt_sem_take(rt_sem_t s, rt_int32_t timeout)
{ (void)timeout; assert(s->value == 1 && !heap_locks); --s->value; ++heap_locks; return RT_EOK; }
rt_err_t rt_sem_release(rt_sem_t s)
{ assert(!s->value && heap_locks == 1); ++s->value; --heap_locks; return RT_EOK; }
static struct rt_memheap_item *header(void *p)
{ return (struct rt_memheap_item *)((unsigned char *)p - H); }
static size_t physical(void *p)
{ return (unsigned char *)header(p)->next - (unsigned char *)header(p); }
static void setup(void)
{ assert(rt_memheap_init(&heap, "exact", pool, sizeof(pool)) == RT_EOK); }
static void audit(void)
{
    struct rt_memheap_item *item = heap.block_list;
    size_t live = 0, free_bytes = 0, headers = 1, sized = 0;
    while ((unsigned char *)item->next > (unsigned char *)item) {
        size_t span = (unsigned char *)item->next - (unsigned char *)item;
        if (item->magic & 1) { live += span; sized += item->size; }
        else { free_bytes += span - H; ++headers; }
        item = item->next;
    }
    assert(heap.pool_size == live + free_bytes + headers * H);
    assert(heap.available_size == free_bytes && heap.actual_used_size == sized);
    assert(!heap_locks);
}
#endif

static unsigned no_fit_calls;
static size_t last_failed_size;

void rt_memheap_alloc_no_fit(struct rt_memheap *h, rt_size_t aligned)
{
    assert(h == heaps[0] && !heap_locks);
    ++no_fit_calls;
    last_failed_size = aligned;
}

static void empty_again(size_t available)
{
    assert(heaps[0]->available_size == available && !heaps[0]->actual_used_size);
    assert(heaps[0]->free_list->next_free == heaps[0]->block_list);
    assert(heaps[0]->block_list->next_free == heaps[0]->free_list);
    audit();
}

static void exact_whole(void)
{
    size_t available = heaps[0]->available_size;
    assert(!rt_memheap_alloc(heaps[0], available + RT_ALIGN_SIZE));
    empty_again(available);
    assert(no_fit_calls == 1 && last_failed_size == available + RT_ALIGN_SIZE);
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        unsigned char *p = rt_memheap_alloc(heaps[0], available);
        assert(p && physical(p) == available + H && header(p)->size == available);
        assert(!heaps[0]->available_size);
        memset(p, 0x5a, available);
        assert(p[0] == 0x5a && p[available - 1] == 0x5a);
        audit(); rt_memheap_free(p); empty_again(available);
    }
    assert(no_fit_calls == 1);
}

static void exact_unsplit(void)
{
    size_t available = heaps[0]->available_size;
    /* Every aligned remainder below header + minimum payload stays in the block. */
    for (size_t remainder = 0; remainder < H + 12; remainder += RT_ALIGN_SIZE) {
        size_t requested = available - remainder;
        void *p = rt_memheap_alloc(heaps[0], requested);
        assert(p && physical(p) == available + H && header(p)->size == requested);
        assert(!heaps[0]->available_size);
        audit(); rt_memheap_free(p); empty_again(available);
    }
}

static void exact_fragmented(void)
{
    size_t available = heaps[0]->available_size;
    void *left = rt_memheap_alloc(heaps[0], 128);
    void *barrier = rt_memheap_alloc(heaps[0], 128);
    void *right = rt_memheap_alloc(heaps[0], heaps[0]->available_size);
    assert(left && barrier && right);
    rt_memheap_free(left); rt_memheap_free(right); audit();
    size_t fragmented = heaps[0]->available_size;
    assert(!rt_memheap_alloc(heaps[0], fragmented));
    assert(heaps[0]->available_size == fragmented && header(barrier)->size == 128);
    assert(no_fit_calls == 1 && last_failed_size == fragmented);
    audit(); rt_memheap_free(barrier); empty_again(available);
}

#ifdef RT_USING_MEMHEAP_OWNER
static void exact_credit(bool sufficient, size_t remainder)
{
    size_t available = heaps[0]->available_size, charged = available + H;
    size_t granted = sufficient ? charged : charged - RT_ALIGN_SIZE;
    epd_memory_owner_t owner = epd_memory_owner_create("exact-credit");
    /* Install the promise before the first allocator observation in this heap. */
    epd_memory_credit_t credit = request(owner, granted, 0, EPD_MEMORY_OK);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(credit);
    epd_memory_credit_snapshot_t snapshot;
    assert(epd_memory_rejection_start(4, 1000) == EPD_MEMORY_OK);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        void *p = rt_memheap_alloc(heaps[0], available - remainder);
        assert(epd_memory_credit_get(credit, &snapshot));
        if (sufficient) {
            assert(p && header(p)->owner == credit && physical(p) == charged);
            assert(!snapshot.unused[backends[0]] && snapshot.physical[backends[0]] == charged);
            audit();
            /* Free from SYSTEM must settle the allocation's original credit. */
            epd_memory_owner_leave_trusted(previous);
            rt_memheap_free(p);
            previous = epd_memory_owner_enter_trusted(credit);
        } else {
            epd_memory_rejection_snapshot_t rejection;
            assert(!p && epd_memory_rejection_read(&rejection) && rejection.valid);
            assert(rejection.reason == EPD_MEMORY_REJECT_CREDIT_UNUSED && rejection.physical == charged);
        }
        assert(epd_memory_credit_get(credit, &snapshot));
        assert(snapshot.unused[backends[0]] == granted && !snapshot.physical[backends[0]]);
        assert(!snapshot.allocating[backends[0]]);
        empty_again(available);
    }
    assert(!no_fit_calls); /* Owner denial is separate from physical heap failure. */
    epd_memory_rejection_stop(); epd_memory_owner_leave_trusted(previous);
    epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit); epd_memory_owner_release_trusted(owner);
    assert(!epd_memory_owner_get(credit, &(epd_memory_owner_snapshot_t){0}));
    audit();
}
#endif

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    setup();
#ifdef RT_USING_MEMHEAP_OWNER
    assert(epd_memory_init() == EPD_MEMORY_OK);
#endif
    if (!strcmp(argv[1], "whole")) exact_whole();
    else if (!strcmp(argv[1], "unsplit")) exact_unsplit();
    else if (!strcmp(argv[1], "fragmented")) exact_fragmented();
#ifdef RT_USING_MEMHEAP_OWNER
    else if (!strcmp(argv[1], "credit_enough")) exact_credit(true, 0);
    else if (!strcmp(argv[1], "credit_short")) exact_credit(false, 0);
    else if (!strcmp(argv[1], "credit_unsplit_enough")) exact_credit(true, RT_ALIGN_SIZE);
    else if (!strcmp(argv[1], "credit_unsplit_short")) exact_credit(false, RT_ALIGN_SIZE);
#endif
    else return 2;
    printf("PASS exact_fit %s ticks=%u\n", argv[1], RT_TICK_PER_SECOND);
    return 0;
}
