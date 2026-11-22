#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "host_sections.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define RT_EOK 0
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_PRIO 1
#define RT_NAME_MAX 8
#define GUI_APP_ID_MAX_LEN 16
#define PORTAL_PACKAGE_PATH_MAX 256
#define PORTAL_PACKAGE_ID_SIZE 33
#define STORAGE_PATH_MAX 512
#define STORAGE_SD_ROOT "/sdcard"
#define APP_SAP_SIZE_MAX (16u * 1024u * 1024u)
#define rt_kprintf printf
typedef uint32_t rt_tick_t;
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef struct { const char *id; } sleep_guard_owner_t;
struct rt_mutex { unsigned depth; };
static unsigned storage_depth, backend_opens, backend_closes;
static unsigned mutex_initializations, boot_calls, panel_resumes, key_wakes;
static bool close_fails;
static int next_fd = 10;
static int rt_mutex_init(struct rt_mutex *m, const char *name, int flags)
{ (void)name; (void)flags; m->depth = 0; ++mutex_initializations; return RT_EOK; }
static void rt_mutex_take(struct rt_mutex *m, int wait) { (void)wait; CHECK(!m->depth); ++m->depth; }
static void rt_mutex_release(struct rt_mutex *m) { CHECK(m->depth == 1); --m->depth; }
static void storage_lock(void) { CHECK(!storage_depth); ++storage_depth; }
static void storage_unlock(void) { CHECK(storage_depth == 1); --storage_depth; }
static bool storage_changing(void) { return false; }
static bool storage_release_requested(void) { return false; }
static bool board_power_pending(void) { return false; }
static bool system_reset_write_allowed(void) { return true; }
static bool usb_card_reader_active(void) { return false; }
static bool app_installer_busy(void) { return false; }
static uint32_t storage_card_session(void) { return 7; }
static bool storage_session_valid(storage_volume_t volume, uint32_t session)
{ return volume == STORAGE_FLASH || session == 7; }
static const char *storage_root(storage_volume_t volume) { return volume == STORAGE_SD ? "/sdcard" : "/flash"; }
static const char *storage_path_root(const char *path) { return strncmp(path, "/sdcard", 7) ? "/flash" : "/sdcard"; }
static bool storage_mkdirs(const char *path) { (void)path; CHECK(storage_depth); return true; }
static int test_open(const char *path, int flags, ...)
{ (void)path; (void)flags; CHECK(storage_depth); ++backend_opens; return next_fd++; }
static int test_close(int fd)
{ (void)fd; CHECK(storage_depth); ++backend_closes; return close_fails ? -1 : 0; }
static int test_unlink(const char *path) { (void)path; CHECK(storage_depth); return 0; }
#define open test_open
#define close test_close
#define unlink test_unlink
struct dfs_filesystem_ops { const char *name; };
struct dfs_filesystem { const struct dfs_filesystem_ops *ops; };
static struct dfs_filesystem *dfs_filesystem_lookup(const char *path)
{
    (void)path;
    static const struct dfs_filesystem_ops ops = {"elm"};
    static struct dfs_filesystem fs = {&ops};
    return &fs;
}
#include "portal.inc"
#include "storage.inc"

typedef struct { uint32_t session, state, paths[6]; } app_package_revision_t;
static app_package_revision_t revision = {7, 1, {1}};
static unsigned background_revision = UINT32_MAX;
static uint32_t storage_version = 1;
static uint32_t storage_revision(void) { return storage_version; }
static uint32_t app_catalog_revision(void) { return 2; }
static bool builtin_id(const char *id) { return !strcmp(id, "Main"); }
static void *dlmodule_find(const char *id) { (void)id; return NULL; }
static bool app_package_revision(const char *root, const char *id, const char *library,
                                 app_package_revision_t *out)
{ CHECK(root && id && !library); *out = revision; return true; }
#define valid_id catalog_valid_id
#include "catalog.inc"
#undef valid_id

#define RT_PM_ENTER_SLEEP 1
#define RT_PM_EXIT_SLEEP 2
#define PM_SLEEP_MODE_DEEP 3
#define HAL_HPAON_GET_WSR() 1u
static const bool pm_initialized = true;
static const uint32_t key_wsr_mask = 1;
static void epd_panel_resume(void) { ++panel_resumes; }
static bool epd_power_key_wakeup(void) { ++key_wakes; return true; }
#define initialized pm_initialized
#include "wake.inc"
#undef initialized

static bool all_zero(const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i) if (bytes[i]) return false;
    return true;
}

static void boot(void)
{
    ++boot_calls;
    CHECK(host_init_portal_package_history_init() == RT_EOK);
    CHECK(host_init_storage_owned_files_init() == RT_EOK);
    CHECK(host_init_app_load_estimates_init() == RT_EOK);
    CHECK(all_zero(history, sizeof(history)));
    CHECK(all_zero(owned_files, sizeof(owned_files)));
    CHECK(all_zero(load_estimates, sizeof(load_estimates)));
}

static void cold_boot(void)
{
    memset(history, 0xa5, sizeof(history));
    memset(owned_files, 0xa5, sizeof(owned_files));
    memset(load_estimates, 0xa5, sizeof(load_estimates));
    boot();
}

static const char upload_id[] = "00000000000000000000000000000001";
static void upload(unsigned number)
{
    char id[PORTAL_PACKAGE_ID_SIZE];
    snprintf(id, sizeof(id), "%032u", number);
    CHECK(portal_package_begin(id, STORAGE_SD, 100) == 201);
    /* Model installer completion; the next begin archives the result. */
    package.state = DONE; package.cleanup = false; package.fd = -1;
}

static int populate(void)
{
    CHECK(portal_package_init());
    CHECK(app_catalog_load_required("weather") == 0);
    CHECK(storage_file_release(STORAGE_SD, 1));
    upload(1); upload(2);
    CHECK(portal_package_begin(upload_id, STORAGE_SD, 100) == 200);
    storage_lock();
    int fd = owned_open("/sdcard/live.bin", O_RDONLY, false);
    CHECK(fd >= 0 && owned_find(fd));
    close_fails = true;
    CHECK(!owned_close(fd) && owned_find(fd)->pending);
    close_fails = false;
    storage_unlock();
    remember_load_required("/sdcard/apps/weather", "weather", 123456, &revision);
    CHECK(app_catalog_load_required("weather") == 123456);
    return fd;
}

static void wake_and_check(int fd)
{
    unsigned char history_copy[sizeof(history)], owned_copy[sizeof(owned_files)], estimates_copy[sizeof(load_estimates)];
    memcpy(history_copy, history, sizeof(history));
    memcpy(owned_copy, owned_files, sizeof(owned_files));
    memcpy(estimates_copy, load_estimates, sizeof(load_estimates));
    unsigned next_history = history_next, next_estimate = load_estimate_next;
    for (unsigned i = 0; i < 3; ++i)
    {
        board_pm_notify(RT_PM_ENTER_SLEEP, PM_SLEEP_MODE_DEEP, NULL);
        board_pm_notify(RT_PM_EXIT_SLEEP, PM_SLEEP_MODE_DEEP, NULL);
        CHECK(portal_package_init());
        app_catalog_background_resume();
        CHECK(!memcmp(history_copy, history, sizeof(history)));
        CHECK(!memcmp(owned_copy, owned_files, sizeof(owned_files)));
        CHECK(!memcmp(estimates_copy, load_estimates, sizeof(load_estimates)));
        CHECK(history_next == next_history && load_estimate_next == next_estimate);
        CHECK(portal_package_begin(upload_id, STORAGE_SD, 100) == 200);
        CHECK(app_catalog_load_required("weather") == 123456);
        CHECK(owned_find(fd) && owned_find(fd)->pending);
    }
    CHECK(boot_calls == 1 && mutex_initializations == 1 && panel_resumes == 3 && key_wakes == 3);
    unsigned closed = backend_closes;
    CHECK(storage_file_release(STORAGE_SD, 99));
    CHECK(!owned_find(fd) && backend_closes == closed + 1);
}

static void capacity(void)
{
    CHECK(sizeof(history) / sizeof(history[0]) == 8);
    CHECK(sizeof(owned_files) / sizeof(owned_files[0]) == 8);
    CHECK(sizeof(load_estimates) / sizeof(load_estimates[0]) == 4);
    CHECK(portal_package_init());
    for (unsigned i = 1; i <= 9; ++i) upload(i);
    for (unsigned i = 1; i <= 8; ++i)
    {
        char id[PORTAL_PACKAGE_ID_SIZE]; snprintf(id, sizeof(id), "%032u", i);
        CHECK(portal_package_begin(id, STORAGE_SD, 100) == 200);
        CHECK(portal_package_begin(id, STORAGE_SD, 101) == 409);
    }
    storage_lock();
    int fds[8];
    for (unsigned i = 0; i < 8; ++i)
    {
        char path[64]; snprintf(path, sizeof(path), "/sdcard/live%u.bin", i);
        fds[i] = owned_open(path, O_RDONLY, false);
        CHECK(fds[i] >= 0 && owned_find(fds[i]));
        unsigned opens = backend_opens;
        CHECK(owned_open(path, O_RDONLY, false) < 0 && backend_opens == opens);
    }
    unsigned opens = backend_opens;
    CHECK(owned_open("/sdcard/extra.bin", O_RDONLY, false) < 0 && backend_opens == opens);
    CHECK(owned_close(fds[0]));
    fds[0] = owned_open("/sdcard/extra.bin", O_RDONLY, false);
    CHECK(fds[0] >= 0 && owned_find(fds[0]) && backend_opens == opens + 1);
    for (unsigned i = 0; i < 8; ++i) CHECK(owned_close(fds[i]));
    CHECK(all_zero(owned_files, sizeof(owned_files)));
    storage_unlock();
    for (unsigned i = 0; i < 4; ++i)
    {
        char id[8]; snprintf(id, sizeof(id), "app%u", i);
        remember_load_required("/sdcard/apps/item", id, 100 + i, &revision);
    }
    for (unsigned i = 0; i < 4; ++i)
    {
        char id[8]; snprintf(id, sizeof(id), "app%u", i);
        CHECK(app_catalog_load_required(id) == 100 + i);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    cold_boot();
    if (!strcmp(argv[1], "boot")) CHECK(!backend_opens && !mutex_initializations);
    else if (!strcmp(argv[1], "capacity")) capacity();
    else
    {
        int fd = populate();
        if (!strcmp(argv[1], "sleep_resume")) wake_and_check(fd);
        else if (!strcmp(argv[1], "revision_after_wake"))
        {
            wake_and_check(fd);
            ++revision.paths[0];
            CHECK(app_catalog_load_required("weather") == 0);
        }
        else if (!strcmp(argv[1], "reboot")) boot();
        else CHECK(!strcmp(argv[1], "first_use"));
    }
    CHECK(!storage_depth && !package_lock.depth);
    printf("PASS %s\n", argv[1]);
    return 0;
}
