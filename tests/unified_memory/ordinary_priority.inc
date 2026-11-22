/* SPDX-License-Identifier: Apache-2.0 */
static void ordinary_waiter(epd_memory_priority_t priority, epd_memory_credit_t *held,
                            epd_memory_credit_t *waiting)
{
    *held = floor_request(30000, EPD_MEMORY_PRIORITY_NORMAL, 0, EPD_MEMORY_OK);
    *waiting = floor_request(4000, priority, 2000, EPD_MEMORY_WAITING);
    ticks += RT_TICK_PER_SECOND / 2;
}

static void ordinary_allocation_priority(epd_memory_priority_t priority)
{
    epd_memory_credit_t held, waiting;
    epd_memory_owner_t app = epd_memory_owner_create("ordinary-work");
    ordinary_waiter(priority, &held, &waiting);
    /* Both SYSTEM and an ordinary application call the real SDK allocator.
     * Neither path has a credit or application-specific admission rule. */
    for (unsigned actor = 0; actor < 2; ++actor) {
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(actor ? app : 0);
        epd_memory_recovery_t recovery = {0};
        assert(epd_memory_recovery_begin(&recovery, backends[0],
            epd_app_allocation_peak(256), false) == EPD_MEMORY_RECOVERY_READY);
        void *p = rt_memheap_alloc(heaps[0], 256); assert(p);
        p = rt_memheap_realloc(heaps[0], p, 512); assert(p);
        rt_memheap_free(p);
        epd_memory_owner_leave_trusted(previous);
    }
    assert(!pressure_calls && !recovery_sleeps);
    epd_memory_credit_snapshot_t snapshot;
    assert(epd_memory_credit_get(waiting, &snapshot) &&
        snapshot.state == EPD_MEMORY_CREDIT_WAITING && !snapshot.bypasses);
    floor_drop(waiting); floor_drop(held);
    epd_memory_owner_release_trusted(app); audit();
}
static void test_ordinary_nearby(void)
{ ordinary_allocation_priority(EPD_MEMORY_PRIORITY_NEARBY); }
static void test_ordinary_optional(void)
{ ordinary_allocation_priority(EPD_MEMORY_PRIORITY_OPTIONAL); }

static void ordinary_preserves_fairness(epd_memory_priority_t priority)
{
    epd_memory_credit_t held, waiting;
    ordinary_waiter(priority, &held, &waiting);
    assert(!rt_memheap_alloc(heaps[0], 256));
    epd_memory_recovery_t recovery = {0};
    assert(epd_memory_recovery_begin(&recovery, backends[0],
        epd_app_allocation_peak(256), false) == EPD_MEMORY_RECOVERY_PENDING);
    assert(recovery.reason == EPD_MEMORY_REJECT_FAIRNESS && !pressure_calls);
    floor_drop(waiting);
    assert(epd_memory_recovery_poll(&recovery, false) == EPD_MEMORY_RECOVERY_READY);
    void *p = rt_memheap_alloc(heaps[0], 256); assert(p); rt_memheap_free(p);
    floor_drop(held); audit();
}
static void test_ordinary_normal_fairness(void)
{ ordinary_preserves_fairness(EPD_MEMORY_PRIORITY_NORMAL); }
static void test_ordinary_interactive_fairness(void)
{ ordinary_preserves_fairness(EPD_MEMORY_PRIORITY_INTERACTIVE); }

static void test_ordinary_reservation(void)
{
    epd_memory_credit_t held, waiting;
    ordinary_waiter(EPD_MEMORY_PRIORITY_NEARBY, &held, &waiting);
    assert(!rt_memheap_alloc(heaps[0], 4096));
    epd_memory_rejection_snapshot_t rejection;
    assert(epd_memory_rejection_start(4, 1000) == EPD_MEMORY_OK);
    assert(!rt_memheap_alloc(heaps[0], 4096));
    assert(epd_memory_rejection_read(&rejection) && rejection.valid &&
        rejection.reason != EPD_MEMORY_REJECT_FAIRNESS);
    floor_drop(waiting); floor_drop(held); audit();
}
