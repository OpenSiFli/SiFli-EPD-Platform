#define main original_host_main
#include "host.c"
#undef main

static void continuation(const char *name)
{
    setup(); assert(epd_memory_init() == EPD_MEMORY_OK);
    epd_memory_owner_t account = epd_memory_owner_create("continuation");
    epd_memory_credit_t first = request(account, 1024, 0, EPD_MEMORY_OK);
    epd_memory_owner_t before = epd_memory_owner_enter_trusted(first);
    epd_memory_recovery_t old = {0}, next = {0};
    ticks = UINT32_MAX - RT_TICK_PER_SECOND / 10;
    assert(epd_memory_recovery_begin(&old, backends[0], 512, false) == EPD_MEMORY_RECOVERY_READY);
    uint32_t started = old.started;
    void *retained = rt_memheap_alloc(heaps[0], 64); assert(retained);
    memset(retained, 0x71, 64);
    epd_memory_owner_leave_trusted(before);
    epd_memory_credit_end(first); epd_memory_owner_release_trusted(first);
    epd_memory_credit_t second = request(account, 2048, 0, EPD_MEMORY_OK);
    before = epd_memory_owner_enter_trusted(second);
    if (!strcmp(name, "cancel")) {
        assert(epd_memory_recovery_poll(&old, true) == EPD_MEMORY_RECOVERY_DENIED);
        assert(epd_memory_recovery_continue(&next, &old) == EPD_MEMORY_RECOVERY_DENIED && !next.used);
    } else if (!strcmp(name, "deadline")) {
        ticks += (APP_MEMORY_PRESSURE_TIMEOUT_MS * RT_TICK_PER_SECOND + 999u) / 1000u;
        assert(epd_memory_recovery_continue(&next, &old) == EPD_MEMORY_RECOVERY_DENIED);
        assert(next.started == started && next.result == EPD_MEMORY_TIMEOUT);
    } else if (!strcmp(name, "closed")) {
        epd_memory_credit_end(second);
        assert(epd_memory_recovery_continue(&next, &old) == EPD_MEMORY_RECOVERY_DENIED);
        assert(next.result == EPD_MEMORY_CLOSED);
    } else if (!strcmp(name, "generation")) {
        old.pressure_requested = true; old.generation = 17;
        pressure_status.generation = 18; pressure_status.pending = true;
        assert(epd_memory_recovery_continue(&next, &old) == EPD_MEMORY_RECOVERY_DENIED);
        assert(next.result == EPD_MEMORY_CANCELLED && next.generation == 17);
    } else {
        ticks += RT_TICK_PER_SECOND / 5;
        assert(epd_memory_recovery_continue(&next, &old) == EPD_MEMORY_RECOVERY_READY);
        assert(next.started == started && next.owner == second && old.owner == first);
        assert(old.state == EPD_MEMORY_RECOVERY_DENIED && old.result == EPD_MEMORY_CANCELLED);
        assert(epd_memory_recovery_poll(&old, false) == EPD_MEMORY_RECOVERY_DENIED);
        assert(epd_memory_recovery_continue(&next, &next) == EPD_MEMORY_RECOVERY_DENIED);
        void *fresh = rt_memheap_alloc(heaps[0], 256); assert(fresh && header(fresh)->owner == second);
        rt_memheap_free(fresh);
        epd_memory_recovery_t same = {0};
        assert(epd_memory_recovery_continue(&same, &next) == EPD_MEMORY_RECOVERY_DENIED && !same.used);
        assert(next.state == EPD_MEMORY_RECOVERY_READY);
    }
    assert(header(retained)->owner == first && ((unsigned char *)retained)[63] == 0x71);
    rt_memheap_free(retained); epd_memory_owner_leave_trusted(before);
    epd_memory_credit_end(second); epd_memory_owner_release_trusted(second);
    epd_memory_owner_release_trusted(account); assert(!pressure_calls); audit();
}

static void pressure_budget(void)
{
    setup(); assert(epd_memory_init() == EPD_MEMORY_OK);
    void *blocks[6]; epd_memory_recovery_t old = {0}, next = {0}, final = {0};
    capacity_then_fragmentation(&old, blocks);
    uint32_t started = old.started;
    epd_memory_owner_t first = epd_memory_owner_create("first");
    epd_memory_owner_t before = epd_memory_owner_enter_trusted(first);
    assert(epd_memory_recovery_continue(&next, &old) == EPD_MEMORY_RECOVERY_READY);
    assert(next.pressure_requested && !next.fragmentation_requested && pressure_calls == 1);
    assert(!rt_memheap_alloc(heaps[0], 10000));
    assert(epd_memory_recovery_failed(&next) == EPD_MEMORY_RECOVERY_PENDING && pressure_calls == 2);
    uint32_t generation = next.generation;
    epd_memory_owner_leave_trusted(before);
    epd_memory_owner_t second = epd_memory_owner_create("second");
    before = epd_memory_owner_enter_trusted(second);
    assert(epd_memory_recovery_continue(&final, &next) == EPD_MEMORY_RECOVERY_PENDING);
    assert(final.started == started && final.generation == generation && final.fragmentation_requested);
    pressure_status.pending = false; pressure_status.result = APP_MEMORY_PRESSURE_SATISFIED;
    assert(epd_memory_recovery_poll(&final, false) == EPD_MEMORY_RECOVERY_READY);
    assert(!rt_memheap_alloc(heaps[0], 10000));
    assert(epd_memory_recovery_failed(&final) == EPD_MEMORY_RECOVERY_DENIED && pressure_calls == 2);
    epd_memory_recovery_t forbidden = {0};
    assert(epd_memory_recovery_continue(&forbidden, &final) == EPD_MEMORY_RECOVERY_DENIED && !forbidden.used);
    epd_memory_owner_leave_trusted(before);
    for (unsigned i = 0; i < 6; ++i) rt_memheap_free(blocks[i]);
    epd_memory_owner_release_trusted(first); epd_memory_owner_release_trusted(second); audit();
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strncmp(argv[1], "recovery_", 9) || !strcmp(argv[1], "--list")) return original_host_main(argc, argv);
    if (!strcmp(argv[1], "budget")) pressure_budget(); else continuation(argv[1]);
    printf("PASS recovery continuation %s tick=%u\n", argv[1], RT_TICK_PER_SECOND);
    return 0;
}
