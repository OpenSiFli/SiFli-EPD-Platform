/* Exercise the production shell command with the real manager and SDK heap. */
#define main memory_fixture_main
#define rt_kprintf fixture_kprintf
#define rt_sem_take fixture_sem_take
#include "host.c"
#undef rt_sem_take
#undef rt_kprintf
#undef main

static char output[65536];
static size_t output_used;
static void (*print_hook)(void);

void rt_kprintf(const char *fmt, ...)
{
    assert(!heap_locks && !critical_depth && !irq_depth);
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(output + output_used, sizeof(output) - output_used, fmt, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(output) - output_used);
    output_used += (size_t)n;
    if (print_hook && !strncmp(fmt, "mem_block heap=", 15)) {
        void (*hook)(void) = print_hook;
        print_hook = NULL;
        hook();
    }
}

rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t timeout)
{
    if (!sem->value && !heap_locks) {
        assert(timeout == 0); /* Another thread owns this heap. */
        return -RT_ETIMEOUT;
    }
    return fixture_sem_take(sem, timeout);
}

#define RT_USING_FINSH
#define EPD_INPUT_TEST_ENABLED 1
#include "../../src/platform/epd_memory_diag.c"

static void reset_output(void) { output_used = 0; output[0] = 0; }
static int command(epd_memory_owner_t owner, bool self)
{
    char number[24];
    snprintf(number, sizeof(number), "%u", owner);
    char *argv[] = {"mem_blocks", number, "self"};
    reset_output();
    return mem_blocks(self ? 3 : 2, argv);
}
static unsigned rows(void)
{
    unsigned count = 0;
    for (const char *p = output; (p = strstr(p, "mem_block heap=")); ++p) ++count;
    return count;
}
static void expect_block(void *ptr, bool present)
{
    char text[160];
    struct rt_memheap_item *item = header(ptr);
    snprintf(text, sizeof(text), "addr=%p size=%u physical=%lu owner=%u ret_addr=0x%08lx",
        ptr, item->size, (unsigned long)physical(ptr), item->owner,
#ifdef RT_USING_MEMTRACE
        (unsigned long)item->ret_addr
#else
        0ul
#endif
    );
    assert(!!strstr(output, text) == present);
}
static struct rt_memheap *system_heap(void)
{
    struct rt_list_node *node;
    rt_list_for_each(node, &heap_objects.object_list) {
        struct rt_memheap *heap = rt_list_entry(node, struct rt_memheap, parent.list);
        if (!strcmp(heap->parent.name, "heap")) return heap;
    }
    assert(false); return NULL;
}
static void *allocate_for(struct rt_memheap *heap, epd_memory_owner_t owner, size_t bytes)
{
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(owner);
    assert(previous != INVALID);
    void *ptr = rt_memheap_alloc(heap, bytes);
    epd_memory_owner_leave_trusted(previous);
    assert(ptr); return ptr;
}
static void name_heaps(void)
{
    strcpy(heaps[0]->parent.name, "app_image");
    strcpy(heaps[1]->parent.name, "lv_psram");
    assert(epd_memory_sdk_init() == EPD_MEMORY_OK);
}

static void descendants(void)
{
    epd_memory_owner_t root = epd_memory_owner_create("sample");
    epd_memory_owner_t foreign = epd_memory_owner_create("other");
    epd_memory_owner_t service = epd_memory_scope_create_trusted(root, EPD_MEMORY_SCOPE_SERVICE);
    epd_memory_owner_t task = epd_memory_scope_create_trusted(service, EPD_MEMORY_SCOPE_TASK);
    epd_memory_credit_t credit = request(task, 512, 0, EPD_MEMORY_OK);
    void *sram = allocate_for(system_heap(), root, 120);
    void *lv = allocate_for(heaps[1], service, 36);
    void *app = allocate_for(heaps[0], credit, 128);
    void *excluded = allocate_for(heaps[0], foreign, 52);
    assert(epd_memory_credit_end(credit));
    epd_memory_owner_release_trusted(credit);
    epd_memory_owner_close_trusted(task); epd_memory_owner_release_trusted(task);
    epd_memory_owner_close_trusted(service); epd_memory_owner_release_trusted(service);
    epd_memory_owner_close_trusted(root);
    epd_memory_owner_snapshot_t before, after;
    epd_memory_account_snapshot_t account_before, account_after;
    assert(epd_memory_owner_get(root, &before));
    assert(epd_memory_account_snapshot(root, &account_before));
    assert(command(root, false) == RT_EOK && rows() == 3);
    expect_block(sram, true); expect_block(lv, true); expect_block(app, true); expect_block(excluded, false);
    assert(strstr(output, "name=heap ") && strstr(output, "name=lv_psram ") && strstr(output, "name=app_image "));
    assert(strstr(output, "heaps=3 incomplete=0"));
    assert(epd_memory_owner_get(root, &after));
    assert(before.references == after.references && before.allocations == after.allocations && after.closed);
    assert(epd_memory_account_snapshot(root, &account_after));
    assert(!memcmp(&account_before, &account_after, sizeof(account_before)));
    assert(command(service, false) == RT_EOK && rows() == 2);
    expect_block(lv, true); expect_block(app, true); expect_block(sram, false);
    assert(command(service, true) == RT_EOK && rows() == 1); expect_block(lv, true);
    assert(command(credit, true) == RT_EOK && rows() == 1); expect_block(app, true);
    assert(command(0, false) == RT_EOK && rows() == 4);
    rt_memheap_free(app); rt_memheap_free(lv); rt_memheap_free(sram); rt_memheap_free(excluded);
    epd_memory_owner_release_trusted(root);
    assert(!epd_memory_owner_get(root, &after));
    assert(command(root, false) == -RT_EINVAL && strstr(output, "not_found"));
    audit();
}

static void *moving[48];
static epd_memory_owner_t moving_owner;
static void coalesce_and_allocate(void)
{
    /* Invalidate the next batch's old header and reuse its payload. */
    for (unsigned i = 14; i <= 18; ++i) { rt_memheap_free(moving[i]); moving[i] = NULL; }
    moving[14] = allocate_for(heaps[0], moving_owner, 600);
    memset(moving[14], 0xa5, 600);
}
static void mutation(void)
{
    moving_owner = epd_memory_owner_create("moving");
    for (unsigned i = 0; i < 48; ++i) moving[i] = allocate_for(heaps[0], moving_owner, 64);
    print_hook = coalesce_and_allocate;
    assert(command(moving_owner, false) == RT_EOK);
    assert(!print_hook && strstr(output, "incomplete=0"));
    expect_block(moving[47], true);
    uintptr_t previous = 0;
    unsigned count = 0;
    for (const char *p = output; (p = strstr(p, " addr=")); ++p) {
        void *address = NULL;
        assert(sscanf(p, " addr=%p", &address) == 1);
        assert((uintptr_t)address > previous);
        previous = (uintptr_t)address; ++count;
    }
    assert(count > MEM_BLOCK_BATCH && count <= 48);
    for (unsigned i = 0; i < 48; ++i) if (moving[i]) rt_memheap_free(moving[i]);
    audit();
}

static void busy(void)
{
    epd_memory_owner_t root = epd_memory_owner_create("busy");
    void *a = allocate_for(heaps[0], root, 64);
    void *b = allocate_for(heaps[1], root, 64);
    heaps[0]->lock.value = 0;
    assert(command(root, false) == -RT_ERROR);
    assert(strstr(output, "name=app_image status=busy") && strstr(output, "incomplete=1"));
    expect_block(b, true); expect_block(a, false);
    heaps[0]->lock.value = 1;
    rt_memheap_free(a); rt_memheap_free(b); audit();
}

static void corrupt(void)
{
    epd_memory_owner_t root = epd_memory_owner_create("corrupt");
    void *ptr = allocate_for(heaps[0], root, 64);
    struct rt_memheap_item *item = header(ptr), *next = item->next;
    item->next = item; /* Must stop instead of looping or following arbitrary data. */
    assert(command(root, false) == -RT_ERROR && strstr(output, "status=invalid"));
    item->next = (void *)(uintptr_t)1;
    assert(command(root, false) == -RT_ERROR && strstr(output, "status=invalid"));
    item->next = next;
    rt_memheap_free(ptr); audit();
}

static void arguments(void)
{
    const char *bad[] = {"", "-1", "1x", "4294967296", "4294967295"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char *argv[] = {"mem_blocks", (char *)bad[i]};
        reset_output(); assert(mem_blocks(2, argv) == -RT_EINVAL);
    }
    char *argv[] = {"mem_blocks", "0", "invalid", "extra"};
    for (unsigned n = 1; n <= 4; ++n) {
        if (n == 2) continue;
        reset_output(); assert(mem_blocks(n, argv) == -RT_EINVAL);
    }
    assert(command(0, true) == RT_EOK && rows() == 0);
    audit();
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setup(); assert(epd_memory_init() == EPD_MEMORY_OK); name_heaps();
    if (!strcmp(argv[1], "descendants")) descendants();
    else if (!strcmp(argv[1], "mutation")) mutation();
    else if (!strcmp(argv[1], "busy")) busy();
    else if (!strcmp(argv[1], "corrupt")) corrupt();
    else if (!strcmp(argv[1], "arguments")) arguments();
    else return 2;
    printf("PASS mem_blocks %s\n", argv[1]);
    return 0;
}
