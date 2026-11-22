/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOKS_PERF_HOST_API_H
#define BOOKS_PERF_HOST_API_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <windows.h>
#include <crtdbg.h>
#include "epd_reading_data.h"
void host_data_probe(void (*probe)(void));
void host_data_resources(void);

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
typedef int rt_err_t, rt_bool_t, rt_int32_t;
static uint32_t host_tick;
static uint32_t rt_tick_get(void) { return host_tick; }
typedef size_t rt_size_t;
typedef uintptr_t rt_base_t;
typedef CRITICAL_SECTION *rt_mutex_t;
typedef struct epd_service { int unused; } epd_service_t;
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EIO 5
#define RT_ENOMEM 12
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_NULL NULL
#define RT_WAITING_FOREVER -1
#define RT_IPC_FLAG_PRIO 0
#define EPD_APP_PSRAM 0
#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_SD_ROOT "/sdcard"
#define STORAGE_PATH_MAX 512
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef struct { uint64_t total, used; } storage_info_t;
static size_t available_bytes = 8u * 1024u * 1024u;
static uint64_t disk_free = 64u * 1024u * 1024u;
static bool memory_pressure, service_cancelled, cover_ui;
typedef struct { bool pending; } app_memory_pressure_status_t;
static app_memory_pressure_status_t app_memory_pressure_query(void) { return (app_memory_pressure_status_t){memory_pressure}; }
static size_t epd_app_psram_available(void) { return available_bytes; }
static void rt_thread_mdelay(unsigned ms) { (void)ms; }
typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define rt_snprintf snprintf
static rt_mutex_t rt_mutex_create(const char *n, int f) { (void)n; (void)f; rt_mutex_t m = malloc(sizeof(*m)); InitializeCriticalSection(m); return m; }
static void rt_mutex_take(rt_mutex_t m, int t) { (void)t; EnterCriticalSection(m); }
static void rt_mutex_release(rt_mutex_t m) { LeaveCriticalSection(m); }
static void rt_mutex_delete(rt_mutex_t m) { DeleteCriticalSection(m); free(m); }
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t k) { (void)k; }
static int storage_depth, unlocks, stats, opens, signature_calls, signature_requests;
static uint64_t signature_bytes;
static uint32_t media_session = 1, media_revision = 1, next_version = 10;
static uint32_t root_revision[STORAGE_COUNT] = {1, 1};
static bool media_present[2] = {true, true}, changing, signature_cache = true;
static bool signature_fail, directory_fail, stat_fail;
static void (*signature_probe)(const char *);
static void (*unlock_probe)(void);
static void storage_lock(void) { ++storage_depth; }
static void storage_unlock(void) { CHECK(storage_depth > 0); if (!--storage_depth) { ++unlocks; if (unlock_probe) unlock_probe(); } }
static bool allocation_fail;
static void *epd_app_alloc(size_t n, int k) { (void)k; return allocation_fail ? NULL : malloc(n); }
static void *epd_app_realloc(void *p, size_t n, int k) { (void)k; return realloc(p, n); }
static void (*free_probe)(void *);
static void epd_app_free(void *p) { if (p && free_probe) free_probe(p); free(p); }
static void rt_kprintf(const char *fmt, ...) { (void)fmt; }
static const char *storage_root(storage_volume_t v) { return v == STORAGE_SD ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static const char *storage_path_root(const char *p) { return !strncmp(p, "/sdcard", 7) ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static const char *storage_relative_path(const char *p) { return p + strlen(storage_path_root(p)); }
static bool storage_available(storage_volume_t v) { return media_present[v]; }
static bool storage_info(storage_volume_t v, storage_info_t *info) { *info=(storage_info_t){64u*1024u*1024u,64u*1024u*1024u-disk_free};return storage_available(v); }
static bool storage_mkdirs(const char *p) { (void)p;return true; }
static bool storage_path_available(const char *p) { return media_present[!strcmp(storage_path_root(p), STORAGE_SD_ROOT)]; }
static bool storage_changing(void) { return changing; }
static uint32_t storage_card_session(void) { return media_session; }
static uint32_t storage_revision(void) { return media_revision; }
static bool storage_session_valid(storage_volume_t v, uint32_t s) { return media_present[v] && (v == STORAGE_FLASH || (!changing && s == media_session)); }
#ifdef HOST_REAL_WATCH
#include "watch_bridge.h"
#else
static uint32_t volume_revision[STORAGE_COUNT] = {1, 1};
static uint32_t storage_volume_revision(storage_volume_t v) { return volume_revision[v]; }
static uint32_t storage_directory_revision(const char *p) { return root_revision[!strcmp(storage_path_root(p), STORAGE_SD_ROOT)]; }
#endif

typedef struct {
    char path[512]; unsigned char data[131072]; size_t size;
    uint32_t version, writers, cached_version, cached_hash;
    bool present, directory;
} host_file_t;
static host_file_t fs[320];
static unsigned fs_count;
static host_file_t *find_file(const char *p, bool create)
{
    for (unsigned i = 0; i < fs_count; ++i) if (!strcmp(fs[i].path, p)) return &fs[i];
    if (!create) return NULL;
    CHECK(fs_count < 320 && strlen(p) < sizeof(fs[0].path));
    host_file_t *f = &fs[fs_count++]; strcpy(f->path, p); f->version = ++next_version; return f;
}
static void file_changed(host_file_t *f)
{
    f->version = ++next_version;
#ifdef HOST_REAL_WATCH
    host_watch_changed(f->path, !f->present);
#else
    volume_revision[!strcmp(storage_path_root(f->path), STORAGE_SD_ROOT)] = next_version;
#endif
}
static void set_writers(host_file_t *f, unsigned writers)
{ f->writers = writers; file_changed(f); }
static host_file_t *put_file(const char *p, const void *data, size_t n)
{
    host_file_t *f = find_file(p, true); CHECK(n <= sizeof(f->data));
    if (!f->present && !strchr(p + strlen(storage_path_root(p)) + 1, '/'))
        ++root_revision[!strcmp(storage_path_root(p), STORAGE_SD_ROOT)];
    if (n) memcpy(f->data, data, n);
    f->size = n; file_changed(f); f->present = true; return f;
}
#ifndef HOST_REAL_WATCH
static uint32_t storage_path_revision(const char *p) { return find_file(p, true)->version; }
static uint32_t storage_file_writers(const char *p) { return find_file(p, true)->writers; }
#endif
static int host_stat(const char *p, struct stat *s)
{
    CHECK(storage_depth); ++stats;
    host_file_t *f = find_file(p, false);
    if (stat_fail) { errno = EIO; return -1; }
    if (!storage_path_available(p) || !f || !f->present) { errno = ENOENT; return -1; }
    memset(s, 0, sizeof(*s)); s->st_size = (long)f->size; s->st_mtime = 42;
    s->st_mode = f->directory ? _S_IFDIR : _S_IFREG; return 0;
}
#define stat(p, s) host_stat(p, s)
typedef bool (*storage_signature_update_t)(void *, const void *, size_t, uint32_t *);
static bool storage_file_signature_with(const char *p, uint32_t *out, storage_signature_update_t update, void *user)
{
    CHECK(storage_depth == 0); ++signature_requests;
    host_file_t *f = find_file(p, true);
    if (signature_fail || !f->present || f->writers || !storage_path_available(p)) return false;
    uint32_t before = f->version;
    if ((signature_cache || update) && before == f->cached_version) { *out = f->cached_hash; return true; }
    ++signature_calls;
    uint32_t h = 2166136261u;
    if (update) {
        for (size_t at = 0; at < f->size;) {
            size_t n = f->size - at < 4096 ? f->size - at : 4096;
            if (!update(user, f->data + at, n, &h)) return false;
            at += n;
        }
    } else for (size_t i = 0; i < f->size; ++i) h = (h ^ f->data[i]) * 16777619u;
    signature_bytes += f->size;
    if (signature_probe) signature_probe(p);
    if (before != f->version || f->writers || !storage_path_available(p)) return false;
    f->cached_version = f->version; f->cached_hash = *out = h; return true;
}
static bool storage_file_signature(const char *p, uint32_t *out)
{ return storage_file_signature_with(p, out, NULL, NULL); }
typedef struct { host_file_t *f; size_t at; } host_fd_t;
static host_fd_t descriptors[32];
static unsigned cover_reads, cover_writes, cover_opens, cover_closes, close_failures, write_failures, commit_failures;
static size_t cover_read_max, cover_write_max;
static int host_open(const char *p, int flags, ...)
{
    CHECK(storage_depth); ++opens; host_file_t *f = find_file(p, false); (void)flags;
    if(strstr(p,"/covers/")) {
        CHECK(!cover_ui);++cover_opens;
        if(flags & O_CREAT){ f=find_file(p,true);f->present=true;if(flags&O_TRUNC)f->size=0;file_changed(f); }
    }
    if (!f || !f->present) { errno = ENOENT; return -1; }
    for (int i = 1; i < 32; ++i) if (!descriptors[i].f) {
#ifdef HOST_REAL_WATCH
        host_watch_pin(p, true);
#endif
        descriptors[i].f = f; descriptors[i].at = 0; return i;
    }
    CHECK(false); return -1;
}
static int host_read(int fd, void *p, size_t n)
{
    CHECK(storage_depth); host_fd_t *d = &descriptors[fd]; CHECK(d->f);
    if(strstr(d->f->path,"/covers/")){CHECK(!cover_ui);++cover_reads;if(n>cover_read_max)cover_read_max=n;}
    if (n > d->f->size - d->at) n = d->f->size - d->at;
    memcpy(p, d->f->data + d->at, n); d->at += n; return (int)n;
}
static int host_close(int fd) {
    CHECK(storage_depth && descriptors[fd].f);
    if(strstr(descriptors[fd].f->path,"/covers/")){CHECK(!cover_ui);++cover_closes;if(close_failures){--close_failures;return -1;}}
#ifdef HOST_REAL_WATCH
    host_watch_pin(descriptors[fd].f->path, false);
#endif
    descriptors[fd].f = NULL; return 0;
}
static int host_fstat(int fd, struct stat *st) { return host_stat(descriptors[fd].f->path, st); }
static long host_lseek(int fd, long at, int origin) { CHECK(storage_depth && origin == SEEK_SET); descriptors[fd].at = at; return at; }
static int host_unlink(const char *p) { host_file_t *f = find_file(p, false); if (!f || !f->present) { errno = ENOENT; return -1; } f->present = false; file_changed(f); return 0; }
static int host_write(int fd,const void *p,size_t n) {
    CHECK(storage_depth&&!cover_ui);host_fd_t *d=&descriptors[fd];CHECK(d->f&&strstr(d->f->path,"/covers/"));
    ++cover_writes;if(n>cover_write_max)cover_write_max=n;if(write_failures){--write_failures;return -1;}
    CHECK(d->at+n<=sizeof(d->f->data));memcpy(d->f->data+d->at,p,n);d->at+=n;
    if(d->at>d->f->size)d->f->size=d->at;file_changed(d->f);return (int)n;
}
static int host_fsync(int fd) { CHECK(storage_depth&&!cover_ui&&descriptors[fd].f);return 0; }
static bool storage_file_commit(const char *tmp,const char *path) {
    CHECK(storage_depth&&!cover_ui);if(commit_failures){--commit_failures;return false;}
    host_file_t *f=find_file(tmp,false);CHECK(f&&f->present);put_file(path,f->data,f->size);f->present=false;file_changed(f);return true;
}
#define open host_open
#define read host_read
#define write host_write
#define fsync host_fsync
#define close host_close
#define fstat host_fstat
#define lseek host_lseek
#define unlink host_unlink
struct dirent { char d_name[256]; };
typedef struct { unsigned at; char root[512]; struct dirent entry; } DIR;
static unsigned dirs_open, dirs_closed;
static DIR *opendir(const char *p) { CHECK(storage_depth); DIR *d = calloc(1, sizeof(*d)); strcpy(d->root, p); ++dirs_open; return d; }
static struct dirent *readdir(DIR *d)
{
    CHECK(storage_depth); if (directory_fail) { errno = EIO; return NULL; }
    size_t n = strlen(d->root);
    while (d->at < fs_count) {
        host_file_t *f = &fs[d->at++];
        if (!f->present || strncmp(f->path, d->root, n) || f->path[n] != '/' || strchr(f->path + n + 1, '/')) continue;
        strcpy(d->entry.d_name, f->path + n + 1); return &d->entry;
    }
    return NULL;
}
static int closedir(DIR *d) { CHECK(storage_depth); ++dirs_closed; free(d); return 0; }
static bool storage_file_recover(const char *p) { (void)p; return true; }
static bool history_ui, history_write_fail;
static unsigned history_writes;
static void (*history_write_probe)(void);
static bool storage_file_replace(const char *p, const void *d, size_t n) {
    CHECK(!history_ui); ++history_writes;
    if (history_write_probe) history_write_probe();
    if (history_write_fail) return false;
    put_file(p, d, n); return true;
}
static bool storage_app_available(const char *id) { (void)id; return true; }
static bool storage_app_path(char *out, size_t n, const char *id, storage_app_area_t area, const char *rel)
{ (void)id; (void)area; return snprintf(out, n, "/flash/cache/%s", rel) > 0; }
static bool storage_record_save(const char *p, uint32_t magic, uint32_t version, const void *data, size_t size)
{
    unsigned char framed[65536]; uint32_t head[] = {magic, version, (uint32_t)size, 0};
    CHECK(size + sizeof(head) <= sizeof(framed));
    memcpy(framed, head, sizeof(head)); memcpy(framed + sizeof(head), data, size);
    put_file(p, framed, size + sizeof(head)); return true;
}
static bool storage_record_load(const char *p, uint32_t magic, uint32_t version, void *out, size_t size)
{
    host_file_t *f = find_file(p, false); uint32_t head[4];
    if (!f || !f->present || f->size != sizeof(head) + size) return false;
    memcpy(head, f->data, sizeof(head));
    if (head[0] != magic || head[1] != version || head[2] != size) return false;
    memcpy(out, f->data + sizeof(head), size); return true;
}
struct rt_work { int unused; }; struct rt_workqueue { int unused; };
static struct rt_workqueue work_queue;
static struct rt_workqueue *rt_workqueue_sysq(void) { return &work_queue; }
static void rt_work_init(struct rt_work *w, void (*f)(struct rt_work *, void *), void *p) { (void)w; (void)f; (void)p; }
static void rt_workqueue_submit_work(struct rt_workqueue *q, struct rt_work *w, int t) { (void)q; (void)w; (void)t; }
static void rt_workqueue_cancel_work_sync(struct rt_workqueue *q, struct rt_work *w) { (void)q; (void)w; }
static void rt_workqueue_cancel_work(struct rt_workqueue *q, struct rt_work *w) { (void)q; (void)w; }
static int rt_tick_from_millisecond(int n) { return n; }
static bool epd_service_cancelled(epd_service_t *s) { (void)s; return service_cancelled; }
static void epd_service_wake(epd_service_t *s) { (void)s; }
static void epd_service_ui_request(epd_service_t *s, int t) { (void)s; (void)t; }
static char recent_name[128];
static unsigned recent_progress;
static void epd_app_set_recent_reading(const char *s, unsigned p)
{ snprintf(recent_name, sizeof(recent_name), "%s", s); recent_progress = p; }
static struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
#include "../../modules/books/bookshelf.h"
#define BOOK_DOCUMENT_H
typedef struct { unsigned magic, cf, w, h, stride; } host_image_header_t;
typedef struct { host_image_header_t header; unsigned data_size; const uint8_t *data; } lv_image_dsc_t;
#define LV_IMAGE_HEADER_MAGIC 0x19u
#define LV_COLOR_FORMAT_RGB565 0x12u
#define DOCUMENT_COVER_WIDTH 168u
#define DOCUMENT_COVER_HEIGHT 220u
typedef struct { lv_image_dsc_t image; void *memory; } document_bitmap_t;
typedef struct { int result; bool cover; } document_t;
#include "document_types.h"
#include "../../modules/books/ui_bookshelf_data.h"
#include "../../modules/books/formats/formats.h"
#include "../../modules/books/document/document_cache.h"
#include "../../modules/books/file_version.h"
typedef book_format_t document_format_t;
#define DOCUMENT_UNKNOWN BOOK_FORMAT_UNKNOWN
#define DOCUMENT_EPUB BOOK_FORMAT_EPUB
#define DOCUMENT_SPLIT_EPUB BOOK_FORMAT_SPLIT_EPUB
#ifdef HOST_REAL_COVER_CACHE
#include "../../modules/books/cover_cache.h"
#else
static bool books_cover_cache_request(const bookshelf_item_t *f,const book_file_version_t *v){(void)f;(void)v;return false;}
static int books_cover_cache_take(char *t,size_t n,document_bitmap_t *b){(void)t;(void)n;(void)b;return -1;}
static void books_cover_cache_save(const bookshelf_item_t *f,const book_file_version_t *v,const char *t,const document_bitmap_t *b){(void)f;(void)v;(void)t;(void)b;}
static void books_cover_cache_cancel(void){}
static bool books_cover_cache_pressure(void){return memory_pressure;}
static void books_cover_cache_shutdown(void){}
#endif
static document_t metadata_job;
static unsigned metadata_opens, metadata_closes, image_drops;
static bool display_busy;
bool epd_app_refresh_done(void) { return !display_busy; }
#define TEST_COVER_BYTES (112u * 146u * 2u)
static document_t *document_metadata_open(const bookshelf_item_t *file)
{
    CHECK(file->format == DOCUMENT_EPUB || file->format == DOCUMENT_SPLIT_EPUB);
    ++metadata_opens; metadata_job.result = 0; metadata_job.cover = false; return &metadata_job;
}
static int document_metadata_take(document_t *d, char *title, size_t capacity, document_bitmap_t *cover)
{
    CHECK(d == &metadata_job);
    if (d->result > 0) {
        snprintf(title, capacity, "%s", "真实书名");
        if (d->cover) {
            cover->memory = epd_app_alloc(TEST_COVER_BYTES, EPD_APP_PSRAM);
            CHECK(cover->memory);
            memset(cover->memory, 0x5a, TEST_COVER_BYTES);
            cover->image.data = cover->memory;
            cover->image.header=(host_image_header_t){LV_IMAGE_HEADER_MAGIC,LV_COLOR_FORMAT_RGB565,112,146,224};
            cover->image.data_size=TEST_COVER_BYTES;
        }
    }
    return d->result;
}
static void document_close(document_t *d) { CHECK(d == &metadata_job); ++metadata_closes; }
static void lv_image_cache_drop(const lv_image_dsc_t *image) { (void)image; CHECK(!display_busy); ++image_drops; }
static document_format_t document_format(const char *p, bool dir)
{ if (dir) return BOOK_FORMAT_SPLIT_EPUB; const char *dot = strrchr(p, '.'); return dot && !strcmp(dot, ".txt") ? BOOK_FORMAT_TXT : dot && !strcmp(dot, ".md") ? BOOK_FORMAT_MARKDOWN : BOOK_FORMAT_UNKNOWN; }
#ifdef HOST_REAL_SPLIT
bool document_split_identity(const char *, uint32_t *, uint32_t *);
#else
static bool document_split_identity(const char *p, uint32_t *s, uint32_t *h) { (void)p; (void)s; (void)h; return false; }
#endif
#endif
