#ifndef PREPARE_CONTROL_BYTES
#define PREPARE_CONTROL_BYTES 0u
#endif

static void prepare_optional_pending(bool initial)
{
    epd_memory_owner_t previous = epd_memory_owner_enter(EPD_MEMORY_OWNER_SYSTEM);
    void *occupied = epd_app_alloc(CAPACITY * 3 / 4, EPD_APP_PSRAM);
    CHECK(occupied); epd_memory_owner_leave(previous);
    epd_activity_work_t *work = queued(EPD_RESOURCE_OPTIONAL, 0, initial ? CAPACITY / 2 : 4096, 4096);
    CHECK(work); drain();
    if (!initial) {
        CHECK(epd_activity_work_ready(work) == 1);
        CHECK(epd_activity_work_prepare(work, CAPACITY / 2, 4096) == 0);
        drain();
    }
    epd_resource_activity_snapshot_t waiting, current;
    epd_resource_activity_t activity = epd_activity_work_activity(work);
    CHECK(epd_resource_activity_get(activity, &waiting) == EPD_RESOURCE_OK);
    CHECK(waiting.state == EPD_RESOURCE_WAITING && waiting.requested.kind == EPD_RESOURCE_OPTIONAL);
    CHECK(waiting.requested.deadline > epd_memory_now_ms());
    unsigned notices = component_wakes, runtime_wakes = wakes;
    epd_activity_guard_t guard;
    for (unsigned i = 0; i < 32; ++i) {
        ticks += rt_tick_from_millisecond(10);
        CHECK(epd_activity_work_prepare(work, CAPACITY / 2, 4096) == 0);
        CHECK(!epd_activity_work_enter(work, &guard));
        drain();
        CHECK(epd_resource_activity_get(activity, &current) == EPD_RESOURCE_OK);
        CHECK(current.state == EPD_RESOURCE_WAITING);
        CHECK(current.requested_revision == waiting.requested_revision);
        CHECK(current.requested.deadline == waiting.requested.deadline);
        CHECK(component_wakes == notices && wakes == runtime_wakes);
        CHECK(!irq && !mutexes && !blocked && task_count == 1 && group_count == 1);
    }
    ticks += rt_tick_from_millisecond(501); drain();
    CHECK(epd_activity_work_prepare(work, CAPACITY / 2, 4096) == -1);
    epd_app_free(occupied); drain();
    CHECK(epd_activity_work_prepare(work, CAPACITY / 2, 4096) == -1);
    epd_activity_work_close(work); drain();
    CHECK(!task_count && !group_count && !semaphores && physical[1] == PREPARE_CONTROL_BYTES);
    CHECK(!irq && !mutexes && !blocked);
}
static void prepare_activity_case(const char *name)
{
    if (!strcmp(name, "prepare_optional") || !strcmp(name, "prepare_optional_initial")) {
        prepare_optional_pending(!strcmp(name, "prepare_optional_initial")); return;
    }
    bool sram = !strcmp(name, "prepare_sram");
    bool timeout = !strcmp(name, "prepare_timeout");
    bool close_pending = !strcmp(name, "prepare_close");
    epd_activity_work_t *work = queued(EPD_RESOURCE_CONTINUOUS, 0, 4096, 4096);
    CHECK(work); drain(); CHECK(epd_activity_work_ready(work) == 1);
    epd_activity_guard_t guard;
    CHECK(epd_activity_work_enter(work, &guard));
    void *retained = epd_app_alloc(1024, EPD_APP_PSRAM); CHECK(retained);
    memset(retained, 0x5a, 1024);
    epd_memory_owner_t old_token = block_owner(retained);
    epd_activity_work_leave(&guard);
    epd_memory_owner_t previous = epd_memory_owner_enter(EPD_MEMORY_OWNER_SYSTEM);
    void *occupied = epd_app_alloc(CAPACITY * 3 / 4, sram ? EPD_APP_SRAM : EPD_APP_PSRAM);
    CHECK(occupied); epd_memory_owner_leave(previous);
    size_t psram_peak = sram ? 8192 : CAPACITY / 2;
    size_t sram_peak = sram ? CAPACITY / 2 : 8192;
    CHECK(epd_activity_work_prepare(work, psram_peak, sram_peak) == 0);
    drain(); CHECK(epd_activity_work_prepare(work, psram_peak, sram_peak) == 0);
    CHECK(!epd_activity_work_enter(work, &guard) && !blocked);
    if (timeout) {
        ticks += rt_tick_from_millisecond(501); drain();
        CHECK(epd_activity_work_prepare(work, psram_peak, sram_peak) == -1);
        epd_app_free(occupied); drain();
        CHECK(epd_activity_work_prepare(work, psram_peak, sram_peak) == -1);
    } else if (!close_pending) {
        epd_app_free(occupied); drain();
        CHECK(epd_activity_work_prepare(work, psram_peak, sram_peak) == 1);
        CHECK(epd_activity_work_enter(work, &guard));
        void *a = epd_app_alloc(psram_peak - 32, EPD_APP_PSRAM);
        void *b = epd_app_alloc(sram_peak - 32, EPD_APP_SRAM);
        CHECK(a && b && block_owner(a) != old_token);
        CHECK(((unsigned char *)retained)[1023] == 0x5a && block_owner(retained) == old_token);
        epd_app_free(a); epd_app_free(b);
        epd_activity_work_leave(&guard);
    }
    epd_activity_work_close(work); drain();
    if (close_pending) epd_app_free(occupied);
    CHECK(physical[1] == PREPARE_CONTROL_BYTES + 1056 && ((unsigned char *)retained)[0] == 0x5a);
    epd_app_free(retained); drain();
    CHECK(!task_count && !group_count && !semaphores && !blocked);
}
