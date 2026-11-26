
#define main unified_fixture_main
#include "host.c"
#undef main

/* Only the imported fixture's separate UI initialization case uses this. */
epd_memory_result_t app_memory_init(void) { abort(); }

static struct rt_memheap placement_heap;
static unsigned char app_storage[4 * 1024 * 1024] __attribute__((aligned(32)));
static unsigned no_fit_calls;
static bool enabled;

rt_bool_t rt_memheap_use_bounded_fit(const struct rt_memheap *heap)
{
    return enabled && heap == &placement_heap;
}

void rt_memheap_alloc_no_fit(struct rt_memheap *heap, rt_size_t aligned)
{
    if (heap != &placement_heap) return;
    assert(!heap_locks && aligned);
    ++no_fit_calls;
}

static void *allocate(size_t bytes)
{
    void *ptr = rt_memheap_alloc(&placement_heap, bytes);
    assert(ptr);
    return ptr;
}

static void verify(void *ptr, epd_memory_owner_t owner)
{
    epd_memory_block_snapshot_t block;
    assert(epd_memory_block_get(ptr, &block) == EPD_MEMORY_OK && block.owner == owner);
}

static void mixed_sizes(size_t original, unsigned big_size, unsigned small_size)
{
    void *large = allocate(big_size);
    void *left = allocate(64);
    void *small = allocate(small_size);
    void *right = allocate(64);
    void *tiny = allocate(1024);
    void *end = allocate(64);
    void *tail = allocate(placement_heap.available_size);
    rt_memheap_free(large);
    rt_memheap_free(small);
    rt_memheap_free(tiny);
    void *workspace = allocate(small_size);
    assert(workspace == (enabled ? small : large));
    void *pixels = rt_memheap_alloc(&placement_heap, big_size);
    assert((enabled && pixels == large) || (!enabled && !pixels));
    if (pixels) rt_memheap_free(pixels);
    rt_memheap_free(workspace);
    rt_memheap_free(left);
    rt_memheap_free(right);
    rt_memheap_free(end);
    rt_memheap_free(tail);
    assert(placement_heap.available_size == original);
}

static void bounded_fallback(size_t original)
{
    void *large = allocate(4096);
    void *guard[19], *holes[18];
    for (unsigned i = 0; i < 18; ++i) {
        guard[i] = allocate(64);
        holes[i] = allocate(i == 17 ? 512 : 128);
    }
    guard[18] = allocate(64);
    void *tail = allocate(placement_heap.available_size);
    rt_memheap_free(large);
    for (unsigned i = 0; i < 18; ++i) rt_memheap_free(holes[i]);
    void *ptr = allocate(512);
    assert(ptr == large);
    rt_memheap_free(ptr);
    for (unsigned i = 0; i < 19; ++i) rt_memheap_free(guard[i]);
    rt_memheap_free(tail);
    assert(placement_heap.available_size == original);
}

static void owner_limit_fallback(size_t original, epd_memory_owner_t owner,
                                 epd_memory_backend_t backend)
{
    void *large = allocate(4096);
    void *left = allocate(64);
    void *near = allocate(516);
    void *right = allocate(64);
    void *tail = allocate(placement_heap.available_size);
    rt_memheap_free(large);
    rt_memheap_free(near);
    epd_memory_account_snapshot_t account;
    assert(epd_memory_account_snapshot(owner, &account));
    size_t active = account.backend[backend].physical[EPD_MEMORY_ACTIVE];
    size_t limit = active + 512 + RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    assert(epd_memory_owner_set_limit(owner, backend, limit - 1) == EPD_MEMORY_OK);
    size_t before = placement_heap.available_size;
    assert(!rt_memheap_alloc(&placement_heap, 512));
    assert(placement_heap.available_size == before);
    assert(epd_memory_owner_set_limit(owner, backend, limit) == EPD_MEMORY_OK);
    void *allocation = allocate(512);
    assert(allocation == large);
    verify(allocation, owner);
    assert(epd_memory_owner_set_limit(owner, backend, SIZE_MAX) == EPD_MEMORY_OK);
    rt_memheap_free(allocation);
    rt_memheap_free(left);
    rt_memheap_free(right);
    rt_memheap_free(tail);
    assert(placement_heap.available_size == original);
}

static void realloc_flow(size_t original, epd_memory_owner_t owner)
{
    unsigned char *ptr = allocate(256);
    memset(ptr, 0x5a, 256);
    void *barrier = allocate(128);
    unsigned char *moved = rt_memheap_realloc(&placement_heap, ptr, 1024);
    assert(moved && moved != ptr);
    verify(moved, owner);
    for (unsigned i = 0; i < 256; ++i) assert(moved[i] == 0x5a);
    unsigned char *shrunk = rt_memheap_realloc(&placement_heap, moved, 512);
    assert(shrunk == moved);
    unsigned char *grown = rt_memheap_realloc(&placement_heap, shrunk, 768);
    assert(grown == shrunk);
    size_t before = placement_heap.available_size;
    assert(!rt_memheap_realloc(&placement_heap, grown, original + 1024));
    assert(placement_heap.available_size == before);
    verify(grown, owner);
    for (unsigned i = 0; i < 256; ++i) assert(grown[i] == 0x5a);
    rt_memheap_free(grown);
    rt_memheap_free(barrier);
    assert(placement_heap.available_size == original);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    enabled = argv[1][0] == '1';
    setup();
    assert(epd_memory_init() == EPD_MEMORY_OK);
    assert(rt_memheap_init(&placement_heap, "fit_heap", app_storage, sizeof(app_storage)) == RT_EOK);
    epd_memory_backend_t backend;
    epd_memory_backend_config_t config = { EPD_MEMORY_REGION_HCPU_PSRAM, sizeof(app_storage), 0,
                                          EPD_MEMORY_BACKEND_NONE };
    assert(epd_memory_sdk_register_heap(&placement_heap, &config, &backend) == EPD_MEMORY_OK);
    size_t original = placement_heap.available_size;
    epd_memory_owner_t owner = epd_memory_owner_create("fit-test");
    assert(owner != EPD_MEMORY_OWNER_INVALID);
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    assert(previous != EPD_MEMORY_OWNER_INVALID);

    mixed_sizes(original, 690719, 48372);
    mixed_sizes(original, 900000, 690719);
    bounded_fallback(original);
    owner_limit_fallback(original, owner, backend);
    realloc_flow(original, owner);
    assert(!rt_memheap_alloc(&placement_heap, original + 1024));
    assert(no_fit_calls == (enabled ? 2u : 4u));
    void *all = allocate(original);
    verify(all, owner);
    rt_memheap_free(all);
    assert(placement_heap.available_size == original);
    epd_memory_owner_leave_trusted(previous);
    epd_memory_owner_close_trusted(owner);
    epd_memory_owner_release_trusted(owner);
    epd_memory_owner_snapshot_t retired;
    assert(!epd_memory_owner_get(owner, &retired));
    printf("PASS bounded_fit=%u no_fit=%u\n", enabled, no_fit_calls);
}
