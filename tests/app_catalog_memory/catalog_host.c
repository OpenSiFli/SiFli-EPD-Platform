#define main unified_memory_fixture_main
#define epd_app_alloc fixture_epd_app_alloc
#define epd_app_free fixture_epd_app_free
#include "host.c"
#undef epd_app_alloc
#undef epd_app_free
#undef main
#include <dirent.h>
#include <errno.h>
#include <limits.h>

typedef enum { STORAGE_FLASH, STORAGE_COUNT } storage_volume_t;
#define GUI_APP_ID_MAX_LEN 16
#define app_image_psram_memheap (*heaps[0])
#include "catalog_entry.inc"
#include "epd_memory.h"

static unsigned builtin_count, builtin_index, growths, fail_after;
static unsigned system_enter_failures;
static bool fail_system_enter, scan_packages;
static const char *package_directory;
static char builtin_id[16];
typedef struct { const char *id, *name; const void *icon; } builtin_app_desc_t;
static const builtin_app_desc_t builtin = { builtin_id, "Built-in", &builtin_count };
static const builtin_app_desc_t *builtin_next(void)
{
    if (builtin_index >= builtin_count) return NULL;
    snprintf(builtin_id, sizeof(builtin_id), "app%02u", builtin_index++);
    return &builtin;
}
static const builtin_app_desc_t *gui_builtin_app_list_open(void)
{ builtin_index = 0; return builtin_next(); }
static const builtin_app_desc_t *gui_builtin_app_list_get_next(const builtin_app_desc_t *item)
{ assert(item == &builtin); return builtin_next(); }
static void gui_builtin_app_list_close(const builtin_app_desc_t *item) { (void)item; }
#define LV_EXT_STR_GET(name) (name)
#define epd_get_str(key) #key
#define LOG_W(...) ((void)0)
static void storage_lock(void) {}
static void storage_unlock(void) {}
static bool app_installer_volume_ready(storage_volume_t volume)
{ assert(volume == STORAGE_FLASH); return scan_packages; }
static bool storage_apps_enabled(storage_volume_t volume) { return volume == STORAGE_FLASH; }
static const char *storage_app_directory(storage_volume_t volume)
{ assert(volume == STORAGE_FLASH); return package_directory; }
static bool storage_app_locate(const char *id, storage_volume_t *volume)
{ assert(!strcmp(id, "external")); *volume = STORAGE_FLASH; return true; }
static bool app_package_read(const char *directory, const char *name, epd_app_entry_t *entry)
{
    assert(directory == package_directory);
    if (strcmp(name, "external")) return false;
    *entry = (epd_app_entry_t){ .id = "external", .background = true };
    return true;
}
typedef enum { APP_PACKAGE_OK, APP_PACKAGE_MISSING, APP_PACKAGE_IO_ERROR, APP_PACKAGE_NO_MEMORY }
    app_package_status_t;
static const char *epd_locale_get(void) { return "en"; }
static app_package_status_t package_read_status(const char *directory, const char *name,
                               epd_app_entry_t *entry, const char *locale)
{
    (void)locale;
    return app_package_read(directory, name, entry) ? APP_PACKAGE_OK : APP_PACKAGE_MISSING;
}
static int storage_app_locate_checked(const char *id, storage_volume_t *volume)
{ return storage_app_locate(id, volume); }
bool app_memory_pressure_background_blocked(void) { return false; }
static int app_recovery_check_dynamic(void) { return RT_EOK; }
static bool storage_changing(void) { return false; }
static bool app_installer_busy(void) { return false; }
static bool app_service_stopping(void) { return false; }
static bool app_service_running(const char *id) { assert(!strcmp(id, "external")); return true; }
typedef struct { int unused; } app_entity_info;
static bool module_open_requested(const char *id, app_entity_info *info, bool start)
{ (void)id; (void)info; (void)start; abort(); }
static void module_close(const char *id, app_entity_info *info)
{ (void)id; (void)info; abort(); }

rt_err_t __real_rt_thread_mem_context_set(rt_thread_t thread, rt_uint32_t context);
rt_err_t __wrap_rt_thread_mem_context_set(rt_thread_t thread, rt_uint32_t context)
{
    if (fail_system_enter && context == EPD_MEMORY_OWNER_SYSTEM) {
        ++system_enter_failures;
        return -RT_EBUSY;
    }
    return __real_rt_thread_mem_context_set(thread, context);
}

void epd_app_free(void *memory);
#include "allocator.inc"
static void *observe_realloc(void *items, size_t size, epd_app_memory_t region)
{
    ++growths;
    if (fail_after && growths >= fail_after) return NULL;
    epd_memory_owner_t owner = epd_memory_owner_current();
    void *next = epd_app_realloc(items, size, region);
    if (next) {
        epd_memory_block_snapshot_t block;
        assert(epd_memory_block_get(next, &block) == EPD_MEMORY_OK);
        assert(block.owner == owner);
        if (!CATALOG_BASELINE && size >= sizeof(epd_app_entry_t))
            assert(owner == EPD_MEMORY_OWNER_SYSTEM);
    }
    return next;
}
#define epd_app_realloc observe_realloc
#include "catalog.inc"
#undef epd_app_realloc

int main(int argc, char **argv)
{
    assert(argc == 2 || (argc == 3 && !strcmp(argv[2], "--require-system")));
    package_directory = argv[1];
    setup();
    assert(epd_memory_init() == EPD_MEMORY_OK);
    epd_memory_owner_t app = epd_memory_owner_create("catalog-test");
    epd_memory_owner_t page = epd_memory_scope_create_trusted(app, EPD_MEMORY_SCOPE_PAGE);
    assert(app != EPD_MEMORY_OWNER_INVALID && page != EPD_MEMORY_OWNER_INVALID);
    assert(epd_memory_owner_enter_trusted(page) == EPD_MEMORY_OWNER_SYSTEM);

    scan_packages = true;
    size_t app_before = account_bytes(app, backends[0]);
    unsigned before = growths;
    app_catalog_process();
    assert(growths > before && account_bytes(app, backends[0]) == app_before);
    assert(epd_memory_owner_current() == page);
    puts("PASS temporary background ids allocated and released");

    builtin_count = 16;
    before = growths;
    assert(app_catalog_refresh());
    assert(growths - before == 6 && app_catalog_count() == 17);
    assert(epd_memory_owner_current() == page);
    epd_memory_block_snapshot_t block;
    assert(epd_memory_block_get(entries, &block) == EPD_MEMORY_OK);
    if (argc == 3 && (block.owner != EPD_MEMORY_OWNER_SYSTEM ||
                      block.account != EPD_MEMORY_OWNER_SYSTEM)) {
        fprintf(stderr, "FAIL published catalog snapshot must belong to SYSTEM\n");
        return 3;
    }
    if (CATALOG_BASELINE) {
        assert(block.owner == page && block.account == app);
        epd_memory_owner_leave_trusted(EPD_MEMORY_OWNER_SYSTEM);
        epd_memory_owner_close_trusted(page);
        epd_memory_owner_release_trusted(page);
        epd_memory_owner_close_trusted(app);
        epd_memory_owner_snapshot_t state;
        assert(epd_memory_owner_get(page, &state) && state.closed &&
               state.references == 0 && state.allocations == 1);
        assert(epd_memory_owner_get(app, &state) && state.closed && state.references == 2);
        puts("PASS baseline reproduces stranded page and APP blocked at two references");
        assert(app_catalog_refresh());
        assert(epd_memory_block_get(entries, &block) == EPD_MEMORY_OK &&
               block.owner == EPD_MEMORY_OWNER_SYSTEM);
        assert(!epd_memory_owner_get(page, &state));
        assert(epd_memory_owner_get(app, &state) && state.references == 1);
        epd_memory_owner_release_trusted(app);
        assert(!epd_memory_owner_get(app, &state));
        epd_app_free(entries);
        entries = NULL;
        audit();
        puts("PASS baseline SYSTEM refresh frees the old snapshot and unblocks APP retirement");
        return 0;
    }
    assert(block.owner == EPD_MEMORY_OWNER_SYSTEM && block.account == EPD_MEMORY_OWNER_SYSTEM);
    assert(account_bytes(app, backends[0]) == app_before);
    puts("PASS refresh and repeated growth charge SYSTEM and restore page context");

    const epd_app_entry_t *saved = app_catalog_get(0);
    const size_t system_bytes = account_bytes(EPD_MEMORY_OWNER_SYSTEM, backends[0]);
    epd_memory_owner_snapshot_t page_before, page_after;
    assert(epd_memory_owner_get(page, &page_before));
    fail_after = growths + 2;
    assert(!app_catalog_refresh());
    fail_after = 0;
    assert(saved == app_catalog_get(0) && app_catalog_count() == 17);
    assert(!strcmp(saved->id, "app00"));
    assert(epd_memory_owner_current() == page);
    assert(account_bytes(EPD_MEMORY_OWNER_SYSTEM, backends[0]) == system_bytes);
    puts("PASS allocation failure frees partial snapshot and preserves published snapshot");

    before = growths;
    fail_system_enter = true;
    assert(!app_catalog_refresh());
    fail_system_enter = false;
    assert(system_enter_failures == 1 && growths == before);
    assert(saved == app_catalog_get(0) && app_catalog_count() == 17);
    assert(epd_memory_owner_current() == page);
    assert(epd_memory_owner_get(page, &page_after));
    assert(page_after.references == page_before.references && page_after.allocations == 0);
    assert(account_bytes(EPD_MEMORY_OWNER_SYSTEM, backends[0]) == system_bytes);
    puts("PASS SYSTEM enter failure preserves published snapshot and page references");

    epd_memory_owner_leave_trusted(EPD_MEMORY_OWNER_SYSTEM);
    epd_memory_owner_close_trusted(page);
    epd_memory_owner_release_trusted(page);
    assert(!epd_memory_owner_get(page, &page_after));
    assert(epd_memory_owner_get(app, &page_after) && page_after.references == 1);
    epd_memory_owner_close_trusted(app);
    epd_memory_owner_release_trusted(app);
    assert(!epd_memory_owner_get(app, &page_after));
    assert(saved == app_catalog_get(0) && !strcmp(saved->id, "app00"));
    epd_app_free(entries);
    entries = NULL;
    audit();
    puts("PASS page and APP retire while catalog remains valid");
    return 0;
}
