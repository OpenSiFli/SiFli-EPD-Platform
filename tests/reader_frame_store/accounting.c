#include "reader_frame_store.h"
#define VIEW_FRAME_BUDGET (6u * 1024u * 1024u)
typedef int epd_app_memory_t;
#define EPD_APP_PSRAM 1
static size_t frame_bytes;
static epd_resource_cache_t view_resource;
static void rt_enter_critical(void) {}
static void rt_exit_critical(void) {}
#define RT_ALIGN_SIZE 4u
#define RT_ALIGN(n, a) (((n) + (a) - 1) & ~((a) - 1))
struct rt_memheap_item { uint8_t arm_header[40]; };
/* PEAK */
static void *epd_app_alloc(size_t bytes, epd_app_memory_t region)
{ CHECK(region == EPD_APP_PSRAM); return allocate(epd_app_allocation_peak(bytes)); }
static void epd_app_free(void *p) { dispose(p); }
/* PRODUCTION */
static void check_owner(const reader_frame_store_t *store)
{
    size_t total = 0;
    for (const reader_frame_block_t *b = store->first; b; b = b->next) {
        epd_memory_block_snapshot_t block;
        CHECK(epd_memory_block_get(b, &block) == EPD_MEMORY_OK);
        CHECK(block.owner == view_resource.owner && block.account == app);
        CHECK(block.physical == epd_app_allocation_peak(sizeof(*b) + b->stored));
        total += block.physical;
    }
    CHECK(total == store->charge);
}
#include "supply_accounting.inc"
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "supply")) { supply_accounting(); return 0; }
    bool denied = !strcmp(argv[1], "denied");
    setup(); CHECK(epd_memory_backend_set_headroom(backend, CAPACITY / 4));
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(app);
    CHECK(epd_resource_cache_open(&view_resource, EPD_RESOURCE_WORKER, cache_shrink, NULL) == 0);
    const size_t raw_bytes = 690688, raw_physical = epd_app_allocation_peak(raw_bytes);
    uint8_t *raw = allocate(raw_physical); CHECK(raw);
    CHECK(epd_resource_cache_adopt(&view_resource, raw) == 0);
    memset(raw, 255, raw_bytes);
    for (size_t i = 0; i < raw_bytes; i += 80) raw[i] = (uint8_t)i;
    epd_memory_owner_leave_trusted(previous);
    epd_resource_activity_t parent = begin(pages[0], EPD_RESOURCE_INTERACTIVE, 0, 0); drain();
    epd_resource_context_request_t request = {0}; epd_resource_activity_t activity;
    request.kind = EPD_RESOURCE_NEARBY; request.interactive_parent = parent;
    request.backend = backend; request.minimum_bytes = request.target_bytes = 65536;
    previous = epd_memory_owner_enter_trusted(pages[0]);
    CHECK(epd_resource_context_begin(&request, NULL, &activity) == EPD_RESOURCE_ACCEPTED);
    epd_memory_owner_leave_trusted(previous); drain();
    epd_resource_activity_snapshot_t activity_info;
    CHECK(epd_resource_activity_get(activity, &activity_info) == 0 && activity_info.state == EPD_RESOURCE_ADMITTED);
    previous = epd_memory_owner_enter_trusted(pages[0]);
    epd_memory_owner_t old_credit = epd_resource_context_enter(activity);
    CHECK(old_credit != EPD_MEMORY_OWNER_INVALID);
    frame_bytes = raw_bytes;
    reader_frame_store_t store = {0};
    CHECK(reader_frame_store_append(&store, raw, raw_bytes, raw_bytes, &frame_cold_allocator));
    if (denied) {
        /* Consume only the existing grant before testing a new reservation.
         * Tightening a budget must not revoke already admitted unused credit. */
        epd_memory_credit_snapshot_t grant;
        CHECK(epd_memory_credit_get((epd_memory_credit_t)activity_info.credit, &grant));
        CHECK(epd_memory_credit_trim((epd_memory_credit_t)activity_info.credit, backend,
            grant.unused[backend]) == EPD_MEMORY_OK);
        CHECK(epd_memory_owner_set_limit(app, backend, raw_physical + store.charge) == EPD_MEMORY_OK);
        CHECK(!reader_frame_store_append(&store, raw, raw_bytes, raw_bytes, &frame_cold_allocator));
    } else {
        while (store.raw < raw_bytes)
            CHECK(reader_frame_store_append(&store, raw, raw_bytes, raw_bytes, &frame_cold_allocator));
    }
    check_owner(&store);
    CHECK(physical == raw_physical + store.charge && frame_bytes == raw_bytes + store.charge);
    account_usage(app, physical, 0);
    epd_memory_credit_snapshot_t credit;
    CHECK(epd_memory_credit_get((epd_memory_credit_t)activity_info.credit, &credit));
    CHECK(!credit.physical[backend] && !credit.allocating[backend]);
    epd_memory_backend_snapshot_t heap;
    CHECK(epd_memory_backend_get(backend, &heap));
    CHECK(heap.available == CAPACITY - physical && heap.available >= CAPACITY / 4);
    size_t peak = physical;
    epd_resource_context_leave(old_credit); epd_memory_owner_leave_trusted(previous);
    CHECK(epd_resource_activity_dispose(activity) == EPD_RESOURCE_ACCEPTED); drain();
    check_owner(&store); /* Finished work leaves cache blocks with their owner. */
    reader_frame_store_clear(&store, &frame_cold_allocator);
    CHECK(physical == raw_physical && frame_bytes == raw_bytes);
    CHECK(raw[0] == 0 && raw[1] == 255 && raw[80] == 80);
    dispose(raw); frame_bytes = 0;
    CHECK(epd_resource_cache_close(&view_resource) == EPD_RESOURCE_OK);
    close_all();
    printf("PASS real manager/runtime %s: peak=%zu raw=%zu, credit->cache owner transfer, headroom, zero residual physical/allocating/owners\n",
        denied ? "partial denial" : "complete cold store", peak, raw_physical);
    return 0;
}
