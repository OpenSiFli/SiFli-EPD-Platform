/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
static epd_memory_credit_t floor_request(size_t bytes, epd_memory_priority_t priority,
                                        uint32_t timeout, epd_memory_result_t expected)
{
    epd_memory_credit_spec_t spec = {0};
    epd_memory_credit_t credit;
    spec.bytes[backends[0]] = bytes; spec.priority = priority; spec.timeout_ms = timeout;
    epd_memory_result_t actual = epd_memory_credit_request(0, &spec, &credit);
    if (actual != expected) fprintf(stderr, "floor request bytes=%zu priority=%d actual=%d expected=%d\n",
                                   bytes, priority, actual, expected);
    assert(actual == expected);
    return credit;
}
static void floor_drop(epd_memory_credit_t credit)
{ epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit); }

static void test_headroom_priority(void)
{
    void *observed = rt_memheap_alloc(heaps[0], 16); assert(observed); rt_memheap_free(observed);
    assert(epd_memory_backend_set_headroom(backends[0], 8192));
    size_t free = epd_app_psram_available();
    epd_memory_credit_t nearby = floor_request(free - 8192, EPD_MEMORY_PRIORITY_NEARBY, 0, EPD_MEMORY_OK);
    floor_request(1, EPD_MEMORY_PRIORITY_OPTIONAL, 0, EPD_MEMORY_NO_CAPACITY);
    floor_request(1, EPD_MEMORY_PRIORITY_NEARBY, 0, EPD_MEMORY_NO_CAPACITY);
    epd_memory_credit_t continuous = floor_request(4096, EPD_MEMORY_PRIORITY_NORMAL, 0, EPD_MEMORY_OK);
    epd_memory_credit_t foreground = floor_request(4096, EPD_MEMORY_PRIORITY_INTERACTIVE, 0, EPD_MEMORY_OK);
    floor_drop(nearby); floor_drop(continuous); floor_drop(foreground); audit();
}
static void test_headroom_wait_reclaim(void)
{
    assert(epd_memory_backend_set_headroom(backends[0], 16384));
    void *occupied = rt_memheap_alloc(heaps[0], 12000); assert(occupied);
    epd_memory_credit_t waiting = floor_request(6000, EPD_MEMORY_PRIORITY_NEARBY, 1000, EPD_MEMORY_WAITING);
    size_t need[EPD_MEMORY_BACKEND_COUNT];
    assert(epd_memory_credit_reclaim_need(waiting, need));
    assert(need[backends[0]] == 16384 + 6000 - epd_app_psram_available());
    epd_memory_poll(0);
    epd_memory_credit_snapshot_t snapshot;
    assert(epd_memory_credit_get(waiting, &snapshot) && snapshot.state == EPD_MEMORY_CREDIT_WAITING);
    rt_memheap_free(occupied); epd_memory_poll(0);
    assert(epd_memory_credit_get(waiting, &snapshot) && snapshot.state == EPD_MEMORY_CREDIT_LIVE);
    floor_drop(waiting); audit();
}
static void test_headroom_existing_grant(void)
{
    void *observed = rt_memheap_alloc(heaps[0], 16); assert(observed); rt_memheap_free(observed);
    assert(epd_memory_backend_set_headroom(backends[0], 8192));
    epd_memory_credit_t nearby = floor_request(4096, EPD_MEMORY_PRIORITY_NEARBY, 0, EPD_MEMORY_OK);
    epd_memory_credit_t foreground = floor_request(epd_app_psram_available() - 4096,
                                                   EPD_MEMORY_PRIORITY_INTERACTIVE, 0, EPD_MEMORY_OK);
    size_t addition[EPD_MEMORY_BACKEND_COUNT] = {0}; addition[backends[0]] = 64;
    assert(epd_memory_credit_expand(nearby, addition) == EPD_MEMORY_NO_CAPACITY);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(nearby);
    epd_memory_recovery_t recovery = {0};
    assert(epd_memory_recovery_begin(&recovery, backends[0], 1024, false) == EPD_MEMORY_RECOVERY_READY);
    void *p = rt_memheap_alloc(heaps[0], 512); assert(p);
    rt_memheap_free(p); epd_memory_owner_leave_trusted(previous);
    floor_drop(foreground); floor_drop(nearby); audit();
}
static void test_headroom_recovery_growth(void)
{
    assert(epd_memory_backend_set_headroom(backends[0], 16384));
    epd_memory_credit_t nearby = floor_request(4096, EPD_MEMORY_PRIORITY_NEARBY, 0, EPD_MEMORY_OK);
    void *occupied = rt_memheap_alloc(heaps[0], 12000); assert(occupied);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(nearby);
    epd_memory_recovery_t recovery = {0};
    assert(epd_memory_recovery_begin(&recovery, backends[0], 6000, false) == EPD_MEMORY_RECOVERY_PENDING);
    assert(pressure_calls == 1 && pressure_status.min_available == 16384 + 6000);
    rt_memheap_free(occupied);
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_READY);
    epd_memory_credit_snapshot_t snapshot;
    assert(epd_memory_credit_get(nearby, &snapshot) && snapshot.unused[backends[0]] == 6000);
    epd_memory_owner_leave_trusted(previous); floor_drop(nearby); audit();
}
static void test_headroom_replace(void)
{
    void *observed = rt_memheap_alloc(heaps[0], 16); assert(observed); rt_memheap_free(observed);
    assert(epd_memory_backend_set_headroom(backends[0], 8192));
    size_t free = epd_app_psram_available();
    epd_memory_credit_t old = floor_request(free - 8192, EPD_MEMORY_PRIORITY_NEARBY, 0, EPD_MEMORY_OK), next;
    epd_memory_credit_spec_t spec = {0};
    spec.bytes[backends[0]] = free - 8192 + 1; spec.priority = EPD_MEMORY_PRIORITY_NEARBY;
    assert(epd_memory_credit_replace_prepare(old, 0, &spec, &next) == EPD_MEMORY_NO_CAPACITY);
    spec.priority = EPD_MEMORY_PRIORITY_INTERACTIVE;
    assert(epd_memory_credit_replace_prepare(old, 0, &spec, &next) == EPD_MEMORY_OK);
    assert(epd_memory_credit_commit(next) == EPD_MEMORY_OK);
    floor_drop(old); floor_drop(next); audit();
}
static void test_headroom_impossible(void)
{
    assert(epd_memory_backend_set_headroom(backends[0], SIZE_MAX));
    assert(floor_request(1, EPD_MEMORY_PRIORITY_OPTIONAL, 1000, EPD_MEMORY_NO_CAPACITY) == INVALID);
    epd_memory_credit_t normal = floor_request(128, EPD_MEMORY_PRIORITY_NORMAL, 0, EPD_MEMORY_OK);
    floor_drop(normal);
    assert(epd_memory_backend_set_headroom(backends[0], 0));
    epd_memory_credit_t optional = floor_request(128, EPD_MEMORY_PRIORITY_OPTIONAL, 0, EPD_MEMORY_OK);
    floor_drop(optional); audit();
}

static void headroom_replace_cross_backend(unsigned mode)
{
    epd_memory_credit_t old = floor_request(24576, EPD_MEMORY_PRIORITY_INTERACTIVE, 0, EPD_MEMORY_OK), next;
    assert(epd_memory_backend_set_headroom(backends[0], 16384));
    assert(epd_memory_backend_set_headroom(backends[1], 4096));
    void *occupied = rt_memheap_alloc(heaps[1], 30000); assert(occupied);
    epd_memory_credit_spec_t spec = {0};
    spec.bytes[backends[0]] = 24576; spec.bytes[backends[1]] = 1024;
    spec.priority = EPD_MEMORY_PRIORITY_NEARBY; spec.timeout_ms = 1000;
    assert(epd_memory_credit_replace_prepare(old, 0, &spec, &next) == EPD_MEMORY_WAITING);
    epd_memory_credit_snapshot_t snapshot;
    assert(epd_memory_credit_get(next, &snapshot));
    uint32_t deadline = snapshot.deadline_ms;
    size_t need[EPD_MEMORY_BACKEND_COUNT];
    assert(epd_memory_credit_reclaim_need(next, need));
    assert(!need[backends[0]] && need[backends[1]] == 4096 + 1024 - heaps[1]->available_size);
    for (unsigned i = 0; i < 64; ++i) {
        epd_memory_poll(epd_memory_now_ms());
        assert(epd_memory_credit_get(next, &snapshot) && snapshot.state == EPD_MEMORY_CREDIT_WAITING);
        assert(snapshot.deadline_ms == deadline && !snapshot.unused[backends[0]] && !snapshot.unused[backends[1]]);
    }
    if (mode == 1) {
        assert(epd_memory_credit_cancel(old));
        epd_memory_poll(epd_memory_now_ms());
        assert(epd_memory_credit_get(next, &snapshot) && snapshot.state == EPD_MEMORY_CREDIT_REJECTED);
        assert(snapshot.result == EPD_MEMORY_CLOSED);
        assert(epd_memory_credit_commit(next) != EPD_MEMORY_OK);
    } else {
        void *borrowed = NULL;
        if (mode == 2) {
            epd_memory_owner_t previous = epd_memory_owner_enter_trusted(old);
            borrowed = rt_memheap_alloc(heaps[0], 512); assert(borrowed);
            epd_memory_owner_leave_trusted(previous);
        }
        rt_memheap_free(occupied); occupied = NULL;
        epd_memory_poll(epd_memory_now_ms());
        assert(epd_memory_credit_get(next, &snapshot) && snapshot.state == EPD_MEMORY_CREDIT_PREPARED);
        assert(!snapshot.unused[backends[0]] && snapshot.unused[backends[1]] == 1024);
        if (borrowed) {
            assert(epd_memory_credit_commit(next) == EPD_MEMORY_BUSY);
            assert(epd_memory_credit_get(old, &snapshot) && snapshot.state == EPD_MEMORY_CREDIT_LIVE);
            rt_memheap_free(borrowed);
        }
        assert(epd_memory_credit_commit(next) == EPD_MEMORY_OK);
        assert(epd_memory_credit_get(old, &snapshot) && snapshot.state == EPD_MEMORY_CREDIT_ENDED);
        epd_memory_backend_snapshot_t heap;
        assert(epd_memory_backend_get(backends[0], &heap) && heap.usage.unused == 24576);
        assert(epd_memory_backend_get(backends[1], &heap) && heap.usage.unused == 1024);
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(next);
        void *a = rt_memheap_alloc(heaps[0], 512), *b = rt_memheap_alloc(heaps[1], 512);
        assert(a && b); audit();
        rt_memheap_free(a); rt_memheap_free(b); epd_memory_owner_leave_trusted(previous);
    }
    if (occupied) rt_memheap_free(occupied);
    floor_drop(next); floor_drop(old); audit();
}
static void test_headroom_replace_cross_backend(void) { headroom_replace_cross_backend(0); }
static void test_headroom_replace_cancelled_source(void) { headroom_replace_cross_backend(1); }
static void test_headroom_replace_used_source(void) { headroom_replace_cross_backend(2); }

static void test_headroom_shared_growth(void)
{
    void *observed = rt_memheap_alloc(heaps[0], 16); assert(observed); rt_memheap_free(observed);
    assert(epd_memory_backend_set_headroom(backends[0], 8192));
    for (unsigned kind = EPD_MEMORY_PRIORITY_NEARBY; kind <= EPD_MEMORY_PRIORITY_OPTIONAL; ++kind) {
        epd_memory_credit_t source = floor_request(4096, (epd_memory_priority_t)kind, 0, EPD_MEMORY_OK), shared;
        assert(epd_memory_credit_share_prepare(&source, 1, 0, backends[0], 4096, &shared) == EPD_MEMORY_OK);
        assert(epd_memory_credit_share_commit(shared, NULL, 0) == EPD_MEMORY_OK);
        epd_memory_credit_snapshot_t snapshot;
        assert(epd_memory_credit_get(shared, &snapshot) && snapshot.priority == (epd_memory_priority_t)kind);
        epd_memory_credit_t filler = floor_request(epd_app_psram_available() - 8192,
                                                   EPD_MEMORY_PRIORITY_INTERACTIVE, 0, EPD_MEMORY_OK);
        size_t addition[EPD_MEMORY_BACKEND_COUNT] = {0}; addition[backends[0]] = 1024;
        assert(epd_memory_credit_expand(shared, addition) == EPD_MEMORY_NO_CAPACITY);
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(shared);
        epd_memory_recovery_t existing = {0}, growth = {0};
        assert(epd_memory_recovery_begin(&existing, backends[0], 1024, false) == EPD_MEMORY_RECOVERY_READY);
        void *p = rt_memheap_alloc(heaps[0], 512); assert(p); rt_memheap_free(p);
        assert(epd_memory_recovery_begin(&growth, backends[0], 5120, false) == EPD_MEMORY_RECOVERY_PENDING);
        assert(growth.reason == EPD_MEMORY_REJECT_RESERVED && !pressure_calls);
        assert(epd_memory_credit_get(shared, &snapshot) && snapshot.unused[backends[0]] == 4096);
        floor_drop(filler);
        assert(epd_memory_recovery_poll(&growth, false) == EPD_MEMORY_RECOVERY_READY);
        assert(epd_memory_credit_get(shared, &snapshot) && snapshot.unused[backends[0]] == 5120);
        p = rt_memheap_alloc(heaps[0], 4800); assert(p); audit(); rt_memheap_free(p);
        epd_memory_owner_leave_trusted(previous);
        floor_drop(shared); floor_drop(source); audit();
    }
}

static void test_headroom_shared_contributors(void)
{
    const size_t amounts[] = {2048, 2304, 3328};
    const epd_memory_priority_t expected[] = {EPD_MEMORY_PRIORITY_OPTIONAL,
        EPD_MEMORY_PRIORITY_NEARBY, EPD_MEMORY_PRIORITY_INTERACTIVE};
    assert(epd_memory_backend_set_headroom(backends[0], 8192));
    for (unsigned i = 0; i < 3; ++i) {
        epd_memory_credit_t sources[] = {
            floor_request(128, EPD_MEMORY_PRIORITY_INTERACTIVE, 0, EPD_MEMORY_OK),
            floor_request(2048, EPD_MEMORY_PRIORITY_OPTIONAL, 0, EPD_MEMORY_OK),
            floor_request(1024, EPD_MEMORY_PRIORITY_NEARBY, 0, EPD_MEMORY_OK),
            floor_request(512, EPD_MEMORY_PRIORITY_INTERACTIVE, 0, EPD_MEMORY_OK)
        }, shared;
        /* Neither an empty leading source nor an unused tail may promote. */
        assert(epd_memory_credit_trim(sources[0], backends[0], 128) == EPD_MEMORY_OK);
        assert(epd_memory_credit_share_prepare(sources, 4, 0, backends[0], amounts[i], &shared) == EPD_MEMORY_OK);
        epd_memory_credit_snapshot_t snapshot;
        assert(epd_memory_credit_get(shared, &snapshot) && snapshot.priority == expected[i]);
        epd_memory_backend_snapshot_t heap;
        assert(epd_memory_backend_get(backends[0], &heap) && heap.usage.unused == 3584);
        assert(epd_memory_credit_share_commit(shared, NULL, 0) == EPD_MEMORY_OK);
        assert(epd_memory_credit_get(shared, &snapshot) && snapshot.priority == expected[i]);
        assert(snapshot.unused[backends[0]] == amounts[i]);
        assert(epd_memory_backend_get(backends[0], &heap) && heap.usage.unused == 3584);
        floor_drop(shared);
        for (unsigned n = 0; n < 4; ++n) floor_drop(sources[n]);
        audit();
    }
}

static void test_headroom_shared_fairness(void)
{
    assert(epd_memory_backend_set_headroom(backends[0], 512));
    epd_memory_credit_t source = floor_request(1024, EPD_MEMORY_PRIORITY_NEARBY, 0, EPD_MEMORY_OK), shared;
    assert(epd_memory_credit_share_prepare(&source, 1, 0, backends[0], 1024, &shared) == EPD_MEMORY_OK);
    assert(epd_memory_credit_share_commit(shared, NULL, 0) == EPD_MEMORY_OK);
    epd_memory_credit_t held = floor_request(30000, EPD_MEMORY_PRIORITY_NORMAL, 0, EPD_MEMORY_OK);
    epd_memory_credit_t waiting = floor_request(4000, EPD_MEMORY_PRIORITY_NORMAL, 1000, EPD_MEMORY_WAITING);
    size_t addition[EPD_MEMORY_BACKEND_COUNT] = {0}; addition[backends[0]] = 64;
    for (unsigned i = 0; i < 3; ++i) assert(epd_memory_credit_expand(shared, addition) == EPD_MEMORY_OK);
    assert(epd_memory_credit_expand(shared, addition) == EPD_MEMORY_WAITING);
    epd_memory_credit_snapshot_t snapshot;
    assert(epd_memory_credit_get(shared, &snapshot) && snapshot.unused[backends[0]] == 1216);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(shared);
    epd_memory_recovery_t recovery = {0};
    assert(epd_memory_recovery_begin(&recovery, backends[0], 1280, false) == EPD_MEMORY_RECOVERY_PENDING);
    assert(recovery.reason == EPD_MEMORY_REJECT_FAIRNESS && !pressure_calls);
    floor_drop(waiting);
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_READY);
    epd_memory_owner_leave_trusted(previous);
    floor_drop(held); floor_drop(shared); floor_drop(source); audit();
}
