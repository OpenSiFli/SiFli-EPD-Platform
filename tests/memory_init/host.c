#define main original_host_main
#define app_memory_init original_app_memory_declaration
#define epd_json_init original_json_init
#include "host_setup.inc"
#undef epd_json_init
#undef app_memory_init
#undef main
#include "epd_resource_runtime.h"
#define LOG_E(...) do { rt_kprintf(__VA_ARGS__); rt_kprintf("\n"); } while (0)
static unsigned json_calls, operation_calls, resource_calls, panel_calls;
void epd_json_init(void) { ++json_calls; }
bool epd_operation_runtime_init(void) { ++operation_calls; return true; }
epd_memory_owner_t app_memory_foreground_page(void *context, epd_memory_owner_t account)
{ (void)context; return account; }
int epd_resource_runtime_init(const epd_resource_runtime_config_t *config)
{
    assert(config->foreground_page == app_memory_foreground_page);
    ++resource_calls;
    return EPD_RESOURCE_OK;
}
static int epd_panel_boot(void) { ++panel_calls; return RT_EOK; }
static lv_draw_buf_handlers_t startup_font_work_handlers;
#define font_work_handlers startup_font_work_handlers
#include "startup_actual.inc"
#undef font_work_handlers
static struct rt_memheap extra[7];
static unsigned char extra_storage[7][2048] __attribute__((aligned(32)));
static unsigned char occupied_keys[6];

int main(int argc, char **argv)
{
    assert(argc == 2);
    bool ok = !strcmp(argv[1], "duplicate");
    bool busy = !strcmp(argv[1], "manager-busy");
    rt_base_t irq_guard = 0;
    if (busy) {
        irq_guard = rt_hw_interrupt_disable();
        assert(epd_memory_sdk_init() == EPD_MEMORY_BUSY);
    } else {
        setup();
        assert(epd_memory_sdk_init() == EPD_MEMORY_OK);
        unsigned registered = 0, listed = 0;
        epd_memory_backend_snapshot_t snapshot;
        for (unsigned i = 0; i < EPD_MEMORY_BACKEND_COUNT; ++i)
            registered += epd_memory_backend_get(i, &snapshot);
        for (rt_list_t *node = heap_objects.object_list.next; node != &heap_objects.object_list; node = node->next)
            ++listed;
        assert(listed >= 2 && listed <= EPD_MEMORY_BACKEND_COUNT && registered >= 2);
        unsigned count = 0;
        bool overflow = !strcmp(argv[1], "enumeration-full");
        if (overflow) count = EPD_MEMORY_BACKEND_COUNT + 1 - listed;
        if (!strcmp(argv[1], "registry-full")) {
            epd_memory_backend_config_t config = {EPD_MEMORY_REGION_SRAM, 1024, 0,
                                                   EPD_MEMORY_BACKEND_NONE};
            for (unsigned i = 0; i < EPD_MEMORY_BACKEND_COUNT - registered; ++i) {
                epd_memory_backend_t slot;
                assert(epd_memory_backend_register(&occupied_keys[i], &config, &slot) == EPD_MEMORY_OK);
            }
            count = 1;
        }
        for (unsigned i = 0; i < count; ++i)
            assert(rt_memheap_init(&extra[i], "extra", extra_storage[i], sizeof(extra_storage[i])) == RT_EOK);
        epd_memory_result_t observed = epd_memory_sdk_init();
        printf("SDK %s result=%d\n", argv[1], observed);
        assert(observed == (ok ? EPD_MEMORY_OK : EPD_MEMORY_NO_METADATA));
        epd_memory_backend_t slot;
        if (ok) {
            assert(epd_memory_backend_find(heaps[0], &slot) && slot == backends[0]);
            assert(epd_memory_backend_find(heaps[1], &slot) && slot == backends[1]);
            unsigned after = 0;
            for (unsigned i = 0; i < EPD_MEMORY_BACKEND_COUNT; ++i)
                after += epd_memory_backend_get(i, &snapshot);
            assert(after == registered);
        } else {
            assert(!epd_memory_backend_find(&extra[count - 1], &slot));
            if (overflow && count > 1) assert(epd_memory_backend_find(&extra[0], &slot));
        }
    }
    int result = startup_slice();
    if (busy) rt_hw_interrupt_enable(irq_guard);
    printf("boot %s result=%d json=%u operation=%u resource=%u panel=%u\n",
           argv[1], result, json_calls, operation_calls, resource_calls, panel_calls);
    if (json_calls) assert(font_handlers.buf_malloc_cb == font_pixels_alloc);
    else assert(!font_handlers.buf_malloc_cb);
    if (ok) {
        assert(result == 0 && json_calls == 1 && operation_calls == 1 && resource_calls == 1 && panel_calls == 1);
    } else {
        if (result != -RT_ERROR || json_calls || operation_calls || resource_calls || panel_calls) {
            fprintf(stderr, "INIT_FAIL_FAST_GATE\n");
            return 1;
        }
    }
    return 0;
}
