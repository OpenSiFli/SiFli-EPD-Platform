/* The progress gate uses actual observed frees from the manager/runtime
 * allocator ledger. Geometry, rendering and fragmented holes are exercised
 * separately by prewarm.inc with the production frame storage functions. */
typedef struct view_frame { struct view_frame *next; unsigned layout; } view_frame_t;
static struct {
    bool pixels_blocked, pixels_memory_blocked, pixels_retry_ready;
    unsigned pixels_blocked_intent, pixels_blocked_priority, intent, layout, session;
    size_t pixels_blocked_bytes, pixels_blocked_available;
    view_frame_t *frames;
} view;
static void *state;
static unsigned frame_priority(void *owner, unsigned number) { (void)owner; return number; }
static bool frame_spare_available(const view_frame_t *frame) { (void)frame; return false; }
static size_t epd_app_psram_available(void) { return CAPACITY - physical; }
#define rt_kprintf(...) do { if (0) printf(__VA_ARGS__); } while (0)
/* SUPPLY PRODUCTION */
static void supply_accounting(void)
{
    setup(); const size_t floor = 1024u * 1024u;
    CHECK(epd_memory_backend_set_headroom(backend, floor));
    const size_t raw = epd_app_allocation_peak(690688u + 31u);
    const size_t need = epd_app_allocation_peak(690688u + 31u + 65536u);
    void *frames[7] = {0}; reclaim_fixture_t retired = {0};
    epd_memory_owner_t old = epd_memory_owner_enter_trusted(app);
    CHECK(epd_resource_cache_open(&view_resource, EPD_RESOURCE_WORKER, cache_shrink, NULL) == 0);
    for (unsigned i = 0; i < 3; ++i) {
        frames[i] = allocate(raw); CHECK(frames[i]); memset(frames[i], (int)i, raw);
        CHECK(epd_resource_cache_adopt(&view_resource, frames[i]) == 0);
    }
    CHECK(epd_resource_cache_update(&view_resource, &app_image_psram_memheap, 3 * raw, 0, 4, 0) == 0);
    CHECK(epd_resource_cache_open(&retired.cache, EPD_RESOURCE_WORKER, reclaim_payload, &retired) == 0);
    size_t obsolete = CAPACITY - floor - 3 * raw - need + 4096u;
    retired.data = allocate(obsolete); CHECK(retired.data);
    CHECK(epd_resource_cache_adopt(&retired.cache, retired.data) == 0);
    CHECK(epd_memory_owner_set_class(retired.cache.owner, EPD_MEMORY_RECLAIMABLE) == EPD_MEMORY_OK);
    CHECK(epd_resource_cache_update(&retired.cache, &app_image_psram_memheap, obsolete, obsolete, 1, 1) == 0);
    epd_memory_owner_leave_trusted(old);
    epd_resource_activity_t parent = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 0, 0); drain();
    epd_resource_context_request_t request = {0}; epd_resource_activity_t nearby;
    request.kind = EPD_RESOURCE_NEARBY; request.interactive_parent = parent; request.backend = backend;
    request.minimum_bytes = request.target_bytes = 65536u;
    old = epd_memory_owner_enter_trusted(pages[0]);
    CHECK(epd_resource_context_begin(&request, NULL, &nearby) == EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(old); drain();
    old = epd_memory_owner_enter_trusted(pages[0]);
    epd_memory_owner_t context_owner = epd_resource_context_enter(nearby);
    CHECK(context_owner != EPD_MEMORY_OWNER_INVALID);
    epd_memory_recovery_t recovery = {0}; bool reclaim = false;
    recovery.owner = epd_memory_credit_current(); recovery.backend = backend; recovery.physical = need;
    CHECK(epd_memory_recovery_admit(&recovery, false, true, CAPACITY - physical, &reclaim) == EPD_MEMORY_RECOVERY_PENDING);
    CHECK(reclaim && recovery.reason == EPD_MEMORY_REJECT_CAPACITY);
    CHECK(recovery.min_available == floor + need);
    view.intent = view.layout = 1; frame_bytes = 3 * 690688u;
    optional_pixels_block(-3, 3);
    for (unsigned i = 0; i < 100; ++i) {
        epd_memory_owner_t churn = epd_memory_scope_create(app, EPD_MEMORY_SCOPE_TASK);
        CHECK(churn != EPD_MEMORY_OWNER_INVALID); epd_memory_owner_release(churn);
        CHECK(!optional_pixels_ready_for(3));
    }
    epd_resource_context_leave(context_owner); epd_memory_owner_leave_trusted(old);
    CHECK(epd_resource_reclaim(backend, 4096u, 1) == 4096u); drain();
    CHECK(retired.calls == 1 && !retired.data && physical == 3 * raw);
    CHECK(optional_pixels_ready_for(3));
    old = epd_memory_owner_enter_trusted(pages[0]); context_owner = epd_resource_context_enter(nearby);
    CHECK(context_owner != EPD_MEMORY_OWNER_INVALID);
    for (unsigned i = 3; i < 7; ++i) {
        recovery.owner = epd_memory_credit_current();
        CHECK(epd_memory_recovery_admit(&recovery, false, true, CAPACITY - physical, &reclaim) == EPD_MEMORY_RECOVERY_READY);
        CHECK(!reclaim); frames[i] = allocate(raw); CHECK(frames[i]); memset(frames[i], (int)i, raw);
        CHECK(epd_resource_cache_adopt(&view_resource, frames[i]) == EPD_RESOURCE_OK);
        CHECK(epd_resource_cache_update(&view_resource, &app_image_psram_memheap, (i + 1u) * raw, 0, 4, 0) == 0);
    }
    epd_resource_context_leave(context_owner); epd_memory_owner_leave_trusted(old);
    CHECK(epd_resource_reclaim(backend, CAPACITY, 8) == 0); drain();
    CHECK(physical == 7 * raw && CAPACITY - physical >= floor);
    for (unsigned i = 0; i < 7; ++i) {
        epd_memory_block_snapshot_t block;
        CHECK(epd_memory_block_get(frames[i], &block) == EPD_MEMORY_OK);
        CHECK(block.owner == view_resource.owner && block.account == app && block.physical == raw);
        CHECK(((unsigned char *)frames[i])[0] == i && ((unsigned char *)frames[i])[raw - 1] == i);
    }
    CHECK(epd_resource_activity_dispose(nearby) == EPD_RESOURCE_ACCEPTED); drain();
    for (unsigned i = 0; i < 7; ++i) dispose(frames[i]); frame_bytes = 0;
    CHECK(epd_resource_cache_update(&view_resource, &app_image_psram_memheap, 0, 0, 4, 0) == 0);
    CHECK(epd_resource_cache_close(&retired.cache) == 0 && epd_resource_cache_close(&view_resource) == 0);
    close_all();
    printf("PASS real 10MiB manager/resource: NEARBY incremental growth respects 1MiB floor, pressure releases obsolete cache, new-page gate rearms on observed free, seven raw owner-accounted frames protected; physical=%zu zero residual\n", 7 * raw);
}
