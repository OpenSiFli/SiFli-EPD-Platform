#include "host.h"
#include "lifecycle_host.h"
#undef rt_free
#define rt_free epd_app_free
#include "@REPO@/src/platform/epd_json.c"
#undef rt_free
#define rt_free free

static int dir_open_error;
static int dir_read_error;
static bool fail_dir_close, fail_precedence;
static int injected_close(int fd)
{
    bool failed = host_io_error("close", handles[fd].file->path);
    int result = host_close(fd);
    return failed ? -1 : result;
}
static int injected_stat(const char *path, struct stat *st)
{
    if (fail_precedence && strstr(path, "/app.json")) { errno = -EIO; return -1; }
    return host_stat(path, st);
}
static DIR *injected_opendir(const char *path)
{
    if (dir_open_error) { errno = dir_open_error; return NULL; }
    return host_opendir(path);
}
static struct dirent *injected_readdir(DIR *dir)
{
    struct dirent *entry = host_readdir(dir);
    if (!entry && dir_read_error) errno = dir_read_error;
    return entry;
}
static int injected_closedir(DIR *dir)
{
    int result = host_closedir(dir);
    return fail_dir_close ? -1 : result;
}

#undef close
#define close injected_close
#undef stat
#define stat(...) injected_stat(__VA_ARGS__)
#undef opendir
#define opendir injected_opendir
#undef readdir
#define readdir injected_readdir
#undef closedir
#define closedir injected_closedir
#include "@OVERLAY@/src/ui/platform/app_package.c"

int app_module_verified(const char *root, const char *id, const char *library, size_t *peak)
{ (void)root; (void)id; (void)library; (void)peak; return 0; }

epd_memory_owner_t epd_memory_owner_enter_trusted(epd_memory_owner_t owner)
{ assert(owner == EPD_MEMORY_OWNER_SYSTEM); return owner; }

void epd_memory_owner_leave_trusted(epd_memory_owner_t previous)
{ assert(previous == EPD_MEMORY_OWNER_SYSTEM); }

#define rt_snprintf snprintf
#define app_id_valid(id) ((id) && *(id))
#include "@OVERLAY@/locate_actual.inc"
static bool valid_id(const char *id) { return id[0] != '.'; }
static bool builtin_id(const char *id) { (void)id; return false; }
static const char *epd_locale_get(void) { return "en"; }
static bool app_installer_volume_ready(storage_volume_t v) { return v == STORAGE_FLASH; }
typedef struct { const char *id, *name; const void *icon; } builtin_app_desc_t;
static const builtin_app_desc_t builtin = {"settings", "Settings", &builtin};
static const builtin_app_desc_t *gui_builtin_app_list_open(void) { return &builtin; }
static const builtin_app_desc_t *gui_builtin_app_list_get_next(const builtin_app_desc_t *item)
{ (void)item; return NULL; }
static void gui_builtin_app_list_close(const builtin_app_desc_t *item) { (void)item; }
#define LV_EXT_STR_GET(name) (name)
#include "@OVERLAY@/catalog_actual.inc"

static void expect_preserved(const epd_app_entry_t *saved, LONG live)
{
    assert(!app_catalog_refresh());
    assert(app_catalog_get(0) == saved && app_catalog_count() == 2);
    assert(!strcmp(app_catalog_get(1)->id, "demo"));
    assert(host_alloc_live == live && !lock_depth && !host_watch_pin_count());
}

int main(void)
{
    const char manifest[] = "{\"name\":\"Demo\"}";
    const char corrupt[] = "{";
    char elf[52] = {0};
    host_watch_reset();
    available = true;
    epd_json_init();
    host_put("/flash/apps", NULL, 0, true);
    host_put("/flash/apps/demo", NULL, 0, true);
    host_put("/flash/apps/demo/app.json", manifest, sizeof(manifest) - 1, false);
    host_put("/flash/apps/demo/demo.so", elf, sizeof(elf), false);
    assert(app_catalog_refresh() && app_catalog_count() == 2);
    const epd_app_entry_t *saved = app_catalog_get(0);
    LONG live = host_alloc_live;

    host_alloc_fail = host_alloc_attempt + 2;
    expect_preserved(saved, live);
    host_alloc_fail = host_alloc_attempt + 3;
    expect_preserved(saved, live);
    host_alloc_fail = 0;
    puts("PASS manifest buffer and JSON allocation failures preserve snapshot");

    host_fail_path = "/flash/apps/demo/app.json";
    const char *operations[] = {"open", "read", "short", "close"};
    for (size_t i = 0; i < sizeof(operations) / sizeof(*operations); ++i) {
        host_fail_io = operations[i];
        expect_preserved(saved, live);
        assert(!host_fail_io);
    }
    puts("PASS manifest open/read/close failures and premature EOF preserve snapshot");

    dir_open_error = -ENOMEM;
    expect_preserved(saved, live);
    dir_open_error = -EIO;
    expect_preserved(saved, live);
    dir_open_error = 0;
    dir_read_error = -EIO;
    expect_preserved(saved, live);
    dir_read_error = -ENOTDIR;
    expect_preserved(saved, live);
    dir_read_error = 0;
    bool *faults[] = {&fail_dir_close, &fail_precedence};
    for (size_t i = 0; i < sizeof(faults) / sizeof(*faults); ++i) {
        *faults[i] = true;
        expect_preserved(saved, live);
        *faults[i] = false;
    }
    puts("PASS directory and package precedence I/O failures preserve snapshot");

    host_put("/flash/apps/missing", NULL, 0, true);
    host_put("/flash/apps/corrupt", NULL, 0, true);
    host_put("/flash/apps/corrupt/app.json", corrupt, sizeof(corrupt) - 1, false);
    host_put("/flash/apps/no_binary", NULL, 0, true);
    host_put("/flash/apps/no_binary/app.json", manifest, sizeof(manifest) - 1, false);
    assert(app_catalog_refresh() && app_catalog_count() == 2);
    assert(!strcmp(app_catalog_get(1)->id, "demo"));
    puts("PASS missing and malformed packages are skipped on successful refresh");

    const int absent_directory[] = {-ENOTDIR, ENOTDIR, -ENOENT, ENOENT};
    for (size_t i = 0; i < sizeof(absent_directory) / sizeof(*absent_directory); ++i) {
        dir_open_error = absent_directory[i];
        assert(app_catalog_refresh() && app_catalog_count() == 1);
        assert(!strcmp(app_catalog_get(0)->id, "settings"));
    }
    dir_open_error = 0;
    assert(app_catalog_refresh() && app_catalog_count() == 2);
    puts("PASS signed ENOTDIR/ENOENT publish built-ins and a restored directory is rescanned");

    epd_app_free(entries);
    assert(!host_alloc_live && !lock_depth && !host_watch_pin_count());
    return 0;
}
