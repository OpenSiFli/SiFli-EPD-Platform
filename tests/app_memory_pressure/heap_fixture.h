/* Native host pointers model the RT-Thread free-list and physical spans. The
 * fixture does not claim to reproduce the target ABI or allocator placement. */
#define RT_ALIGN_SIZE 8u
#define RT_ALIGN(value, alignment) (((value) + (alignment) - 1u) & ~((alignment) - 1u))
struct rt_semaphore { bool held, busy; };
struct rt_memheap_item {
    uint32_t magic;
    struct rt_memheap *pool_ptr;
    struct rt_memheap_item *next, *prev, *next_free, *prev_free;
};
struct rt_memheap {
    size_t pool_size;
    struct rt_memheap_item *free_list;
    struct rt_semaphore lock;
};
static struct rt_memheap_item heap_sentinel;
static struct rt_memheap app_image_psram_memheap = {
    .pool_size = 1000000u + 2u * RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE),
    .free_list = &heap_sentinel
};
static union {
    struct rt_memheap_item alignment;
    unsigned char bytes[128u * 1024u];
} heap_arena;
static unsigned heap_sem_attempts, heap_sem_acquires, heap_sem_releases;
static void (*heap_after_scan)(void);

static rt_err_t rt_sem_take(struct rt_semaphore *sem, rt_int32_t timeout)
{
    CHECK(sem == &app_image_psram_memheap.lock && timeout == 0);
    CHECK(!interrupt_depth && !pressure_heap_locked && !sem->held);
    ++heap_sem_attempts;
    if (sem->busy) return -RT_EBUSY;
    sem->held = pressure_heap_locked = true;
    ++heap_sem_acquires;
    return RT_EOK;
}

static rt_err_t rt_sem_release(struct rt_semaphore *sem)
{
    CHECK(sem == &app_image_psram_memheap.lock && sem->held && pressure_heap_locked);
    CHECK(!interrupt_depth);
    sem->held = pressure_heap_locked = false;
    ++heap_sem_releases;
    /* Deterministically interleave a worker publication after the heap scan,
     * before the UI publishes its result. Never recurse while holding the heap. */
    void (*hook)(void) = heap_after_scan;
    heap_after_scan = NULL;
    if (hook) hook();
    return RT_EOK;
}

static void heap_spans(const size_t *spans, size_t count)
{
    CHECK(!interrupt_depth && !pressure_heap_locked);
    memset(&heap_arena, 0, sizeof(heap_arena));
    memset(&heap_sentinel, 0, sizeof(heap_sentinel));
    struct rt_memheap_item *previous = &heap_sentinel;
    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        CHECK(spans[i] >= sizeof(struct rt_memheap_item));
        CHECK(spans[i] == RT_ALIGN(spans[i], RT_ALIGN_SIZE));
        CHECK(offset + spans[i] + sizeof(struct rt_memheap_item) < sizeof(heap_arena.bytes));
        struct rt_memheap_item *item = (struct rt_memheap_item *)(heap_arena.bytes + offset);
        item->pool_ptr = &app_image_psram_memheap;
        item->next = (struct rt_memheap_item *)(heap_arena.bytes + offset + spans[i]);
        item->prev_free = previous;
        previous->next_free = item;
        previous = item;
        /* An allocated header separates free blocks; next and next_free are
         * deliberately different addresses. Only next defines the free span. */
        offset += spans[i] + RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE);
    }
    previous->next_free = &heap_sentinel;
    heap_sentinel.prev_free = previous;
    app_image_psram_memheap.free_list = &heap_sentinel;
}

static void heap_one_span(size_t span) { heap_spans(&span, 1); }
