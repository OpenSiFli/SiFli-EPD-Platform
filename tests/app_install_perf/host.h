#ifndef INSTALL_PERF_HOST_H
#define INSTALL_PERF_HOST_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <windows.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#undef assert
#define assert(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)
#define EPD_APP_H
#define EPD_MEMORY_H
#define APP_CATALOG_H
#define EPD_POWER_H
#define EPD_I18N_H
#define RT_NAME_MAX 32
#define GUI_APP_ID_MAX_LEN 32
#define EPD_APP_ABI 2u
#define EPD_APP_PSRAM 0
#define RT_EOK 0
#define RT_WAITING_FOREVER -1
#define RT_IPC_FLAG_FIFO 0
#define RT_IPC_FLAG_PRIO 1
#define RTM_EXPORT(symbol)
#define RT_NULL NULL
#define EPD_MEMORY_HOST
#define RT_USING_THREAD_MEM_CONTEXT
#ifndef RT_TICK_PER_SECOND
#define RT_TICK_PER_SECOND 1000
#endif
#define RT_EINVAL 3
#define LV_USE_EXECUTION_CONTEXT 1
static char host_warning[256];
#define LOG_W(...) ((void)snprintf(host_warning, sizeof(host_warning), __VA_ARGS__))
#define EPD_AI_OK 0
#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_SD_ROOT "/sdcard"
#define STORAGE_FLASH_APPS "/flash/apps"
#define STORAGE_SD_APPS "/sdcard/.epd/apps"
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;
typedef intptr_t rt_base_t;
typedef uint32_t rt_tick_t;
typedef int rt_err_t;
typedef int32_t rt_int32_t;
typedef int epd_app_memory_t;
typedef void *rt_thread_t;
typedef struct lv_obj_t lv_obj_t;
typedef struct lv_group_t lv_group_t;
typedef void rt_list_t;
typedef struct { char id[32]; } gui_runing_app_t;
struct rt_semaphore { int unused; };
struct rt_mutex { CRITICAL_SECTION guard; };
typedef struct { uint32_t entry_func, user_data; } app_entity_info;
typedef struct { uint32_t abi; const char *id; } epd_app_info_t;
typedef struct { int unused; } epd_background_t;
typedef struct { const char *id; } sleep_guard_owner_t;
typedef struct { const char *locale; } host_lang_t;
typedef const host_lang_t *lv_i18n_lang_pack_t;
typedef void *lv_ext_res_mng_t;
#define LV_RES_OK 0
typedef struct {
    char id[32], name[48]; const void *icon; char icon_file[160];
    bool has_settings, resources_only, background, handles_content, lockscreen;
    uint32_t version, data_version; char build_id[65]; storage_volume_t volume;
} epd_app_entry_t;
static int ui_icon_album;
static uint32_t session = 1, state_revision = 1;
static storage_volume_t host_app_volume = STORAGE_FLASH;
static bool available = true, changing, releasing;
static __declspec(thread) int lock_depth, irq_depth;
static int lock_yields;
static SRWLOCK io_guard = SRWLOCK_INIT, irq_guard = SRWLOCK_INIT;
static void storage_lock(void) { if (!lock_depth++) AcquireSRWLockExclusive(&io_guard); }
static void storage_unlock(void) { assert(lock_depth > 0); if (!--lock_depth) { ++lock_yields; ReleaseSRWLockExclusive(&io_guard); } }
static const char *storage_path_root(const char *path) { return !strncmp(path, "/sdcard", 7) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static const char *storage_root(storage_volume_t volume) { return volume == STORAGE_SD ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static bool storage_session_valid(storage_volume_t volume, uint32_t token) { return available && (volume == STORAGE_FLASH || token == session); }
static bool storage_apps_enabled(storage_volume_t volume) { (void)volume; return available; }
static bool storage_available(storage_volume_t volume) { (void)volume; return available; }
static bool storage_path_available(const char *path) { (void)path; return available; }
static uint32_t storage_card_session(void) { return session; }
static uint32_t storage_revision(void) { return state_revision; }
static bool storage_changing(void) { return changing; }
static bool storage_release_requested(void) { return releasing; }
uint32_t storage_path_revision(const char *path);
uint32_t storage_file_writers(const char *path);
bool storage_path_pin(const char *path);
void storage_path_unpin(const char *path);
unsigned host_watch_pin_count(void);
void host_watch_reset(void);
void host_watch_remount(storage_volume_t volume);
void host_watch_changed(const char *path, bool directory);
void host_watch_pin(const char *path, int flags, bool acquire);
void host_watch_rename(const char *from, const char *to);
static bool storage_mkdirs(const char *path);
static bool storage_app_path(char *out, size_t size, const char *id, storage_app_area_t area, const char *relative) {
    (void)area; return snprintf(out, size, "%s/%s%s%s", host_app_volume == STORAGE_SD ? STORAGE_SD_APPS : STORAGE_FLASH_APPS, id, *relative ? "/" : "", relative) < size;
}
static const char *storage_app_directory(storage_volume_t volume) { return volume == STORAGE_SD ? STORAGE_SD_APPS : STORAGE_FLASH_APPS; }
static bool storage_app_locate(const char *id, storage_volume_t *volume) { (void)id; *volume = host_app_volume; return true; }
static __declspec(thread) unsigned host_alloc_attempt, host_alloc_fail;
static volatile LONG host_alloc_live;
static LONG host_alloc_resident; /* Explicit runtime-lifetime control blocks. */
static void *epd_app_alloc(size_t size, int area) {
    (void)area; assert(!irq_depth);
    if (++host_alloc_attempt == host_alloc_fail) return NULL;
    void *memory = malloc(size);
    if (memory) InterlockedIncrement(&host_alloc_live);
    return memory;
}
static void epd_app_free(void *p) { if (p) InterlockedDecrement(&host_alloc_live); free(p); }
static void *epd_app_realloc(void *p, size_t size, int area) {
    if (!p) return epd_app_alloc(size, area);
    if (!size) { epd_app_free(p); return NULL; }
    assert(!irq_depth);
    if (++host_alloc_attempt == host_alloc_fail) return NULL;
    return realloc(p, size);
}
#define rt_calloc calloc
#define rt_free free
#define rt_memcpy memcpy
#define rt_memset memset
#define rt_kprintf printf
#define rt_snprintf snprintf
static rt_base_t rt_hw_interrupt_disable(void) { int previous = irq_depth++; if (!previous) AcquireSRWLockExclusive(&irq_guard); return previous; }
static void rt_hw_interrupt_enable(rt_base_t level) { assert(irq_depth == level + 1); if (!--irq_depth) ReleaseSRWLockExclusive(&irq_guard); }
static int rt_mutex_init(struct rt_mutex *mutex, const char *name, int flags) { (void)name; (void)flags; InitializeCriticalSection(&mutex->guard); return RT_EOK; }
static int rt_mutex_take(struct rt_mutex *mutex, int timeout) { (void)timeout; assert(!lock_depth && !irq_depth); EnterCriticalSection(&mutex->guard); return RT_EOK; }
static void rt_mutex_release(struct rt_mutex *mutex) { LeaveCriticalSection(&mutex->guard); }
static int rt_mutex_detach(struct rt_mutex *mutex) { DeleteCriticalSection(&mutex->guard); return RT_EOK; }
static void rt_set_errno(int value) { errno = value; }
static int rt_get_errno(void) { return errno; }
static int app_recovery_check_dynamic(void) { return RT_EOK; }
static void ui_reader_font_cache_reserve(size_t peak) { (void)peak; assert(!lock_depth); }
static int rt_sem_init(struct rt_semaphore *sem, const char *name, int count, int flags) { (void)sem; (void)name; (void)count; (void)flags; return RT_EOK; }
static int rt_sem_take(struct rt_semaphore *sem, int timeout) { (void)sem; (void)timeout; return -1; }
static void rt_sem_release(struct rt_semaphore *sem) { (void)sem; }
static int rt_sem_detach(struct rt_semaphore *sem) { (void)sem; return RT_EOK; }
static rt_tick_t rt_tick_get(void) { return 0; }
static rt_tick_t rt_tick_from_millisecond(int ms) { return ms; }
static rt_thread_t epd_thread_create_psram(const char *name, void (*fn)(void *), void *arg, int stack, int priority, int tick) { (void)name; (void)fn; (void)arg; (void)stack; (void)priority; (void)tick; return (void *)1; }
static int rt_thread_startup(rt_thread_t thread) { (void)thread; return 0; }
static void rt_thread_delete(rt_thread_t thread) { (void)thread; }
static bool sleep_guard_acquire(sleep_guard_owner_t *owner) { (void)owner; return true; }
static void sleep_guard_release(sleep_guard_owner_t *owner) { (void)owner; }
static bool system_reset_blocks_apps(void) { return false; }
static int ai_settings_start(void) { return 0; }
static int ai_settings_revoke_app_sync(const char *id) { (void)id; return 0; }
static bool reminder_remove_app(const char *id) { (void)id; return true; }
static bool app_service_running(const char *id) { (void)id; return false; }
static void *app_service_start(const char *id, const epd_background_t *background, void *module) { (void)id; (void)background; return module; }
static void app_service_stop(const char *id) { (void)id; }
static void gui_app_exit(const char *id) { (void)id; }
static void gui_app_exec_now(void) {}
static gui_runing_app_t *gui_app_trav(rt_list_t **cursor) { (void)cursor; return NULL; }
static void app_catalog_changed(void) {}
static void epd_power_notify(void) {}
#define epd_get_str(...) "message"
static bool app_package_read(const char *directory, const char *id, epd_app_entry_t *entry);
struct rt_dlmodule { char name[32]; int nref; void (*cleanup_func)(struct rt_dlmodule *); };
static void (*host_module_init)(void);
static struct rt_dlmodule resident[16];
static bool fail_module_open;
typedef uint32_t rt_uint32_t;
typedef rt_uint32_t (*dlmodule_symbol_resolver)(const char *);
static dlmodule_symbol_resolver host_resolver;
static const char *host_missing_import;
static int dlmodule_register_ex_symbol_resolver(dlmodule_symbol_resolver resolver) {
    if (host_resolver) return 1;
    host_resolver = resolver; return RT_EOK;
}
static int dlmodule_unregister_ex_symbol_resolver(void) { host_resolver = NULL; return RT_EOK; }
static struct rt_dlmodule *dlmodule_find(const char *name) {
    for (int i = 0; i < 16; ++i) if (resident[i].nref && !strcmp(resident[i].name, name)) return &resident[i];
    return NULL;
}
static void *dlopen(const char *path, int mode) {
    if (fail_module_open) return NULL;
    if (host_missing_import) {
        assert(host_resolver);
        if (!host_resolver(host_missing_import)) return NULL;
    }
    (void)mode; const char *name = strrchr(path, '/') + 1; char base[32];
    snprintf(base, sizeof(base), "%.*s", (int)strlen(name) - 3, name);
    struct rt_dlmodule *module = dlmodule_find(base);
    bool fresh = !module;
    if (!module) for (int i = 0; i < 16; ++i) if (!resident[i].nref) { module = &resident[i]; strcpy(module->name, base); module->cleanup_func = NULL; break; }
    assert(module); ++module->nref;
    if (fresh && host_module_init) host_module_init();
    return module;
}
static void dlclose(void *p) { --((struct rt_dlmodule *)p)->nref; }
static void *(*host_symbol_hook)(void *module, const char *symbol);
static void *dlsym(void *module, const char *symbol) {
    if (host_symbol_hook) return host_symbol_hook(module, symbol);
    (void)module; static const epd_app_info_t descriptor = {EPD_APP_ABI, "demo"};
    return !strcmp(symbol, "epd_app_info") ? (void *)&descriptor : (void *)(uintptr_t)0x1234;
}
static lv_ext_res_mng_t dlmodule_get_res_mng(void *p) { (void)p; return NULL; }
static int lv_ext_set_locale(lv_ext_res_mng_t p, const char *l) { (void)p; (void)l; return 0; }
static const lv_i18n_lang_pack_t *lv_ext_get_lang_pack(lv_ext_res_mng_t p) { (void)p; return NULL; }

static __declspec(thread) unsigned hash_chunks;
static int host_sha_start(mbedtls_sha256_context *ctx, int sha224) {
    assert(!lock_depth && !irq_depth); return mbedtls_sha256_starts_ret(ctx, sha224);
}
static int host_sha_update(mbedtls_sha256_context *ctx, const unsigned char *data, size_t size) {
    assert(!lock_depth && !irq_depth); ++hash_chunks; return mbedtls_sha256_update_ret(ctx, data, size);
}
static int host_sha_finish(mbedtls_sha256_context *ctx, unsigned char *out) {
    assert(!lock_depth && !irq_depth); return mbedtls_sha256_finish_ret(ctx, out);
}
static int host_sha_once(const unsigned char *data, size_t size, unsigned char *out, int sha224) {
    assert(!lock_depth && !irq_depth); return mbedtls_sha256_ret(data, size, out, sha224);
}
#define mbedtls_sha256_starts_ret host_sha_start
#define mbedtls_sha256_update_ret host_sha_update
#define mbedtls_sha256_finish_ret host_sha_finish
#define mbedtls_sha256_ret host_sha_once

typedef int mode_t;
typedef int64_t off_t;
struct stat { int st_mode; int64_t st_size; };
struct statfs { uint32_t f_bsize, f_bfree; };
struct dirent { char d_name[256]; };
typedef struct { char path[256]; int next; struct dirent item; } DIR;
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 2
#define O_EXCL 4
#define S_ISREG(mode) ((mode) == 1)
#define S_ISDIR(mode) ((mode) == 2)
typedef struct { char path[256]; unsigned char *data; size_t size; bool directory; uint32_t revision, writers; uint64_t bytes_read; } host_file_t;
static host_file_t files[256];
static struct { host_file_t *file; size_t offset; int flags; char pinned_path[256]; } handles[64];
static int file_count_host;
static const char *mutate_on_read;
static bool remove_on_read;
static bool fail_publish;
static const char *host_fail_io, *host_fail_path;
static unsigned host_fail_skip;
static bool host_io_error(const char *operation, const char *path) {
    if (!host_fail_io || strcmp(operation, host_fail_io) || (host_fail_path && strcmp(path, host_fail_path))) return false;
    if (host_fail_skip) { --host_fail_skip; return false; }
    host_fail_io = NULL; errno = EIO; return true;
}
static host_file_t *host_find(const char *path) { for (int i = 0; i < file_count_host; ++i) if (!strcmp(files[i].path, path)) return &files[i]; return NULL; }
static host_file_t *host_put(const char *path, const void *data, size_t size, bool directory) {
    host_file_t *file = host_find(path);
    if (!file) {
        for (int i = 0; i < file_count_host; ++i) if (!files[i].path[0]) { file = &files[i]; break; }
        if (!file) { assert(file_count_host < 256); file = &files[file_count_host++]; }
        assert(!file->writers);
        file->bytes_read = 0;
        strcpy(file->path, path);
    }
    free(file->data); file->data = malloc(size + 1); if (size) memcpy(file->data, data, size);
    file->data[size] = 0; file->size = size; file->directory = directory; ++file->revision;
    host_watch_changed(path, directory); return file;
}
static bool storage_mkdirs(const char *path) { if (!host_find(path)) host_put(path, NULL, 0, true); return true; }
static int host_open(const char *path, int flags, ...) {
    assert(lock_depth && !irq_depth);
    if (host_io_error("open", path)) return -1;
    host_file_t *file = host_find(path);
    if (file && (flags & O_EXCL)) return -1;
    if (!file && (flags & O_CREAT)) file = host_put(path, NULL, 0, false);
    if (!file) { errno = -ENOENT; return -1; }
    for (int i = 1; i < 64; ++i) if (!handles[i].file) { handles[i].file = file; handles[i].offset = 0;
        handles[i].flags = flags; strcpy(handles[i].pinned_path, path); host_watch_pin(path, flags, true); return i; }
    return -1;
}
static int host_close(int fd) { assert(lock_depth && !irq_depth); assert(fd > 0 && handles[fd].file);
    host_watch_pin(handles[fd].pinned_path, handles[fd].flags, false); handles[fd].file = NULL; return 0; }
static int host_read(int fd, void *out, size_t size) {
    assert(lock_depth && !irq_depth);
    host_file_t *file = handles[fd].file; assert(file);
    if (host_io_error("read", file->path)) return -1;
    if (host_io_error("short", file->path)) return 0;
    if (strstr(file->path, ".so") || strstr(file->path, ".sap")) assert(lock_depth == 1);
    size_t remaining = file->size - handles[fd].offset; if (size > remaining) size = remaining;
    memcpy(out, file->data + handles[fd].offset, size); handles[fd].offset += size; file->bytes_read += size;
    if (mutate_on_read && !strcmp(file->path, mutate_on_read)) { ++file->revision; host_watch_changed(file->path, false); mutate_on_read = NULL; }
    if (remove_on_read) { ++session; available = false; remove_on_read = false; }
    return (int)size;
}
static int host_write(int fd, const void *data, size_t size) {
    assert(lock_depth && !irq_depth);
    host_file_t *file = handles[fd].file; size_t end = handles[fd].offset + size;
    if (host_io_error("write", file->path)) return -1;
    if (end > file->size) { file->data = realloc(file->data, end + 1); file->size = end; }
    memcpy(file->data + handles[fd].offset, data, size); handles[fd].offset = end; ++file->revision;
    host_watch_changed(file->path, false); return (int)size;
}
static int host_stat(const char *path, struct stat *st) { assert(lock_depth && !irq_depth); host_file_t *f = host_find(path); if (!f) { errno = -ENOENT; return -1; } st->st_size = f->size; st->st_mode = f->directory ? 2 : 1; return 0; }
static int host_fstat(int fd, struct stat *st) { return host_stat(handles[fd].file->path, st); }
static off_t host_seek(int fd, off_t offset, int origin) { assert(lock_depth && !irq_depth); (void)origin; if (offset < 0 || (uint64_t)offset > handles[fd].file->size) return -1; handles[fd].offset = (size_t)offset; return offset; }
static int host_sync(int fd) { assert(lock_depth && !irq_depth); (void)fd; return 0; }
static int host_mkdir(const char *path, mode_t mode) { assert(lock_depth && !irq_depth); (void)mode; if (host_find(path)) return -1; host_put(path, NULL, 0, true); return 0; }
static int host_unlink(const char *path) { assert(lock_depth && !irq_depth); if (host_io_error("unlink", path)) return -1; host_file_t *f = host_find(path); if (!f) return -1; host_watch_changed(path, true); f->path[0] = 0; ++f->revision; return 0; }
static int host_rename(const char *from, const char *to) {
    assert(lock_depth && !irq_depth);
    if (host_io_error("rename", from)) return -1;
    host_watch_rename(from, to);
    if (fail_publish && strstr(from, ".app-install")) return -1;
    if (!host_find(from) || host_find(to)) return -1;
    size_t n = strlen(from);
    for (int i = 0; i < file_count_host; ++i) if (!strncmp(files[i].path, from, n) && (!files[i].path[n] || files[i].path[n] == '/')) {
        char new_path[256]; snprintf(new_path, sizeof(new_path), "%s%s", to, files[i].path + n); strcpy(files[i].path, new_path); ++files[i].revision;
    }
    return 0;
}
static DIR *host_opendir(const char *path) { assert(lock_depth && !irq_depth); host_file_t *f = host_find(path); if (!f || !f->directory) { errno = -ENOENT; return NULL; } DIR *dir = calloc(1, sizeof(*dir)); strcpy(dir->path, path); return dir; }
static struct dirent *host_readdir(DIR *dir) { assert(lock_depth && !irq_depth); size_t n = strlen(dir->path); while (dir->next < file_count_host) { const char *path = files[dir->next++].path; if (!strncmp(path, dir->path, n) && path[n] == '/' && !strchr(path + n + 1, '/')) { strcpy(dir->item.d_name, path + n + 1); return &dir->item; } } return NULL; }
static int host_closedir(DIR *dir) { assert(lock_depth && !irq_depth); free(dir); return 0; }
static int host_statfs(const char *path, struct statfs *st) { assert(lock_depth && !irq_depth); (void)path; st->f_bsize = 4096; st->f_bfree = 65536; return 0; }
static FILE *host_fopen(const char *path, const char *mode) { (void)mode; int fd = host_open(path, 0); return fd < 0 ? NULL : (FILE *)(intptr_t)fd; }
static char *host_fgets(char *line, int size, FILE *file) { int fd = (int)(intptr_t)file, i = 0; while (i < size - 1 && host_read(fd, line + i, 1) == 1) { if (line[i++] == '\n') break; } line[i] = 0; return i ? line : NULL; }
static int host_fclose(FILE *file) { return host_close((int)(intptr_t)file); }
static int host_ferror(FILE *file) { (void)file; return 0; }
#define open host_open
#define close host_close
#define read host_read
#define write host_write
#define stat(...) host_stat(__VA_ARGS__)
#define fstat host_fstat
#define lseek host_seek
#define fsync host_sync
#define mkdir host_mkdir
#define unlink host_unlink
#define rmdir host_unlink
#define rename host_rename
#define opendir host_opendir
#define readdir host_readdir
#define closedir host_closedir
#define dfs_statfs host_statfs
#define fopen host_fopen
#define fgets host_fgets
#define fclose host_fclose
#define ferror host_ferror
#endif
