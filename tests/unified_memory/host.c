/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include "epd_memory_manager.h"
#include "app_memory.h"
#include "src/draw/lv_draw_buf_private.h"
#include <rtthread.h>
#include <rthw.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sanitizer/asan_interface.h>
#include "app_memory_pressure.h"
#include "../../src/ui/ui_font_memory.h"
static rt_err_t recovery_sleep(rt_int32_t ms);
#define rt_thread_mdelay recovery_sleep
#include "../../modules/common/memory_retry.h"
#undef rt_thread_mdelay

extern void epd_memory_host_retained_storage(void **address, size_t *size);

#define H RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE)
#define INVALID EPD_MEMORY_OWNER_INVALID
unsigned char unified_test_storage[3][32768] __attribute__((aligned(32)));
#define storage unified_test_storage
static unsigned char thread_stack[1024] __attribute__((aligned(32)));
struct rt_memheap app_image_psram_memheap;
static struct rt_memheap secondary_heap;
static struct rt_memheap *heaps[2] = {&app_image_psram_memheap, &secondary_heap};
static struct rt_object_information heap_objects, thread_objects;
rt_thread_t rt_current_thread;
rt_list_t rt_thread_defunct;
rt_list_t rt_thread_priority_table[RT_THREAD_PRIORITY_MAX];
static struct rt_thread actors[3];
static rt_err_t error_value;
static rt_tick_t ticks;
static int heap_locks, irq_depth, critical_depth;
static rt_int32_t last_heap_timeout;
static epd_memory_backend_t backends[2];
static unsigned cleanup_calls;
static unsigned pressure_calls, pressure_forced, recovery_sleeps;
static app_memory_pressure_status_t pressure_status;
static void *release_during_recovery;
size_t epd_app_psram_available(void) { return app_image_psram_memheap.available_size; }
size_t epd_app_psram_capacity(void) { return app_image_psram_memheap.pool_size - 2 * H; }
static uint32_t pressure_publish(size_t bytes, bool force)
{
    assert(!irq_depth && !heap_locks && !critical_depth);
    ++pressure_calls; pressure_forced += force;
    pressure_status.generation++;
    pressure_status.pending = true; pressure_status.result = APP_MEMORY_PRESSURE_PENDING;
    pressure_status.min_available = bytes; pressure_status.available = epd_app_psram_available();
    return pressure_status.generation;
}
uint32_t app_memory_pressure_request(size_t bytes) { return pressure_publish(bytes, false); }
uint32_t app_memory_pressure_allocation_failed(size_t bytes) { return pressure_publish(bytes, true); }
uint32_t app_memory_pressure_allocation_failed_block(size_t bytes, size_t physical)
{
    assert(physical && physical <= bytes);
    return pressure_publish(bytes, true);
}
app_memory_pressure_status_t app_memory_pressure_query(void) { return pressure_status; }
static rt_err_t recovery_sleep(rt_int32_t ms)
{
    assert(!irq_depth && !heap_locks && !critical_depth);
    ++recovery_sleeps;
    ticks += ((uint64_t)(uint32_t)ms * RT_TICK_PER_SECOND + 999u) / 1000u;
    if (release_during_recovery) {
        rt_memheap_free(release_during_recovery); release_during_recovery = NULL;
        pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    }
    return RT_EOK;
}
static uint32_t diagnostic_counter, diagnostic_reads;
uint32_t epd_memory_test_cycles(void)
{
    assert(irq_depth);
    ++diagnostic_reads;
    diagnostic_counter += 100;
    return diagnostic_counter;
}
static lv_draw_buf_handlers_t font_handlers;
lv_draw_buf_handlers_t *lv_draw_buf_get_font_handlers(void) { return &font_handlers; }
void *epd_app_alloc(size_t bytes, epd_app_memory_t region)
{ return rt_memheap_alloc(heaps[region == EPD_APP_PSRAM ? 0 : 1], bytes); }
void epd_app_free(void *ptr) { rt_memheap_free(ptr); }
#ifndef EPD_JSON_REAL
/* JSON allocation and startup hooks are exercised by platform_json/run.py. */
void epd_json_init(void) {}
#endif

static struct rt_memheap_item *header(void *p)
{ return (struct rt_memheap_item *)((unsigned char *)p - H); }
static size_t physical(void *p)
{ return (unsigned char *)header(p)->next - (unsigned char *)header(p); }

void rt_assert_handler(const char *expr, const char *func, rt_size_t line)
{ fprintf(stderr, "SDK ASSERT %s:%lu: %s\n", func, line, expr); abort(); }
void *rt_memset(void *p, int v, rt_ubase_t n) { return memset(p, v, n); }
void *rt_memcpy(void *d, const void *s, rt_ubase_t n) { return memcpy(d, s, n); }
rt_int32_t rt_memcmp(const void *a, const void *b, rt_ubase_t n) { return memcmp(a, b, n); }
rt_int32_t rt_strncmp(const char *a, const char *b, rt_ubase_t n) { return strncmp(a, b, n); }
rt_size_t rt_strlen(const char *s) { return strlen(s); }
rt_err_t rt_get_errno(void) { return error_value; }
void rt_set_errno(rt_err_t value) { error_value = value; }
rt_tick_t rt_tick_get(void) { return ticks; }
uint32_t rt_system_get_time(void) { return ticks; }
void rt_kprintf(const char *fmt, ...)
{ va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
#ifdef EPD_MEMORY_LOCK_PROFILE
void profile_guard_enter(void);
void profile_guard_leave(void);
#endif
rt_base_t rt_hw_interrupt_disable(void) {
#ifdef EPD_MEMORY_LOCK_PROFILE
    if (!irq_depth) profile_guard_enter();
#endif
    return irq_depth++;
}
void rt_hw_interrupt_enable(rt_base_t level) {
    assert(irq_depth > level); irq_depth = level;
#ifdef EPD_MEMORY_LOCK_PROFILE
    if (!level) profile_guard_leave();
#endif
}
void rt_enter_critical(void) { ++critical_depth; }
void rt_exit_critical(void) { assert(critical_depth); --critical_depth; }
rt_uint16_t rt_critical_level(void) { return critical_depth; }
void rt_schedule(void) { }
rt_uint8_t rt_interrupt_get_nest(void) { return 0; }
void rt_schedule_remove_thread(rt_thread_t t) { (void)t; }
void rt_schedule_insert_thread(rt_thread_t t) { (void)t; }
rt_uint8_t *rt_hw_stack_init(void *entry, void *arg, rt_uint8_t *stack, void *exit)
{ (void)entry; (void)arg; (void)exit; return stack; }
void rt_timer_init(rt_timer_t timer, const char *name, void (*fn)(void *), void *arg,
                   rt_tick_t time, rt_uint8_t flag)
{ (void)name; (void)fn; (void)arg; (void)time; (void)flag; memset(timer, 0, sizeof(*timer)); }
rt_err_t rt_timer_detach(rt_timer_t timer) { (void)timer; return RT_EOK; }
rt_err_t rt_timer_stop(rt_timer_t timer) { (void)timer; return RT_EOK; }
void rt_thread_free_sig(rt_thread_t t) { (void)t; }
void rt_object_init(struct rt_object *o, enum rt_object_class_type type, const char *name)
{
    memset(o, 0, sizeof(*o));
    o->type = type | RT_Object_Class_Static;
    strncpy(o->name, name, RT_NAME_MAX - 1);
    rt_list_init(&o->list);
    if (type == RT_Object_Class_MemHeap) rt_list_insert_before(&heap_objects.object_list, &o->list);
    if (type == RT_Object_Class_Thread) rt_list_insert_before(&thread_objects.object_list, &o->list);
}
rt_uint8_t rt_object_get_type(rt_object_t o) { return o->type & ~RT_Object_Class_Static; }
rt_bool_t rt_object_is_systemobject(rt_object_t o) { return !!(o->type & RT_Object_Class_Static); }
void rt_object_detach(rt_object_t o) { rt_list_remove(&o->list); }
struct rt_object_information *rt_object_get_information(enum rt_object_class_type type)
{ return type == RT_Object_Class_MemHeap ? &heap_objects : &thread_objects; }
rt_object_t rt_object_allocate(enum rt_object_class_type type, const char *name)
{
    rt_object_t o = rt_malloc(sizeof(struct rt_thread));
    if (o) { rt_object_init(o, type, name); o->type &= ~RT_Object_Class_Static; }
    return o;
}
void rt_object_delete(rt_object_t o) { rt_object_detach(o); rt_free(o); }
rt_err_t rt_sem_init(rt_sem_t s, const char *name, rt_uint32_t value, rt_uint8_t flag)
{ (void)flag; rt_object_init((rt_object_t)s, RT_Object_Class_Semaphore, name); s->value = value; return RT_EOK; }
rt_err_t rt_sem_take(rt_sem_t s, rt_int32_t timeout)
{
    assert(!irq_depth);
    last_heap_timeout = timeout;
    if (!s->value) {
        assert(timeout == 0 && heap_locks == 1);
        return -RT_ETIMEOUT;
    }
    assert(s->value == 1 && heap_locks == 0);
    --s->value; ++heap_locks; return RT_EOK;
}
rt_err_t rt_sem_release(rt_sem_t s)
{ assert(!irq_depth && !s->value && heap_locks == 1); ++s->value; --heap_locks; return RT_EOK; }

static void setup(void)
{
    epd_memory_backend_config_t config = { EPD_MEMORY_REGION_HCPU_PSRAM, sizeof(storage[1]), 0,
                                          EPD_MEMORY_BACKEND_NONE };
    rt_list_init(&heap_objects.object_list);
    rt_list_init(&thread_objects.object_list);
    rt_list_init(&rt_thread_defunct);
    rt_current_thread = &actors[0];
    rt_system_heap_init(storage[0], storage[0] + sizeof(storage[0]));
    assert(rt_memheap_init(heaps[0], "psram", storage[1], sizeof(storage[1])) == RT_EOK);
    assert(rt_memheap_init(heaps[1], "sram", storage[2], sizeof(storage[2])) == RT_EOK);
    assert(epd_memory_sdk_register_heap(heaps[0], &config, &backends[0]) == EPD_MEMORY_OK);
    config.region = EPD_MEMORY_REGION_SRAM;
    assert(epd_memory_sdk_register_heap(heaps[1], &config, &backends[1]) == EPD_MEMORY_OK);
}

static size_t account_bytes(epd_memory_owner_t owner, unsigned b)
{
    epd_memory_account_snapshot_t snapshot;
    size_t bytes = 0;
    assert(epd_memory_account_snapshot(owner, &snapshot));
    for (unsigned c = 0; c < EPD_MEMORY_CLASS_COUNT; ++c) bytes += snapshot.backend[b].physical[c];
    return bytes;
}

static void audit_accounts(void)
{
    epd_memory_account_snapshot_t global, account;
    epd_memory_usage_t sum[EPD_MEMORY_BACKEND_COUNT] = {{0}};
    epd_memory_metadata_snapshot_t metadata;
    epd_memory_owner_snapshot_t owner;
    epd_memory_metadata_snapshot(&metadata);
    assert(epd_memory_scope_snapshot(0, &global));
    for (unsigned slot = 0; slot < metadata.descriptors_capacity; ++slot) {
        if (!epd_memory_owner_at(slot, &owner) || owner.owner != owner.account) continue;
        assert(epd_memory_account_snapshot(owner.owner, &account));
        for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
            for (unsigned c = 0; c < EPD_MEMORY_CLASS_COUNT; ++c)
                sum[b].physical[c] += account.backend[b].physical[c];
            sum[b].sized += account.backend[b].sized;
            sum[b].unused += account.backend[b].unused;
            sum[b].allocating += account.backend[b].allocating;
            sum[b].allocations += account.backend[b].allocations;
        }
    }
    for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) {
        epd_memory_backend_snapshot_t backend;
        if (epd_memory_backend_get(b, &backend))
            assert(!memcmp(&global.backend[b], &backend.usage, sizeof(backend.usage)));
        for (unsigned c = 0; c < EPD_MEMORY_CLASS_COUNT; ++c)
            assert(sum[b].physical[c] == global.backend[b].physical[c]);
        assert(sum[b].sized == global.backend[b].sized);
        assert(sum[b].unused == global.backend[b].unused);
        assert(sum[b].allocating == global.backend[b].allocating);
        assert(sum[b].allocations == global.backend[b].allocations);
        /* Peaks occur at different times and are not additive. */
    }
}

static void audit(void)
{
    rt_list_t *n;
    epd_memory_metadata_snapshot_t metadata;
    for (n = heap_objects.object_list.next; n != &heap_objects.object_list; n = n->next) {
        struct rt_memheap *h = (struct rt_memheap *)rt_list_entry(n, struct rt_object, list);
        struct rt_memheap_item *item = h->block_list;
        size_t occupied = 0, sized = 0, free_bytes = 0, headers = 1;
        uint32_t blocks = 0;
        epd_memory_backend_t b;
        epd_memory_backend_snapshot_t snapshot;
        while ((unsigned char *)item->next > (unsigned char *)item) {
            size_t span = (unsigned char *)item->next - (unsigned char *)item;
            if (item->magic & 1) {
                epd_memory_owner_snapshot_t owner;
                assert(epd_memory_owner_get(item->owner, &owner));
                occupied += span; sized += item->size; ++blocks;
            } else { free_bytes += span - H; ++headers; }
            item = item->next;
        }
        assert(h->available_size == free_bytes);
        assert(h->pool_size == occupied + free_bytes + headers * H);
        if (!epd_memory_backend_find(h, &b)) { assert(!occupied); continue; }
        assert(epd_memory_backend_get(b, &snapshot));
        size_t actual = 0;
        for (unsigned c = 0; c < EPD_MEMORY_CLASS_COUNT; ++c) actual += snapshot.usage.physical[c];
        assert(actual == occupied && snapshot.usage.sized == sized && snapshot.usage.allocations == blocks);
        assert(!snapshot.usage.allocating);
    }
    epd_memory_metadata_snapshot(&metadata);
    assert(!metadata.protocol_errors);
    assert(!irq_depth && !heap_locks && !critical_depth);
    audit_accounts();
}

static epd_memory_credit_t request(epd_memory_owner_t owner, size_t bytes, uint32_t timeout,
                                  epd_memory_result_t expected)
{
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, timeout};
    epd_memory_credit_t credit;
    spec.bytes[backends[0]] = bytes;
    assert(epd_memory_credit_request(owner, &spec, &credit) == expected);
    return credit;
}

static epd_memory_result_t host_transfer(void *, epd_memory_owner_t);
static epd_memory_result_t host_transfer_try(void *, epd_memory_owner_t);
static void test_boot_metadata(void)
{
    void *retained;
    size_t length;
    epd_memory_metadata_snapshot_t metadata;
    epd_memory_owner_snapshot_t owner;
    epd_memory_host_retained_storage(&retained, &length);
    memset(retained, 0xa5, length); /* NOLOAD retained memory is not cold-zeroed. */
    __asan_poison_memory_region(retained, length);
    epd_memory_diagnostic_snapshot_t diagnostic;
    assert(!epd_memory_diagnostic_read(&diagnostic));
    assert(epd_memory_diagnostic_start(10, 10) == EPD_MEMORY_BUSY);
    epd_memory_diagnostic_stop();
    epd_memory_rejection_snapshot_t rejection;
    assert(!epd_memory_rejection_read(&rejection));
    assert(epd_memory_rejection_start(1, 100) == EPD_MEMORY_BUSY);
    epd_memory_rejection_stop();
    epd_memory_rejection_adapter(INVALID, NULL, EPD_MEMORY_REJECT_CONTEXT);
    assert(epd_memory_try_change(NULL) == EPD_MEMORY_INVALID);
    epd_memory_metadata_snapshot(&metadata); assert(!metadata.initialized);
    assert(epd_memory_owner_create("too-early") == INVALID);
    assert(epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED) == INVALID);
    assert(!epd_memory_owner_get(0x101, &owner));
    assert(epd_memory_owner_get(0, &owner));
    assert(epd_memory_owner_at(0, &owner) && owner.owner == 0);
    assert(!epd_memory_owner_at(1, &owner));
    epd_memory_poll(0);
    rt_thread_t saved_thread = rt_current_thread;
    rt_current_thread = NULL;
    void *p = rt_malloc(120); assert(p && header(p)->owner == 0);
    p = rt_realloc(p, 200); assert(p && header(p)->owner == 0);
    rt_current_thread = saved_thread;
    void *q = rt_memheap_alloc(heaps[0], 128); assert(q);
    size_t before = account_bytes(0, backends[0]); assert(before);
    audit();
    __asan_unpoison_memory_region(retained, length);
    assert(epd_memory_init() == EPD_MEMORY_OK);
    epd_memory_metadata_snapshot(&metadata); assert(metadata.initialized);
    assert(account_bytes(0, backends[0]) == before);
    epd_memory_owner_t a = epd_memory_owner_create("after-init"); assert(a != INVALID);
    assert(epd_memory_init() == EPD_MEMORY_OK);
    assert(epd_memory_owner_get(a, &owner));
    rt_free(p); rt_memheap_free(q);
    epd_memory_owner_release_trusted(a); audit();
}

static void test_owner_at(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("diagnostic");
    unsigned slot = a & 255u;
    epd_memory_owner_snapshot_t first, second;
    epd_memory_metadata_snapshot_t metadata;
    assert(epd_memory_owner_at(slot, &first) && first.owner == a && first.references == 1);
    assert(!strcmp(first.appid, "diagnostic"));
    assert(epd_memory_owner_get(a, &second) && !memcmp(&first, &second, sizeof(first)));
    epd_memory_metadata_snapshot(&metadata);
    assert(!epd_memory_owner_at(metadata.descriptors_capacity, &second));
    assert(!epd_memory_owner_at(slot, NULL));
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(a);
    void *p = rt_memheap_alloc(heaps[0], 32); assert(p);
    epd_memory_owner_leave_trusted(previous); epd_memory_owner_release_trusted(a);
    assert(epd_memory_owner_at(slot, &second) && !second.references && second.allocations == 1);
    rt_memheap_free(p);
    assert(!epd_memory_owner_at(slot, &second));
    assert(!epd_memory_owner_get(first.owner, &second));
    epd_memory_owner_t reused = epd_memory_owner_create("new-generation");
    assert((reused & 255u) == slot && reused != a);
    assert(epd_memory_owner_at(slot, &second) && second.owner == reused && second.references == 1);
    epd_memory_owner_release_trusted(reused); audit();
}

static void test_early(void)
{
    rt_thread_t saved_thread = rt_current_thread;
    rt_current_thread = NULL;
    void *p = rt_malloc(100);
    rt_current_thread = saved_thread;
    epd_memory_backend_t id, again;
    epd_memory_backend_snapshot_t heap;
    assert(p && header(p)->owner == 0);
    assert(epd_memory_backend_find(header(p)->pool_ptr, &id));
    assert(epd_memory_backend_get(id, &heap));
    assert(heap.config.region == EPD_MEMORY_REGION_UNKNOWN);
    heap.config.region = EPD_MEMORY_REGION_SRAM;
    assert(epd_memory_sdk_register_heap(header(p)->pool_ptr, &heap.config, &again) == EPD_MEMORY_OK);
    assert(again == id);
    rt_free(p); audit();
}

static void test_backend_init(void)
{
    void *early = rt_malloc(64);
    epd_memory_backend_t early_id, id;
    epd_memory_backend_snapshot_t snapshot;
    assert(early && epd_memory_backend_find(header(early)->pool_ptr, &early_id));
    void *backing = rt_memheap_alloc(heaps[0], 4096);
    struct rt_memheap child;
    assert(backing);
    assert(rt_memheap_init(&child, "nested", backing, 4096) == RT_EOK);
    void *nested = rt_memheap_alloc(&child, 64); assert(nested);
    assert(app_memory_init() == EPD_MEMORY_OK);
    const lv_draw_buf_handlers_t *cache = app_memory_font_cache_handlers();
    assert(cache != &font_handlers && !font_handlers.buf_malloc_cb && !font_handlers.buf_free_cb);
    assert(cache->buf_malloc_cb && cache->buf_free_cb == epd_app_free);
    void *font = cache->buf_malloc_cb(32, 0); assert(font);
    cache->buf_free_cb(font);
    assert(epd_memory_backend_find(header(early)->pool_ptr, &id) && id == early_id);
    assert(epd_memory_backend_get(id, &snapshot) && snapshot.config.region == EPD_MEMORY_REGION_SRAM);
    assert(!strcmp(snapshot.name, "heap") && snapshot.config.capacity == sizeof(storage[0]) - H);
    assert(epd_memory_backend_find(&child, &id));
    assert(epd_memory_backend_get(id, &snapshot) && snapshot.config.parent == backends[0]);
    assert(snapshot.config.region == EPD_MEMORY_REGION_HCPU_PSRAM);
    assert(epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, &id) && id == backends[0]);
    assert(!epd_memory_app_backend(EPD_MEMORY_REGION_SRAM, &id));
    assert(!epd_memory_app_backend(EPD_MEMORY_REGION_ACPU, &id));
    assert(!epd_memory_app_backend(EPD_MEMORY_REGION_HCPU_PSRAM, NULL));
    size_t total;
    epd_memory_usage_t usage;
    assert(epd_memory_region_snapshot(EPD_MEMORY_REGION_HCPU_PSRAM, &total, &usage) == EPD_MEMORY_OK);
    assert(total == sizeof(storage[1]) - H && usage.physical[EPD_MEMORY_ACTIVE] == physical(backing));
    assert(epd_memory_backend_get(backends[1], &snapshot) && snapshot.config.region == EPD_MEMORY_REGION_ACPU);
    assert(epd_memory_sdk_init() == EPD_MEMORY_OK);
    rt_memheap_free(nested); rt_memheap_detach(&child);
    rt_memheap_free(backing); rt_free(early); audit();
}

static void test_region_policy(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("strict");
    assert(epd_memory_owner_set_backends(a, UINT32_C(1) << backends[0]) == EPD_MEMORY_OK);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(a);
    assert(!rt_memheap_alloc(heaps[1], 32));
    void *p = rt_memheap_alloc(heaps[0], 32); assert(p);
    epd_memory_owner_leave_trusted(prev);
    assert(!rt_memheap_realloc(heaps[1], p, 64));
    assert(header(p)->owner == a && account_bytes(a, backends[0]) == physical(p));
    rt_memheap_free(p); epd_memory_owner_release_trusted(a); audit();
}

static void test_association(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("association"), b = epd_memory_owner_create("other");
    epd_memory_owner_t page = epd_memory_scope_create_trusted(a, EPD_MEMORY_SCOPE_PAGE);
    epd_memory_account_snapshot_t snap;
    epd_memory_backend_snapshot_t before, after;
    epd_memory_owner_snapshot_t owner;
    assert(epd_memory_owner_set_limit(a, backends[0], 1000) == EPD_MEMORY_OK);
    epd_memory_credit_t credit = request(page, 500, 0, EPD_MEMORY_OK);
    assert(epd_memory_backend_get(backends[0], &before));
    assert(epd_memory_association_change(page, backends[0], 0, 400) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(b, backends[0], 0, 400) == EPD_MEMORY_OK);
    assert(epd_memory_backend_get(backends[0], &after));
    assert(!memcmp(&before, &after, sizeof(before)));
    assert(epd_memory_account_snapshot(page, &snap) && snap.associated[backends[0]] == 400);
    assert(epd_memory_scope_snapshot(page, &snap) && !snap.associated[backends[0]]);
    assert(epd_memory_association_change(a, backends[0], 400, 501) == EPD_MEMORY_LIMIT);
    assert(epd_memory_association_change(a, backends[0], 401, 0) == EPD_MEMORY_INVALID);
    assert(request(page, 101, 0, EPD_MEMORY_LIMIT) == INVALID);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(page);
    assert(!rt_memheap_alloc(heaps[0], 128)); /* Header also shares the limit. */
    epd_memory_owner_leave_trusted(previous);
    previous = epd_memory_owner_enter_trusted(credit);
    void *p = rt_memheap_alloc(heaps[0], 128); assert(p);
    epd_memory_owner_leave_trusted(previous);
    assert(epd_memory_association_change(a, backends[0], 400, 500) == EPD_MEMORY_OK);
    assert(epd_memory_credit_expand(credit, (size_t[EPD_MEMORY_BACKEND_COUNT]){1}) == EPD_MEMORY_LIMIT);
    rt_memheap_free(p);
    epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit);
    assert(epd_memory_association_change(a, backends[0], 500, 1000) == EPD_MEMORY_OK);
    assert(epd_memory_owner_set_backends(a, EPD_MEMORY_ALL_BACKENDS) == EPD_MEMORY_BUSY);
    epd_memory_owner_close_trusted(a);
    assert(epd_memory_association_change(a, backends[0], 1000, 1001) == EPD_MEMORY_CLOSED);
    epd_memory_owner_release_trusted(page); epd_memory_owner_release_trusted(a);
    assert(epd_memory_owner_get(a, &owner) && !owner.references);
    assert(epd_memory_association_change(a, backends[0], 1000, 0) == EPD_MEMORY_OK);
    assert(!epd_memory_owner_get(a, &owner));
    assert(epd_memory_association_change(a, backends[0], 0, 1) == EPD_MEMORY_INVALID);
    assert(epd_memory_association_change(b, backends[0], 400, 0) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(b); audit();
}

static void test_guards(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("same"), b = epd_memory_owner_create("same");
    epd_memory_owner_snapshot_t snap;
    assert(a != INVALID && b != INVALID && a != b);
    epd_memory_owner_t zero = epd_memory_owner_enter_trusted(a);
    assert(zero == 0 && epd_memory_owner_current() == a);
    epd_memory_owner_release_trusted(a);
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(b);
    assert(old == a);
    assert(epd_memory_owner_get(a, &snap) && snap.references == 1);
    assert(epd_memory_owner_close_trusted(a));
    epd_memory_owner_leave_trusted(old);
    assert(epd_memory_owner_current() == a);
    assert(!rt_memheap_alloc(heaps[0], 64));
    epd_memory_owner_leave_trusted(zero);
    assert(!epd_memory_owner_get(a, &snap));
    assert(epd_memory_owner_enter_trusted(a) == INVALID);
    assert(epd_memory_owner_current() == 0);
    epd_memory_owner_release_trusted(b); audit();
}

static void test_cross_thread(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("app"), prev = epd_memory_owner_enter_trusted(a);
    void *p = rt_memheap_alloc(heaps[0], 123);
    size_t bytes = physical(p);
    assert(p && account_bytes(a, backends[0]) == bytes);
    epd_memory_owner_leave_trusted(prev);
    epd_memory_owner_close_trusted(a);
    epd_memory_owner_release_trusted(a);
    rt_current_thread = &actors[1];
    assert(rt_memheap_realloc(heaps[0], p, 64) == p);
    assert(!rt_memheap_realloc(heaps[0], p, 800));
    epd_memory_owner_snapshot_t snap;
    assert(epd_memory_owner_get(a, &snap) && snap.references == 0 && snap.allocations == 1);
    rt_memheap_free(p);
    assert(!epd_memory_owner_get(a, &snap));
    epd_memory_owner_t reopened = epd_memory_owner_create("app");
    assert(reopened != a && !epd_memory_owner_retain_trusted(a));
    epd_memory_owner_release_trusted(reopened); audit();
}

static void test_credit(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("credit");
    epd_memory_credit_t c = request(a, 1200, 0, EPD_MEMORY_OK);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(c);
    epd_memory_credit_snapshot_t snap;
    void *p = rt_memheap_alloc(heaps[0], 300);
    assert(p && header(p)->owner == c);
    assert(epd_memory_owner_current() == a && epd_memory_credit_current() == c);
    assert(epd_memory_credit_get(c, &snap));
    assert(snap.unused[backends[0]] + snap.physical[backends[0]] == 1200);
    rt_memheap_free(p);
    assert(epd_memory_credit_get(c, &snap) && snap.unused[backends[0]] == 1200);
    p = rt_memheap_alloc(heaps[0], 300);
    epd_memory_credit_end(c);
    assert(epd_memory_credit_get(c, &snap) && !snap.unused[backends[0]]);
    assert(!rt_memheap_alloc(heaps[0], 8));
    rt_memheap_free(p);
    assert(epd_memory_credit_get(c, &snap) && !snap.unused[backends[0]] && !snap.physical[backends[0]]);
    epd_memory_owner_leave_trusted(prev);
    epd_memory_owner_release_trusted(c);
    epd_memory_owner_release_trusted(a); audit();
}

static void test_reservations(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("holder");
    epd_memory_credit_t c = request(a, 30000, 0, EPD_MEMORY_OK);
    assert(!rt_memheap_alloc(heaps[0], 4000));
    void *p = rt_memheap_alloc(heaps[1], 4000);
    assert(p); rt_memheap_free(p);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(c);
    p = rt_memheap_alloc(heaps[0], 4000); assert(p);
    epd_memory_owner_leave_trusted(prev);
    rt_memheap_free(p);
    epd_memory_credit_end(c); epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a); audit();
}

static void test_credit_expand(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("expand");
    epd_memory_credit_t c = request(a, 600, 0, EPD_MEMORY_OK);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(c);
    unsigned char *p = rt_memheap_alloc(heaps[0], 256); assert(p);
    memset(p, 0x5a, 256);
    epd_memory_owner_leave_trusted(prev);
    void *barrier = rt_memheap_alloc(heaps[0], 100); assert(barrier);
    assert(!rt_memheap_realloc(heaps[0], p, 600));
    size_t addition[EPD_MEMORY_BACKEND_COUNT] = {0};
    addition[backends[0]] = SIZE_MAX;
    epd_memory_credit_snapshot_t before, after;
    assert(epd_memory_credit_get(c, &before));
    assert(epd_memory_credit_expand(c, addition) == EPD_MEMORY_NO_CAPACITY);
    assert(epd_memory_credit_get(c, &after) && !memcmp(&before, &after, sizeof(before)));
    addition[backends[0]] = 700;
    assert(epd_memory_credit_expand(c, addition) == EPD_MEMORY_OK);
    unsigned char *q = rt_memheap_realloc(heaps[0], p, 600);
    assert(q && q != p && header(q)->owner == c);
    for (unsigned i = 0; i < 256; ++i) assert(q[i] == 0x5a);
    assert(epd_memory_credit_get(c, &after));
    assert(after.unused[backends[0]] + after.physical[backends[0]] == 1300);
    rt_memheap_free(q); rt_memheap_free(barrier);
    epd_memory_credit_end(c); epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a); audit();
}

static void test_credit_replace(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("replace");
    epd_memory_credit_t old = request(a, 20000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t other = request(0, 10000, 0, EPD_MEMORY_OK), prepared;
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, 0};
    epd_memory_credit_snapshot_t snap;
    epd_memory_backend_snapshot_t heap;
    spec.bytes[backends[0]] = 21000;
    assert(epd_memory_credit_replace_prepare(old, a, &spec, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_get(prepared, &snap) && snap.state == EPD_MEMORY_CREDIT_PREPARED);
    assert(snap.unused[backends[0]] == 1000 && snap.prepared_bytes[backends[0]] == 21000);
    assert(epd_memory_backend_get(backends[0], &heap) && heap.usage.unused == 31000);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(prepared);
    assert(!rt_memheap_alloc(heaps[0], 16));
    epd_memory_owner_leave_trusted(previous);
    previous = epd_memory_owner_enter_trusted(old);
    void *p = rt_memheap_alloc(heaps[0], 100); assert(p);
    epd_memory_owner_leave_trusted(previous);
    assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_BUSY);
    rt_memheap_free(p);
    assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_OK);
    assert(!epd_memory_credit_abort(prepared));
    assert(epd_memory_credit_get(old, &snap) && snap.state == EPD_MEMORY_CREDIT_ENDED);
    assert(epd_memory_credit_get(prepared, &snap) && snap.unused[backends[0]] == 21000);
    epd_memory_owner_release_trusted(old);
    old = prepared;
    previous = epd_memory_owner_enter_trusted(old);
    p = rt_memheap_alloc(heaps[0], 100); assert(p);
    epd_memory_owner_leave_trusted(previous);
    spec.bytes[backends[0]] = 1000;
    assert(epd_memory_credit_replace_prepare(old, a, &spec, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_abort(prepared));
    epd_memory_owner_release_trusted(prepared);
    assert(epd_memory_credit_get(old, &snap) && snap.state == EPD_MEMORY_CREDIT_LIVE);
    assert(epd_memory_credit_replace_prepare(old, a, &spec, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(old); /* Existing block alone keeps old token valid. */
    assert(header(p)->owner == old);
    rt_memheap_free(p);
    assert(!epd_memory_credit_get(old, &snap));
    assert(epd_memory_credit_get(prepared, &snap) && snap.unused[backends[0]] == 1000);
    epd_memory_credit_end(prepared); epd_memory_owner_release_trusted(prepared);
    epd_memory_credit_end(other); epd_memory_owner_release_trusted(other);
    epd_memory_owner_release_trusted(a); audit();
}

static void test_credit_replace_wait(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("replace-wait");
    epd_memory_credit_t old = request(a, 1000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t other = request(0, 30000, 0, EPD_MEMORY_OK), prepared;
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, 100};
    epd_memory_credit_snapshot_t snap;
    epd_memory_metadata_snapshot_t metadata;
    spec.bytes[backends[0]] = 5000;
    assert(epd_memory_credit_replace_prepare(old, a, &spec, &prepared) == EPD_MEMORY_WAITING);
    assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_WAITING);
    epd_memory_credit_end(other); epd_memory_owner_release_trusted(other);
    epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(prepared, &snap) && snap.state == EPD_MEMORY_CREDIT_PREPARED);
    assert(epd_memory_credit_abort(prepared)); epd_memory_owner_release_trusted(prepared);
    assert(epd_memory_credit_get(old, &snap) && snap.unused[backends[0]] == 1000);
    other = request(0, 30000, 0, EPD_MEMORY_OK);
    assert(epd_memory_credit_replace_prepare(old, a, &spec, &prepared) == EPD_MEMORY_WAITING);
    assert(epd_memory_credit_get(prepared, &snap));
    epd_memory_poll(snap.deadline_ms);
    assert(epd_memory_credit_get(prepared, &snap) && snap.state == EPD_MEMORY_CREDIT_TIMED_OUT);
    epd_memory_metadata_snapshot(&metadata); assert(!metadata.transactions_used);
    epd_memory_owner_release_trusted(prepared);
    assert(epd_memory_credit_get(old, &snap) && snap.unused[backends[0]] == 1000);
    epd_memory_credit_end(other); epd_memory_owner_release_trusted(other);
    assert(epd_memory_credit_replace_prepare(old, a, &spec, &prepared) == EPD_MEMORY_OK);
    epd_memory_owner_close_trusted(a);
    epd_memory_metadata_snapshot(&metadata); assert(!metadata.transactions_used);
    epd_memory_owner_release_trusted(prepared); epd_memory_owner_release_trusted(old);
    epd_memory_owner_release_trusted(a); audit();
}

static void test_credit_share(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("share-a"), b = epd_memory_owner_create("share-b");
    epd_memory_owner_t shared = epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED);
    epd_memory_credit_t sources[2] = {request(a, 1000, 0, EPD_MEMORY_OK), request(b, 2000, 0, EPD_MEMORY_OK)};
    epd_memory_credit_t prepared, duplicate[2] = {sources[0], sources[0]};
    epd_memory_credit_snapshot_t snap;
    epd_memory_backend_snapshot_t heap;
    assert(epd_memory_credit_share_prepare(duplicate, 2, shared, backends[0], 1000, &prepared) == EPD_MEMORY_INVALID);
    assert(epd_memory_credit_share_prepare(sources, 2, shared, backends[0], 4000, &prepared) == EPD_MEMORY_NO_CAPACITY);
    assert(epd_memory_credit_share_prepare(sources, 2, shared, backends[0], 2500, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_backend_get(backends[0], &heap) && heap.usage.unused == 3000);
    assert(epd_memory_credit_get(prepared, &snap) && !snap.unused[backends[0]] && snap.prepared_bytes[backends[0]] == 2500);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(sources[1]);
    void *p = rt_memheap_alloc(heaps[0], 600); assert(p);
    epd_memory_owner_leave_trusted(previous);
    assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_BUSY);
    assert(epd_memory_credit_get(sources[0], &snap) && snap.unused[backends[0]] == 1000);
    rt_memheap_free(p);
    assert(epd_memory_owner_set_limit(shared, backends[0], 2000) == EPD_MEMORY_OK);
    assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_LIMIT);
    assert(epd_memory_owner_set_limit(shared, backends[0], 3000) == EPD_MEMORY_OK);
    assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_get(sources[0], &snap) && !snap.unused[backends[0]]);
    assert(epd_memory_credit_get(sources[1], &snap) && snap.unused[backends[0]] == 500);
    assert(epd_memory_backend_get(backends[0], &heap) && heap.usage.unused == 3000);
    epd_memory_owner_close_trusted(a); epd_memory_owner_close_trusted(b);
    epd_memory_owner_release_trusted(sources[0]); epd_memory_owner_release_trusted(sources[1]);
    epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(b);
    previous = epd_memory_owner_enter_trusted(prepared);
    p = rt_memheap_alloc(heaps[0], 1000); assert(p && header(p)->owner == prepared);
    epd_memory_owner_leave_trusted(previous); rt_memheap_free(p);
    epd_memory_credit_end(prepared); epd_memory_owner_release_trusted(prepared);
    epd_memory_owner_release_trusted(shared); audit();
}

static void test_share_association(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("share-limit-a"), b = epd_memory_owner_create("share-limit-b");
    epd_memory_owner_t c = epd_memory_owner_create("hit-limit"), shared = epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED);
    epd_memory_credit_t credits[2], prepared;
    epd_memory_account_snapshot_t snap;
    epd_memory_credit_snapshot_t source;
    epd_memory_backend_snapshot_t before, after;
    epd_memory_association_t changes[3] = {{a, 0, 1000}, {b, 0, 500}, {c, 0, 60}};
    assert(epd_memory_owner_set_limit(a, backends[0], 1000) == EPD_MEMORY_OK);
    assert(epd_memory_owner_set_limit(b, backends[0], 500) == EPD_MEMORY_OK);
    assert(epd_memory_owner_set_limit(c, backends[0], 50) == EPD_MEMORY_OK);
    credits[0] = request(a, 1000, 0, EPD_MEMORY_OK); credits[1] = request(b, 500, 0, EPD_MEMORY_OK);
    assert(epd_memory_credit_share_prepare(credits, 2, shared, backends[0], 1500, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_backend_get(backends[0], &before));
    assert(epd_memory_credit_share_commit(prepared, changes, 3) == EPD_MEMORY_LIMIT);
    assert(epd_memory_credit_get(credits[0], &source) && source.unused[backends[0]] == 1000);
    assert(epd_memory_account_snapshot(a, &snap) && !snap.associated[backends[0]]);
    changes[2].new_bytes = 20; changes[0].new_bytes = 1001;
    assert(epd_memory_credit_share_commit(prepared, changes, 3) == EPD_MEMORY_LIMIT);
    changes[0].new_bytes = 1000; changes[2].account = b;
    assert(epd_memory_credit_share_commit(prepared, changes, 3) == EPD_MEMORY_INVALID);
    changes[2].account = c;
    assert(epd_memory_credit_share_commit(prepared, changes, 3) == EPD_MEMORY_OK);
    assert(epd_memory_backend_get(backends[0], &after) && !memcmp(&before, &after, sizeof(before)));
    assert(epd_memory_account_snapshot(a, &snap) && snap.associated[backends[0]] == 1000 && !snap.backend[backends[0]].unused);
    assert(epd_memory_account_snapshot(b, &snap) && snap.associated[backends[0]] == 500 && !snap.backend[backends[0]].unused);
    assert(request(a, 1, 0, EPD_MEMORY_LIMIT) == INVALID);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(prepared);
    void *p = rt_memheap_alloc(heaps[0], 1200); assert(p);
    epd_memory_owner_leave_trusted(previous);
    assert(account_bytes(a, backends[0]) == 0 && account_bytes(b, backends[0]) == 0);
    epd_memory_owner_close_trusted(a);
    assert(epd_memory_association_change(a, backends[0], 1000, 0) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(b, backends[0], 500, 0) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(c, backends[0], 20, 0) == EPD_MEMORY_OK);
    rt_memheap_free(p); epd_memory_credit_end(prepared); epd_memory_owner_release_trusted(prepared);
    epd_memory_credit_end(credits[1]); epd_memory_owner_release_trusted(credits[0]); epd_memory_owner_release_trusted(credits[1]);
    epd_memory_owner_release_trusted(shared); epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(b); epd_memory_owner_release_trusted(c); audit();
}

static void test_association_batch(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("batch-a"), b = epd_memory_owner_create("batch-b");
    epd_memory_association_t changes[2] = {{a, 0, 100}, {b, 0, 200}};
    epd_memory_account_snapshot_t snap;
    epd_memory_owner_snapshot_t owner;
    assert(epd_memory_owner_set_limit(a, backends[0], 100) == EPD_MEMORY_OK);
    assert(epd_memory_owner_set_limit(b, backends[0], 100) == EPD_MEMORY_OK);
    assert(epd_memory_association_batch(backends[0], changes, 2) == EPD_MEMORY_LIMIT);
    assert(epd_memory_account_snapshot(a, &snap) && !snap.associated[backends[0]]);
    changes[1].new_bytes = 100; changes[1].account = a;
    assert(epd_memory_association_batch(backends[0], changes, 2) == EPD_MEMORY_INVALID);
    changes[1].account = b;
    assert(epd_memory_association_batch(backends[0], changes, 2) == EPD_MEMORY_OK);
    assert(epd_memory_account_snapshot(a, &snap) && snap.associated[backends[0]] == 100);
    epd_memory_owner_close_trusted(a); epd_memory_owner_close_trusted(b);
    epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(b);
    changes[0].old_bytes = 100; changes[0].new_bytes = 0;
    changes[1].old_bytes = 101; changes[1].new_bytes = 0;
    assert(epd_memory_association_batch(backends[0], changes, 2) == EPD_MEMORY_INVALID);
    assert(epd_memory_owner_get(a, &owner));
    changes[1].old_bytes = 100;
    assert(epd_memory_association_batch(backends[0], changes, 2) == EPD_MEMORY_OK);
    assert(!epd_memory_owner_get(a, &owner) && !epd_memory_owner_get(b, &owner));
    audit();
}

static void test_credit_associate(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("reuse-a"), b = epd_memory_owner_create("reuse-b");
    epd_memory_credit_snapshot_t credit;
    epd_memory_account_snapshot_t account;
    epd_memory_association_t changes[2] = {{a, 0, 1000}, {b, 0, 11}};
    assert(epd_memory_owner_set_limit(a, backends[0], 1000) == EPD_MEMORY_OK);
    assert(epd_memory_owner_set_limit(b, backends[0], 10) == EPD_MEMORY_OK);
    epd_memory_credit_t first = request(a, 1000, 0, EPD_MEMORY_OK);
    assert(epd_memory_credit_associate(first, backends[0], 1000, changes, 2) == EPD_MEMORY_LIMIT);
    assert(epd_memory_credit_get(first, &credit) && credit.unused[backends[0]] == 1000);
    assert(epd_memory_account_snapshot(a, &account) && !account.associated[backends[0]]);
    assert(epd_memory_credit_associate(first, backends[0], 1000, changes, 1) == EPD_MEMORY_OK);
    assert(epd_memory_credit_get(first, &credit) && !credit.unused[backends[0]]);
    assert(epd_memory_account_snapshot(a, &account) && account.associated[backends[0]] == 1000);
    assert(epd_memory_owner_set_limit(a, backends[0], 2000) == EPD_MEMORY_OK);
    epd_memory_credit_t second = request(a, 1000, 0, EPD_MEMORY_OK);
    assert(epd_memory_credit_associate(second, backends[0], 1000, NULL, 0) == EPD_MEMORY_OK);
    assert(epd_memory_account_snapshot(a, &account) && account.associated[backends[0]] == 1000 && !account.backend[backends[0]].unused);
    epd_memory_credit_end(first); epd_memory_credit_end(second);
    assert(epd_memory_credit_associate(first, backends[0], 1, NULL, 0) == EPD_MEMORY_CLOSED);
    epd_memory_owner_release_trusted(first); epd_memory_owner_release_trusted(second);
    assert(epd_memory_association_change(a, backends[0], 1000, 0) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(a);
    assert(epd_memory_owner_set_limit(b, backends[0], 1000) == EPD_MEMORY_OK);
    first = request(b, 1000, 0, EPD_MEMORY_OK);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(first);
    void *p = rt_memheap_alloc(heaps[0], 128); assert(p);
    epd_memory_owner_leave_trusted(previous);
    size_t used = physical(p);
    changes[0] = (epd_memory_association_t){b, 0, 1000};
    assert(epd_memory_credit_associate(first, backends[0], 1000, changes, 1) == EPD_MEMORY_LIMIT);
    changes[0].new_bytes = 1000 - used;
    assert(epd_memory_credit_associate(first, backends[0], 1000, changes, 1) == EPD_MEMORY_OK);
    assert(epd_memory_credit_get(first, &credit) && !credit.unused[backends[0]] && credit.physical[backends[0]] == used);
    rt_memheap_free(p);
    assert(epd_memory_credit_associate(first, backends[0], used, NULL, 0) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(b, backends[0], 1000 - used, 0) == EPD_MEMORY_OK);
    epd_memory_credit_end(first); epd_memory_owner_release_trusted(first); epd_memory_owner_release_trusted(b); audit();
}

static void test_block_get(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("inspect"), b = epd_memory_owner_create("recipient");
    epd_memory_credit_t credit = request(a, 1000, 0, EPD_MEMORY_OK);
    epd_memory_block_snapshot_t snap, saved;
    memset(&snap, 0x5a, sizeof(snap)); saved = snap;
    assert(epd_memory_block_get(NULL, &snap) == EPD_MEMORY_INVALID);
    assert(!memcmp(&snap, &saved, sizeof(snap)));
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(credit);
    void *p = rt_memheap_alloc(heaps[0], 128); assert(p);
    epd_memory_owner_leave_trusted(previous);
    assert(rt_sem_take(&heaps[0]->lock, RT_WAITING_FOREVER) == RT_EOK);
    assert(epd_memory_block_get(p, &snap) == EPD_MEMORY_BUSY && last_heap_timeout == 0);
    assert(!memcmp(&snap, &saved, sizeof(snap)));
    assert(rt_sem_release(&heaps[0]->lock) == RT_EOK);
    assert(epd_memory_block_get(p, &snap) == EPD_MEMORY_OK);
    assert(snap.owner == credit && snap.account == a && snap.backend == backends[0]);
    assert(snap.size == header(p)->size && snap.physical == physical(p));
    epd_memory_owner_close_trusted(a); epd_memory_owner_release_trusted(credit); epd_memory_owner_release_trusted(a);
    assert(epd_memory_block_get(p, &snap) == EPD_MEMORY_OK && snap.account == a);
    assert(host_transfer(p, b) == EPD_MEMORY_OK);
    assert(epd_memory_block_get(p, &snap) == EPD_MEMORY_OK && snap.owner == b && snap.account == b);
    rt_memheap_free(p); epd_memory_owner_release_trusted(b); audit();
}

static void test_automatic_budget(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("reader-size"), b = epd_memory_owner_create("unrelated");
    epd_memory_owner_t shared = epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED);
    epd_memory_budget_snapshot_t budget;
    epd_memory_backend_snapshot_t heap, after;
    assert(epd_memory_sdk_init() == EPD_MEMORY_OK);
    assert(epd_memory_backend_get(backends[0], &heap));
    size_t capacity = heap.config.capacity - heap.config.unavailable;
    assert(epd_memory_owner_budget(a, backends[0], &budget) && budget.application_limit && budget.effective_limit == capacity);
    assert(epd_memory_owner_budget(shared, backends[0], &budget) && !budget.application_limit && budget.effective_limit == SIZE_MAX);
    epd_memory_credit_t large = request(a, capacity * 3 / 4, 0, EPD_MEMORY_OK);
    epd_memory_credit_end(large); epd_memory_owner_release_trusted(large);
    assert(epd_memory_association_change(a, backends[0], 0, capacity) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(a, backends[0], capacity, capacity + 1) == EPD_MEMORY_LIMIT);
    assert(epd_memory_owner_set_limit(a, backends[0], SIZE_MAX) == EPD_MEMORY_OK);
    assert(epd_memory_owner_budget(a, backends[0], &budget) && budget.effective_limit == capacity);
    assert(epd_memory_backend_get(backends[0], &after) && !after.usage.unused && !after.usage.allocations);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(a);
    assert(!rt_memheap_alloc(heaps[0], 32)); epd_memory_owner_leave_trusted(previous);
    previous = epd_memory_owner_enter_trusted(shared);
    void *system = rt_memheap_alloc(heaps[0], heaps[0]->available_size - RT_ALIGN_SIZE); assert(system);
    epd_memory_owner_leave_trusted(previous);
    assert(physical(system) == capacity && !heaps[0]->available_size);
    /* Pure hits remain admissible even when the actual shared pool is full. */
    assert(epd_memory_association_change(b, backends[0], 0, capacity) == EPD_MEMORY_OK);
    rt_memheap_free(system);
    epd_memory_backend_config_t smaller = heap.config;
    smaller.unavailable = capacity / 2;
    epd_memory_backend_t same;
    assert(epd_memory_sdk_register_heap(heap.key, &smaller, &same) == EPD_MEMORY_OK && same == backends[0]);
    assert(epd_memory_owner_budget(a, same, &budget) && budget.above_limit && budget.effective_limit == capacity - smaller.unavailable);
    assert(epd_memory_association_change(a, same, capacity, capacity / 2) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(a, same, capacity / 2, capacity) == EPD_MEMORY_LIMIT);
    assert(epd_memory_sdk_register_heap(heap.key, &heap.config, &same) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(a, same, capacity / 2, capacity) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(a, same, capacity, 0) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(b, same, capacity, 0) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(shared); epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(b); audit();
}

static void test_shared_duplicate_promises(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("same-rule-a"), b = epd_memory_owner_create("same-rule-b");
    epd_memory_credit_t credits[2], prepared;
    epd_memory_credit_snapshot_t credit;
    epd_memory_backend_snapshot_t heap;
    epd_memory_association_t changes[2] = {{a, 0, 64}, {b, 0, 64}};
    assert(epd_memory_owner_set_limit(a, backends[0], 64) == EPD_MEMORY_OK);
    assert(epd_memory_owner_set_limit(b, backends[0], 64) == EPD_MEMORY_OK);
    credits[0] = request(a, 64, 0, EPD_MEMORY_OK); credits[1] = request(b, 64, 0, EPD_MEMORY_OK);
    assert(epd_memory_credit_share_prepare(credits, 2, 0, backends[0], 64, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_share_commit(prepared, changes, 2) == EPD_MEMORY_OK);
    assert(epd_memory_credit_get(credits[0], &credit) && !credit.unused[backends[0]]);
    assert(epd_memory_credit_get(credits[1], &credit) && !credit.unused[backends[0]]);
    assert(epd_memory_backend_get(backends[0], &heap) && heap.usage.unused == 64);
    epd_memory_credit_end(prepared); epd_memory_owner_release_trusted(prepared);
    for (unsigned i = 0; i < 2; ++i) {
        assert(epd_memory_association_change(changes[i].account, backends[0], 64, 0) == EPD_MEMORY_OK);
        epd_memory_credit_end(credits[i]); epd_memory_owner_release_trusted(credits[i]); epd_memory_owner_release_trusted(changes[i].account);
    }
    audit();
}

static void test_budget_lowered_promise(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("accepted"), b = epd_memory_owner_create("reuse");
    epd_memory_credit_t source = request(a, 1000, 0, EPD_MEMORY_OK), prepared;
    epd_memory_association_t change = {a, 0, 1000};
    assert(epd_memory_owner_set_limit(a, backends[0], 500) == EPD_MEMORY_OK);
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, 0};
    spec.bytes[backends[0]] = 1000;
    assert(epd_memory_credit_replace_prepare(source, a, &spec, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(source); source = prepared;
    assert(epd_memory_credit_share_prepare(&source, 1, 0, backends[0], 1000, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_share_commit(prepared, &change, 1) == EPD_MEMORY_OK);
    assert(request(a, 1, 0, EPD_MEMORY_LIMIT) == INVALID);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(prepared);
    void *p = rt_memheap_alloc(heaps[0], 256); assert(p);
    epd_memory_owner_leave_trusted(previous); rt_memheap_free(p);
    epd_memory_credit_end(prepared); epd_memory_owner_release_trusted(prepared);
    epd_memory_credit_end(source); epd_memory_owner_release_trusted(source);
    assert(epd_memory_association_change(a, backends[0], 1000, 0) == EPD_MEMORY_OK);
    source = request(b, 1000, 0, EPD_MEMORY_OK); change.account = b;
    assert(epd_memory_owner_set_limit(b, backends[0], 500) == EPD_MEMORY_OK);
    assert(epd_memory_credit_associate(source, backends[0], 1000, &change, 1) == EPD_MEMORY_OK);
    assert(epd_memory_association_change(b, backends[0], 1000, 1001) == EPD_MEMORY_LIMIT);
    assert(epd_memory_association_change(b, backends[0], 1000, 0) == EPD_MEMORY_OK);
    epd_memory_credit_end(source); epd_memory_owner_release_trusted(source);
    epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(b); audit();
}

static void test_diagnostic(void)
{
    epd_memory_diagnostic_snapshot_t s;
    assert(!diagnostic_reads);
    assert(epd_memory_diagnostic_start(0, 1) == EPD_MEMORY_INVALID);
    assert(epd_memory_diagnostic_start(1000001, 1) == EPD_MEMORY_INVALID);
    assert(epd_memory_diagnostic_start(1, 60001) == EPD_MEMORY_INVALID);
    diagnostic_counter = UINT32_MAX - 150;
    assert(epd_memory_diagnostic_start(1, 1000) == EPD_MEMORY_OK);
    (void)epd_memory_owner_current();
    assert(epd_memory_diagnostic_read(&s) && !s.active && s.exhausted && !s.remaining);
    assert(s.path[EPD_MEMORY_DIAG_CONTROL].samples == 1 && s.path[EPD_MEMORY_DIAG_CONTROL].max_cycles == 100);
    unsigned reads = diagnostic_reads;
    (void)epd_memory_owner_current(); assert(diagnostic_reads == reads);
    epd_memory_owner_t owner = epd_memory_owner_create("diagnostic-nested");
    assert(epd_memory_diagnostic_start(100, 1000) == EPD_MEMORY_OK);
    rt_base_t level = rt_hw_interrupt_disable();
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    epd_memory_owner_leave_trusted(previous);
    rt_hw_interrupt_enable(level);
    void *p = rt_memheap_alloc(heaps[0], 64); assert(p); rt_memheap_free(p);
    assert(epd_memory_diagnostic_read(&s) && s.active && s.nested_calls);
    assert(s.path[EPD_MEMORY_DIAG_CONTROL].entered_irq_off);
    assert(s.path[EPD_MEMORY_DIAG_ADMIT].samples && s.path[EPD_MEMORY_DIAG_SETTLE].samples == 2);
    epd_memory_diagnostic_stop(); reads = diagnostic_reads;
    (void)epd_memory_owner_current(); assert(diagnostic_reads == reads);
    assert(epd_memory_diagnostic_start(100, 1) == EPD_MEMORY_OK);
    ++ticks;
    assert(epd_memory_diagnostic_read(&s) && !s.active && s.timed_out && s.remaining == 100);
    epd_memory_owner_release_trusted(owner);
    audit();
}

static void test_rejection(void)
{
    epd_memory_rejection_snapshot_t s;
    epd_memory_owner_t a = epd_memory_owner_create("reject");
    epd_memory_credit_t c = request(a, 512, 0, EPD_MEMORY_OK);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(c);
    assert(!rt_memheap_alloc(heaps[1], 64));
    assert(epd_memory_rejection_read(&s) && !s.active && !s.valid);
    assert(epd_memory_rejection_start(0, 10) == EPD_MEMORY_INVALID);
    assert(epd_memory_rejection_start(1025, 10) == EPD_MEMORY_INVALID);
    assert(epd_memory_rejection_start(1, 60001) == EPD_MEMORY_INVALID);
    rt_base_t level = rt_hw_interrupt_disable();
    assert(epd_memory_rejection_start(1, 10) == EPD_MEMORY_BUSY);
    rt_hw_interrupt_enable(level);
    assert(epd_memory_rejection_start(2, 1000) == EPD_MEMORY_OK);
    assert(!rt_memheap_alloc(heaps[1], 64));
    assert(epd_memory_rejection_read(&s) && s.valid && s.active && s.captured == 1);
    assert(s.token == c && s.current_token == c && s.account == a && s.backend == backends[1]);
    assert(s.reason == EPD_MEMORY_REJECT_CREDIT_UNUSED && s.result == EPD_MEMORY_NO_CAPACITY);
    assert(s.kind == EPD_MEMORY_CHANGE_ALLOC && s.scope == EPD_MEMORY_SCOPE_CREDIT);
    assert(s.credit_state == EPD_MEMORY_CREDIT_LIVE && !s.closed && !s.unused);
    assert(s.size == 64 && s.physical == 64 + H && s.delta == s.physical && s.available_after > 10000);
    epd_memory_owner_close_trusted(a);
    assert(!rt_memheap_alloc(heaps[0], 64));
    assert(epd_memory_rejection_read(&s) && s.captured == 2 && s.exhausted && !s.active);
    assert(s.reason == EPD_MEMORY_REJECT_CLOSED_PATH && s.result == EPD_MEMORY_CLOSED);
    assert(!rt_memheap_alloc(heaps[1], 64));
    assert(epd_memory_rejection_read(&s) && s.captured == 2 && s.backend == backends[0]);
    ticks = UINT32_MAX - 5;
    assert(epd_memory_rejection_start(2, 1000) == EPD_MEMORY_OK);
    ticks += RT_TICK_PER_SECOND;
    assert(!rt_memheap_alloc(heaps[1], 64));
    assert(epd_memory_rejection_read(&s) && s.timed_out && !s.active && !s.valid);
    epd_memory_owner_leave_trusted(previous);
    epd_memory_credit_end(c); epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a);
    assert(epd_memory_rejection_start(2, 1000) == EPD_MEMORY_OK);
    /* Corrupt context models a stale SDK token before heap selection. */
    actors[0].mem_context = INVALID;
    assert(!rt_memheap_alloc(heaps[0], 64));
    actors[0].mem_context = 0;
    assert(epd_memory_rejection_read(&s) && s.reason == EPD_MEMORY_REJECT_CONTEXT);
    assert(s.token == INVALID && s.current_token == INVALID && s.backend == EPD_MEMORY_BACKEND_NONE);
    struct rt_memheap_owner_event invalid_event = {0};
    invalid_event.heap = heaps[0]; invalid_event.operation = 255;
    assert(rt_memheap_owner_try_change(&invalid_event) != RT_EOK);
    assert(epd_memory_rejection_read(&s) && s.reason == EPD_MEMORY_REJECT_ADAPTER && s.exhausted);
    assert(epd_memory_rejection_start(2, 1000) == EPD_MEMORY_OK);
    epd_memory_rejection_stop();
    assert(epd_memory_try_change(NULL) == EPD_MEMORY_INVALID);
    assert(epd_memory_rejection_read(&s) && !s.active && !s.valid);
    assert(!diagnostic_reads); /* Rejection capture never starts DWT sampling. */
    audit();
}

static void test_rejection_realloc(void)
{
    epd_memory_rejection_snapshot_t s;
    void *table = rt_malloc(32), *barrier = rt_malloc(32);
    assert(table && barrier && header(table)->owner == 0);
    epd_memory_owner_t a = epd_memory_owner_create("table-owner");
    epd_memory_credit_t c = request(a, 1024, 0, EPD_MEMORY_OK);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(c);
    void *owned = rt_memheap_alloc(heaps[0], 32); assert(owned);
    assert(epd_memory_rejection_start(8, 1000) == EPD_MEMORY_OK);
    void *next = rt_realloc(table, 256);
    assert(next && next != table && header(next)->owner == 0);
    table = next;
    assert(epd_memory_rejection_read(&s) && !s.valid);
    epd_memory_credit_end(c);
    next = rt_realloc(table, 512); assert(next && header(next)->owner == 0);
    table = next;
    assert(epd_memory_rejection_read(&s) && !s.valid);
    epd_memory_owner_leave_trusted(previous);
    assert(!rt_memheap_realloc(heaps[0], owned, 512));
    assert(epd_memory_rejection_read(&s) && s.valid && s.result == EPD_MEMORY_CLOSED);
    assert(s.reason == EPD_MEMORY_REJECT_CLOSED_PATH && s.credit_state == EPD_MEMORY_CREDIT_ENDED && s.closed);
    assert(s.token == c && s.current_token == 0 && s.account == a);
    assert(s.kind == EPD_MEMORY_CHANGE_REALLOC_INPLACE && s.old_physical == physical(owned));
    assert(s.delta == s.physical - s.old_physical && header(owned)->owner == c);
    rt_free(table); rt_free(barrier); rt_memheap_free(owned);
    epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a);
    /* Snapshot remains displayable after the descriptor is released. */
    assert(epd_memory_rejection_read(&s) && s.token == c);
    epd_memory_rejection_stop(); audit();
}

static void test_credit_transactions_bounded(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("bounded-tx");
    epd_memory_credit_t source = request(a, 1000, 0, EPD_MEMORY_OK), prepared[128], failed;
    epd_memory_metadata_snapshot_t metadata;
    epd_memory_credit_snapshot_t snap;
    epd_memory_metadata_snapshot(&metadata);
    unsigned count = metadata.transactions_capacity;
    assert(count < 128);
    for (unsigned i = 0; i < count; ++i)
        assert(epd_memory_credit_share_prepare(&source, 1, 0, backends[0], 100, &prepared[i]) == EPD_MEMORY_OK);
    assert(epd_memory_credit_share_prepare(&source, 1, 0, backends[0], 100, &failed) == EPD_MEMORY_NO_METADATA);
    assert(failed == EPD_MEMORY_OWNER_INVALID);
    epd_memory_metadata_snapshot(&metadata); assert(metadata.transactions_used == count);
    epd_memory_owner_close_trusted(a);
    for (unsigned i = 0; i < count; ++i) {
        assert(epd_memory_credit_commit(prepared[i]) == EPD_MEMORY_CLOSED);
        assert(epd_memory_credit_abort(prepared[i])); epd_memory_owner_release_trusted(prepared[i]);
    }
    epd_memory_metadata_snapshot(&metadata); assert(!metadata.transactions_used);
    assert(epd_memory_credit_get(source, &snap) && snap.state == EPD_MEMORY_CREDIT_CANCELLED);
    epd_memory_owner_release_trusted(source); epd_memory_owner_release_trusted(a); audit();
}

static void test_cleanup(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("cleanup");
    epd_memory_owner_t page = epd_memory_scope_create_trusted(a, EPD_MEMORY_SCOPE_PAGE);
    epd_memory_owner_t clean = epd_memory_scope_create_trusted(a, EPD_MEMORY_SCOPE_CLEANUP);
    epd_memory_owner_t nested = epd_memory_scope_create_trusted(clean, EPD_MEMORY_SCOPE_CLEANUP);
    epd_memory_credit_t normal = request(page, 1000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t accepted = request(clean, 1000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t nested_credit = request(nested, 1000, 0, EPD_MEMORY_OK);
    epd_memory_credit_snapshot_t snap;
    assert(epd_memory_owner_close_trusted(a));
    assert(epd_memory_owner_close_trusted(a));
    assert(epd_memory_credit_get(normal, &snap) && snap.state == EPD_MEMORY_CREDIT_CANCELLED);
    assert(epd_memory_credit_get(accepted, &snap) && snap.state == EPD_MEMORY_CREDIT_LIVE);
    request(clean, 10, 0, EPD_MEMORY_CLOSED);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(clean);
    assert(!rt_memheap_alloc(heaps[0], 16));
    epd_memory_owner_leave_trusted(prev);
    size_t extra[EPD_MEMORY_BACKEND_COUNT] = {0};
    extra[backends[0]] = 1;
    assert(epd_memory_credit_expand(accepted, extra) == EPD_MEMORY_CLOSED);
    prev = epd_memory_owner_enter_trusted(accepted);
    void *p = rt_memheap_alloc(heaps[0], 100); assert(p);
    assert(!rt_memheap_alloc(heaps[0], 2000));
    epd_memory_owner_close_trusted(clean);
    assert(epd_memory_credit_get(nested_credit, &snap) && snap.state == EPD_MEMORY_CREDIT_CANCELLED);
    assert(!rt_memheap_alloc(heaps[0], 8));
    rt_memheap_free(p);
    epd_memory_owner_leave_trusted(prev);
    epd_memory_owner_release_trusted(normal); epd_memory_owner_release_trusted(accepted);
    epd_memory_owner_release_trusted(nested_credit); epd_memory_owner_release_trusted(nested);
    epd_memory_owner_release_trusted(page); epd_memory_owner_release_trusted(clean);
    epd_memory_owner_snapshot_t owner;
    assert(epd_memory_owner_get(a, &owner) && owner.references == 1);
    epd_memory_owner_release_trusted(a); audit();
}

static void test_close_credit(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("close-credit");
    epd_memory_owner_t clean = epd_memory_scope_create_trusted(a, EPD_MEMORY_SCOPE_CLEANUP);
    epd_memory_credit_t c = request(clean, 1000, 0, EPD_MEMORY_OK);
    epd_memory_owner_close_trusted(a);
    epd_memory_owner_close_trusted(c);
    epd_memory_credit_snapshot_t snap;
    assert(epd_memory_credit_get(c, &snap) && snap.state == EPD_MEMORY_CREDIT_CANCELLED);
    epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(clean); epd_memory_owner_release_trusted(a); audit();
}

static void test_realloc_peak(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("move");
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(a);
    unsigned char *p = rt_memheap_alloc(heaps[0], 256);
    void *barrier = rt_memheap_alloc(heaps[0], 256);
    assert(p && barrier); memset(p, 0x5a, 256);
    size_t old_total = physical(p) + physical(barrier);
    epd_memory_owner_leave_trusted(prev);
    assert(epd_memory_owner_set_limit(a, backends[0], old_total + 600) == EPD_MEMORY_OK);
    assert(!rt_memheap_realloc(heaps[0], p, 600)); /* Full new block needs its header too. */
    for (unsigned i = 0; i < 256; ++i) assert(p[i] == 0x5a);
    assert(epd_memory_owner_set_limit(a, backends[0], old_total + 600 + H) == EPD_MEMORY_OK);
    unsigned char *q = rt_memheap_realloc(heaps[0], p, 600);
    assert(q && q != p && header(q)->owner == a);
    epd_memory_account_snapshot_t snap;
    assert(epd_memory_account_snapshot(a, &snap));
    assert(snap.backend[backends[0]].peak_committed == old_total + physical(q));
    for (unsigned i = 0; i < 256; ++i) assert(q[i] == 0x5a);
    rt_memheap_free(barrier); rt_memheap_free(q);
    epd_memory_owner_release_trusted(a); audit();
}

static void test_unsplit(void)
{
    void *p = rt_memheap_alloc(heaps[0], 256), *barrier = rt_memheap_alloc(heaps[0], 100);
    assert(p && barrier); rt_memheap_free(p);
    epd_memory_owner_t a = epd_memory_owner_create("unsplit");
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(a);
    assert(epd_memory_owner_set_limit(a, backends[0], 252 + H) == EPD_MEMORY_OK);
    assert(!rt_memheap_alloc(heaps[0], 252));
    assert(epd_memory_owner_set_limit(a, backends[0], 256 + H) == EPD_MEMORY_OK);
    p = rt_memheap_alloc(heaps[0], 252);
    assert(p && physical(p) == 256 + H && account_bytes(a, backends[0]) == 256 + H);
    rt_memheap_free(p); epd_memory_owner_leave_trusted(prev);
    rt_memheap_free(barrier); epd_memory_owner_release_trusted(a); audit();
}

static epd_memory_result_t host_transfer(void *p, epd_memory_owner_t to)
{
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(to);
    epd_memory_result_t result = epd_memory_block_transfer(p, to);
    if (old != INVALID) epd_memory_owner_leave_trusted(old);
    return result;
}
static epd_memory_result_t host_transfer_try(void *p, epd_memory_owner_t to)
{
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(to);
    epd_memory_result_t result = epd_memory_block_transfer_try(p, to);
    if (old != INVALID) epd_memory_owner_leave_trusted(old);
    return result;
}
static void test_public_transfer_account(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("caller"), b = epd_memory_owner_create("foreign");
    epd_memory_owner_t shared = epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED);
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(a);
    epd_memory_owner_t page = epd_memory_scope_create(a, EPD_MEMORY_SCOPE_PAGE);
    void *p = rt_memheap_alloc(heaps[0], 64); assert(p);
    assert(epd_memory_owner_set_limit(a, backends[0], physical(p)) == EPD_MEMORY_OK);
    epd_memory_owner_t targets[] = {0, b, shared};
    for (unsigned i = 0; i < 3; ++i) {
        assert(epd_memory_block_transfer(p, targets[i]) == EPD_MEMORY_INVALID);
        assert(epd_memory_block_transfer_try(p, targets[i]) == EPD_MEMORY_INVALID);
        assert(header(p)->owner == a && !rt_memheap_alloc(heaps[0], 1));
    }
    assert(epd_memory_block_transfer(p, page) == EPD_MEMORY_OK && header(p)->owner == page);
    assert(epd_memory_block_transfer_try(p, a) == EPD_MEMORY_OK && header(p)->owner == a);
    epd_memory_owner_leave_trusted(old);
    assert(epd_memory_block_transfer_try_trusted(p, b) == EPD_MEMORY_OK && header(p)->owner == b);
    rt_memheap_free(p);
    epd_memory_owner_release_trusted(page); epd_memory_owner_release_trusted(shared);
    epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(b); audit();
}
static void test_shared_font_settlement(void)
{
    const char *ids[] = {"memdemo", "mx731"};
    for (unsigned i = 0; i < 2; ++i) {
        epd_memory_owner_t app = epd_memory_owner_create(ids[i]);
        epd_resource_cache_t cache = {1, epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED)};
        epd_memory_credit_t credit = request(app, 1024, 0, EPD_MEMORY_OK);
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(credit);
        void *p = rt_memheap_alloc(heaps[0], 64); assert(p);
        size_t charge = physical(p), shared_before = account_bytes(0, backends[0]);
        assert(epd_resource_cache_adopt(&cache, p) == EPD_RESOURCE_INVALID);
        assert(header(p)->owner == credit && epd_memory_credit_current() == credit);
        /* Platform settlement must still honor the destination's existing cap. */
        assert(epd_memory_owner_set_limit(cache.owner, backends[0], charge - 1) == EPD_MEMORY_OK);
        assert(ui_font_memory_cache_adopt(&cache, p) != EPD_RESOURCE_OK);
        assert(header(p)->owner == credit && epd_memory_credit_current() == credit);
        assert(epd_memory_owner_set_limit(cache.owner, backends[0], SIZE_MAX) == EPD_MEMORY_OK);
        epd_memory_credit_end(credit);
        assert(ui_font_memory_cache_adopt(&cache, p) == EPD_RESOURCE_OK);
        assert(header(p)->owner == cache.owner && epd_memory_credit_current() == credit);
        assert(account_bytes(0, backends[0]) == shared_before + charge);
        assert(!account_bytes(app, backends[0]));
        assert(epd_resource_cache_adopt(&cache, p) == EPD_RESOURCE_INVALID);
        epd_resource_cache_t stale = {1, EPD_MEMORY_OWNER_INVALID};
        assert(ui_font_memory_cache_adopt(&stale, p) == EPD_RESOURCE_STOPPED);
        assert(epd_memory_credit_current() == credit);
        rt_memheap_free(p);
        epd_memory_owner_leave_trusted(previous);
        epd_memory_owner_release_trusted(credit);
        epd_memory_owner_release_trusted(app);
        epd_memory_owner_release_trusted(cache.owner);
        audit();
    }
}
static void test_transfer(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("source"), b = epd_memory_owner_create("destination");
    epd_memory_credit_t c = request(a, 1000, 0, EPD_MEMORY_OK);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(c);
    void *p = rt_memheap_alloc(heaps[0], 256); assert(p);
    epd_memory_owner_leave_trusted(prev);
    epd_memory_credit_end(c);
    epd_memory_owner_release_trusted(c);
    assert(epd_memory_owner_set_limit(b, backends[0], 1) == EPD_MEMORY_OK);
    assert(host_transfer(p, b) != EPD_MEMORY_OK && header(p)->owner == c);
    assert(epd_memory_owner_set_limit(b, backends[0], 1000) == EPD_MEMORY_OK);
    assert(host_transfer(p, b) == EPD_MEMORY_OK && header(p)->owner == b);
    epd_memory_owner_snapshot_t gone;
    assert(!epd_memory_owner_get(c, &gone));
    assert(!account_bytes(a, backends[0]) && account_bytes(b, backends[0]) == physical(p));
    rt_memheap_free(p); epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(b); audit();
}

static void test_transfer_try(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("try-source");
    epd_memory_owner_t b = epd_memory_owner_create("try-destination");
    epd_memory_credit_t credit = request(a, 1000, 0, EPD_MEMORY_OK);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(credit);
    void *p = rt_memheap_alloc(heaps[0], 256); assert(p);
    epd_memory_owner_leave_trusted(previous);
    epd_memory_account_snapshot_t before, after;
    epd_memory_backend_snapshot_t heap_before, heap_after;
    epd_memory_credit_snapshot_t credit_before, credit_after;
    struct rt_memheap_item original;
    memcpy(&original, header(p), sizeof(original));
    assert(epd_memory_account_snapshot(a, &before));
    assert(epd_memory_backend_get(backends[0], &heap_before));
    assert(epd_memory_credit_get(credit, &credit_before));
    assert(rt_sem_take(&heaps[0]->lock, RT_WAITING_FOREVER) == RT_EOK);
    rt_current_thread = &actors[1];
    assert(host_transfer_try(p, b) == EPD_MEMORY_BUSY);
    assert(last_heap_timeout == 0 && heap_locks == 1 && !heaps[0]->lock.value);
    assert(!memcmp(header(p), &original, sizeof(original)));
    assert(epd_memory_account_snapshot(a, &after) && !memcmp(&before, &after, sizeof(before)));
    assert(epd_memory_backend_get(backends[0], &heap_after) &&
           !memcmp(&heap_before, &heap_after, sizeof(heap_before)));
    rt_current_thread = &actors[0];
    assert(rt_sem_release(&heaps[0]->lock) == RT_EOK);
    assert(host_transfer_try(NULL, b) == EPD_MEMORY_INVALID);
    assert(host_transfer_try(p, INVALID) == EPD_MEMORY_INVALID);
    assert(epd_memory_owner_set_limit(b, backends[0], 1) == EPD_MEMORY_OK);
    assert(host_transfer_try(p, b) == EPD_MEMORY_BUSY);
    assert(last_heap_timeout == 0 && !heap_locks && heaps[0]->lock.value == 1);
    assert(!memcmp(header(p), &original, sizeof(original)));
    assert(epd_memory_account_snapshot(a, &after) && !memcmp(&before, &after, sizeof(before)));
    assert(epd_memory_owner_set_limit(b, backends[0], 1000) == EPD_MEMORY_OK);
    assert(host_transfer_try(p, b) == EPD_MEMORY_OK);
    assert(last_heap_timeout == 0 && header(p)->owner == b);
    assert(epd_memory_credit_get(credit, &credit_after) &&
           credit_after.unused[backends[0]] == credit_before.unused[backends[0]]);
    assert(!account_bytes(a, backends[0]) && account_bytes(b, backends[0]) == physical(p));
    assert(epd_memory_backend_get(backends[0], &heap_after) &&
           !memcmp(&heap_before, &heap_after, sizeof(heap_before)));
    epd_memory_owner_t pending = epd_memory_scope_create_trusted(b, EPD_MEMORY_SCOPE_TASK);
    assert(pending != INVALID);
    epd_memory_owner_close_trusted(b);
    assert(host_transfer_try(p, pending) == EPD_MEMORY_OK && header(p)->owner == pending);
    assert(last_heap_timeout == 0 && account_bytes(b, backends[0]) == physical(p));
    rt_memheap_free(p);
    epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit);
    epd_memory_owner_release_trusted(pending);
    epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(b); audit();
}

static void test_closed_transfer(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("settle"), other = epd_memory_owner_create("other");
    epd_memory_owner_t pending = epd_memory_scope_create_trusted(a, EPD_MEMORY_SCOPE_TASK);
    epd_memory_credit_t credit = request(a, 1000, 0, EPD_MEMORY_OK);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(credit);
    void *p = rt_memheap_alloc(heaps[0], 100); assert(p);
    epd_memory_owner_leave_trusted(previous);
    size_t charged = physical(p);
    epd_memory_owner_close_trusted(a); epd_memory_owner_close_trusted(other);
    epd_memory_credit_snapshot_t snapshot;
    assert(epd_memory_credit_get(credit, &snapshot) && !snapshot.unused[backends[0]]);
    assert(host_transfer(p, other) != EPD_MEMORY_OK && header(p)->owner == credit);
    assert(epd_memory_owner_set_limit(pending, backends[0], charged - 1) == EPD_MEMORY_OK);
    assert(host_transfer(p, pending) != EPD_MEMORY_OK && header(p)->owner == credit);
    assert(epd_memory_owner_set_limit(pending, backends[0], charged) == EPD_MEMORY_OK);
    assert(epd_memory_owner_set_class(pending, EPD_MEMORY_PENDING_FREE) == EPD_MEMORY_OK);
    assert(host_transfer(p, pending) == EPD_MEMORY_OK && header(p)->owner == pending);
    assert(account_bytes(a, backends[0]) == charged);
    epd_memory_account_snapshot_t account;
    assert(epd_memory_account_snapshot(a, &account) &&
           account.backend[backends[0]].physical[EPD_MEMORY_PENDING_FREE] == charged);
    assert(!rt_memheap_realloc(heaps[0], p, 200));
    previous = epd_memory_owner_enter_trusted(pending);
    assert(!rt_memheap_alloc(heaps[0], 4));
    epd_memory_owner_leave_trusted(previous);
    rt_memheap_free(p);
    assert(epd_memory_credit_get(credit, &snapshot) && !snapshot.unused[backends[0]]);
    epd_memory_owner_release_trusted(credit); epd_memory_owner_release_trusted(pending);
    epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(other); audit();
}

static void test_waits(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("waiter");
    epd_memory_credit_t held = request(a, 30000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t old = request(a, 4000, 1000, EPD_MEMORY_WAITING);
    for (unsigned i = 0; i < 3; ++i) {
        void *p = rt_memheap_alloc(heaps[0], 32); assert(p); rt_memheap_free(p);
    }
    assert(!rt_memheap_alloc(heaps[0], 32));
    void *other = rt_memheap_alloc(heaps[1], 32); assert(other); rt_memheap_free(other);
    epd_memory_credit_snapshot_t snap;
    assert(epd_memory_credit_get(old, &snap) && snap.bypasses == 3);
    epd_memory_credit_end(held);
    epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(old, &snap) && snap.state == EPD_MEMORY_CREDIT_LIVE);
    epd_memory_credit_cancel(old);
    epd_memory_credit_end(old); /* First terminal transition wins. */
    assert(epd_memory_credit_get(old, &snap) && snap.state == EPD_MEMORY_CREDIT_CANCELLED);
    epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(old); epd_memory_owner_release_trusted(a); audit();
}

static void test_timeout(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("timeout");
    epd_memory_credit_t held = request(a, 30000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t waiting = request(a, 4000, 100, EPD_MEMORY_WAITING);
    uint32_t delay;
    assert(epd_memory_next_deadline(epd_memory_now_ms(), &delay) && delay == 100);
    ticks += RT_TICK_PER_SECOND / 10;
    epd_memory_poll(epd_memory_now_ms());
    epd_memory_credit_snapshot_t snap;
    assert(epd_memory_credit_get(waiting, &snap) && snap.state == EPD_MEMORY_CREDIT_TIMED_OUT);
    epd_memory_credit_cancel(waiting);
    assert(epd_memory_credit_get(waiting, &snap) && snap.state == EPD_MEMORY_CREDIT_TIMED_OUT);
    epd_memory_owner_release_trusted(waiting); epd_memory_credit_end(held);
    epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(a); audit();
}

static void test_wait_independent_capacity(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("multi-wait");
    epd_memory_credit_t held = request(a, 30000, 0, EPD_MEMORY_OK), waiter;
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, 1000};
    epd_memory_credit_snapshot_t snapshot;
    spec.bytes[backends[0]] = 4000; spec.bytes[backends[1]] = 1000;
    assert(epd_memory_credit_request(a, &spec, &waiter) == EPD_MEMORY_WAITING);
    ticks += RT_TICK_PER_SECOND / 2;
    for (unsigned i = 0; i < 20; ++i) {
        void *p = rt_memheap_alloc(heaps[1], 768);
        assert(p); rt_memheap_free(p);
    }
    assert(epd_memory_credit_get(waiter, &snapshot) && !snapshot.bypasses);
    epd_memory_credit_end(held); epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(waiter, &snapshot) && snapshot.state == EPD_MEMORY_CREDIT_LIVE);
    epd_memory_credit_end(waiter); epd_memory_owner_release_trusted(waiter);
    epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(a); audit();
}

static void test_deadline_wrap(void)
{
    ticks = (rt_tick_t)(UINT32_MAX / (1000u / RT_TICK_PER_SECOND) - 5u);
    epd_memory_owner_t a = epd_memory_owner_create("wrap");
    epd_memory_credit_t held = request(a, 30000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t waiter = request(a, 4000, 100, EPD_MEMORY_WAITING);
    epd_memory_credit_snapshot_t snap;
    ticks += RT_TICK_PER_SECOND / 10u - 1u;
    epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(waiter, &snap) && snap.state == EPD_MEMORY_CREDIT_WAITING);
    ++ticks;
    epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(waiter, &snap) && snap.state == EPD_MEMORY_CREDIT_TIMED_OUT);
    epd_memory_owner_release_trusted(waiter); epd_memory_credit_end(held);
    epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(a); audit();
}

static void test_stale_poll(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("stale-clock");
    epd_memory_credit_t held = request(0, 30000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t waiter = request(a, 4000, 100, EPD_MEMORY_WAITING);
    epd_memory_credit_snapshot_t snap;
    uint32_t sampled = epd_memory_now_ms(), delay;
    ticks += RT_TICK_PER_SECOND / 10u;
    /* A worker can be preempted after sampling and before polling. */
    epd_memory_poll(sampled);
    assert(epd_memory_now_ms() - sampled == 100);
    assert(epd_memory_credit_get(waiter, &snap) && snap.state == EPD_MEMORY_CREDIT_TIMED_OUT);
    epd_memory_owner_release_trusted(waiter);
    waiter = request(a, 4000, 100, EPD_MEMORY_WAITING);
    sampled = epd_memory_now_ms();
    ticks += RT_TICK_PER_SECOND / 10u;
    assert(epd_memory_next_deadline(sampled, &delay) && !delay);
    epd_memory_poll(sampled);
    assert(epd_memory_credit_get(waiter, &snap) && snap.state == EPD_MEMORY_CREDIT_TIMED_OUT);
    epd_memory_owner_release_trusted(waiter); epd_memory_credit_end(held);
    epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(a); audit();
}

static unsigned capacity_events;
static void capacity_event(void)
{
    assert(!irq_depth && !heap_locks && !critical_depth);
    ++capacity_events;
}

static void test_capacity_events(void)
{
    epd_memory_set_capacity_notifier(capacity_event);
    assert(capacity_events == 1);
    epd_memory_dispatch_changes(); assert(capacity_events == 1);
    epd_memory_owner_t a = epd_memory_owner_create("capacity-event");
    void *p = rt_memheap_alloc(heaps[0], 29000); assert(p);
    epd_memory_credit_t waiter = request(a, 5000, 1000, EPD_MEMORY_WAITING);
    uint32_t epoch = epd_memory_capacity_epoch();
    unsigned events = capacity_events;
    rt_memheap_free(p);
    assert(epd_memory_capacity_epoch() != epoch && capacity_events == events + 1);
    epd_memory_poll(epd_memory_now_ms());
    epd_memory_credit_snapshot_t snap;
    assert(epd_memory_credit_get(waiter, &snap) && snap.state == EPD_MEMORY_CREDIT_LIVE);
    events = capacity_events;
    rt_base_t level = rt_hw_interrupt_disable();
    epd_memory_credit_end(waiter);
    assert(capacity_events == events);
    rt_hw_interrupt_enable(level);
    epd_memory_dispatch_changes(); assert(capacity_events == events + 1);
    epd_memory_owner_release_trusted(waiter);
    waiter = request(a, 5000, 0, EPD_MEMORY_OK);
    events = capacity_events;
    assert(epd_memory_credit_trim(waiter, backends[0], 100) == EPD_MEMORY_OK);
    assert(capacity_events == events + 1);
    /* A thread's final credit context reference also returns unused capacity. */
    assert(rt_thread_mem_context_set(NULL, waiter) == RT_EOK);
    epd_memory_owner_release_trusted(waiter);
    events = capacity_events;
    assert(rt_thread_mem_context_set(NULL, 0) == RT_EOK);
    assert(capacity_events == events + 1 && !epd_memory_credit_get(waiter, &snap));
    epd_memory_owner_release_trusted(a); audit();
    epd_memory_set_capacity_notifier(NULL);
}

static void test_wait_limit_shrink(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("lowered-limit");
    epd_memory_credit_t held = request(0, 30000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t waiter = request(a, 4000, 1000, EPD_MEMORY_WAITING);
    epd_memory_credit_snapshot_t snap;
    for (unsigned i = 0; i < 3; ++i) {
        void *p = rt_memheap_alloc(heaps[0], 32); assert(p); rt_memheap_free(p);
    }
    assert(!rt_memheap_alloc(heaps[0], 32));
    assert(epd_memory_owner_set_limit(a, backends[0], 1000) == EPD_MEMORY_OK);
    epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(waiter, &snap) && snap.state == EPD_MEMORY_CREDIT_REJECTED &&
           snap.result == EPD_MEMORY_LIMIT);
    epd_memory_credit_t next = request(0, 100, 0, EPD_MEMORY_OK);
    epd_memory_credit_end(next); epd_memory_owner_release_trusted(next);
    epd_memory_owner_release_trusted(waiter);

    /* Replacement admission must check the full target, not just its delta. */
    epd_memory_credit_t old = request(a, 1000, 0, EPD_MEMORY_OK);
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, 1000};
    spec.bytes[backends[0]] = 5000;
    assert(epd_memory_owner_set_limit(a, backends[0], 6000) == EPD_MEMORY_OK);
    assert(epd_memory_credit_replace_prepare(old, a, &spec, &waiter) == EPD_MEMORY_WAITING);
    assert(epd_memory_owner_set_limit(a, backends[0], 4500) == EPD_MEMORY_OK);
    epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(waiter, &snap) && snap.state == EPD_MEMORY_CREDIT_REJECTED &&
           snap.result == EPD_MEMORY_LIMIT);
    epd_memory_owner_release_trusted(waiter); epd_memory_credit_end(old); epd_memory_owner_release_trusted(old);
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(a); audit();
}

static void test_wait_replace_closed(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("ended-source");
    epd_memory_credit_t old = request(a, 1000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t held = request(0, 30000, 0, EPD_MEMORY_OK), waiter;
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, 1000};
    epd_memory_credit_snapshot_t snap;
    epd_memory_metadata_snapshot_t metadata;
    spec.bytes[backends[0]] = 5000;
    assert(epd_memory_credit_replace_prepare(old, a, &spec, &waiter) == EPD_MEMORY_WAITING);
    epd_memory_credit_end(old);
    epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(waiter, &snap) && snap.state == EPD_MEMORY_CREDIT_REJECTED &&
           snap.result == EPD_MEMORY_CLOSED);
    epd_memory_metadata_snapshot(&metadata); assert(!metadata.transactions_used);
    epd_memory_owner_release_trusted(waiter); epd_memory_owner_release_trusted(old);
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(a); audit();
}

static void test_released_waiter(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("abandon");
    epd_memory_credit_t held = request(a, 30000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t waiter = request(a, 4000, 100, EPD_MEMORY_WAITING);
    epd_memory_owner_release_trusted(waiter);
    uint32_t delay;
    assert(!epd_memory_next_deadline(epd_memory_now_ms(), &delay));
    epd_memory_poll(epd_memory_now_ms());
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(a); audit();
}

static void test_limit_shrink(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("limit");
    epd_memory_credit_t c = request(a, 1000, 0, EPD_MEMORY_OK);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(c);
    assert(epd_memory_owner_set_limit(a, backends[0], 1) == EPD_MEMORY_OK);
    void *p = rt_memheap_alloc(heaps[0], 100); assert(p);
    epd_memory_owner_leave_trusted(prev);
    request(a, 2, 0, EPD_MEMORY_LIMIT);
    assert(epd_memory_owner_set_class(c, EPD_MEMORY_INFLIGHT) == EPD_MEMORY_OK);
    epd_memory_account_snapshot_t snap;
    assert(epd_memory_account_snapshot(a, &snap));
    assert(snap.backend[backends[0]].physical[EPD_MEMORY_INFLIGHT] == physical(p));
    assert(epd_memory_owner_set_class(c, EPD_MEMORY_PENDING_FREE) == EPD_MEMORY_OK);
    epd_memory_credit_end(c);
    assert(epd_memory_account_snapshot(a, &snap));
    assert(snap.backend[backends[0]].physical[EPD_MEMORY_PENDING_FREE] == physical(p));
    rt_memheap_free(p); epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a); audit();
}

static void test_generation_retire(void)
{
#ifdef EPD_MEMORY_TEST_GENERATIONS
    epd_memory_metadata_snapshot_t meta;
    unsigned allocations = 0;
    epd_memory_owner_t old = INVALID;
    while (1) {
        epd_memory_owner_t owner = epd_memory_owner_create("generation");
        if (owner == INVALID) break;
        if (old != INVALID) assert(!epd_memory_owner_retain_trusted(old));
        old = owner;
        epd_memory_owner_release_trusted(owner);
        ++allocations;
    }
    epd_memory_metadata_snapshot(&meta);
    assert(allocations == (meta.descriptors_capacity - 1) * 3);
    assert(meta.retired_slots == meta.descriptors_capacity - 1 && meta.descriptors_used == 1);
    void *p = rt_memheap_alloc(heaps[0], 16); assert(p); rt_memheap_free(p); audit();
#endif
}

static void test_multibackend(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("multi");
    epd_memory_credit_t held = request(a, 30000, 0, EPD_MEMORY_OK), c;
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, 500};
    spec.bytes[backends[0]] = 4000; spec.bytes[backends[1]] = 1000;
    assert(epd_memory_credit_request(a, &spec, &c) == EPD_MEMORY_WAITING);
    epd_memory_backend_snapshot_t snapshot;
    assert(epd_memory_backend_get(backends[1], &snapshot) && !snapshot.usage.unused);
    epd_memory_credit_end(held); epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_backend_get(backends[1], &snapshot) && snapshot.usage.unused == 1000);
    epd_memory_credit_end(c); epd_memory_owner_release_trusted(c);
    epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(a); audit();
}

static void test_pending_close(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("pending");
    epd_memory_credit_t c = request(a, 1000, 0, EPD_MEMORY_OK);
    epd_memory_change_t e = { EPD_MEMORY_CHANGE_ALLOC, c, backends[0], 100, 200, 0, 0, 30000, 29800 };
    assert(epd_memory_try_change(&e) == EPD_MEMORY_OK);
    epd_memory_account_snapshot_t usage;
    assert(epd_memory_account_snapshot(0, &usage) && !usage.backend[backends[0]].allocating);
    assert(epd_memory_account_snapshot(a, &usage) && usage.backend[backends[0]].allocating == 200);
    audit_accounts();
    epd_memory_owner_close_trusted(a); epd_memory_owner_release_trusted(c);
    epd_memory_owner_release_trusted(a);
    audit_accounts();
    epd_memory_notify(&e);
    audit_accounts();
    epd_memory_owner_snapshot_t snap;
    assert(epd_memory_owner_get(c, &snap) && snap.allocations == 1 && !snap.references);
    e.kind = EPD_MEMORY_CHANGE_FREE; e.old_size = e.size; e.old_physical = e.physical;
    e.size = e.physical = 0; e.available_after = 30000;
    epd_memory_notify(&e);
    assert(!epd_memory_owner_get(c, &snap) && !epd_memory_owner_get(a, &snap)); audit();
}

static void test_exhaustion(void)
{
    epd_memory_owner_t owners[256];
    epd_memory_metadata_snapshot_t meta;
    unsigned count = 0;
    while (count < 256 && (owners[count] = epd_memory_owner_create("slot")) != INVALID) ++count;
    epd_memory_metadata_snapshot(&meta);
    assert(count + 1 == meta.descriptors_capacity && meta.descriptors_used == meta.descriptors_capacity);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(owners[0]);
    void *p = rt_memheap_alloc(heaps[0], 16); assert(p); /* No per-block metadata allocation. */
    epd_memory_owner_leave_trusted(prev);
    epd_memory_owner_close_trusted(owners[0]); epd_memory_owner_release_trusted(owners[0]);
    assert(epd_memory_owner_create("still-pinned") == INVALID);
    rt_memheap_free(p);
    epd_memory_owner_t newer = epd_memory_owner_create("new");
    assert(newer != INVALID && newer != owners[0] && !epd_memory_owner_retain_trusted(owners[0]));
    for (unsigned i = 1; i < count; ++i) epd_memory_owner_release_trusted(owners[i]);
    epd_memory_owner_release_trusted(newer); audit();
}

static void test_scope_refs(void)
{
    epd_memory_owner_t root = epd_memory_owner_create("root");
    epd_memory_owner_t page = epd_memory_scope_create_trusted(root, EPD_MEMORY_SCOPE_PAGE);
    epd_memory_owner_t task = epd_memory_scope_create_trusted(page, EPD_MEMORY_SCOPE_TASK);
    epd_memory_owner_snapshot_t snap;
    epd_memory_owner_release_trusted(page);
    assert(epd_memory_owner_get(root, &snap) && snap.references == 2);
    epd_memory_owner_close_trusted(root); epd_memory_owner_release_trusted(task);
    assert(epd_memory_owner_get(root, &snap) && snap.references == 1);
    epd_memory_owner_release_trusted(root); audit();
}

static void noop(void *arg) { (void)arg; }
static void cleanup(rt_thread_t thread)
{
    assert(thread->mem_context != 0);
    assert(epd_memory_owner_current() == thread->mem_context);
    /* The SDK invokes cleanup with its outer IRQ guard held. Check admission
     * without taking a heap lock from this callback. */
    assert(request(thread->mem_context, 16, 0, EPD_MEMORY_CLOSED) == INVALID);
    ++cleanup_calls;
}
static void test_thread_lifecycle(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("thread"), prev = epd_memory_owner_enter_trusted(a);
    assert(rt_thread_init(&actors[2], "child", noop, NULL, thread_stack, sizeof(thread_stack), 10, 10) == RT_EOK);
    actors[2].cleanup = cleanup;
    epd_memory_owner_leave_trusted(prev);
    epd_memory_owner_close_trusted(a);
    epd_memory_owner_snapshot_t snap;
    assert(epd_memory_owner_get(a, &snap) && snap.references == 2);
    assert(rt_thread_detach(&actors[2]) == RT_EOK);
    rt_thread_idle_excute();
    assert(epd_memory_owner_current() == EPD_MEMORY_OWNER_SYSTEM);
    rt_thread_mem_context_cleanup(&actors[2]);
    assert(cleanup_calls == 1 && epd_memory_owner_get(a, &snap) && snap.references == 1);
    epd_memory_owner_release_trusted(a); audit();
}

static void reclaim(void *arg)
{
    assert(!irq_depth && !heap_locks);
    assert(epd_memory_reclaim_run(noop, NULL) == EPD_MEMORY_BUSY);
    ++*(unsigned *)arg;
    void *p = rt_memheap_alloc(heaps[0], 10); assert(p); rt_memheap_free(p);
}
static void test_reclaim(void)
{
    unsigned calls = 0;
    assert(epd_memory_reclaim_run(reclaim, &calls) == EPD_MEMORY_OK && calls == 1);
    assert(epd_memory_reclaim_run(reclaim, &calls) == EPD_MEMORY_OK && calls == 2); audit();
    rt_base_t level = rt_hw_interrupt_disable();
    assert(epd_memory_reclaim_run(reclaim, &calls) == EPD_MEMORY_BUSY);
    rt_hw_interrupt_enable(level);
}

static void test_random(void)
{
    epd_memory_owner_t a[4];
    void *blocks[64] = {0};
    uint32_t seed = 1234567;
    for (unsigned i = 0; i < 4; ++i) a[i] = epd_memory_owner_create(i & 1 ? "books" : "custom");
    for (unsigned i = 0; i < 4000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        unsigned slot = (seed >> 16) & 63;
        epd_memory_owner_t prev = epd_memory_owner_enter_trusted(a[(seed >> 8) & 3]);
        if (!blocks[slot]) blocks[slot] = rt_memheap_alloc(heaps[seed & 1], 1 + ((seed >> 20) & 1023));
        else if (seed & 4) {
            void *p = rt_memheap_realloc(header(blocks[slot])->pool_ptr, blocks[slot], 1 + ((seed >> 21) & 511));
            if (p) blocks[slot] = p;
        } else { rt_memheap_free(blocks[slot]); blocks[slot] = NULL; }
        epd_memory_owner_leave_trusted(prev); audit();
    }
    for (unsigned i = 0; i < 64; ++i) if (blocks[i]) rt_memheap_free(blocks[i]);
    for (unsigned i = 0; i < 4; ++i) epd_memory_owner_release_trusted(a[i]); audit();
}

struct testcase { const char *name; void (*run)(void); };
static epd_memory_credit_t suspend_request(epd_memory_owner_t owner, size_t psram, size_t sram)
{
    epd_memory_credit_spec_t spec = {{0}, EPD_MEMORY_PRIORITY_NORMAL, 0};
    epd_memory_credit_t credit;
    spec.bytes[backends[0]] = psram; spec.bytes[backends[1]] = sram;
    assert(epd_memory_credit_request(owner, &spec, &credit) == EPD_MEMORY_OK);
    return credit;
}

static void test_suspend_buffers(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("suspend-buffers");
    epd_memory_credit_t c = suspend_request(a, 2048, 512);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(c);
    unsigned char *p = rt_memheap_alloc(heaps[0], 256);
    void *q = rt_memheap_alloc(heaps[1], 64);
    assert(p && q); memset(p, 0x5a, 256);
    epd_memory_owner_leave_trusted(prev);
    epd_memory_credit_snapshot_t s;
    epd_memory_account_snapshot_t account_before, account_after;
    epd_memory_metadata_snapshot_t metadata_before, metadata_after;
    epd_memory_backend_snapshot_t before[2], after;
    epd_memory_set_capacity_notifier(capacity_event);
    unsigned events = capacity_events;
    epd_memory_metadata_snapshot(&metadata_before);
    rt_base_t level = rt_hw_interrupt_disable();
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK);
    assert(capacity_events == events);
    rt_hw_interrupt_enable(level);
    epd_memory_dispatch_changes(); assert(capacity_events == events + 1);
    epd_memory_metadata_snapshot(&metadata_after);
    assert(metadata_before.descriptors_used == metadata_after.descriptors_used &&
           metadata_before.fixed_bytes == metadata_after.fixed_bytes);
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK);
    assert(epd_memory_credit_get(c, &s) && s.state == EPD_MEMORY_CREDIT_SUSPENDED);
    assert(s.physical[backends[0]] == physical(p) && s.physical[backends[1]] == physical(q));
    for (unsigned b = 0; b < EPD_MEMORY_BACKEND_COUNT; ++b) assert(!s.unused[b]);
    rt_current_thread = &actors[1]; rt_memheap_free(q);
    assert(epd_memory_credit_get(c, &s) && !s.unused[backends[1]] && !s.physical[backends[1]]);
    assert(rt_memheap_realloc(heaps[0], p, 128) == p);
    assert(epd_memory_credit_get(c, &s) && !s.unused[backends[0]] && s.physical[backends[0]] == physical(p));
    assert(!rt_memheap_realloc(heaps[0], p, 512));
    rt_current_thread = &actors[0];
    prev = epd_memory_owner_enter_trusted(c);
    assert(!rt_memheap_alloc(heaps[0], 16));
    epd_memory_owner_leave_trusted(prev);
    epd_memory_credit_t g = suspend_request(a, 1024, 256);
    for (unsigned i = 0; i < 2; ++i) assert(epd_memory_backend_get(backends[i], &before[i]));
    assert(epd_memory_account_snapshot(a, &account_before));
    epd_memory_metadata_snapshot(&metadata_before);
    events = capacity_events;
    level = rt_hw_interrupt_disable();
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_OK);
    assert(capacity_events == events);
    rt_hw_interrupt_enable(level);
    epd_memory_dispatch_changes(); assert(capacity_events == events + 1);
    epd_memory_metadata_snapshot(&metadata_after);
    assert(metadata_before.descriptors_used == metadata_after.descriptors_used &&
           metadata_before.fixed_bytes == metadata_after.fixed_bytes);
    assert(epd_memory_account_snapshot(a, &account_after) &&
           !memcmp(&account_before, &account_after, sizeof(account_after)));
    for (unsigned i = 0; i < 2; ++i) {
        assert(epd_memory_backend_get(backends[i], &after));
        assert(!memcmp(&before[i], &after, sizeof(after)));
    }
    assert(epd_memory_credit_get(g, &s) && s.state == EPD_MEMORY_CREDIT_ENDED);
    epd_memory_owner_release_trusted(g);
    assert(epd_memory_credit_get(c, &s) && s.state == EPD_MEMORY_CREDIT_LIVE);
    assert(s.unused[backends[0]] == 1024 && s.unused[backends[1]] == 256);
    p = rt_memheap_realloc(heaps[0], p, 512); assert(p && header(p)->owner == c);
    for (unsigned i = 0; i < 128; ++i) assert(p[i] == 0x5a);
    rt_memheap_free(p);
    epd_memory_credit_end(c); epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a); audit();
}

static void test_suspend_atomic(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("suspend-atomic");
    epd_memory_owner_t page = epd_memory_scope_create_trusted(a, EPD_MEMORY_SCOPE_PAGE);
    epd_memory_credit_t c = suspend_request(a, 1024, 256);
    epd_memory_credit_snapshot_t before, source, after;
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK);
    epd_memory_credit_t wrong_parent = suspend_request(page, 128, 0);
    assert(epd_memory_credit_resume_commit(c, wrong_parent) == EPD_MEMORY_INVALID);
    epd_memory_credit_end(wrong_parent); epd_memory_owner_release_trusted(wrong_parent);
    epd_memory_credit_t g = suspend_request(a, 512, 128);
    assert(epd_memory_credit_get(c, &before) && epd_memory_credit_get(g, &source));
    assert(epd_memory_owner_set_backends(c, UINT32_C(1) << backends[0]) == EPD_MEMORY_OK);
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_REGION);
    assert(epd_memory_credit_get(c, &after) && !memcmp(&before, &after, sizeof(after)));
    assert(epd_memory_credit_get(g, &after) && !memcmp(&source, &after, sizeof(after)));
    assert(epd_memory_owner_set_backends(c, EPD_MEMORY_ALL_BACKENDS) == EPD_MEMORY_OK);
    assert(epd_memory_owner_set_limit(c, backends[1], 64) == EPD_MEMORY_OK);
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_LIMIT);
    assert(epd_memory_credit_get(c, &after) && !memcmp(&before, &after, sizeof(after)));
    assert(epd_memory_credit_get(g, &after) && !memcmp(&source, &after, sizeof(after)));
    assert(epd_memory_owner_set_limit(c, backends[1], SIZE_MAX) == EPD_MEMORY_OK);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(g);
    void *p = rt_memheap_alloc(heaps[0], 32); assert(p);
    epd_memory_owner_leave_trusted(prev);
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_BUSY);
    rt_memheap_free(p);
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(g);
    epd_memory_credit_end(c);
    assert(epd_memory_credit_resume_commit(c, EPD_MEMORY_CREDIT_NO_GRANT) == EPD_MEMORY_CLOSED);
    epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(page); epd_memory_owner_release_trusted(a); audit();
}

static void test_suspend_pending_transaction(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("suspend-busy");
    epd_memory_credit_t c = suspend_request(a, 2048, 0);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(c);
    void *p = rt_memheap_alloc(heaps[0], 128); assert(p);
    epd_memory_owner_leave_trusted(prev);
    /* Same-size in-place event has no heap mutation but pins a real block. */
    epd_memory_change_t change = {EPD_MEMORY_CHANGE_REALLOC_INPLACE, c, backends[0],
        header(p)->size, physical(p), header(p)->size, physical(p),
        heaps[0]->available_size, heaps[0]->available_size};
    assert(epd_memory_try_change(&change) == EPD_MEMORY_OK);
    epd_memory_credit_snapshot_t before, after, source_before, source_after;
    assert(epd_memory_credit_get(c, &before));
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_BUSY);
    assert(epd_memory_credit_get(c, &after) && !memcmp(&before, &after, sizeof(after)));
    epd_memory_notify(&change);
    epd_memory_credit_t prepared;
    assert(epd_memory_credit_share_prepare(&c, 1, 0, backends[0], 64, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_BUSY);
    assert(epd_memory_credit_suspend(prepared) == EPD_MEMORY_BUSY);
    assert(epd_memory_credit_abort(prepared)); epd_memory_owner_release_trusted(prepared);
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK);
    epd_memory_credit_t g = suspend_request(a, 512, 0);
    assert(epd_memory_credit_share_prepare(&g, 1, 0, backends[0], 64, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_get(c, &before) && epd_memory_credit_get(g, &source_before));
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_BUSY);
    assert(epd_memory_credit_get(c, &after) && !memcmp(&before, &after, sizeof(after)));
    assert(epd_memory_credit_get(g, &source_after) && !memcmp(&source_before, &source_after, sizeof(source_after)));
    assert(epd_memory_credit_abort(prepared)); epd_memory_owner_release_trusted(prepared);
    assert(epd_memory_try_change(&change) == EPD_MEMORY_OK);
    assert(epd_memory_credit_get(c, &before) && epd_memory_credit_get(g, &source_before));
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_BUSY);
    assert(epd_memory_credit_get(c, &after) && !memcmp(&before, &after, sizeof(after)));
    assert(epd_memory_credit_get(g, &source_after) && !memcmp(&source_before, &source_after, sizeof(source_after)));
    epd_memory_notify(&change);
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(g); rt_memheap_free(p);
    epd_memory_credit_end(c); epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a); audit();
}

static void test_suspend_fair_resume(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("resume-fair");
    epd_memory_credit_t c = suspend_request(a, 128, 0);
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK);
    epd_memory_credit_t held = request(0, 30000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t g = request(a, 4000, 1000, EPD_MEMORY_WAITING);
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_BUSY);
    for (unsigned i = 0; i < 3; ++i) {
        void *p = rt_memheap_alloc(heaps[0], 32); assert(p); rt_memheap_free(p);
    }
    assert(!rt_memheap_alloc(heaps[0], 32));
    epd_memory_credit_snapshot_t s;
    assert(epd_memory_credit_get(g, &s) && s.bypasses == 3 && s.state == EPD_MEMORY_CREDIT_WAITING);
    assert(epd_memory_credit_get(c, &s) && s.state == EPD_MEMORY_CREDIT_SUSPENDED);
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held);
    epd_memory_poll(epd_memory_now_ms());
    assert(epd_memory_credit_get(g, &s) && s.state == EPD_MEMORY_CREDIT_LIVE);
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(g);
    assert(epd_memory_credit_get(c, &s) && s.unused[backends[0]] == 4000);
    epd_memory_credit_end(c); epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a); audit();
}

static void test_suspend_cancel_timeout(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("resume-cancel");
    epd_memory_credit_t c = suspend_request(a, 1024, 0);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(c);
    void *p = rt_memheap_alloc(heaps[0], 64); assert(p);
    epd_memory_owner_leave_trusted(prev);
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK);
    epd_memory_credit_t held = request(0, 30000, 0, EPD_MEMORY_OK);
    epd_memory_credit_t g = request(a, 4000, 1000, EPD_MEMORY_WAITING);
    assert(epd_memory_credit_cancel(g));
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_CLOSED);
    epd_memory_owner_release_trusted(g);
    g = request(a, 4000, 100, EPD_MEMORY_WAITING);
    ticks += RT_TICK_PER_SECOND / 10;
    epd_memory_poll(epd_memory_now_ms());
    epd_memory_credit_snapshot_t s;
    assert(epd_memory_credit_get(g, &s) && s.state == EPD_MEMORY_CREDIT_TIMED_OUT);
    assert(epd_memory_credit_resume_commit(c, g) == EPD_MEMORY_CLOSED);
    epd_memory_owner_release_trusted(g);
    assert(epd_memory_credit_get(c, &s) && s.state == EPD_MEMORY_CREDIT_SUSPENDED);
    assert(header(p)->owner == c);
    epd_memory_owner_close_trusted(a);
    assert(epd_memory_credit_get(c, &s) && s.state == EPD_MEMORY_CREDIT_CANCELLED);
    assert(epd_memory_credit_resume_commit(c, EPD_MEMORY_CREDIT_NO_GRANT) == EPD_MEMORY_CLOSED);
    rt_memheap_free(p);
    epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a);
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held); audit();
}

static void test_suspend_zero_lifetime(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("resume-zero");
    epd_memory_credit_t c = suspend_request(a, 1024, 0);
    epd_memory_owner_t prev = epd_memory_owner_enter_trusted(c);
    void *p = rt_memheap_alloc(heaps[0], 64); assert(p);
    epd_memory_owner_leave_trusted(prev);
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK);
    assert(epd_memory_credit_resume_commit(c, INVALID) == EPD_MEMORY_INVALID);
    epd_memory_credit_snapshot_t s;
    epd_memory_metadata_snapshot_t before, after;
    epd_memory_metadata_snapshot(&before);
    assert(epd_memory_credit_resume_commit(c, EPD_MEMORY_CREDIT_NO_GRANT) == EPD_MEMORY_OK);
    epd_memory_metadata_snapshot(&after);
    assert(before.descriptors_used == after.descriptors_used && before.fixed_bytes == after.fixed_bytes);
    assert(epd_memory_credit_get(c, &s) && s.state == EPD_MEMORY_CREDIT_LIVE && !s.unused[backends[0]]);
    assert(header(p)->owner == c && !rt_memheap_realloc(heaps[0], p, 128));
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(c);
    assert(epd_memory_credit_get(c, &s) && s.state == EPD_MEMORY_CREDIT_CANCELLED);
    assert(epd_memory_credit_resume_commit(c, EPD_MEMORY_CREDIT_NO_GRANT) == EPD_MEMORY_CLOSED);
    rt_memheap_free(p);
    assert(!epd_memory_credit_get(c, &s));
    c = suspend_request(a, 128, 0);
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK && epd_memory_credit_end(c));
    assert(epd_memory_credit_resume_commit(c, EPD_MEMORY_CREDIT_NO_GRANT) == EPD_MEMORY_CLOSED);
    epd_memory_owner_release_trusted(c);
    c = suspend_request(a, 128, 0);
    assert(epd_memory_credit_suspend(c) == EPD_MEMORY_OK && epd_memory_credit_cancel(c));
    assert(epd_memory_credit_get(c, &s) && s.state == EPD_MEMORY_CREDIT_CANCELLED);
    assert(epd_memory_credit_resume_commit(c, EPD_MEMORY_CREDIT_NO_GRANT) == EPD_MEMORY_CLOSED);
    epd_memory_owner_release_trusted(c);
    epd_memory_owner_release_trusted(a); audit();
}

static void test_system_account(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("books"), b = epd_memory_owner_create("peer");
    epd_memory_owner_t shared = epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED);
    epd_memory_owner_t task = epd_memory_scope_create_trusted(shared, EPD_MEMORY_SCOPE_TASK);
    epd_memory_owner_t page = epd_memory_scope_create_trusted(a, EPD_MEMORY_SCOPE_PAGE);
    epd_memory_owner_t owners[] = {0, shared, task, page, b};
    void *blocks[5][2];
    epd_memory_account_snapshot_t system, before, after;
    for (unsigned i = 0; i < 5; ++i) {
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owners[i]);
        for (unsigned n = 0; n < 2; ++n) {
            blocks[i][n] = rt_memheap_alloc(heaps[n], 32 + i * 64 + n * 4);
            assert(blocks[i][n] && header(blocks[i][n])->owner == owners[i]);
        }
        epd_memory_owner_leave_trusted(previous);
    }
    assert(epd_memory_account_snapshot(0, &system));
    assert(epd_memory_account_snapshot(task, &after) && !memcmp(&system, &after, sizeof(system)));
    for (unsigned n = 0; n < 2; ++n) {
        size_t bytes = physical(blocks[0][n]) + physical(blocks[1][n]) + physical(blocks[2][n]);
        assert(system.backend[backends[n]].allocations == 3);
        assert(account_bytes(0, backends[n]) == bytes);
        assert(account_bytes(a, backends[n]) == physical(blocks[3][n]));
        assert(account_bytes(b, backends[n]) == physical(blocks[4][n]));
        epd_memory_budget_snapshot_t budget;
        assert(epd_memory_owner_budget(0, backends[n], &budget) && budget.private_committed == bytes);
    }
    audit();
    /* Reclassifying APP blocks must not underflow or alter SYSTEM classes. */
    assert(epd_memory_owner_set_class(page, EPD_MEMORY_RECLAIMABLE) == EPD_MEMORY_OK);
    assert(epd_memory_account_snapshot(0, &after) && !memcmp(&system, &after, sizeof(system)));
    assert(epd_memory_owner_set_class(task, EPD_MEMORY_PENDING_FREE) == EPD_MEMORY_OK);
    assert(epd_memory_account_snapshot(0, &before));
    for (unsigned n = 0; n < 2; ++n)
        assert(before.backend[backends[n]].physical[EPD_MEMORY_PENDING_FREE] == physical(blocks[2][n]));
    /* An application's transient peak does not raise account 0's own peak. */
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(page);
    void *large = rt_memheap_alloc(heaps[0], 8192); assert(large);
    epd_memory_owner_leave_trusted(previous); rt_memheap_free(large);
    assert(epd_memory_account_snapshot(0, &after) && !memcmp(&before, &after, sizeof(before)));
    epd_memory_credit_t system_credit = request(task, 512, 0, EPD_MEMORY_OK);
    epd_memory_credit_t app_credit = request(page, 1024, 0, EPD_MEMORY_OK);
    assert(epd_memory_account_snapshot(0, &after) && after.backend[backends[0]].unused == 512);
    assert(epd_memory_account_snapshot(page, &after) && after.backend[backends[0]].unused == 1024);
    audit();
    /* Live buffers cross the SYSTEM/account boundary in either direction. */
    assert(host_transfer(blocks[3][0], shared) == EPD_MEMORY_OK);
    assert(!account_bytes(a, backends[0])); audit();
    assert(host_transfer(blocks[3][0], page) == EPD_MEMORY_OK);
    assert(account_bytes(a, backends[0]) == physical(blocks[3][0])); audit();
    epd_memory_owner_close_trusted(a); epd_memory_owner_close_trusted(b);
    epd_memory_owner_release_trusted(app_credit); epd_memory_credit_end(system_credit);
    epd_memory_owner_release_trusted(system_credit);
    audit();
    rt_thread_t saved = rt_current_thread;
    rt_current_thread = &actors[2];
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned n = 0; n < 2; ++n) rt_memheap_free(blocks[i][n]);
    rt_current_thread = saved;
    epd_memory_owner_release_trusted(page); epd_memory_owner_release_trusted(task); epd_memory_owner_release_trusted(shared);
    epd_memory_owner_release_trusted(a); epd_memory_owner_release_trusted(b); audit();
}

static void test_system_scope_policy(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("global-limit");
    epd_memory_owner_t shared = epd_memory_scope_create_trusted(0, EPD_MEMORY_SCOPE_SHARED);
    epd_memory_credit_t credit = request(a, 512, 0, EPD_MEMORY_OK), prepared;
    assert(epd_memory_owner_set_limit(0, backends[0], 512) == EPD_MEMORY_OK);
    assert(!rt_memheap_alloc(heaps[0], 16));
    assert(epd_memory_owner_set_backends(0, EPD_MEMORY_ALL_BACKENDS) == EPD_MEMORY_BUSY);
    /* Changing account attribution consumes no additional global commitment. */
    assert(epd_memory_credit_share_prepare(&credit, 1, shared, backends[0], 512, &prepared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_commit(prepared) == EPD_MEMORY_OK);
    epd_memory_account_snapshot_t s;
    assert(epd_memory_account_snapshot(0, &s) && s.backend[backends[0]].unused == 512);
    assert(epd_memory_account_snapshot(a, &s) && !s.backend[backends[0]].unused);
    audit();
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(prepared);
    void *p = rt_memheap_alloc(heaps[0], 128); assert(p);
    epd_memory_owner_leave_trusted(previous);
    epd_memory_credit_end(prepared); epd_memory_owner_release_trusted(prepared);
    size_t bytes = physical(p);
    assert(epd_memory_owner_set_limit(0, backends[0], bytes) == EPD_MEMORY_OK);
    assert(host_transfer(p, a) == EPD_MEMORY_OK);
    assert(!account_bytes(0, backends[0]) && account_bytes(a, backends[0]) == bytes);
    assert(!rt_memheap_alloc(heaps[0], 16)); audit();
    rt_memheap_free(p); epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit);
    assert(epd_memory_owner_set_limit(0, backends[0], SIZE_MAX) == EPD_MEMORY_OK);
    epd_memory_owner_release_trusted(shared); epd_memory_owner_release_trusted(a); audit();
}

static void test_allocation_peak(void)
{
    const size_t payloads[] = {1, 13, 127, 1024, 4096};
    assert(!epd_app_allocation_peak(0));
    assert(epd_app_allocation_peak(SIZE_MAX) == SIZE_MAX);
    assert(epd_app_allocation_peak(SIZE_MAX - H) == SIZE_MAX);
    for (unsigned i = 0; i < sizeof(payloads) / sizeof(payloads[0]); ++i) {
        size_t rounded = RT_ALIGN(payloads[i], RT_ALIGN_SIZE);
        if (rounded < 12) rounded = 12;
        size_t hole = rounded + H + 12 - RT_ALIGN_SIZE;
        void *p = rt_memheap_alloc(heaps[0], hole); assert(p);
        void *barrier = rt_memheap_alloc(heaps[0], 32); assert(barrier);
        rt_memheap_free(p);
        p = rt_memheap_alloc(heaps[0], payloads[i]); assert(p);
        assert(physical(p) == hole + H);
        assert(physical(p) == epd_app_allocation_peak(payloads[i]));
        rt_memheap_free(p); rt_memheap_free(barrier);
    }
    audit();
}

#include "recovery.inc"
#include "headroom.inc"
#include "ordinary_priority.inc"
static const struct testcase cases[] = {
    {"ordinary_nearby", test_ordinary_nearby}, {"ordinary_optional", test_ordinary_optional},
    {"ordinary_normal_fairness", test_ordinary_normal_fairness},
    {"ordinary_interactive_fairness", test_ordinary_interactive_fairness},
    {"ordinary_reservation", test_ordinary_reservation},
    {"headroom_priority", test_headroom_priority}, {"headroom_wait_reclaim", test_headroom_wait_reclaim},
    {"headroom_existing_grant", test_headroom_existing_grant}, {"headroom_recovery_growth", test_headroom_recovery_growth},
    {"headroom_replace", test_headroom_replace}, {"headroom_impossible", test_headroom_impossible},
    {"headroom_replace_cross_backend", test_headroom_replace_cross_backend},
    {"headroom_replace_cancelled_source", test_headroom_replace_cancelled_source},
    {"headroom_replace_used_source", test_headroom_replace_used_source},
    {"headroom_shared_growth", test_headroom_shared_growth},
    {"headroom_shared_contributors", test_headroom_shared_contributors},
    {"headroom_shared_fairness", test_headroom_shared_fairness},
    {"recovery_credit", test_recovery_credit}, {"recovery_closed_limit", test_recovery_closed_limit},
    {"recovery_reserved", test_recovery_reserved}, {"recovery_fairness", test_recovery_fairness},
    {"recovery_physical", test_recovery_physical}, {"recovery_fragmentation", test_recovery_fragmentation},
    {"recovery_fragmentation_many", test_recovery_fragmentation_many},
    {"recovery_capacity_then_fragmentation", test_recovery_capacity_then_fragmentation},
    {"recovery_capacity_fragmentation_exhausted", test_recovery_capacity_fragmentation_exhausted},
    {"recovery_capacity_fragmentation_deadline", test_recovery_capacity_fragmentation_deadline},
    {"recovery_credit_fragmentation", test_recovery_credit_fragmentation},
    {"recovery_fragmentation_many_exhausted", test_recovery_fragmentation_many_exhausted},
    {"recovery_fragmentation_many_timeout", test_recovery_fragmentation_many_timeout},
    {"recovery_fragmentation_many_released", test_recovery_fragmentation_many_released},
    {"recovery_cancel_timeout", test_recovery_cancel_timeout}, {"recovery_context_cleanup", test_recovery_context_cleanup},
    {"recovery_already_released", test_recovery_already_released},
    {"system_account", test_system_account}, {"system_scope_policy", test_system_scope_policy},
    {"allocation_peak", test_allocation_peak},
    {"suspend_buffers", test_suspend_buffers},
    {"suspend_atomic", test_suspend_atomic},
    {"suspend_pending_transaction", test_suspend_pending_transaction},
    {"suspend_fair_resume", test_suspend_fair_resume},
    {"suspend_cancel_timeout", test_suspend_cancel_timeout},
    {"suspend_zero_lifetime", test_suspend_zero_lifetime},

    {"early", test_early}, {"backend_init", test_backend_init}, {"region_policy", test_region_policy},
    {"guards", test_guards}, {"cross_thread", test_cross_thread},
    {"boot_metadata", test_boot_metadata},
    {"owner_at", test_owner_at},
    {"credit", test_credit}, {"reservations", test_reservations}, {"cleanup", test_cleanup},
    {"credit_expand", test_credit_expand},
    {"credit_replace", test_credit_replace},
    {"credit_replace_wait", test_credit_replace_wait},
    {"credit_share", test_credit_share},
    {"association", test_association},
    {"share_association", test_share_association},
    {"association_batch", test_association_batch},
    {"block_get", test_block_get},
    {"credit_associate", test_credit_associate},
    {"automatic_budget", test_automatic_budget},
    {"shared_duplicate_promises", test_shared_duplicate_promises},
    {"budget_lowered_promise", test_budget_lowered_promise},
    {"diagnostic", test_diagnostic},
    {"rejection", test_rejection}, {"rejection_realloc", test_rejection_realloc},
    {"credit_transactions_bounded", test_credit_transactions_bounded},
    {"close_credit", test_close_credit},
    {"realloc_peak", test_realloc_peak}, {"unsplit", test_unsplit}, {"transfer", test_transfer},
    {"waits", test_waits}, {"wait_independent_capacity", test_wait_independent_capacity},
    {"timeout", test_timeout}, {"multibackend", test_multibackend},
    {"closed_transfer", test_closed_transfer},
    {"transfer_try", test_transfer_try}, {"public_transfer_account", test_public_transfer_account},
    {"shared_font_settlement", test_shared_font_settlement},
    {"deadline_wrap", test_deadline_wrap}, {"released_waiter", test_released_waiter},
    {"stale_poll", test_stale_poll}, {"wait_limit_shrink", test_wait_limit_shrink},
    {"capacity_events", test_capacity_events},
    {"wait_replace_closed", test_wait_replace_closed},
    {"limit_shrink", test_limit_shrink},
    {"pending_close", test_pending_close}, {"exhaustion", test_exhaustion},
    {"scope_refs", test_scope_refs}, {"thread_lifecycle", test_thread_lifecycle},
    {"reclaim", test_reclaim}, {"random", test_random},
#ifdef EPD_MEMORY_TEST_GENERATIONS
    {"generation_retire", test_generation_retire}
#endif
};

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) puts(cases[i].name);
        return 0;
    }
    setup();
    if (strcmp(argv[1], "boot_metadata")) assert(epd_memory_init() == EPD_MEMORY_OK);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (!strcmp(argv[1], cases[i].name)) {
            cases[i].run();
            printf("PASS %s\n", argv[1]);
            return 0;
        }
    }
    return 2;
}
