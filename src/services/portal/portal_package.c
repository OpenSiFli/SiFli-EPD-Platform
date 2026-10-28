#include "storage_internal.h"
#include "storage_file_internal.h"
#include "portal_package.h"
#include "epd_memory.h"
#include "portal_protocol.h"
#include "storage_file.h"
#include "system_reset_guard.h"
#include "usb_card_reader.h"
#include "boards/board_service.h"
#include "platform/app_installer.h"
#include "platform/app_sap.h"
#include "sleep_guard.h"
#include <dfs_posix.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef enum { EMPTY, UPLOADING, READY, QUEUED, INSTALLING, DONE, FAILED } package_state_t;
#define CLEANUP_ATTEMPTS 3
#define RESULT_HISTORY 8
typedef enum { CLEANUP_OK, CLEANUP_IO, CLEANUP_RESOURCE, CLEANUP_STORAGE, CLEANUP_SESSION } cleanup_error_t;
static struct rt_mutex package_lock;
static bool initialized;
static sleep_guard_owner_t cleanup_hold = {.id = "portal-package-cleanup"};
#define LOCK() rt_mutex_take(&package_lock, RT_WAITING_FOREVER)
#define UNLOCK() rt_mutex_release(&package_lock)
typedef struct
{
    package_state_t state;
    bool blocked, cleanup, cleaning, closing;
    int fd;
    char id[PORTAL_PACKAGE_ID_SIZE], path[PORTAL_PACKAGE_PATH_MAX];
    char message[256], phase[32];
    storage_volume_t volume;
    uint32_t session, ticket;
    uint64_t length, uploaded, completed, total;
    rt_tick_t ready_at, cleanup_at;
    unsigned cleanup_attempts;
    cleanup_error_t cleanup_error;
    int cleanup_errno;
} package_t;
static package_t package = { .fd = -1 };
static package_t history[RESULT_HISTORY];
static unsigned history_next;

bool portal_package_init(void)
{
    if (!initialized) initialized = rt_mutex_init(&package_lock, "websap", RT_IPC_FLAG_PRIO) == RT_EOK;
    return initialized;
}
static bool valid_id(const char *id)
{
    if (!id || strlen(id) != PORTAL_PACKAGE_ID_SIZE - 1) return false;
    for (const char *p = id; *p; ++p)
        if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f')) return false;
    return true;
}
static bool active(void)
{ return package.state >= UPLOADING && package.state <= INSTALLING; }
/* These accessors only read published state. Never wait for the filesystem
 * while holding package_lock: status reads must remain responsive. */
static bool available(void)
{
    return !package.blocked && !storage_changing() && !storage_release_requested() &&
        !board_power_pending() && system_reset_write_allowed() &&
        !(package.volume == STORAGE_SD && usb_card_reader_active()) &&
        storage_session_valid(package.volume, package.session);
}
static void fail(const char *message)
{
    package.state = FAILED;
    snprintf(package.message, sizeof(package.message), "%s", message);
}
static void cleanup_failed(cleanup_error_t error, int code)
{
    package.cleanup = false;
    package.cleanup_error = error;
    package.cleanup_errno = code;
    rt_kprintf("portal: sap retained path=%s volume=%u session=%lu reason=%u errno=%d\n",
        package.path, (unsigned)package.volume, (unsigned long)package.session, (unsigned)error, code);
}
static void cleanup_attempt_failed(cleanup_error_t error, int code)
{
    package.cleanup_at = rt_tick_get();
    if (package.cleanup_attempts >= CLEANUP_ATTEMPTS) cleanup_failed(error, code);
}
/* Only the current reservation may delete its file, within its storage session. */
static void cleanup_worker(void *unused)
{
    (void)unused;
    storage_lock();
    LOCK();
    bool valid = storage_session_valid(package.volume, package.session);
    bool ready = available();
    UNLOCK();
    int code = 0;
    if (ready && unlink(package.path) != 0) code = errno;
    storage_unlock();
    LOCK();
    if (!valid) cleanup_failed(CLEANUP_SESSION, 0);
    else if (!ready) cleanup_failed(CLEANUP_STORAGE, 0);
    else if (!code || code == ENOENT || code == -ENOENT) package.cleanup = false;
    else cleanup_attempt_failed(CLEANUP_IO, code);
    package.cleaning = false; package.cleanup_at = rt_tick_get();
    sleep_guard_release(&cleanup_hold);
    UNLOCK();
}
void portal_package_block(bool blocked)
{ if (initialized) { LOCK(); package.blocked = blocked; UNLOCK(); } }
bool portal_package_storage_idle(void)
{
    if (!initialized) return true;
    LOCK();
    bool idle = !active() && !package.closing && !package.cleaning;
    UNLOCK(); return idle;
}
int portal_package_begin(const char *id, storage_volume_t volume, uint64_t length)
{
    if (!initialized) return 503;
    if (!valid_id(id) || (unsigned)volume >= STORAGE_COUNT || !length) return 400;
    if (length > APP_SAP_SIZE_MAX) return 413;
    LOCK();
    if (!strcmp(id, package.id))
    {
        int status = package.volume == volume && package.length == length ? 200 : 409;
        UNLOCK(); return status;
    }
    for (unsigned i = 0; i < RESULT_HISTORY; ++i)
        if (!strcmp(id, history[i].id))
        {
            int status = history[i].volume == volume && history[i].length == length ? 200 : 409;
            UNLOCK(); return status;
        }
    if (active() || package.cleanup || package.cleaning || package.closing || app_installer_busy()) { UNLOCK(); return 409; }
    storage_volume_t previous_volume = package.volume;
    uint32_t previous_session = package.session;
    package.volume = volume; package.session = storage_card_session();
    if (!available())
    { package.volume = previous_volume; package.session = previous_session; UNLOCK(); return 503; }
    if (package.id[0])
    {
        history[history_next] = package;
        history[history_next].volume = previous_volume;
        history[history_next].session = previous_session;
        history_next = (history_next + 1) % RESULT_HISTORY;
    }
    package.cleanup_attempts = 0; package.cleanup_at = 0;
    package.cleanup_error = CLEANUP_OK; package.cleanup_errno = 0;
    strcpy(package.id, id); package.length = length; package.uploaded = 0;
    package.completed = package.total = 0; package.ticket = 0;
    package.state = UPLOADING; package.message[0] = 0; strcpy(package.phase, "uploading");
    snprintf(package.path, sizeof(package.path), "%s/.portal-sap/%s.sap", storage_root(volume), id);
    UNLOCK();

    storage_lock();
    LOCK(); bool ready = available(); UNLOCK();
    int status = 503, fd = -1;
    if (ready)
    {
        char directory[PORTAL_PACKAGE_PATH_MAX];
        snprintf(directory, sizeof(directory), "%s/.portal-sap", storage_root(volume));
        status = 507;
        /* Existing files, including residues from previous boots, stay untouched. */
        if (storage_mkdirs(directory))
        {
            fd = open(package.path, O_CREAT | O_EXCL | O_WRONLY, 0);
            if (fd < 0 && (errno == EEXIST || errno == -EEXIST)) status = 409;
        }
        if (fd >= 0) status = 201;
    }
    storage_unlock();
    LOCK();
    package.fd = fd;
    if (fd >= 0) package.cleanup = true;
    else fail(status == 503 ? "目标存储不可用。" : status == 409 ?
        "上传路径已有文件，请重新发起上传。" : "无法创建安装包，请检查存储空间。");
    UNLOCK(); return status;
}
int portal_package_write(const void *data, size_t size)
{
    storage_lock();
    LOCK();
    int status = package.state != UPLOADING || !available() ? 503 :
        size > package.length - package.uploaded ? 400 : 200;
    int fd = package.fd;
    UNLOCK();
    size_t completed = 0;
    const unsigned char *p = data;
    while (status == 200 && completed < size)
    {
        int written = write(fd, p + completed, size - completed);
        if (written <= 0) status = 507;
        else completed += (size_t)written;
    }
    storage_unlock();
    LOCK(); package.uploaded += completed; UNLOCK(); return status;
}
int portal_package_finish(char path[PORTAL_PACKAGE_PATH_MAX])
{
    storage_lock();
    LOCK();
    int status = package.state != UPLOADING || package.uploaded != package.length ? 400 : !available() ? 503 : 200;
    int fd = package.fd;
    UNLOCK();
    if (status == 200 && fsync(fd) != 0) status = 507;
    if (fd >= 0 && close(fd) != 0 && status == 200) status = 507;
    storage_unlock();
    LOCK(); package.fd = -1;
    if (status == 200 && !available()) status = 503;
    if (status == 200)
    {
        package.state = READY; package.ready_at = rt_tick_get();
        strcpy(package.phase, "preparing"); strcpy(path, package.path);
    }
    else fail(status == 507 ? "安装包保存失败，请检查存储空间。" : "上传未完成或存储已不可用。");
    UNLOCK(); return status;
}
void portal_package_abort(const char *message)
{
    LOCK();
    int fd = -1;
    if (package.state == UPLOADING || package.state == READY || package.state == QUEUED)
    {
        fd = package.fd; package.fd = -1; package.closing = fd >= 0; fail(message);
    }
    UNLOCK();
    if (fd >= 0)
    {
        storage_lock(); close(fd); storage_unlock();
        LOCK(); package.closing = false; UNLOCK();
    }
}
bool portal_package_queue(const char *path, storage_volume_t volume)
{
    LOCK();
    bool ok = package.state == READY && package.volume == volume && !strcmp(package.path, path) && available();
    if (ok) package.state = QUEUED;
    UNLOCK(); return ok;
}
bool portal_package_start(const char *path, storage_volume_t volume, uint32_t *ticket)
{
    LOCK();
    bool owned = package.state == QUEUED && volume == package.volume && !strcmp(package.path, path);
    bool ready = available();
    if (owned) package.state = INSTALLING;
    UNLOCK();
    if (!owned) return false;
    const char *error = NULL;
    epd_app_entry_t entry;
    storage_volume_t existing;
    if (!ready) error = "安装未开始：目标存储不可用或正在交给读卡器。";
    else if (app_installer_busy()) error = "安装未开始：设备正在安装或卸载其他应用。";
    else if (!app_installer_volume_ready(volume)) error = "安装未开始：目标存储尚未就绪。";
    else
    {
        storage_lock();
        app_package_status_t status = app_sap_metadata(path, &entry);
        if (status != APP_PACKAGE_OK) error = app_package_error(status);
        else if (storage_app_locate(entry.id, &existing) && existing != volume)
            error = "此应用已安装在另一存储位置。请在原位置更新，或先卸载再安装。";
        storage_unlock();
    }
    bool started = !error && app_installer_start_package(path, volume, ticket);
    LOCK();
    if (started) package.ticket = *ticket;
    else fail(error ? error : "安装未开始：存储状态发生变化，或设备没有足够资源启动安装。");
    UNLOCK(); return started;
}
void portal_package_process(void)
{
    if (!initialized) return;
    app_job_result_t result; app_installer_result(&result);
    if (rt_mutex_take(&package_lock, 0) != RT_EOK) return;
    if (package.state == INSTALLING && package.ticket)
    {
        if (result.ticket != package.ticket) fail("安装任务已变更，无法取得原任务结果。");
        else
        {
            snprintf(package.phase, sizeof(package.phase), "%s", app_installer_phase_name(result.phase));
            package.completed = result.completed_bytes; package.total = result.total_bytes;
            if (result.state != APP_JOB_RUNNING)
            {
                package.state = result.state == APP_JOB_DONE ? DONE : FAILED;
                snprintf(package.message, sizeof(package.message), "%s", app_installer_message(result.message));
            }
        }
    }
    if (package.state == READY && (rt_tick_t)(rt_tick_get() - package.ready_at) > rt_tick_from_millisecond(30000))
        fail("上传后的安装请求未能提交，请重新上传。");
    bool clean = package.cleanup && !active() && !package.cleaning && !package.closing;
    if (clean && !available())
    {
        cleanup_failed(storage_session_valid(package.volume, package.session) ? CLEANUP_STORAGE : CLEANUP_SESSION, 0);
        clean = false;
    }
    if (clean) clean = !package.cleanup_attempts ||
        (rt_tick_t)(rt_tick_get() - package.cleanup_at) >= rt_tick_from_millisecond(1000);
    if (clean && !sleep_guard_acquire(&cleanup_hold))
    {
        cleanup_failed(CLEANUP_RESOURCE, 0);
        clean = false;
    }
    if (clean) { package.cleaning = true; ++package.cleanup_attempts; }
    UNLOCK();
    if (clean)
    {
        rt_thread_t thread = epd_thread_create_psram("sapclean", cleanup_worker, NULL, 2048, 23, 10);
        if (thread && rt_thread_startup(thread) == RT_EOK) return;
        if (thread) rt_thread_delete(thread);
        LOCK(); package.cleaning = false; cleanup_attempt_failed(CLEANUP_RESOURCE, 0);
        sleep_guard_release(&cleanup_hold); UNLOCK();
    }
}
bool portal_package_result(const char *id, char *buffer, size_t capacity)
{
    if (!initialized) return false;
    LOCK();
    const package_t *job = NULL;
    if (valid_id(id))
    {
        if (!strcmp(package.id, id)) job = &package;
        else for (unsigned i = 0; i < RESULT_HISTORY; ++i)
            if (!strcmp(history[i].id, id)) { job = &history[i]; break; }
    }
    if (!job) { UNLOCK(); return false; }
    portal_json_t j; portal_json_init(&j, buffer, capacity);
    portal_json_raw(&j, "{\"upload\":"); portal_json_string(&j, job->id);
    portal_json_raw(&j, ",\"state\":");
    portal_json_string(&j, job->state == DONE ? "done" : job->state == FAILED ? "failed" : "pending");
    portal_json_raw(&j, ",\"phase\":"); portal_json_string(&j, job->phase);
    portal_json_raw(&j, ",\"message\":"); portal_json_string(&j, job->message);
    portal_json_raw(&j, ",\"completed_bytes\":"); portal_json_number(&j, (int64_t)job->completed);
    portal_json_raw(&j, ",\"total_bytes\":"); portal_json_number(&j, (int64_t)job->total);
    portal_json_raw(&j, ",\"uploaded_bytes\":"); portal_json_number(&j, (int64_t)job->uploaded);
    portal_json_raw(&j, ",\"upload_bytes\":"); portal_json_number(&j, (int64_t)job->length);
    portal_json_raw(&j, job->cleanup && job->state >= DONE ? ",\"cleanup_pending\":true" : ",\"cleanup_pending\":false");
    portal_json_raw(&j, job->cleanup_error != CLEANUP_OK ? ",\"cleanup_failed\":true" : ",\"cleanup_failed\":false");
    portal_json_raw(&j, ",\"cleanup_error\":");
    const char *errors[] = {"", "unlink_failed", "worker_unavailable", "storage_unavailable", "session_changed"};
    portal_json_string(&j, errors[job->cleanup_error]);
    portal_json_raw(&j, ",\"cleanup_errno\":"); portal_json_number(&j, job->cleanup_errno);
    portal_json_raw(&j, ",\"cleanup_path\":"); portal_json_string(&j, job->cleanup_error != CLEANUP_OK ? job->path : "");
    portal_json_raw(&j, ",\"cleanup_volume\":"); portal_json_number(&j, job->volume);
    portal_json_raw(&j, ",\"cleanup_session\":"); portal_json_number(&j, job->session);
    portal_json_raw(&j, "}");
    UNLOCK(); return !j.failed;
}
