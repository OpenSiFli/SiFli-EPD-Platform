/*
 * Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
static bool recovery_not_cancelled(void *context) { (void)context; return false; }
static bool recovery_cancel_after_sleep(void *context) { (void)context; return recovery_sleeps != 0; }

static void test_recovery_credit(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("retry-credit");
    epd_memory_credit_t c = request(a, 64, 0, EPD_MEMORY_OK);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(c);
    epd_memory_rejection_snapshot_t rejection;
    assert(epd_memory_rejection_start(1, 100) == EPD_MEMORY_OK);
    assert(!rt_memheap_alloc(heaps[0], 256));
    assert(epd_memory_rejection_read(&rejection) && rejection.reason == EPD_MEMORY_REJECT_CREDIT_UNUSED);
    epd_memory_retry_t retry = {0};
    assert(epd_memory_retry_wait(&retry, 256, recovery_not_cancelled, NULL));
    assert(retry.owner == c && retry.reason == EPD_MEMORY_REJECT_CHANGE && !pressure_calls && !recovery_sleeps);
    epd_memory_credit_snapshot_t credit;
    assert(epd_memory_credit_get(c, &credit) && credit.unused[backends[0]] == epd_app_allocation_peak(256));
    void *p = rt_memheap_alloc(heaps[0], 256); assert(p && header(p)->owner == c);
    assert(!epd_memory_retry_wait(&retry, 256, recovery_not_cancelled, NULL));
    rt_memheap_free(p); epd_memory_owner_leave_trusted(previous);
    epd_memory_credit_end(c); epd_memory_owner_release_trusted(c); epd_memory_owner_release_trusted(a); audit();
}

static void test_recovery_closed_limit(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("retry-hard-limit");
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(a);
    epd_memory_rejection_snapshot_t rejection;
    assert(epd_memory_owner_set_limit(a, backends[0], 128) == EPD_MEMORY_OK);
    assert(epd_memory_rejection_start(1, 100) == EPD_MEMORY_OK);
    assert(!rt_memheap_alloc(heaps[0], 256));
    assert(epd_memory_rejection_read(&rejection) && rejection.reason == EPD_MEMORY_REJECT_LIMIT);
    epd_memory_retry_t limit = {0}, closed = {0}, terminal = {0};
    assert(!epd_memory_retry_begin(&limit, 256) && limit.result == EPD_MEMORY_LIMIT && !pressure_calls);
    assert(epd_memory_owner_set_limit(a, backends[0], SIZE_MAX) == EPD_MEMORY_OK);
    epd_memory_credit_t c = request(a, 1024, 0, EPD_MEMORY_OK);
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(c);
    epd_memory_credit_end(c);
    assert(!rt_memheap_alloc(heaps[0], 64));
    assert(!epd_memory_retry_begin(&terminal, 64) && terminal.result == EPD_MEMORY_CLOSED);
    epd_memory_owner_leave_trusted(old); epd_memory_owner_release_trusted(c);
    epd_memory_owner_close_trusted(a);
    assert(!rt_memheap_alloc(heaps[0], 64));
    assert(!epd_memory_retry_begin(&closed, 64) && closed.result == EPD_MEMORY_CLOSED && !pressure_calls);
    epd_memory_owner_leave_trusted(previous); epd_memory_owner_release_trusted(a); audit();
}

static void test_recovery_reserved(void)
{
    epd_memory_credit_t held = request(0, 30000, 0, EPD_MEMORY_OK);
    epd_memory_owner_t a = epd_memory_owner_create("retry-reserved");
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(a);
    assert(!rt_memheap_alloc(heaps[0], 4096));
    epd_memory_retry_t retry = {0};
    assert(epd_memory_retry_begin(&retry, 4096));
    assert(retry.reason == EPD_MEMORY_REJECT_RESERVED && !pressure_calls);
    assert(epd_memory_retry_poll(&retry, false) == EPD_MEMORY_RETRY_PENDING);
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held);
    assert(epd_memory_retry_poll(&retry, false) == EPD_MEMORY_RETRY_READY && !pressure_calls);
    void *p = rt_memheap_alloc(heaps[0], 4096); assert(p); rt_memheap_free(p);
    epd_memory_owner_leave_trusted(previous); epd_memory_owner_release_trusted(a); audit();
}

static void test_recovery_fairness(void)
{
    epd_memory_credit_t held = request(0, 30000, 0, EPD_MEMORY_OK);
    epd_memory_owner_t a = epd_memory_owner_create("retry-fairness");
    epd_memory_credit_t waiter = request(a, 4000, 1000, EPD_MEMORY_WAITING);
    for (unsigned i = 0; i < 3; ++i) {
        void *p = rt_memheap_alloc(heaps[0], 32); assert(p); rt_memheap_free(p);
    }
    assert(!rt_memheap_alloc(heaps[0], 32));
    epd_memory_retry_t retry = {0};
    assert(epd_memory_retry_begin(&retry, 32) && retry.reason == EPD_MEMORY_REJECT_FAIRNESS);
    assert(epd_memory_retry_poll(&retry, false) == EPD_MEMORY_RETRY_PENDING && !pressure_calls);
    epd_memory_credit_cancel(waiter); epd_memory_owner_release_trusted(waiter);
    assert(epd_memory_retry_poll(&retry, false) == EPD_MEMORY_RETRY_READY);
    void *p = rt_memheap_alloc(heaps[0], 32); assert(p); rt_memheap_free(p);
    epd_memory_credit_end(held); epd_memory_owner_release_trusted(held); epd_memory_owner_release_trusted(a); audit();
}

static void test_recovery_physical(void)
{
    void *occupied = rt_memheap_alloc(heaps[0], 17000); assert(occupied);
    epd_memory_credit_t reserved = request(0, 12000, 0, EPD_MEMORY_OK);
    epd_memory_owner_t a = epd_memory_owner_create("retry-physical");
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(a);
    assert(!rt_memheap_alloc(heaps[0], 4096));
    release_during_recovery = occupied;
    epd_memory_retry_t retry = {0};
    assert(epd_memory_retry_wait(&retry, 4096, recovery_not_cancelled, NULL));
    assert(pressure_calls == 1 && pressure_forced == 1 && recovery_sleeps == 1);
    assert(pressure_status.min_available == 12000 + epd_app_allocation_peak(4096));
    epd_memory_credit_snapshot_t credit;
    assert(epd_memory_credit_get(reserved, &credit) && credit.unused[backends[0]] == 12000);
    void *p = rt_memheap_alloc(heaps[0], 4096); assert(p); rt_memheap_free(p);
    epd_memory_owner_leave_trusted(previous); epd_memory_owner_release_trusted(a);
    epd_memory_credit_end(reserved); epd_memory_owner_release_trusted(reserved); audit();
}

static void test_recovery_fragmentation(void)
{
    void *blocks[6];
    for (unsigned i = 0; i < 6; ++i) { blocks[i] = rt_memheap_alloc(heaps[0], 4000); assert(blocks[i]); }
    for (unsigned i = 0; i < 6; i += 2) { rt_memheap_free(blocks[i]); blocks[i] = NULL; }
    assert(epd_app_psram_available() > epd_app_allocation_peak(10000));
    epd_memory_recovery_t recovery = {0};
    assert(epd_memory_recovery_begin(&recovery, backends[0], epd_app_allocation_peak(10000), false) == EPD_MEMORY_RECOVERY_READY);
    assert(!rt_memheap_alloc(heaps[0], 10000) && !pressure_calls);
    assert(epd_memory_recovery_failed(&recovery) == EPD_MEMORY_RECOVERY_PENDING && pressure_forced == 1);
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_PENDING && pressure_calls == 1);
    rt_memheap_free(blocks[1]); blocks[1] = NULL;
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_READY);
    void *p = rt_memheap_alloc(heaps[0], 10000); assert(p); rt_memheap_free(p);
    for (unsigned i = 0; i < 6; ++i) rt_memheap_free(blocks[i]);
    audit();
}

static void capacity_then_fragmentation(epd_memory_recovery_t *recovery, void *blocks[6])
{
    for (unsigned i = 0; i < 6; ++i) {
        blocks[i] = rt_memheap_alloc(heaps[0], 4000); assert(blocks[i]);
    }
    assert(epd_app_psram_available() < epd_app_allocation_peak(10000));
    assert(epd_memory_recovery_begin(recovery, backends[0], epd_app_allocation_peak(10000), false) == EPD_MEMORY_RECOVERY_PENDING);
    assert(pressure_calls == 1 && !pressure_forced);
    rt_memheap_free(blocks[0]); blocks[0] = NULL;
    rt_memheap_free(blocks[2]); blocks[2] = NULL;
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(epd_memory_recovery_poll(recovery, false) == EPD_MEMORY_RECOVERY_READY);
    assert(!rt_memheap_alloc(heaps[0], 10000));
}

static void test_recovery_capacity_then_fragmentation(void)
{
    void *blocks[6]; epd_memory_recovery_t recovery = {0};
    capacity_then_fragmentation(&recovery, blocks);
    uint32_t original_start = recovery.started;
    assert(epd_memory_recovery_failed(&recovery) == EPD_MEMORY_RECOVERY_PENDING);
    assert(pressure_calls == 2 && pressure_forced == 1 && recovery.started == original_start);
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_PENDING && pressure_calls == 2);
    rt_memheap_free(blocks[1]); blocks[1] = NULL;
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_READY);
    void *p = rt_memheap_alloc(heaps[0], 10000); assert(p); rt_memheap_free(p);
    for (unsigned i = 0; i < 6; ++i) rt_memheap_free(blocks[i]);
    audit();
}

static void test_recovery_capacity_fragmentation_exhausted(void)
{
    void *blocks[6]; epd_memory_recovery_t recovery = {0};
    capacity_then_fragmentation(&recovery, blocks);
    assert(epd_memory_recovery_failed(&recovery) == EPD_MEMORY_RECOVERY_PENDING && pressure_calls == 2);
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_READY);
    assert(!rt_memheap_alloc(heaps[0], 10000));
    assert(epd_memory_recovery_failed(&recovery) == EPD_MEMORY_RECOVERY_DENIED && pressure_calls == 2);
    assert(recovery.result == EPD_MEMORY_NO_CAPACITY);
    for (unsigned i = 0; i < 6; ++i) rt_memheap_free(blocks[i]);
    audit();
}

static void test_recovery_capacity_fragmentation_deadline(void)
{
    void *blocks[6]; epd_memory_recovery_t recovery = {0};
    ticks = UINT32_MAX - RT_TICK_PER_SECOND;
    capacity_then_fragmentation(&recovery, blocks);
    const uint32_t timeout = (uint32_t)(((uint64_t)APP_MEMORY_PRESSURE_TIMEOUT_MS * RT_TICK_PER_SECOND + 999u) / 1000u);
    ticks += timeout - 1;
    assert(epd_memory_recovery_failed(&recovery) == EPD_MEMORY_RECOVERY_PENDING && pressure_calls == 2);
    ++ticks;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_DENIED);
    assert(recovery.result == EPD_MEMORY_TIMEOUT && pressure_calls == 2);
    for (unsigned i = 0; i < 6; ++i) rt_memheap_free(blocks[i]);
    audit();
}

enum { RECOVERY_FRAGMENT_BLOCKS = 40, RECOVERY_FRAGMENT_REQUEST = 1200 };

static void recovery_many_holes(void **blocks)
{
    for (unsigned i = 0; i < RECOVERY_FRAGMENT_BLOCKS; ++i) {
        blocks[i] = rt_memheap_alloc(heaps[0], 512); assert(blocks[i]);
    }
    /* Consume the tail so only the separated holes can satisfy the request. */
    blocks[RECOVERY_FRAGMENT_BLOCKS] = rt_memheap_alloc(heaps[0], epd_app_psram_available());
    assert(blocks[RECOVERY_FRAGMENT_BLOCKS] && !epd_app_psram_available());
    for (unsigned i = 0; i < RECOVERY_FRAGMENT_BLOCKS; i += 2) {
        rt_memheap_free(blocks[i]); blocks[i] = NULL;
    }
    const struct rt_memheap_item *sentinel = heaps[0]->free_list;
    unsigned holes = 0;
    for (const struct rt_memheap_item *item = sentinel->next_free; item != sentinel; item = item->next_free) {
        assert((size_t)((const unsigned char *)item->next - (const unsigned char *)item) <
               RECOVERY_FRAGMENT_REQUEST);
        ++holes;
    }
    assert(holes == 20);
    assert(epd_app_psram_available() > epd_app_allocation_peak(RECOVERY_FRAGMENT_REQUEST));
    audit();
}

static void recovery_free_many_holes(void **blocks)
{
    for (unsigned i = 0; i <= RECOVERY_FRAGMENT_BLOCKS; ++i) rt_memheap_free(blocks[i]);
    audit();
}

static void test_recovery_fragmentation_many(void)
{
    void *blocks[RECOVERY_FRAGMENT_BLOCKS + 1];
    recovery_many_holes(blocks);
    epd_memory_recovery_t recovery = {0};
    size_t demand = epd_app_allocation_peak(RECOVERY_FRAGMENT_REQUEST);
    assert(epd_memory_recovery_begin(&recovery, backends[0], demand, false) == EPD_MEMORY_RECOVERY_READY);
    assert(!pressure_calls && !rt_memheap_alloc(heaps[0], RECOVERY_FRAGMENT_REQUEST));
    assert(epd_memory_recovery_failed(&recovery) == EPD_MEMORY_RECOVERY_PENDING);
    assert(pressure_calls == 1 && pressure_forced == 1 && pressure_status.min_available == demand);
    for (unsigned i = 0; i < 3; ++i)
        assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_PENDING && pressure_calls == 1);
    rt_memheap_free(blocks[1]); blocks[1] = NULL;
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_READY);
    void *p = rt_memheap_alloc(heaps[0], RECOVERY_FRAGMENT_REQUEST); assert(p);
    assert(pressure_calls == 1); rt_memheap_free(p);
    recovery_free_many_holes(blocks);
}

static void test_recovery_fragmentation_many_exhausted(void)
{
    void *blocks[RECOVERY_FRAGMENT_BLOCKS + 1];
    recovery_many_holes(blocks);
    assert(!rt_memheap_alloc(heaps[0], RECOVERY_FRAGMENT_REQUEST));
    epd_memory_recovery_t recovery = {0};
    assert(epd_memory_recovery_begin(&recovery, backends[0], epd_app_allocation_peak(RECOVERY_FRAGMENT_REQUEST),
                                     true) == EPD_MEMORY_RECOVERY_PENDING);
    assert(pressure_calls == 1 && pressure_forced == 1);
    /* Total capacity can be satisfied while no contiguous block fits. */
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_READY);
    assert(!rt_memheap_alloc(heaps[0], RECOVERY_FRAGMENT_REQUEST));
    assert(epd_memory_recovery_failed(&recovery) == EPD_MEMORY_RECOVERY_DENIED);
    assert(recovery.result == EPD_MEMORY_NO_CAPACITY && pressure_calls == 1);
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_DENIED);
    assert(epd_memory_recovery_failed(&recovery) == EPD_MEMORY_RECOVERY_DENIED && pressure_calls == 1);
    recovery_free_many_holes(blocks);
}

static void test_recovery_credit_fragmentation(void)
{
    void *blocks[RECOVERY_FRAGMENT_BLOCKS + 1];
    recovery_many_holes(blocks);
    epd_memory_owner_t owner = epd_memory_owner_create("fragmented-credit");
    epd_memory_credit_t credit = request(owner, 64, 0, EPD_MEMORY_OK);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(credit);
    assert(!rt_memheap_alloc(heaps[0], RECOVERY_FRAGMENT_REQUEST));
    epd_memory_recovery_t recovery = {0};
    size_t demand = epd_app_allocation_peak(RECOVERY_FRAGMENT_REQUEST);
    assert(epd_memory_recovery_begin(&recovery, backends[0], demand, true) == EPD_MEMORY_RECOVERY_PENDING);
    assert(pressure_calls == 1 && pressure_forced == 1);
    epd_memory_credit_snapshot_t snapshot;
    assert(epd_memory_credit_get(credit, &snapshot) && snapshot.unused[backends[0]] == demand);
    rt_memheap_free(blocks[1]); blocks[1] = NULL;
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_READY);
    void *p = rt_memheap_alloc(heaps[0], RECOVERY_FRAGMENT_REQUEST); assert(p); rt_memheap_free(p);
    epd_memory_owner_leave_trusted(previous);
    epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit); epd_memory_owner_release_trusted(owner);
    recovery_free_many_holes(blocks);
}

static void test_recovery_fragmentation_many_timeout(void)
{
    void *blocks[RECOVERY_FRAGMENT_BLOCKS + 1];
    recovery_many_holes(blocks);
    assert(!rt_memheap_alloc(heaps[0], RECOVERY_FRAGMENT_REQUEST));
    epd_memory_recovery_t recovery = {0};
    const uint32_t timeout = (uint32_t)(((uint64_t)APP_MEMORY_PRESSURE_TIMEOUT_MS * RT_TICK_PER_SECOND + 999u) / 1000u);
    ticks = UINT32_MAX - RT_TICK_PER_SECOND;
    assert(epd_memory_recovery_begin(&recovery, backends[0], epd_app_allocation_peak(RECOVERY_FRAGMENT_REQUEST),
                                     true) == EPD_MEMORY_RECOVERY_PENDING);
    assert(pressure_calls == 1 && pressure_forced == 1);
    ticks += timeout - 1;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_PENDING && pressure_calls == 1);
    ++ticks;
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_DENIED);
    assert(recovery.result == EPD_MEMORY_TIMEOUT && pressure_calls == 1);
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_DENIED);
    assert(epd_memory_recovery_failed(&recovery) == EPD_MEMORY_RECOVERY_DENIED && pressure_calls == 1);
    recovery_free_many_holes(blocks);
}

static void test_recovery_fragmentation_many_released(void)
{
    /* Cover release before begin and between a READY attempt and failed(). */
    for (unsigned started = 0; started < 2; ++started) {
        void *blocks[RECOVERY_FRAGMENT_BLOCKS + 1];
        recovery_many_holes(blocks);
        epd_memory_recovery_t recovery = {0};
        size_t demand = epd_app_allocation_peak(RECOVERY_FRAGMENT_REQUEST);
        if (started)
            assert(epd_memory_recovery_begin(&recovery, backends[0], demand, false) == EPD_MEMORY_RECOVERY_READY);
        assert(!rt_memheap_alloc(heaps[0], RECOVERY_FRAGMENT_REQUEST));
        /* Coalesce the 16th hole, at the end of the bounded scan. */
        rt_memheap_free(blocks[31]); blocks[31] = NULL;
        const struct rt_memheap_item *item = heaps[0]->free_list->next_free;
        for (unsigned i = 0; i < 15; ++i, item = item->next_free)
            assert((size_t)((const unsigned char *)item->next - (const unsigned char *)item) < demand);
        assert(item != heaps[0]->free_list);
        assert((size_t)((const unsigned char *)item->next - (const unsigned char *)item) >= demand);
        assert(item->next_free != heaps[0]->free_list);
        assert((started ? epd_memory_recovery_failed(&recovery) :
                epd_memory_recovery_begin(&recovery, backends[0], demand, true)) == EPD_MEMORY_RECOVERY_READY);
        assert(!pressure_calls && !recovery.pressure_requested);
        void *p = rt_memheap_alloc(heaps[0], RECOVERY_FRAGMENT_REQUEST); assert(p); rt_memheap_free(p);
        recovery_free_many_holes(blocks);
    }
}

static void test_recovery_cancel_timeout(void)
{
    void *occupied = rt_memheap_alloc(heaps[0], 28000); assert(occupied);
    epd_memory_retry_t cancelled = {0};
    assert(!rt_memheap_alloc(heaps[0], 8192));
    assert(!epd_memory_retry_wait(&cancelled, 8192, recovery_cancel_after_sleep, NULL));
    assert(cancelled.result == EPD_MEMORY_CANCELLED && pressure_calls == 1 && recovery_sleeps == 1);
    epd_memory_retry_t timed = {0}, exhausted = {0};
    ticks = UINT32_MAX - RT_TICK_PER_SECOND;
    assert(epd_memory_retry_begin(&timed, 8192));
    ticks += 5u * RT_TICK_PER_SECOND;
    assert(epd_memory_retry_poll(&timed, false) == EPD_MEMORY_RETRY_DONE && timed.result == EPD_MEMORY_TIMEOUT);
    assert(epd_memory_retry_begin(&exhausted, 8192));
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_EXHAUSTED;
    assert(epd_memory_retry_poll(&exhausted, false) == EPD_MEMORY_RETRY_DONE);
    assert(pressure_calls == 3);
    rt_memheap_free(occupied); audit();
}

static void test_recovery_context_cleanup(void)
{
    epd_memory_owner_t a = epd_memory_owner_create("retry-context");
    epd_memory_owner_t cleanup = epd_memory_scope_create_trusted(a, EPD_MEMORY_SCOPE_CLEANUP);
    epd_memory_credit_t credit = request(cleanup, 1024, 0, EPD_MEMORY_OK);
    epd_memory_owner_close_trusted(a);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(credit);
    epd_memory_recovery_t allowed = {0}, beyond = {0};
    assert(epd_memory_recovery_begin(&allowed, backends[0], epd_app_allocation_peak(64), false) == EPD_MEMORY_RECOVERY_READY);
    void *p = rt_memheap_alloc(heaps[0], 64); assert(p); rt_memheap_free(p);
    assert(!rt_memheap_alloc(heaps[0], 2048));
    assert(epd_memory_recovery_begin(&beyond, backends[0], epd_app_allocation_peak(2048), true) == EPD_MEMORY_RECOVERY_DENIED);
    assert(beyond.result == EPD_MEMORY_CLOSED && !pressure_calls);
    epd_memory_owner_leave_trusted(previous);
    assert(epd_memory_recovery_poll(&allowed, false) == EPD_MEMORY_RECOVERY_DENIED);
    assert(allowed.reason == EPD_MEMORY_REJECT_CONTEXT);
    epd_memory_credit_end(credit); epd_memory_owner_release_trusted(credit);
    epd_memory_owner_release_trusted(cleanup); epd_memory_owner_release_trusted(a); audit();
}

static void test_recovery_already_released(void)
{
    void *occupied = rt_memheap_alloc(heaps[0], 28000); assert(occupied);
    assert(!rt_memheap_alloc(heaps[0], 8192));
    /* Another thread, or failure cleanup, can release storage before begin. */
    rt_memheap_free(occupied);
    epd_memory_retry_t retry = {0};
    assert(epd_memory_retry_wait(&retry, 8192, recovery_not_cancelled, NULL));
    assert(!pressure_calls && !recovery_sleeps);
    void *p = rt_memheap_alloc(heaps[0], 8192); assert(p); rt_memheap_free(p);
    audit();
}
