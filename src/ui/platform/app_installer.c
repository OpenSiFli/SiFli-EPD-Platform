#include "reminder_internal.h"
#include "storage_internal.h"
#include "storage_file_internal.h"
#include "app_installer.h"
#include "epd_memory.h"
#include "system_reset_guard.h"
#include "epd_i18n.h"
#include "app_catalog.h"
#include "app_package.h"
#include "app_sap.h"
#include "app_module.h"
#include "storage.h"
#include "app_service.h"
#include "ai_settings.h"
#include "sleep_guard.h"
#include "epd_power.h"
#include "gui_app_int.h"
#include <errno.h>
#include <dfs_posix.h>
#include <dfs_fs.h>
#include <string.h>
#include <stdio.h>
#include "app_installer_io.h"

typedef struct
{
    storage_volume_t volume;
    const char *root, *directory, *staging, *removing, *backup, *journal;
} install_paths_t;
static const install_paths_t paths[STORAGE_COUNT] = {
    {STORAGE_FLASH, STORAGE_FLASH_ROOT, STORAGE_FLASH_APPS,
     "/flash/.app-install", "/flash/.app-remove", "/flash/.app-old", "/flash/system/app-transaction.bin"},
    {STORAGE_SD, STORAGE_SD_ROOT "/.epd", STORAGE_SD_APPS,
     "/sdcard/.epd/.app-install", "/sdcard/.epd/.app-remove", "/sdcard/.epd/.app-old",
     "/sdcard/.epd/app-transaction.bin"}
};
#define COPY_SIZE EPD_PACKAGE_IO_SIZE
#define PACKAGE_DEPTH_MAX 8

static app_job_result_t result;
static sleep_guard_owner_t install_hold = {.id = "app-installer"};
static sleep_guard_owner_t cleanup_hold = {.id = "app-cleanup"};

const char *app_installer_message(app_job_message_t message)
{
    switch (message)
    {
    case APP_JOB_MESSAGE_FAILED: return epd_get_str(platform_install_failed);
    case APP_JOB_MESSAGE_BUSY: return epd_get_str(platform_install_busy);
    case APP_JOB_MESSAGE_CARD_CHANGED: return epd_get_str(platform_install_card_changed);
    case APP_JOB_MESSAGE_CLEANUP_FAILED: return epd_get_str(platform_install_cleanup_failed);
    case APP_JOB_MESSAGE_REMOVED: return epd_get_str(platform_install_removed);
    case APP_JOB_MESSAGE_REMOVE_FAILED: return epd_get_str(platform_install_remove_failed);
    case APP_JOB_MESSAGE_INSERT_CARD: return epd_get_str(platform_install_insert_card);
    case APP_JOB_MESSAGE_BAD_PACKAGE: return epd_get_str(platform_install_bad_package);
    case APP_JOB_MESSAGE_INCOMPATIBLE: return epd_get_str(platform_install_incompatible);
    case APP_JOB_MESSAGE_VERIFY_FAILED: return epd_get_str(platform_install_verify_failed);
    case APP_JOB_MESSAGE_KIND_CHANGED: return epd_get_str(platform_install_kind_changed);
    case APP_JOB_MESSAGE_ALREADY_INSTALLED: return epd_get_str(platform_install_already_installed);
    case APP_JOB_MESSAGE_DATA_INCOMPATIBLE: return epd_get_str(platform_install_data_incompatible);
    case APP_JOB_MESSAGE_READ_FAILED: return epd_get_str(platform_install_read_failed);
    case APP_JOB_MESSAGE_CARD_FULL: return epd_get_str(platform_install_card_full);
    case APP_JOB_MESSAGE_FLASH_FULL: return epd_get_str(platform_install_flash_full);
    case APP_JOB_MESSAGE_NO_MEMORY: return epd_get_str(platform_install_no_memory);
    case APP_JOB_MESSAGE_MIGRATION_FAILED: return epd_get_str(platform_install_migration_failed);
    case APP_JOB_MESSAGE_UPDATED: return epd_get_str(platform_install_updated);
    case APP_JOB_MESSAGE_INSTALLED: return epd_get_str(platform_install_installed);
    case APP_JOB_MESSAGE_INSTALL_FAILED: return epd_get_str(platform_install_install_failed);
    default: return "";
    }
}
const char *app_installer_phase_name(app_job_phase_t phase)
{
    switch (phase)
    {
    case APP_JOB_PHASE_CHECKING: return epd_get_str(platform_install_checking);
    case APP_JOB_PHASE_VERIFYING: return epd_get_str(platform_install_verifying);
    case APP_JOB_PHASE_COPYING: return epd_get_str(platform_install_copying);
    case APP_JOB_PHASE_PUBLISHING: return epd_get_str(platform_install_publishing);
    case APP_JOB_PHASE_MIGRATING: return epd_get_str(platform_install_migrating);
    case APP_JOB_PHASE_REMOVING: return epd_get_str(platform_install_removing);
    case APP_JOB_PHASE_CLEANUP: return epd_get_str(platform_install_cleanup);
    default: return epd_get_str(platform_install_preparing);
    }
}

static void progress_set(app_job_phase_t phase, uint64_t completed, uint64_t total)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (result.state == APP_JOB_RUNNING)
    {
        result.phase = phase;
        result.completed_bytes = completed;
        result.total_bytes = total;
    }
    rt_hw_interrupt_enable(level);
}

static void progress_add(size_t bytes)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (result.state == APP_JOB_RUNNING) result.completed_bytes += bytes;
    rt_hw_interrupt_enable(level);
}

static void extract_progress(uint64_t completed, uint64_t total, void *user)
{
    (void)user;
    progress_set(APP_JOB_PHASE_COPYING, completed, total);
}

static char job_id[GUI_APP_ID_MAX_LEN];
static char job_source[APP_SAP_PATH_MAX];
static bool remove_job;
static storage_volume_t job_volume;
static uint32_t job_session;
static storage_volume_t job_source_volume;
static volatile bool cleanup_running;
static bool cleanup_pending;

void app_installer_result(app_job_result_t *out)
{
    rt_base_t level = rt_hw_interrupt_disable();
    *out = result;
    rt_hw_interrupt_enable(level);
}
bool app_installer_busy(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool busy = result.state == APP_JOB_RUNNING || cleanup_running;
    rt_hw_interrupt_enable(level);
    return busy;
}

bool app_installer_volume_preparing(storage_volume_t volume)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool preparing = volume == STORAGE_SD && (cleanup_pending || cleanup_running);
    rt_hw_interrupt_enable(level);
    return preparing;
}

static bool join_path(char *out, size_t size, const char *base, const char *name)
{
    int n = snprintf(out, size, "%s/%s", base, name);
    return n > 0 && n < size;
}

/* One buffer is shared through recursion; paths and DIR handles are bounded. */
static bool copy_file(const char *from, const char *to, void *buffer)
{
    uint32_t revision = storage_path_revision(from);
    if (storage_file_writers(from)) return false;
    int src = installer_open(from, O_RDONLY), dst = -1;
    if (src < 0) return false;
    dst = installer_open(to, O_CREAT | O_EXCL | O_WRONLY, 0);
    struct stat st;
    bool ok = dst >= 0 && installer_fstat(src, &st) == 0;
    uint64_t copied = 0;
    int n = 0;
    while (ok && storage_path_available(from) && storage_path_available(to) &&
           !storage_release_requested() && (n = installer_read(src, buffer, COPY_SIZE)) > 0)
    {
        if (storage_path_revision(from) != revision || storage_file_writers(from)) { ok = false; break; }
        int offset = 0;
        while (offset < n)
        {
            int written = installer_write(dst, (char *)buffer + offset, n - offset);
            if (written <= 0) { ok = false; break; }
            offset += written;
            copied += written;
            progress_add(written);
        }
    }
    if (n < 0 || !storage_path_available(from) || !storage_path_available(to) || storage_release_requested()) ok = false;
    if (storage_path_revision(from) != revision || storage_file_writers(from)) ok = false;
    if (ok && copied != (uint64_t)st.st_size) ok = false;
    if (ok && installer_fsync(dst) != 0) ok = false;
    if (dst >= 0 && installer_close(dst) != 0) ok = false;
    if (installer_close(src) != 0) ok = false;
    return ok;
}

typedef enum { TREE_SIZE, TREE_COPY, TREE_REMOVE, TREE_CLEAR } tree_operation_t;
typedef struct { uint64_t allocated, payload; } tree_size_t;
static bool tree_walk(const char *from, const char *to, unsigned depth,
                      tree_operation_t operation, uint32_t block_size, tree_size_t *bytes, void *buffer)
{
    bool removing = operation == TREE_REMOVE || operation == TREE_CLEAR;
    if (depth > PACKAGE_DEPTH_MAX || !storage_path_available(from)) return false;
    if (!removing && storage_release_requested()) return false;
    struct stat st;
    if (installer_stat(from, &st) != 0) return false;
    if (S_ISREG(st.st_mode))
    {
        if (operation == TREE_REMOVE) return installer_unlink(from) == 0;
        if (operation == TREE_CLEAR) return false;
        if (st.st_size < 0) return false;
        bytes->allocated += ((uint64_t)st.st_size + block_size - 1) / block_size * block_size;
        bytes->payload += st.st_size;
        if (operation == TREE_COPY) return copy_file(from, to, buffer);
        return true;
    }
    if (!S_ISDIR(st.st_mode)) return false;
    if (operation == TREE_COPY && (depth ? installer_mkdir(to, 0) != 0 : !installer_mkdirs(to))) return false;
    if (!removing) bytes->allocated += block_size;
    DIR *dir = installer_opendir(from);
    if (!dir) return false;
    bool ok = true;
    struct dirent *entry;
    while (ok)
    {
        errno = 0;
        entry = installer_readdir(dir);
        if (!entry) { ok = errno == 0; break; }
        if (!storage_path_available(from)) { ok = false; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char source[256], destination[256];
        ok = join_path(source, sizeof(source), from, entry->d_name);
        if (operation == TREE_COPY) ok = ok && join_path(destination, sizeof(destination), to, entry->d_name);
        if (ok) ok = tree_walk(source, operation == TREE_COPY ? destination : NULL,
                               depth + 1, operation == TREE_CLEAR ? TREE_REMOVE : operation, block_size, bytes, buffer);
    }
    if (installer_closedir(dir) != 0 || !storage_path_available(from)) ok = false;
    if (ok && operation == TREE_REMOVE) ok = installer_rmdir(from) == 0;
    return ok;
}

static bool clear_stage(const char *path)
{
    struct stat st;
    if (installer_stat(path, &st) != 0) return errno == -ENOENT;
    return tree_walk(path, NULL, 0, TREE_REMOVE, 0, NULL, NULL);
}

static struct rt_semaphore prepared;
static bool volume_ready[STORAGE_COUNT], needs_prepare, prepare_ok, exit_sent;
static uint32_t ready_session;
static bool prepared_resources;
static rt_tick_t prepare_started;

static bool volume_ready_set(storage_volume_t volume, bool ready)
{
    rt_base_t level = rt_hw_interrupt_disable();
    volume_ready[volume] = ready;
    if (volume == STORAGE_SD && ready) ready_session = storage_card_session();
    rt_hw_interrupt_enable(level);
    return ready;
}

static int path_state(const char *path)
{
    struct stat st;
    if (installer_stat(path, &st) == 0) return 1;
    return errno == -ENOENT ? 0 : -1;
}

static bool valid_id(const char *id)
{
    size_t n = strlen(id);
    if (!n || n >= RT_NAME_MAX) return false;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

static bool restore_backup(const install_paths_t *p)
{
    int state = path_state(p->backup);
    if (state <= 0) return state == 0;
    char id[GUI_APP_ID_MAX_LEN] = {0}, destination[96];
    int fd = installer_open(p->journal, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    bool ok = installer_fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < sizeof(id);
    if (ok) ok = installer_read(fd, id, (size_t)st.st_size) == st.st_size;
    installer_close(fd);
    if (!ok || !valid_id(id) || strlen(id) != (size_t)st.st_size ||
        !join_path(destination, sizeof(destination), p->directory, id)) return false;
    state = path_state(destination);
    if (state == 0) return installer_rename(p->backup, destination) == 0;
    return state > 0 && clear_stage(p->backup);
}

/* Restore the old installation if publication stopped between the two renames. */
static bool discard_interrupted(const install_paths_t *p)
{
    if (!restore_backup(p) || !clear_stage(p->removing)) return false;
    /* Unpublished files must not prevent other apps from running or uninstalling. */
    if (!clear_stage(p->staging)) rt_kprintf("[apps] package staging cleanup incomplete\n");
    const char *suffixes[] = {".old", ".tmp", ""};
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i)
    {
        char path[128];
        snprintf(path, sizeof(path), "%s%s", p->journal, suffixes[i]);
        if (!clear_stage(path)) return false;
    }
    return true;
}

static bool record_update(const install_paths_t *p)
{
    char parent[128];
    snprintf(parent, sizeof(parent), "%s", p->journal);
    char *slash = strrchr(parent, '/');
    if (!slash) return false;
    *slash = 0;
    if (!installer_mkdirs(parent)) return false;
    int fd = installer_open(p->journal, O_WRONLY | O_CREAT | O_EXCL, 0);
    if (fd < 0) return false;
    size_t length = strlen(job_id);
    bool ok = installer_write(fd, job_id, length) == length && installer_fsync(fd) == 0;
    if (installer_close(fd) != 0) ok = false;
    return ok;
}

static bool same_release(const epd_app_entry_t *a, const epd_app_entry_t *b)
{
    return !strcmp(a->id, b->id) && a->version == b->version && a->data_version == b->data_version &&
           a->resources_only == b->resources_only && !strcmp(a->build_id, b->build_id);
}

static bool migrate_data(const char *id, storage_volume_t volume)
{
    char source[96], target[96], stage[96], other_app[96];
    const install_paths_t *to = &paths[volume], *from = &paths[volume == STORAGE_SD ? STORAGE_FLASH : STORAGE_SD];
    if (!storage_available(volume)) return false;
    snprintf(target, sizeof(target), "%s/data/%s", to->root, id);
    snprintf(source, sizeof(source), "%s/data/%s", from->root, id);
    snprintf(other_app, sizeof(other_app), "%s/%s", from->directory, id);
    int installed = path_state(target);
    if (installed != 0) return installed > 0 && installer_mkdirs(target);
    snprintf(stage, sizeof(stage), "%s/.data-%s", to->root, id);
    int pending = path_state(stage);
    if (pending < 0) return false;
    if (volume == STORAGE_FLASH && !strcmp(id, "books"))
    {
        int old = path_state("/flash/data/reader");
        if (old != 0) return old > 0 && installer_rename("/flash/data/reader", target) == 0;
    }
    uint32_t session = storage_card_session();
    if (!storage_available(from->volume)) return !pending && installer_mkdirs(target);
    int other = path_state(other_app);
    if (other < 0) return false;
    int data = path_state(source);
    if (data < 0) return false;
    if (other || !data)
    {
        return !pending && storage_session_valid(from->volume, session) &&
               storage_session_valid(volume, session) && !storage_release_requested() && installer_mkdirs(target);
    }
    int backup = path_state(from->backup);
    if (backup < 0) return false;
    if (backup)
    {
        /* Only the matching transaction can still own this source data. */
        char saved_id[GUI_APP_ID_MAX_LEN] = {0};
        struct stat st;
        int fd = installer_open(from->journal, O_RDONLY);
        if (fd < 0) return false;
        bool valid = installer_fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                     st.st_size > 0 && st.st_size < sizeof(saved_id);
        if (valid) valid = installer_read(fd, saved_id, (size_t)st.st_size) == st.st_size;
        if (installer_close(fd) != 0) valid = false;
        if (!valid || !valid_id(saved_id) || strlen(saved_id) != (size_t)st.st_size || !strcmp(saved_id, id))
            return false;
    }
    /* Keep the confirmed migration visible across failures and retries. */
    if (!installer_mkdirs(stage)) return false;
    if (app_module_loaded(id) || app_service_running(id)) return false;
    char parent[96];
    snprintf(parent, sizeof(parent), "%s/data", to->root);
    if (!installer_mkdirs(parent) || !tree_walk(stage, NULL, 0, TREE_CLEAR, 0, NULL, NULL)) return false;
    tree_size_t required = {0}, copied = {0};
    if (!tree_walk(source, NULL, 0, TREE_SIZE, 1, &required, NULL)) return false;
    progress_set(APP_JOB_PHASE_MIGRATING, 0, required.payload);
    void *buffer = epd_app_alloc(COPY_SIZE, EPD_APP_PSRAM);
    if (!buffer) return false;
    bool ok = tree_walk(source, stage, 0, TREE_COPY, 1, &copied, buffer) &&
              copied.payload == required.payload && storage_session_valid(from->volume, session) &&
              storage_session_valid(volume, session) && !storage_release_requested() && installer_rename(stage, target) == 0;
    epd_app_free(buffer);
    if (!ok) return false;
    if (!clear_stage(source)) rt_kprintf("[apps] %s: old data cleanup incomplete\n", id);
    return true;
}

/* A pending migration blocks only its app; an unavailable source alone does not. */
static bool data_ready(const char *id, storage_volume_t volume)
{
    char path[96];
    struct stat st;
    const install_paths_t *to = &paths[volume], *from = &paths[volume == STORAGE_SD ? STORAGE_FLASH : STORAGE_SD];
    if (!storage_available(volume)) return false;
    snprintf(path, sizeof(path), "%s/data/%s", to->root, id);
    int target = path_state(path);
    if (target != 0) return target > 0 && installer_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    snprintf(path, sizeof(path), "%s/.data-%s", to->root, id);
    if (path_state(path) != 0) return false;
    if (volume == STORAGE_FLASH && !strcmp(id, "books") && path_state("/flash/data/reader") != 0)
        return false;
    if (!storage_available(from->volume)) return true;
    snprintf(path, sizeof(path), "%s/%s", from->directory, id);
    int other = path_state(path);
    if (other != 0) return other > 0;
    snprintf(path, sizeof(path), "%s/data/%s", from->root, id);
    return path_state(path) == 0;
}

static bool prepare_data(storage_volume_t volume)
{
    if (volume == STORAGE_SD && !storage_apps_enabled(volume)) return true;
    const char *root = storage_app_directory(volume);
    DIR *dir = installer_opendir(root);
    if (!dir) return path_state(root) == 0;
    bool ok = true;
    struct dirent *item;
    while (ok && (item = installer_readdir(dir)) != NULL)
    {
        if (!valid_id(item->d_name)) continue;
        char metadata[128];
        int length = snprintf(metadata, sizeof(metadata), "%s/%s/app.json", root, item->d_name);
        if (length < 0 || (size_t)length >= sizeof(metadata)) { ok = false; break; }
        if (path_state(metadata) == 1 && !migrate_data(item->d_name, volume))
            rt_kprintf("[apps] %s: data migration incomplete\n", item->d_name);
    }
    installer_closedir(dir);
    return ok;
}

void app_installer_init(void)
{
    rt_sem_init(&prepared, "app_pre", 0, RT_IPC_FLAG_FIFO);
    if (!sleep_guard_acquire(&cleanup_hold)) return;
    installer_session = storage_card_session();
    installer_io_active = true;
    for (storage_volume_t v = STORAGE_FLASH; v < STORAGE_COUNT; ++v)
        volume_ready_set(v, storage_available(v) && discard_interrupted(&paths[v]) &&
                           (v != STORAGE_FLASH || prepare_data(v)));
    installer_io_active = false;
    sleep_guard_release(&cleanup_hold);
    if (!volume_ready[STORAGE_FLASH]) rt_kprintf("[apps] internal temporary file cleanup incomplete\n");
}

bool app_installer_volume_ready(storage_volume_t volume)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = (unsigned)volume < STORAGE_COUNT && storage_available(volume) && volume_ready[volume] &&
                 (volume != STORAGE_SD || storage_session_valid(volume, ready_session));
    rt_hw_interrupt_enable(level);
    return ready;
}

static void prepare_card(void *parameter)
{
    (void)parameter;
    uint32_t session = storage_card_session();
    installer_session = session;
    installer_io_active = true;
    bool ready = volume_ready_set(STORAGE_SD, discard_interrupted(&paths[STORAGE_SD]) &&
                           prepare_data(STORAGE_SD) && storage_session_valid(STORAGE_SD, session));
    if (ready && app_installer_volume_ready(STORAGE_FLASH)) prepare_data(STORAGE_FLASH);
    installer_io_active = false;
    if (!ready && storage_available(STORAGE_SD)) rt_kprintf("[apps] TF temporary file cleanup incomplete\n");
    app_catalog_changed();
    rt_base_t level = rt_hw_interrupt_disable();
    cleanup_running = false;
    sleep_guard_release(&cleanup_hold);
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

void app_installer_storage_changed(void)
{
    volume_ready_set(STORAGE_SD, false);
    cleanup_pending = storage_available(STORAGE_SD);
    epd_power_notify();
}

static void start_card_prepare(void)
{
    if (system_reset_blocks_apps() || !cleanup_pending || app_installer_busy() || storage_changing()) return;
    if (!storage_available(STORAGE_SD)) { cleanup_pending = false; return; }
    if (!sleep_guard_acquire(&cleanup_hold)) return;
    rt_thread_t thread = epd_thread_create_psram("appclean", prepare_card, NULL, 12288, 23, 10);
    if (!thread) { cleanup_pending = false; sleep_guard_release(&cleanup_hold); return; }
    cleanup_running = true;
    cleanup_pending = false;
    if (rt_thread_startup(thread) != RT_EOK)
    {
        cleanup_running = false; rt_thread_delete(thread);
        sleep_guard_release(&cleanup_hold);
    }
}

bool app_installer_blocks(const char *id)
{
    uint32_t session = storage_card_session();
    storage_volume_t volume;
    if (storage_release_requested() || storage_changing() || !storage_app_locate(id, &volume) || !app_installer_volume_ready(volume) ||
        !storage_apps_enabled(volume)) return true;
    rt_base_t level = rt_hw_interrupt_disable();
    bool blocked = (result.state == APP_JOB_RUNNING && !strcmp(id, job_id));
    rt_hw_interrupt_enable(level);
    if (!blocked)
    {
        storage_lock();
        blocked = storage_release_requested() || storage_changing() ||
                  !storage_session_valid(volume, session) || !app_installer_volume_ready(volume) ||
                  !storage_apps_enabled(volume) || !data_ready(id, volume);
        storage_unlock();
    }
    return blocked;
}

/* Only the UI thread calls app_fwk. The installer worker never executes a page
 * callback or waits for a service while holding the storage mutex. */
void app_installer_process(void)
{
    start_card_prepare();
    if (!needs_prepare) return;
    if (!exit_sent)
    {
        gui_app_exit(job_id);
        gui_app_exec_now();
        if (!prepared_resources) app_service_stop(job_id);
        exit_sent = true;
    }
    rt_list_t *cursor = NULL;
    gui_runing_app_t *app;
    bool alive = false;
    while ((app = gui_app_trav(&cursor)) != NULL)
        if (!strcmp(app->id, job_id)) alive = true;
    alive = alive || (!prepared_resources && (app_service_running(job_id) || app_module_loaded(job_id)));
    if (alive && (rt_tick_t)(rt_tick_get() - prepare_started) < rt_tick_from_millisecond(35000)) return;
    prepare_ok = !alive;
    needs_prepare = false;
    rt_sem_release(&prepared);
}

static void worker(void *parameter)
{
    (void)parameter;
    const install_paths_t *p = &paths[job_volume];
    char source[APP_SAP_PATH_MAX], destination[96], cache[96];
    app_job_message_t message = APP_JOB_MESSAGE_FAILED;
    bool ok = false, updating = false, publishing = false;
    char manifest_path[128];
    epd_app_entry_t old = {0};
    rt_sem_take(&prepared, RT_WAITING_FOREVER);
    if (!prepare_ok) { message = APP_JOB_MESSAGE_BUSY; goto finish_unlocked; }
    /* Persist revocation before deleting the installation. The settings worker
     * must be able to take storage_lock while this worker waits for its result. */
    if (remove_job && (ai_settings_revoke_app_sync(job_id) != EPD_AI_OK || !reminder_remove_app(job_id)))
    { message = APP_JOB_MESSAGE_REMOVE_FAILED; goto finish_unlocked; }
    installer_session = job_session;
    installer_io_active = true;
    progress_set(APP_JOB_PHASE_CHECKING, 0, 0);
    if (!storage_session_valid(p->volume, job_session) || storage_release_requested() ||
        (!remove_job && !storage_session_valid(job_source_volume, job_session)))
    { message = APP_JOB_MESSAGE_CARD_CHANGED; goto finish; }
    if (!volume_ready_set(p->volume, discard_interrupted(p)))
    { message = APP_JOB_MESSAGE_CLEANUP_FAILED; goto finish; }
    snprintf(destination, sizeof(destination), "%s/%s", p->directory, job_id);
    snprintf(manifest_path, sizeof(manifest_path), "%s/app.json", destination);
    snprintf(cache, sizeof(cache), "%s/cache/%s", p->root, job_id);
    if (remove_job)
    {
        progress_set(APP_JOB_PHASE_REMOVING, 0, 0);
        epd_app_entry_t entry;
        if (!app_package_read(p->directory, job_id, &entry)) goto finish;
        storage_lock();
        ok = installer_io_ready() && !app_module_loaded(job_id) && !app_service_running(job_id) &&
             installer_rename(destination, p->removing) == 0;
        storage_unlock();
        if (ok) ok = clear_stage(p->removing) && clear_stage(cache);
        message = ok ? APP_JOB_MESSAGE_REMOVED : APP_JOB_MESSAGE_REMOVE_FAILED;
        goto finish;
    }
    epd_app_entry_t entry;
    snprintf(source, sizeof(source), "%s", job_source);
    if (storage_file_writers(source) || storage_file_writers(manifest_path)) goto finish;
    app_package_status_t package_status = app_sap_metadata(source, &entry);
    if (package_status != APP_PACKAGE_OK || strcmp(entry.id, job_id))
    { message = APP_JOB_MESSAGE_BAD_PACKAGE; goto finish; }
    int installed = path_state(destination);
    if (installed < 0) goto finish;
    updating = installed != 0;
    if (updating)
    {
        if (!app_package_metadata(destination, job_id, &old)) goto finish;
        bool weather_migration = !strcmp(job_id, "weather") && old.resources_only &&
                                 !entry.resources_only && old.version == 1 && entry.version >= 2;
        if (old.resources_only != entry.resources_only && !weather_migration)
        { message = APP_JOB_MESSAGE_KIND_CHANGED; goto finish; }
        if (entry.version <= old.version)
        { message = APP_JOB_MESSAGE_ALREADY_INSTALLED; goto finish; }
        if (old.data_version && entry.data_version != old.data_version)
        { message = APP_JOB_MESSAGE_DATA_INCOMPATIBLE; goto finish; }
    }
    struct statfs fs;
    if (installer_statfs(storage_root(p->volume), &fs) != 0 || !fs.f_bsize) goto finish;
    tree_size_t required = {0};
    if (app_sap_inspect(source, fs.f_bsize, &required.allocated, &required.payload) != APP_PACKAGE_OK)
    { message = APP_JOB_MESSAGE_READ_FAILED; goto finish; }
    if (required.allocated + (uint64_t)fs.f_bsize * 4 > (uint64_t)fs.f_bfree * fs.f_bsize)
    { message = p->volume == STORAGE_SD ? APP_JOB_MESSAGE_CARD_FULL : APP_JOB_MESSAGE_FLASH_FULL; goto finish; }
    if (path_state(p->root) == 0 && installer_mkdir(p->root, 0) != 0) goto finish;
    if (path_state(p->directory) == 0 && installer_mkdir(p->directory, 0) != 0) goto finish;
    progress_set(APP_JOB_PHASE_COPYING, 0, required.payload);
    if (!clear_stage(p->staging)) { message = APP_JOB_MESSAGE_CLEANUP_FAILED; goto finish; }
    package_status = app_sap_install(source, p->staging, job_id, extract_progress, NULL);
    if (package_status != APP_PACKAGE_OK)
    {
        message = package_status == APP_PACKAGE_INCOMPATIBLE ? APP_JOB_MESSAGE_INCOMPATIBLE : APP_JOB_MESSAGE_VERIFY_FAILED;
        goto finish;
    }
    if (!storage_session_valid(job_source_volume, job_session) ||
        !storage_session_valid(p->volume, job_session) || storage_release_requested())
    { message = APP_JOB_MESSAGE_CARD_CHANGED; goto finish; }
    if (!migrate_data(job_id, p->volume))
    { message = APP_JOB_MESSAGE_MIGRATION_FAILED; goto finish; }
    if (!storage_session_valid(job_source_volume, job_session) ||
        !storage_session_valid(p->volume, job_session) || storage_release_requested())
    { message = APP_JOB_MESSAGE_CARD_CHANGED; goto finish; }
    progress_set(APP_JOB_PHASE_PUBLISHING, 0, 0);
    storage_lock();
    publishing = true;
    if (!installer_io_ready() || !storage_session_valid(p->volume, job_session) ||
        !storage_session_valid(job_source_volume, job_session))
    { message = APP_JOB_MESSAGE_CARD_CHANGED; goto finish; }
    if (app_module_loaded(job_id) || app_service_running(job_id))
    { message = APP_JOB_MESSAGE_BUSY; goto finish; }
    epd_app_entry_t final_entry;
    /* Path tokens may be evicted during staging. Compare release metadata
     * here rather than treating those transient tokens as persisted identity. */
    if (storage_file_writers(source) || storage_file_writers(manifest_path) ||
        !app_package_metadata(p->staging, job_id, &final_entry) || !same_release(&entry, &final_entry) ||
        (updating && (!app_package_metadata(destination, job_id, &final_entry) || !same_release(&old, &final_entry))) ||
        path_state(destination) != (updating ? 1 : 0))
    { message = APP_JOB_MESSAGE_INSTALL_FAILED; goto finish; }
    if (updating && !record_update(p)) goto finish;
    if (updating && installer_rename(destination, p->backup) != 0) goto finish;
    if (installer_rename(p->staging, destination) != 0)
    {
        /* A synchronous publication failure can return the old directory immediately. */
        if (updating && installer_rename(p->backup, destination) != 0) volume_ready_set(p->volume, false);
        message = APP_JOB_MESSAGE_INSTALL_FAILED;
        goto finish;
    }
    storage_unlock();
    publishing = false;
    progress_set(APP_JOB_PHASE_CLEANUP, 0, 0);
    if (!clear_stage(p->backup) || !clear_stage(p->journal) || !clear_stage(cache))
    { message = APP_JOB_MESSAGE_CLEANUP_FAILED; goto finish; }
    ok = true;
    message = updating ? APP_JOB_MESSAGE_UPDATED : APP_JOB_MESSAGE_INSTALLED;
finish:
    if (publishing) storage_unlock();
    if (!ok && storage_session_valid(p->volume, job_session) && !clear_stage(p->staging))
        rt_kprintf("[apps] package staging cleanup incomplete\n");
    installer_io_active = false;
finish_unlocked:
    /* Also restart a previously stopped background service after a rejected update. */
    app_catalog_changed();
    rt_base_t level = rt_hw_interrupt_disable();
    result.message = message;
    result.state = ok ? APP_JOB_DONE : APP_JOB_FAILED;
    sleep_guard_release(&install_hold);
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

static bool source_inside(const char *source, const char *directory);

static bool start(const char *id, const char *source, bool uninstall, storage_volume_t volume, uint32_t *ticket)
{
    if (system_reset_blocks_apps()) return false;
    if ((uninstall && (!id || !valid_id(id))) || (!uninstall && !source)) return false;
    if (app_installer_busy() || !ticket || (unsigned)volume >= STORAGE_COUNT ||
        !app_installer_volume_ready(volume) || storage_release_requested() || storage_changing()) return false;
    if (!sleep_guard_acquire(&install_hold)) return false;
    if (uninstall && ai_settings_start() != EPD_AI_OK)
    { sleep_guard_release(&install_hold); return false; }
    epd_app_entry_t entry;
    uint32_t session = storage_card_session();
    bool found = uninstall ? app_package_read(storage_app_directory(volume), id, &entry) :
        app_sap_metadata(source, &entry) == APP_PACKAGE_OK;
    if (source && found) id = entry.id;
    storage_volume_t existing;
    storage_lock();
    bool installed = found && storage_app_locate(id, &existing);
    /* One app ID has one active installation; updating preserves its volume. */
    found = found && (uninstall || !installed || existing == volume) &&
            storage_session_valid(volume, session) && !storage_release_requested();
    storage_unlock();
    if (source && found)
    {
        char destination[96], cache[96];
        snprintf(destination, sizeof(destination), "%s/%s", paths[volume].directory, id);
        snprintf(cache, sizeof(cache), "%s/cache/%s", paths[volume].root, id);
        found = !source_inside(source, destination) && !source_inside(source, cache);
    }
    if (!found) { sleep_guard_release(&install_hold); return false; }
    rt_thread_t thread = epd_thread_create_psram("appcopy", worker, NULL, 12288, 23, 10);
    if (!thread) { sleep_guard_release(&install_hold); return false; }
    snprintf(job_id, sizeof(job_id), "%s", id);
    snprintf(job_source, sizeof(job_source), "%s", source ? source : "");
    remove_job = uninstall;
    job_volume = volume;
    job_session = session;
    job_source_volume = source && !strcmp(storage_path_root(source), STORAGE_FLASH_ROOT) ? STORAGE_FLASH : STORAGE_SD;
    prepared_resources = entry.resources_only;
    exit_sent = prepare_ok = false;
    prepare_started = rt_tick_get();
    needs_prepare = true;
    rt_base_t level = rt_hw_interrupt_disable();
    result.state = APP_JOB_RUNNING;
    result.message = APP_JOB_MESSAGE_NONE;
    result.phase = APP_JOB_PHASE_PREPARING;
    result.completed_bytes = result.total_bytes = 0;
    *ticket = ++result.ticket;
    rt_hw_interrupt_enable(level);
    if (rt_thread_startup(thread) == RT_EOK) { epd_power_notify(); return true; }
    needs_prepare = false;
    result.state = APP_JOB_FAILED;
    rt_thread_delete(thread);
    sleep_guard_release(&install_hold);
    return false;
}

bool app_installer_start_uninstall(const char *id, storage_volume_t volume, uint32_t *ticket)
{
    return start(id, NULL, true, volume, ticket);
}

static bool source_inside(const char *source, const char *directory)
{
    while (*directory)
    {
        char a = *source++, b = *directory++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return !*source || *source == '/';
}

bool app_installer_start_package(const char *path, storage_volume_t volume, uint32_t *ticket)
{
    if (!path || strlen(path) >= APP_SAP_PATH_MAX) return false;
    size_t length = strlen(path);
    if (length < 4 || path[length - 4] != '.' ||
        (path[length - 3] != 's' && path[length - 3] != 'S') ||
        (path[length - 2] != 'a' && path[length - 2] != 'A') ||
        (path[length - 1] != 'p' && path[length - 1] != 'P')) return false;
    const char *root = storage_path_root(path);
    size_t root_length = strlen(root);
    if (strncmp(path, root, root_length) || path[root_length] != '/') return false;
    for (storage_volume_t v = STORAGE_FLASH; v < STORAGE_COUNT; ++v)
    {
        const char *private_roots[] = {paths[v].staging, paths[v].removing, paths[v].backup};
        for (unsigned i = 0; i < sizeof(private_roots) / sizeof(private_roots[0]); ++i)
        {
            if (source_inside(path, private_roots[i])) return false;
        }
    }
    const char *part = path + root_length + 1;
    for (const char *p = part; ; ++p)
    {
        if (*p && ((unsigned char)*p < 32 || (unsigned char)*p == 127 || strchr("\\:<>\"|?*", *p))) return false;
        if (*p == '/' || !*p)
        {
            size_t n = p - part;
            if (!n || p[-1] == '.' || p[-1] == ' ') return false;
            if (!*p) break;
            part = p + 1;
        }
    }
    return start(NULL, path, false, volume, ticket);
}
