/* SPDX-License-Identifier: Apache-2.0 */
#include "app_service.h"
#include "storage_file.h"
#include "storage.h"
#include "epd_memory.h"
#include "document.h"
#include "document_cache.h"
#include "../file_version.h"
#include "reader_compute_client.h"
#include "epd_compute_pipe.h"
#include "epd_reader_render.h"
#include "epd_reading_data.h"
#include "../compute/document_wire.h"
#include "../formats/formats.h"
#include <rthw.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

/* Memory-only reader lifecycle query; its public declaration is in reader.h. */
bool epd_reader_suspended(void);

#define DOCUMENT_IO_WINDOW 8192u
#define DOCUMENT_CHAPTER_BATCH 64u
#define DOCUMENT_CONVERT_STEPS 8u
#define DOCUMENT_CONVERT_MS 4u
#define DOCUMENT_CONTENT_SLOTS (DOCUMENT_CONTENT_CACHE_BUDGET / DOCUMENT_IO_WINDOW)
typedef struct document_chapters {
    struct document_chapters *next;
    unsigned count;
    uint32_t starts[DOCUMENT_CHAPTER_BATCH];
} document_chapters_t;

typedef struct document_navigation {
    struct document_navigation *next;
    uint32_t record, kind, level, title_at, chapter, title_record;
    bool explicit_toc;
    char text[];
} document_navigation_t;
#define DOCUMENT_NAVIGATION_BUDGET (512u * 1024u)

typedef struct document_location {
    struct document_location *next;
    uint32_t record, bytes;
    document_anchor_t anchor;
} document_location_t;
typedef struct document_part {
    struct document_part *next;
    uint32_t number, title_at;
    char text[];
} document_part_t;

/* One bounded metadata object per live book identity, without document links. */
#define DOCUMENT_TOC_BUDGET DOCUMENT_NAVIGATION_BUDGET
typedef struct document_toc {
    unsigned references, bytes, count;
    uint32_t revision, cursor, nav_chapter;
    bool complete;
    document_navigation_t *first, *tail, *nav_tail;
    document_part_t *parts, *parts_tail, *part_hint;
} document_toc_t;

typedef struct {
    document_t *document;
    uint8_t *data;
    uint32_t start, used;
    uint64_t stamp;
} document_content_t;
struct document_reader {
    struct document_reader *next;
    document_t *document;
    uint32_t window_start, window_used;
    uint8_t *window;
};
struct document_image_request {
    struct document_image_request *next;
    document_t *document;
    uint32_t offset;
    unsigned width, height;
    bool active, done, released;
    int result;
    document_bitmap_t bitmap;
};
struct document_record_request {
    struct document_record_request *next;
    document_t *document;
    uint32_t offset;
    bool active, done, released, foreground;
    int result;
    document_record_data_t data;
};
typedef struct document_save {
    struct document_save *next;
    uint32_t generation;
    unsigned references;
    bool active;
    size_t length;
    char path[STORAGE_PATH_MAX];
    uint8_t data[];
} document_save_t;
typedef struct {
    uint32_t magic, version, content, layout, end, bytes, checksum;
} document_index_header_t;
typedef struct document_remote_host document_remote_host_t;
#define DOCUMENT_INDEX_MAGIC 0x44584942u
#define DOCUMENT_INDEX_BYTES (512u * 1024u)

struct document_note_request {
    document_t *document;
    char target[DOCUMENT_TEXT_LIMIT + 1], text[DOCUMENT_NOTE_BYTES + 1];
    size_t used;
    bool truncated, received;
};
struct document {
    struct document *next;
    bookshelf_item_t file;
    document_source_t source;
    char app_id[64], directory[STORAGE_PATH_MAX], identity[BOOKSHELF_PATH_MAX];
    document_format_t format;
    uint32_t generation, serial, end, link, demand, flow_order;
    uint32_t cache_generation;
    book_file_version_t entry_source_version;
    bool entry_source_valid;
    bool entry_use_logged, entry_receive_logged;
    uint32_t pending_end, input_visible, input_position;
    rt_tick_t last_publish, opened_at;
    bool readable_logged;
    int output, input, cache_result;
    uint8_t *write_buffer;
    unsigned write_used;
    document_reader_t *readers, *default_reader;
    document_reader_t *record_reader;
    document_navigation_t *navigation, *navigation_tail;
    document_toc_t *toc_context;
    document_navigation_t *toc_restore_tail;
    uint32_t title_toc_revision;
    unsigned navigation_bytes, toc_count, explicit_toc_count;
    uint32_t navigation_revision, title_revision, title_record, title_end;
    document_navigation_t *title_node;
    unsigned title_index;
    bool title_valid;
    document_location_t *locations, *locations_tail;
    document_part_t *parts, *parts_tail;
    document_anchor_t requested, source_current;
    uint32_t source_begin, source_next, source_total, chapter, chapter_count;
    uint32_t body_start, body_end, body_publication, nav_cursor, source_revision, source_volume_revision;
    unsigned nav_state;
    int nav_result;
    bool body_first, first_body, nav_requested, nav_pausing, nav_deferred, target_required;
    uint32_t target_record, target_byte;
    unsigned owners;
    bool segmented, target_ready, retained, full_source;
    document_t *previous, *following;
    bool index_requested, index_active, index_done, index_count;
    uint32_t index_layout;
    void *index_blob;
    size_t index_bytes;
    document_chapters_t *chapters, *chapters_tail;
    document_reader_t *chapter_reader;
    uint32_t chapter_scan;
    bool chapter_failed, chapter_restore, prefetch_enabled, adjacent_pending, demand_controlled, suspend_seen;
    uint32_t prefetch_record, prefetch_request, prefetch_done, prefetch_active;
    uint32_t prefetch_publication, prefetch_focus, prefetch_low, prefetch_high;
    uint32_t prefetch_blocks[DOCUMENT_CONTENT_SLOTS];
    unsigned prefetch_cursor, prefetch_count;
    document_reader_t *prefetch_reader;
    uint8_t *prefetch_scratch;
    document_chapter_range_t prefetch_range;
    bool persistent, space_retried, cache_bad, cache_touch, temp_owned, custom_resources;
    document_cache_t *cached;
    char cache[STORAGE_PATH_MAX];
    char work_error[96];
    char cache_error[96];
    document_note_request_t *note;
    bool metadata_only;
    document_bitmap_t cover;
    epd_compute_pipe_host_t pipe;
    document_remote_host_t *remote;
    bool converting, observing, open_pending;
    rt_tick_t open_retry, open_started;
    unsigned open_backoff;
    unsigned finish_stage;
    epd_compute_wait_t compute_wait;
    epd_reader_render_t image_render;
    epd_compute_buffer_t image_packed;
    document_bitmap_t image_retained;
    bool cancelled, claimed, stopped, released, cleanup_done, terminal_logged;
    unsigned cleanup_reported;
    reader_error_t failure_code;
    document_status_t status;
};
static document_t *documents;
static document_t *worker_document;
static document_t *conversion_document;
static document_t *note_conversion_document;
static epd_service_t *worker_service;
static bool worker_stopping;
static uint32_t sequence;
static uint32_t flow_sequence;
enum { DOCUMENT_NAV_PENDING = 1, DOCUMENT_NAV_ACTIVE, DOCUMENT_NAV_DONE, DOCUMENT_NAV_FAILED };
static bool persistence_allowed(void);
static bool navigation_allowed(document_t *d);
static bool flow_allows(document_t *d);
static document_t *flow_owner(void);
static document_image_request_t *image_requests;
static document_record_request_t *record_requests;
static unsigned record_allocating;
static document_save_t *saves;
/* Payloads are allocated by the document worker only. storage_lock protects
 * slot ownership and copies; readers never retain pointers into these slots. */
static document_content_t content_slots[DOCUMENT_CONTENT_SLOTS];
static uint64_t content_stamp;
static document_t *content_target;
static document_t *prefetch_document;
static uint32_t content_payload;
static uint32_t worker_revision;

/* A worker slice can consume input without changing anything a view can use. */
static void worker_changed(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    ++worker_revision;
    rt_hw_interrupt_enable(key);
}
uint32_t document_worker_revision(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    uint32_t revision = worker_revision;
    rt_hw_interrupt_enable(key);
    return revision;
}
static struct {
    uint32_t sequence, ticket;
    size_t target, released, available;
    int result;
    bool resume;
} reclaim;
static bool reclaim_work(void);
static void image_work(document_t *only);
static void save_work(void);
static void save_release(document_save_t *job);
static bool image_pending(document_t *d);
static bool image_referenced(document_t *d);
static void image_collect(void);
static void readers_close(document_t *d);
static unsigned cache_inputs_release(void);
static bool cancelled(void *user);

static bool open_pressure(int error)
{
    return error == ENOMEM || error == -ENOMEM || error == EMFILE || error == -EMFILE ||
           error == ENFILE || error == -ENFILE;
}

/* storage_lock keeps cursor I/O out while idle descriptors are reclaimed. */
static int reader_file_open(document_t *d, const char *path, int flags)
{
    int fd = open(path, flags, 0);
    if (fd >= 0 || !open_pressure(errno) || cancelled(d)) return fd;
    int first_error = errno;
    uint32_t total = 0, used = 0;
    rt_memory_info(&total, &used, NULL);
    unsigned released = cache_inputs_release();
    if (released && !cancelled(d)) { errno = 0; fd = open(path, flags, 0); }
    int final_error = fd >= 0 ? 0 : cancelled(d) ? ECANCELED : released ? errno : first_error;
    rt_kprintf("[document] open_pressure id=%lu errno=%d heap_free=%lu released=%u recovered=%u final_errno=%d path=%s\n",
        (unsigned long)d->serial, first_error, (unsigned long)(total >= used ? total - used : 0),
        released, fd >= 0, final_error, path);
    errno = final_error;
    return fd;
}
static void content_work(document_t *d);
static bool content_pending(document_t *d);
static void content_dispose(document_t *d);
static void prefetch_release(document_t *d);
static bool record_work(void);
static bool record_work_priority(bool foreground_only);
static void record_collect(void);
static bool record_referenced(document_t *d);
static bool index_work(void);
static void cleanup_work(document_t *d);
static bool location_append(document_t *d, uint32_t record, uint32_t length);
static int metadata_apply(document_t *d, const uint8_t *data, size_t length, uint32_t record);
static void adjacent_work(document_t *d);
static bool compute_cleanup(document_t *d, bool close_pipe);
static void convert_step(document_t *d);
static bool conversion_runnable(document_t *d);
static void log_terminal(document_t *d);
static void log_cleanup_pending(document_t *d, unsigned kind, int result);
static void source_profile_publish(document_t *d);
static void source_profile_content(document_t *d, uint32_t end);

static void *allocate(size_t bytes) { return epd_app_alloc(bytes, EPD_APP_PSRAM); }

static bool chapter_format(document_t *d)
{
    return d->format == DOCUMENT_EPUB || d->format == DOCUMENT_SPLIT_EPUB;
}

static bool chapter_root(const char *id, const char *title, unsigned level)
{
    return id && *id && !strchr(id, '#') && (!title || !*title) && !level;
}

/* Single writer; allocation is outside the short publication critical section.
 * The first chapter includes navigation declarations preceding its root. */
static void chapter_append(document_t *d, uint32_t offset)
{
    if (d->chapter_failed) return;
    document_chapters_t *tail = d->chapters_tail, *fresh = NULL;
    if (!tail || tail->count == DOCUMENT_CHAPTER_BATCH)
    {
        if (epd_app_psram_available() >= DOCUMENT_CONTENT_CACHE_RESERVE + sizeof(*fresh) + 64u)
            fresh = allocate(sizeof(*fresh));
        if (fresh) { memset(fresh, 0, sizeof(*fresh)); }
    }
    rt_base_t key = rt_hw_interrupt_disable();
    if (!tail || tail->count == DOCUMENT_CHAPTER_BATCH)
    {
        if (!fresh) { d->chapter_failed = true; rt_hw_interrupt_enable(key); return; }
        if (tail) tail->next = fresh;
        else { d->chapters = fresh; offset = 0; }
        d->chapters_tail = tail = fresh;
    }
    tail->starts[tail->count++] = offset;
    rt_hw_interrupt_enable(key);
}

static bool extension(const char *path, const char *suffix)
{
    const char *p = strrchr(path, '.');
    if (!p) return false;
    while (*p && *suffix && tolower((unsigned char)*p) == *suffix) { ++p; ++suffix; }
    return !*p && !*suffix;
}

document_format_t document_format(const char *path, bool directory)
{
    return book_format_detect(path, directory);
}

const char *document_format_name(document_format_t format)
{
    return book_format_name(format);
}

static uint32_t hash_bytes(uint32_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

bool document_split_identity(const char *path, uint32_t *size, uint32_t *signature)
{
    char child[BOOKSHELF_PATH_MAX];
    if (snprintf(child, sizeof(child), "%s/basepackage.zip", path) >= (int)sizeof(child)) return false;
    struct stat st;
    storage_lock();
    uint32_t generation = storage_card_session();
    storage_volume_t volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    uint32_t revision = storage_volume_revision(volume);
    bool mounted = storage_path_available(path) && !storage_changing();
    DIR *dir = mounted && !stat(child, &st) && S_ISREG(st.st_mode) ? opendir(path) : NULL;
    storage_unlock();
    if (!dir) return false;
    uint32_t tag = 0, total = 0, chapters = 0;
    struct dirent *entry;
    bool valid = true;
    while (valid)
    {
        storage_lock();
        if (!storage_path_available(path) || storage_changing() || generation != storage_card_session() ||
            revision != storage_volume_revision(volume))
        { storage_unlock(); valid = false; break; }
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) valid = false; storage_unlock(); break; }
        if (entry->d_name[0] == '.' ||
            (!extension(entry->d_name, ".zip") && strcmp(entry->d_name, "meta.json"))) { storage_unlock(); continue; }
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
            stat(child, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            (uint64_t)st.st_size > UINT32_MAX - total) { storage_unlock(); valid = false; break; }
        storage_unlock();
        uint32_t content = (uint32_t)st.st_mtime;
        if (!strcmp(storage_path_root(path), STORAGE_FLASH_ROOT) &&
            !document_cache_file_signature(child, &content)) { valid = false; break; }
        uint32_t values[] = {(uint32_t)st.st_size, content};
        tag += hash_bytes(hash_bytes(2166136261u, entry->d_name, strlen(entry->d_name)), values, sizeof(values));
        total += (uint32_t)st.st_size;
        if (extension(entry->d_name, ".zip") && strcmp(entry->d_name, "basepackage.zip")) ++chapters;
    }
    storage_lock();
    closedir(dir);
    if (!storage_path_available(path) || storage_changing() ||
        generation != storage_card_session() || revision != storage_volume_revision(volume)) valid = false;
    storage_unlock();
    if (size) *size = total;
    if (signature) *signature = tag;
    return valid && chapters;
}

static bool media_valid(const char *path, uint32_t generation)
{
    if (!path[0]) return true;
    return storage_path_available(path) &&
        (strcmp(storage_path_root(path), STORAGE_SD_ROOT) ||
         (generation == storage_card_session() && !storage_changing()));
}
/* A stable volume cannot have changed its source path. Keep media and user
 * cancellation checks live; only reuse the successful path-token comparison. */
static bool source_unchanged(document_t *d)
{
    if (!d->body_first) return true;
    storage_volume_t volume = !strcmp(storage_path_root(d->file.path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    uint32_t revision = storage_volume_revision(volume);
    rt_base_t key = rt_hw_interrupt_disable();
    bool known = revision && revision == d->source_volume_revision;
    rt_hw_interrupt_enable(key);
    if (known) return true;
    if (d->source_revision != storage_path_revision(d->file.path)) return false;
    key = rt_hw_interrupt_disable();
    d->source_volume_revision = revision;
    rt_hw_interrupt_enable(key);
    return true;
}

static bool cancelled(void *user)
{
    document_t *d = user;
    if (d->cancelled || (worker_service && epd_service_cancelled(worker_service)) ||
        !source_unchanged(d) ||
        (d->source.cancelled && d->source.cancelled(d->source.user)) ||
        !media_valid(d->file.path, d->generation) ||
        (d->source.external && !media_valid(d->directory, d->cache_generation))) return true;
    /* Read the cache path and its media session as one publication. */
    rt_base_t key = rt_hw_interrupt_disable();
    bool valid = media_valid(d->cache, d->cache_generation);
    rt_hw_interrupt_enable(key);
    return !valid;
}

static void compute_cancel(document_t *d)
{
    /* Caller holds the short publication critical section. */
    d->target_required = d->nav_requested = false;
    document_cache_cancel(d->cached);
    epd_compute_wait_cancel(d->compute_wait);
    if (d->pipe) epd_compute_pipe_cancel(d->pipe);
    if (d->image_render) epd_reader_render_cancel(d->image_render);
}

static bool image_cleanup(document_t *d)
{
    if (!d->image_retained.memory) return true;
    rt_base_t key = rt_hw_interrupt_disable();
    epd_reader_render_t render = d->image_render; d->image_render = NULL;
    rt_hw_interrupt_enable(key);
    int rc = epd_reader_render_shutdown(&render);
    if (rc != EPD_COMPUTE_OK)
    {
        key = rt_hw_interrupt_disable(); d->image_render = render; rt_hw_interrupt_enable(key);
        log_cleanup_pending(d, 2u, rc);
        return false;
    }
    epd_compute_buffer_free(&d->image_packed);
    epd_app_free(d->image_retained.memory);
    memset(&d->image_retained, 0, sizeof(d->image_retained));
    worker_changed();
    return true;
}

static bool compute_cleanup(document_t *d, bool close_pipe)
{
    bool done = image_cleanup(d);
    rt_base_t key = rt_hw_interrupt_disable();
    epd_compute_pipe_host_t pipe = close_pipe ? d->pipe : NULL;
    if (close_pipe) d->pipe = NULL;
    rt_hw_interrupt_enable(key);
    if (epd_compute_pipe_shutdown(&pipe) != EPD_COMPUTE_OK)
    {
        key = rt_hw_interrupt_disable(); d->pipe = pipe; rt_hw_interrupt_enable(key);
        done = false;
    }
    return done;
}
static bool compute_retired_pending(document_t *d)
{
    return d->image_retained.memory != NULL;
}

static void wake_worker(void)
{
    if (worker_service) epd_service_wake(worker_service);
}

uint32_t document_reclaim_request(size_t min_available)
{
    rt_base_t key = rt_hw_interrupt_disable();
    if (!min_available || worker_stopping || !worker_service || epd_service_cancelled(worker_service))
    { rt_hw_interrupt_enable(key); return 0; }
    bool changed = !reclaim.ticket || min_available > reclaim.target;
    if (!reclaim.ticket)
    {
        if (!++reclaim.sequence) ++reclaim.sequence;
        reclaim.ticket = reclaim.sequence;
        reclaim.target = reclaim.released = reclaim.available = 0;
    }
    if (changed) { reclaim.target = min_available; reclaim.result = 0; }
    uint32_t ticket = reclaim.ticket;
    rt_hw_interrupt_enable(key);
    if (changed) wake_worker();
    return ticket;
}

int document_reclaim_poll(uint32_t ticket, size_t *released_bytes, size_t *available_bytes)
{
    rt_base_t key = rt_hw_interrupt_disable();
    bool valid = ticket && ticket == reclaim.ticket && !worker_stopping;
    if (released_bytes) *released_bytes = valid ? reclaim.released : 0;
    if (available_bytes) *available_bytes = valid ? reclaim.available : 0;
    int result = valid ? reclaim.result : -1;
    rt_hw_interrupt_enable(key);
    return result;
}

void document_reclaim_finish(uint32_t ticket)
{
    rt_base_t key = rt_hw_interrupt_disable();
    bool valid = ticket && ticket == reclaim.ticket;
    if (valid) { reclaim.ticket = 0; reclaim.resume = true; }
    rt_hw_interrupt_enable(key);
    if (valid) wake_worker();
}

static bool content_memory_held(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    bool held = reclaim.ticket || reclaim.resume;
    rt_hw_interrupt_enable(key);
    return held;
}

static reader_error_t open_error;
reader_error_t document_open_error(void) { return open_error; }

static unsigned elapsed_ms(rt_tick_t since)
{
    return (unsigned)((uint64_t)(rt_tick_t)(rt_tick_get() - since) * 1000 / RT_TICK_PER_SECOND);
}
static void log_cleanup_pending(document_t *d, unsigned kind, int result)
{
    if (d->cleanup_reported & kind) return;
    d->cleanup_reported |= kind;
    rt_kprintf("[document] cleanup_pending id=%lu kind=%u result=%d elapsed_ms=%u\n",
               (unsigned long)d->serial, kind, result, elapsed_ms(d->opened_at));
}
static void log_terminal(document_t *d)
{
    rt_base_t key = rt_hw_interrupt_disable();
    bool emit = d->status.complete && !d->terminal_logged;
    if (emit) d->terminal_logged = true;
    int result = d->status.result;
    uint32_t end = d->status.end;
    rt_hw_interrupt_enable(key);
    if (emit) rt_kprintf("[document] terminal id=%lu result=%d error=%u end=%lu elapsed_ms=%u cancelled=%u\n",
        (unsigned long)d->serial, result, (unsigned)d->status.error_code,
        (unsigned long)end, elapsed_ms(d->opened_at), (unsigned)cancelled(d));
}

static void log_readable(document_t *d)
{
    if (d->readable_logged || !d->status.end) return;
    d->readable_logged = true;
    rt_kprintf("[document] first_readable id=%lu elapsed_ms=%u end=%lu cache=%u\n",
               (unsigned long)d->serial, elapsed_ms(d->opened_at),
               (unsigned long)d->status.end, (unsigned)d->persistent);
}

static bool cache_lookup(document_t *d)
{
    rt_tick_t started = rt_tick_get();
    bool hit = d->cached && document_cache_lookup(d->cached, d->status.title, sizeof(d->status.title), &d->end);
    rt_kprintf("[document] cache_lookup id=%lu elapsed_ms=%u available=%u hit=%u\n",
               (unsigned long)d->serial, elapsed_ms(started), (unsigned)(d->cached != NULL), (unsigned)hit);
    return hit;
}

static void log_open(document_t *d)
{
    rt_kprintf("[document] open id=%lu elapsed_ms=%u format=%u cache=%u error=%u path=%s\n",
               (unsigned long)d->serial, elapsed_ms(d->opened_at), (unsigned)d->format,
               (unsigned)d->persistent, (unsigned)open_error, d->file.path);
}

static int cache_failure(document_t *d, const char *operation, int error)
{
    if (error < 0) error = -error;
    if (!d->cache_error[0])
    {
        snprintf(d->cache_error, sizeof(d->cache_error), "%s", error == ENOSPC ?
                 "文档缓存存储空间不足，请释放空间后重开" : "文档缓存写入失败，请检查存储后重开");
        rt_kprintf("[document] cache %s failed: path=%s offset=%u errno=%d\n",
                   operation, d->cache, d->end, error);
    }
    d->cache_result = error == ENOSPC ? BOOK_NO_SPACE : BOOK_IO_ERROR;
    return d->cache_result;
}

static int cache_room(document_t *d, size_t length)
{
    /* DFS uses signed 32-bit file offsets on this target. */
    if (d->end <= INT32_MAX && length <= (uint32_t)INT32_MAX - d->end) return BOOK_OK;
    snprintf(d->cache_error, sizeof(d->cache_error), "文档缓存超出文件寻址范围");
    return d->cache_result = BOOK_UNSUPPORTED;
}

static int write_direct(document_t *d, const void *data, size_t length)
{
    if (cancelled(d)) return BOOK_CANCELLED;
    const uint8_t *p = data;
    while (length)
    {
        size_t chunk = length > 4096 ? 4096 : length;
        storage_lock();
        if (cancelled(d)) { storage_unlock(); return BOOK_CANCELLED; }
        errno = 0;
        int n = write(d->output, p, chunk);
        int error = errno;
        storage_unlock();
        if (n <= 0 && (error == ENOSPC || error == -ENOSPC) && !d->space_retried && d->cached)
        {
            d->space_retried = true;
            if (document_cache_reclaim(d->cached)) continue;
        }
        if (n <= 0) return cache_failure(d, "write", error);
        p += n; length -= n;
        if (cancelled(d)) return BOOK_CANCELLED;
    }
    return BOOK_OK;
}

static int write_flush(document_t *d)
{
    if (!d->write_used) return BOOK_OK;
    int result = write_direct(d, d->write_buffer, d->write_used);
    /* A failed/partial flush makes this conversion unusable; never replay it. */
    d->write_used = 0;
    return result;
}

static int write_all(document_t *d, const void *data, size_t length)
{
    if (cancelled(d)) return BOOK_CANCELLED;
    int result = cache_room(d, length);
    if (result != BOOK_OK) return result;
    const uint8_t *p = data;
    while (length)
    {
        size_t n = length;
        if (!d->write_buffer || (!d->write_used && length >= DOCUMENT_IO_WINDOW))
        {
            result = write_direct(d, p, length);
            if (result != BOOK_OK) return result;
        }
        else
        {
            if (n > DOCUMENT_IO_WINDOW - d->write_used) n = DOCUMENT_IO_WINDOW - d->write_used;
            memcpy(d->write_buffer + d->write_used, p, n);
            d->write_used += n;
            if (d->write_used == DOCUMENT_IO_WINDOW && (result = write_flush(d)) != BOOK_OK) return result;
        }
        d->end += n; p += n; length -= n;
    }
    return BOOK_OK;
}

static int cache_publish(document_t *d, bool force)
{
    uint32_t bytes = d->pending_end - d->status.end;
    if (!bytes) return BOOK_OK;
    rt_tick_t now = rt_tick_get();
    uint32_t batch = d->status.end ? 8192 : 2048;
    if (d->nav_state == DOCUMENT_NAV_ACTIVE) batch = 64u * 1024u;
    if (!force && bytes < batch && (d->nav_state == DOCUMENT_NAV_ACTIVE ||
        now - d->last_publish < rt_tick_from_millisecond(50))) return BOOK_OK;
    int flushed = write_flush(d);
    if (flushed != BOOK_OK) return flushed;
    storage_lock();
    int result = BOOK_CANCELLED;
    if (!cancelled(d))
    {
        int output = d->output;
        errno = 0;
        int synced = fsync(output), error = errno;
        if (synced && (error == ENOSPC || error == -ENOSPC) && !d->space_retried && d->cached)
        {
            document_cache_t *cached = d->cached;
            d->space_retried = true;
            /* The document worker retains the spool/cache during reclamation.
             * Metadata computation must not inherit this outer storage lock. */
            storage_unlock();
            bool reclaimed = document_cache_reclaim(cached);
            storage_lock();
            if (cancelled(d) || d->output != output || d->cached != cached)
            { storage_unlock(); return BOOK_CANCELLED; }
            if (reclaimed) { errno = 0; synced = fsync(output); error = errno; }
        }
        result = synced == 0 ? BOOK_OK : cache_failure(d, "sync", error);
    }
    storage_unlock();
    if (result != BOOK_OK) return result;
    rt_base_t key = rt_hw_interrupt_disable();
    d->status.end = d->pending_end;
    ++d->status.publication;
    worker_changed();
    if (d->body_end && d->nav_state != DOCUMENT_NAV_ACTIVE) d->body_publication = d->status.publication;
    rt_hw_interrupt_enable(key);
    d->last_publish = rt_tick_get();
    if (worker_document == d && worker_service) epd_service_ui_request(worker_service, 0);
    source_profile_publish(d);
    log_readable(d);
    /* Only complete, synced records are visible to cooperative pixel jobs. */
    if (worker_document == d)
    {
        record_work();
        bool computed = reader_compute_worker_step(worker_service);
        image_work(d);
        if (!computed) content_work(d);
    }
    return BOOK_OK;
}

static int record_complete(document_t *d)
{
    d->pending_end = d->end;
    return cache_publish(d, false);
}

static int record_write(document_t *d, unsigned kind, unsigned style, const void *data, size_t length)
{
    if (length > INT32_MAX - sizeof(document_record_t)) return BOOK_UNSUPPORTED;
    int room = cache_room(d, sizeof(document_record_t) + length);
    if (room != BOOK_OK) return room;
    document_record_t header = {kind, (uint32_t)length, style, d->link};
    int result = write_all(d, &header, sizeof(header));
    if (result == BOOK_OK && length) result = write_all(d, data, length);
    if (result == BOOK_OK && kind == DOCUMENT_TEXT && length) source_profile_content(d, d->end);
    if (result == BOOK_OK) result = record_complete(d);
    return result;
}

static int sink_text(void *user, const char *text, size_t length, unsigned style)
{
    document_t *d = user;
    while (length)
    {
        size_t n = length > DOCUMENT_TEXT_LIMIT ? DOCUMENT_TEXT_LIMIT : length;
        if (n < length) while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        if (!n) return BOOK_ERROR;
        if (d->segmented && !location_append(d, d->end, n)) return BOOK_NO_MEMORY;
        int result = record_write(d, DOCUMENT_TEXT, style, text, n);
        if (result != BOOK_OK) return result;
        text += n; length -= n;
        style &= ~BOOK_STYLE_PAGE_START;
    }
    return BOOK_OK;
}
static int sink_paragraph(void *user) { return record_write(user, DOCUMENT_PARAGRAPH, 0, NULL, 0); }
static void toc_release(document_toc_t *toc)
{
    if (!toc) return;
    rt_base_t key = rt_hw_interrupt_disable();
    bool last = --toc->references == 0;
    rt_hw_interrupt_enable(key);
    if (!last) return;
    while (toc->first) { document_navigation_t *n = toc->first; toc->first = n->next; epd_app_free(n); }
    while (toc->parts) { document_part_t *p = toc->parts; toc->parts = p->next; epd_app_free(p); }
    epd_app_free(toc);
}

static document_navigation_t *toc_head(document_t *d)
{ return d->toc_context && d->toc_context->count ? d->toc_context->first : d->navigation; }

static bool toc_complete(const document_t *d)
{ return d->toc_context ? d->toc_context->complete : d->nav_state == DOCUMENT_NAV_DONE; }

static void toc_finish(document_toc_t *toc)
{ if (toc && !toc->complete) { toc->complete = true; ++toc->revision; } }

static int toc_append(document_toc_t *toc, const book_document_event_t *e, const uint8_t *data,
                      document_navigation_t **position)
{
    if (!e->length2) return BOOK_OK;
    size_t length = e->length1 + e->length2 + 2;
    /* Each producer keeps its accepted position; only an overlapping replay
     * compares an existing entry. Equal labels at different ordinals survive. */
    document_navigation_t *n = *position ? (*position)->next : toc->first;
    if (n) {
        if (n->level != e->c || n->title_at != e->length1 + 1 ||
            strlen(n->text + n->title_at) != e->length2 || memcmp(n->text, data, e->length1) ||
            memcmp(n->text + n->title_at, data + e->length1, e->length2)) return BOOK_ERROR;
        if (e->b != UINT32_MAX && n->chapter != e->b) {
            rt_base_t key = rt_hw_interrupt_disable();
            n->chapter = e->b; ++toc->revision;
            rt_hw_interrupt_enable(key); worker_changed();
        }
        *position = n;
        return BOOK_OK;
    }
    size_t bytes = sizeof(document_navigation_t) + length;
    if (bytes > DOCUMENT_TOC_BUDGET - toc->bytes) return BOOK_NO_MEMORY;
    n = allocate(bytes);
    if (!n) return BOOK_NO_MEMORY;
    memset(n, 0, sizeof(*n)); n->kind = DOCUMENT_ANCHOR; n->explicit_toc = true;
    n->chapter = e->b; n->level = e->c; n->title_at = e->length1 + 1; n->title_record = UINT32_MAX;
    memcpy(n->text, data, e->length1); n->text[e->length1] = 0;
    memcpy(n->text + n->title_at, data + e->length1, e->length2); n->text[length - 1] = 0;
    rt_base_t key = rt_hw_interrupt_disable();
    if (toc->tail) toc->tail->next = n; else toc->first = n;
    toc->tail = n; toc->bytes += (unsigned)bytes; ++toc->count; ++toc->revision;
    *position = n;
    rt_hw_interrupt_enable(key);
    worker_changed(); return BOOK_OK;
}

static document_navigation_t *navigation_append(document_t *d, uint32_t offset, uint32_t kind, uint32_t level,
                                                const char *data, size_t length, bool explicit_toc)
{
    const char *first = memchr(data, 0, length);
    if (!first) return NULL;
    size_t title_at = (size_t)(first - data) + 1;
    if (kind == DOCUMENT_ANCHOR && (title_at >= length || !memchr(data + title_at, 0, length - title_at)))
        return NULL;
    bool named = kind == DOCUMENT_ANCHOR && data[title_at] != 0;
    uint32_t title_record = UINT32_MAX;
    if (named) {
        for (document_navigation_t *p = d->navigation; p; p = p->next) {
            if (p->kind != DOCUMENT_ANCHOR || strcmp(p->text, data)) continue;
            if (p->explicit_toc == explicit_toc && p->level == level &&
                !strcmp(p->text + p->title_at, data + title_at)) return p;
            if (!p->text[p->title_at] && title_record == UINT32_MAX) title_record = p->record;
        }
    }
    size_t bytes = sizeof(document_navigation_t) + length;
    if (bytes > DOCUMENT_NAVIGATION_BUDGET - d->navigation_bytes) return NULL;
    document_navigation_t *n = allocate(bytes);
    if (!n) return NULL;
    n->next = NULL; n->record = offset; n->kind = kind; n->level = level;
    n->title_at = kind == DOCUMENT_ANCHOR ? (uint32_t)title_at : 0;
    n->chapter = UINT32_MAX;
    n->explicit_toc = explicit_toc;
    n->title_record = title_record;
    memcpy(n->text, data, length);
    /* Resolve once on the document worker; page headers then need no I/O. */
    if (kind == DOCUMENT_ANCHOR && !named) {
        for (document_navigation_t *p = d->navigation; p; p = p->next) {
            if (p->kind != DOCUMENT_ANCHOR || strcmp(p->text, n->text)) continue;
            if (p->text[p->title_at] && p->title_record == UINT32_MAX)
                p->title_record = offset;
        }
    }
    rt_base_t key = rt_hw_interrupt_disable();
    if (d->navigation_tail) d->navigation_tail->next = n;
    else d->navigation = n;
    d->navigation_tail = n;
    d->navigation_bytes += (unsigned)bytes;
    if (kind == DOCUMENT_ANCHOR && n->text[title_at]) ++d->toc_count;
    if (named && explicit_toc) ++d->explicit_toc_count;
    ++d->navigation_revision;
    worker_changed();
    rt_hw_interrupt_enable(key);
    return n;
}
static bool navigation_is_toc(const document_t *d, const document_navigation_t *n)
{
    return n->kind == DOCUMENT_ANCHOR && n->text[n->title_at] &&
           (!d->body_first || (toc_complete(d) && !d->chapter_restore) || n->explicit_toc) &&
           (!(d->toc_context ? d->toc_context->count : d->explicit_toc_count) || n->explicit_toc);
}
static int sink_anchor(void *user, const char *id, const char *title, unsigned level)
{
    document_t *d = user;
    char data[1024];
    size_t a = id ? strlen(id) : 0, b = title ? strlen(title) : 0;
    if (a + b + 2 > sizeof(data)) return BOOK_UNSUPPORTED;
    memcpy(data, id ? id : "", a + 1);
    memcpy(data + a + 1, title ? title : "", b + 1);
    /* Publish the boundary before record_complete can yield to layout work.
     * Queries ignore offsets outside the synced, published prefix. */
    if (chapter_format(d) && chapter_root(id, title, level)) chapter_append(d, d->end);
    if (!navigation_append(d, d->end, DOCUMENT_ANCHOR, level, data, a + b + 2, false)) return BOOK_NO_MEMORY;
    return record_write(user, DOCUMENT_ANCHOR, level, data, a + b + 2);
}
static int sink_link(void *user, const char *target, bool begin)
{
    document_t *d = user;
    if (!begin) { d->link = 0; return BOOK_OK; }
    if (!target || strlen(target) >= 1024) return BOOK_UNSUPPORTED;
    uint32_t offset = d->end;
    if (!navigation_append(d, offset, DOCUMENT_LINK, 0, target, strlen(target) + 1, false)) return BOOK_NO_MEMORY;
    int result = record_write(d, DOCUMENT_LINK, 0, target, strlen(target) + 1);
    if (result == BOOK_OK) d->link = offset + 1;
    return result;
}
static int sink_metadata(void *user, const char *title, const char *author)
{
    (void)author;
    document_t *d = user;
    if (title && *title && strncmp(d->status.title, title, sizeof(d->status.title) - 1))
    {
        rt_base_t key = rt_hw_interrupt_disable();
        snprintf(d->status.title, sizeof(d->status.title), "%s", title);
        worker_changed();
        rt_hw_interrupt_enable(key);
    }
    return cancelled(d) ? BOOK_CANCELLED : BOOK_OK;
}
static bool location_append(document_t *d, uint32_t record, uint32_t length)
{
    if (sizeof(document_location_t) > DOCUMENT_NAVIGATION_BUDGET - d->navigation_bytes) return false;
    document_location_t *n = allocate(sizeof(*n));
    if (!n) return false;
    n->next = NULL; n->record = record; n->bytes = length; n->anchor = d->source_current;
    rt_base_t key = rt_hw_interrupt_disable();
    if (d->locations_tail) d->locations_tail->next = n; else d->locations = n;
    d->locations_tail = n; d->navigation_bytes += sizeof(*n);
    d->source_current.skip_utf8 += length;
    rt_hw_interrupt_enable(key);
    return true;
}

static int metadata_apply(document_t *d, const uint8_t *data, size_t length, uint32_t record)
{
    book_document_event_t e;
    if (length < sizeof(e)) return BOOK_ERROR;
    memcpy(&e, data, sizeof(e));
    if (e.bytes != length || e.length1 > length - sizeof(e) ||
        e.length2 > length - sizeof(e) - e.length1 ||
        e.length1 + e.length2 + 2 > 2 * DOCUMENT_TEXT_LIMIT + 2) return BOOK_ERROR;
    if (e.type == BOOK_DOCUMENT_SOURCE_ANCHOR)
    {
        if ((e.d & 0xff) != BOOK_DOCUMENT_SOURCE_FILE_BYTE &&
            (e.d & 0xff) != BOOK_DOCUMENT_SOURCE_BODY_UTF8) return BOOK_ERROR;
        d->source_current = (document_anchor_t){e.d & 0xff, e.a, e.b, 0};
        if (e.d & BOOK_DOCUMENT_SOURCE_BEGIN) {
            d->source_begin = e.b;
            d->body_start = record + sizeof(document_record_t) + length;
            if (d->body_first && !location_append(d, d->body_start, 0)) return BOOK_NO_MEMORY;
        }
        if (e.d & BOOK_DOCUMENT_SOURCE_END)
        {
            d->source_next = e.b;
            if ((e.d & 0xff) == BOOK_DOCUMENT_SOURCE_BODY_UTF8) {
                d->source_total = e.b;
                d->body_end = record + sizeof(document_record_t) + length;
                if (d->chapter_restore) d->body_publication = d->status.publication;
            }
        }
        if (e.d & BOOK_DOCUMENT_SOURCE_TARGET)
        {
            bool from_begin = (e.d & BOOK_DOCUMENT_SOURCE_TARGET_BEGIN) != 0;
            if (from_begin && (!d->body_start || e.b)) return BOOK_ERROR;
            d->target_record = from_begin ? d->body_start : record + sizeof(document_record_t) + length;
            d->target_byte = 0;
            if (!from_begin && !location_append(d, d->target_record, 0)) return BOOK_NO_MEMORY;
            d->target_ready = true;
            worker_changed();
        }
        return BOOK_OK;
    }
    if (e.type == BOOK_DOCUMENT_CHAPTER)
    {
        if (!e.b || e.a >= e.b || e.b > 8192) return BOOK_ERROR;
        d->chapter_count = e.b;
        if (e.c) d->chapter = e.a;
        document_toc_t *toc = d->toc_context;
        document_part_t *previous = NULL, *part = NULL;
        if (toc) {
            part = toc->parts;
            if (toc->part_hint && toc->part_hint->number <= e.a) part = toc->part_hint;
            while (part && part->number < e.a) { previous = part; part = part->next; }
            if (part && part->number == e.a) {
                toc->part_hint = part;
                return strlen(part->text) == e.length1 &&
                    !memcmp(part->text, data + sizeof(e), e.length1) ? BOOK_OK : BOOK_ERROR;
            }
        }
        size_t bytes = sizeof(document_part_t) + e.length1 + e.length2 + 2;
        if (bytes > (toc ? DOCUMENT_TOC_BUDGET - toc->bytes : DOCUMENT_NAVIGATION_BUDGET - d->navigation_bytes))
            return BOOK_NO_MEMORY;
        document_part_t *n = allocate(bytes);
        if (!n) return BOOK_NO_MEMORY;
        n->next = NULL; n->number = e.a; n->title_at = e.length1 + 1;
        memcpy(n->text, data + sizeof(e), e.length1); n->text[e.length1] = 0;
        memcpy(n->text + n->title_at, data + sizeof(e) + e.length1, e.length2);
        n->text[n->title_at + e.length2] = 0;
        if (toc) {
            rt_base_t key = rt_hw_interrupt_disable();
            n->next = part;
            if (previous) previous->next = n; else toc->parts = n;
            if (!part) toc->parts_tail = n;
            toc->part_hint = n; toc->bytes += (unsigned)bytes; ++toc->revision;
            rt_hw_interrupt_enable(key);
        } else {
            if (d->parts_tail) d->parts_tail->next = n; else d->parts = n;
            d->parts_tail = n; d->navigation_bytes += bytes;
        }
        worker_changed();
        return BOOK_OK;
    }
    if (e.type == BOOK_DOCUMENT_METADATA && e.a == BOOK_DOCUMENT_META_TOC)
    {
        if (d->toc_context) return toc_append(d->toc_context, &e, data + sizeof(e),
            d->nav_state == DOCUMENT_NAV_ACTIVE ? &d->toc_context->nav_tail : &d->toc_restore_tail);
        size_t bytes = e.length1 + e.length2 + 2;
        char *text = allocate(bytes);
        if (!text) return BOOK_NO_MEMORY;
        memcpy(text, data + sizeof(e), e.length1); text[e.length1] = 0;
        memcpy(text + e.length1 + 1, data + sizeof(e) + e.length1, e.length2);
        text[bytes - 1] = 0;
        document_navigation_t *n = navigation_append(d, record, DOCUMENT_ANCHOR, e.c, text, bytes, true);
        if (n && e.b != UINT32_MAX) n->chapter = e.b;
        epd_app_free(text);
        return n ? BOOK_OK : BOOK_NO_MEMORY;
    }
    return BOOK_ERROR;
}
static void result_locked(document_t *d, int result)
{
    if (d->status.result != result) worker_changed();
    d->status.result = result;
    d->status.error_code = result == BOOK_OK ? READER_ERROR_NONE :
        result == BOOK_CANCELLED ? READER_ERROR_CANCELLED :
        result == BOOK_NO_MEMORY ? READER_ERROR_CONVERT_MEMORY :
        result == BOOK_NO_SPACE ? READER_ERROR_NO_SPACE :
        result == BOOK_UNSUPPORTED ? (d->cache_error[0] ? READER_ERROR_CACHE_RANGE : READER_ERROR_UNSUPPORTED) :
        result == BOOK_IO_ERROR ? (d->cache_error[0] ? READER_ERROR_CACHE_WRITE : READER_ERROR_IO) :
        READER_ERROR_PARSE;
    if (result == BOOK_ERROR && d->failure_code) d->status.error_code = d->failure_code;
    if (result == BOOK_OK) return;
    const char *message = result == BOOK_CANCELLED ? "书籍读取已中断，请重新打开" :
        result == BOOK_NO_MEMORY ? "文档处理内存申请失败" :
        result == BOOK_NO_SPACE ? "文档缓存存储空间不足" :
        result == BOOK_UNSUPPORTED ? "文档包含不支持的内容或编码" :
        result == BOOK_IO_ERROR ? "无法读取书籍或写入文档缓存" : "文档损坏或解析失败";
    snprintf(d->status.error, sizeof(d->status.error), "%s", d->cache_error[0] ? d->cache_error :
             d->work_error[0] ? d->work_error : message);
}
static bool bitmap_allocate(document_bitmap_t *result, unsigned width, unsigned height)
{
    if (!width || !height || width > (UINT16_MAX - 3u) / 2u || height > UINT16_MAX) return false;
    unsigned stride = RT_ALIGN(width * 2, 4);
    if (height > (SIZE_MAX - 31u) / stride) return false;
    result->memory = allocate(RT_ALIGN(stride * height, EPD_COMPUTE_CACHE_LINE) + EPD_COMPUTE_CACHE_LINE - 1);
    if (!result->memory) return false;
    result->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    result->image.header.cf = LV_COLOR_FORMAT_RGB565;
    result->image.header.w = width;
    result->image.header.h = height;
    result->image.header.stride = stride;
    result->image.data = (uint8_t *)RT_ALIGN((uintptr_t)result->memory, 32);
    result->image.data_size = stride * height;
    return true;
}
static void bitmap_clean(document_bitmap_t *bitmap)
{
    lv_draw_buf_t buffer = {.data = (uint8_t *)bitmap->image.data, .data_size = bitmap->image.data_size};
    epd_app_clean_draw_buffer(&buffer);
}
static bool cache_open(document_t *d)
{
    rt_tick_t started = rt_tick_get(), stage;
    unsigned lock_ms, mkdir_ms, attempts = 0;
    storage_lock();
    lock_ms = elapsed_ms(started);
    if (cancelled(d)) { storage_unlock(); return false; }
    char directory[STORAGE_PATH_MAX];
    const char *bucket = document_cache_bucket(d->cached);
    int named;
    if (bucket) named = snprintf(directory, sizeof(directory), "%s", bucket);
    else if (d->source.external)
        named = snprintf(directory, sizeof(directory), "%s/doccache/loose", d->directory);
    else {
        if (!document_cache_directory(&d->file, "books", directory, sizeof(directory)))
        { storage_unlock(); return false; }
        size_t n = strlen(directory);
        if (n + sizeof("/loose") > sizeof(directory)) { storage_unlock(); return false; }
        memcpy(directory + n, "/loose", sizeof("/loose"));
        named = (int)strlen(directory);
    }
    if (named < 0 || (size_t)named >= sizeof(directory)) { storage_unlock(); return false; }
    if (!d->source.external) d->cache_generation = storage_card_session();
    errno = 0;
    stage = rt_tick_get();
    bool ok = storage_mkdirs(directory);
    mkdir_ms = elapsed_ms(stage);
    if (!ok) cache_failure(d, "mkdir", errno);
    stage = rt_tick_get();
    static uint32_t spool_sequence;
    uint32_t entropy[] = {(uint32_t)time(NULL), (uint32_t)started, d->serial, ++spool_sequence};
    uint32_t number = hash_bytes(2166136261u, entropy, sizeof(entropy));
    number = hash_bytes(number, d->app_id, strlen(d->app_id));
    for (; ok && !cancelled(d) && attempts < 9; ++number)
    {
        if (attempts == 8 && !document_cache_temporary_number(directory, ".tmp", &number, cancelled, d)) break;
        /* Never truncate a live or abandoned spool after module/card reload. */
        named = snprintf(d->cache, sizeof(d->cache), "%s/%08lx.tmp", directory,
                         (unsigned long)number);
        if (named < 0 || (size_t)named >= sizeof(d->cache)) break;
        errno = 0;
        ++attempts;
        d->output = reader_file_open(d, d->cache, O_CREAT | O_WRONLY | O_EXCL);
        if (d->output >= 0) { d->temp_owned = true; break; }
        if (errno != EEXIST && errno != -EEXIST) break;
    }
    if (ok && d->output < 0 && !cancelled(d)) cache_failure(d, "open", errno);
    unsigned output_ms = elapsed_ms(stage);
    if (d->output >= 0) d->write_buffer = allocate(DOCUMENT_IO_WINDOW);
    d->last_publish = rt_tick_get();
    storage_unlock();
    rt_kprintf("[document] cache_open id=%lu result=%d elapsed_ms=%u lock_ms=%u mkdir_ms=%u scan_ms=0 entries=0 removed=0 output_ms=%u attempts=%u\n",
        (unsigned long)d->serial, d->output >= 0, elapsed_ms(started), lock_ms, mkdir_ms, output_ms, attempts);
    return d->output >= 0;
}

#define DOCUMENT_RESOURCE_LIMIT 24u
typedef struct {
    int descriptor;
    DIR *directory;
    book_stream_t stream;
    bool custom;
    uint32_t revision, media;
    char path[EPD_COMPUTE_PIPE_PATH];
} document_resource_t;
typedef struct {
    rt_tick_t profile_start, slice_start, slice_end, step_start, step_end, output_start;
    rt_tick_t slice_ticks, step_ticks, gap_ticks, gap_max, step_gap_max, trace_ticks;
    uint32_t first_body_end;
    bool enabled, done, readable;
    bool slice_active, step_active, output_active;
    rt_tick_t read_ticks, wait_ticks, output_ticks;
    uint32_t read_bytes, write_bytes, slices, steps;
} document_source_profile_t;
struct document_remote_host {
    document_t *document;
    document_resource_t resources[DOCUMENT_RESOURCE_LIMIT];
    uint32_t image_id, image_row, image_width, image_height, image_stride;
    uint32_t calls, read_bytes, write_bytes;
    rt_tick_t read_ticks, wait_ticks, output_ticks;
    document_source_profile_t profile;
    bool terminal;
    int result;
    uint32_t steps, slices;
};

static unsigned source_profile_ms(rt_tick_t ticks)
{ return (unsigned)((uint64_t)ticks * 1000u / RT_TICK_PER_SECOND); }

/* STEP includes its service callback; slice includes STEP. These nested wall
 * times must not be added. An in-flight callback is included in the snapshot. */
static void source_profile_snapshot(document_remote_host_t *h)
{
    h->profile.read_ticks = h->read_ticks; h->profile.wait_ticks = h->wait_ticks;
    h->profile.output_ticks = h->output_ticks; h->profile.read_bytes = h->read_bytes;
    h->profile.write_bytes = h->write_bytes; h->profile.slices = h->slices; h->profile.steps = h->steps;
}
static void source_profile_log(document_t *d, document_source_profile_t *p, const char *event, int result)
{
    rt_tick_t now = rt_tick_get(), pending_gap = p->slice_active ? 0 : now - p->slice_end;
    rt_tick_t slice = p->slice_ticks + (p->slice_active ? now - p->slice_start : 0);
    rt_tick_t step = p->step_ticks + (p->step_active ? now - p->step_start : 0);
    rt_tick_t gap = p->gap_ticks + pending_gap;
    rt_tick_t output = p->output_ticks + (p->output_active ? now - p->output_start : 0);
    rt_tick_t gap_max = pending_gap > p->gap_max ? pending_gap : p->gap_max;
    rt_tick_t step_gap = now - p->profile_start - step;
    rt_tick_t step_gap_max = p->step_gap_max;
    if (!p->step_active && now - p->step_end > step_gap_max) step_gap_max = now - p->step_end;
    /* Each call fits the 256-byte console buffer, including maximal counters.
     * All three parts describe the same snapshot; print only at milestones. */
    rt_kprintf("[document] source_profile id=%lu at=%s part=1 elapsed_ms=%u wall_ms=%u slice_ms=%u step_ms=%u gap_ms=%u gap_max_ms=%u\n",
        (unsigned long)d->serial, event, source_profile_ms(now - d->opened_at),
        source_profile_ms(now - p->profile_start), source_profile_ms(slice), source_profile_ms(step),
        source_profile_ms(gap), source_profile_ms(gap_max));
    rt_kprintf("[document] source_profile id=%lu at=%s part=2 stepgap_ms=%u stepgap_max_ms=%u service_ms=%u lock_ms=%u output_ms=%u\n",
        (unsigned long)d->serial, event, source_profile_ms(step_gap), source_profile_ms(step_gap_max),
        source_profile_ms(p->read_ticks), source_profile_ms(p->wait_ticks), source_profile_ms(output));
    rt_kprintf("[document] source_profile id=%lu at=%s part=3 read_bytes=%lu output_bytes=%lu slices=%lu steps=%lu trace_ms=%u result=%d\n",
        (unsigned long)d->serial, event, (unsigned long)p->read_bytes, (unsigned long)p->write_bytes,
        (unsigned long)p->slices, (unsigned long)p->steps, source_profile_ms(p->trace_ticks), result);
    p->trace_ticks += rt_tick_get() - now;
}
static void source_profile_content(document_t *d, uint32_t end)
{
    document_remote_host_t *h = d->remote;
    if (h && h->profile.enabled && !h->profile.first_body_end) h->profile.first_body_end = end;
}
static void source_profile_published(document_t *d, document_source_profile_t *p)
{
    if (!p->enabled || p->done || !d->status.end) return;
    if (!p->readable) {
        p->readable = true;
        source_profile_log(d, p, "readable", BOOK_OK);
    }
    if (p->first_body_end && d->status.end >= p->first_body_end) {
        p->done = true;
        source_profile_log(d, p, "body", BOOK_OK);
    }
}
static void source_profile_publish(document_t *d)
{
    document_remote_host_t *h = d->remote;
    if (!h || !h->profile.enabled || h->profile.done) return;
    source_profile_snapshot(h);
    source_profile_published(d, &h->profile);
}

static void log_stage_timing(const document_t *d, const epd_compute_pipe_message_t *done)
{
    if (!done || done->operation != EPD_PIPE_DONE || done->values[0] != BOOK_DOCUMENT_WIRE_VERSION ||
        done->values[2] != BOOK_DOCUMENT_TIMING_MAGIC || done->values[3] != sizeof(book_document_timing_t)) return;
    book_document_timing_t t;
    memcpy(&t, done->path, sizeof(t));
    if (!t.tick_hz) return;
    rt_kprintf("[document] stages id=%lu result=%ld acpu_hz=%lu parse=%lu/%lu inflate=%lu/%lu image=%lu/%lu svg=%lu/%lu wait=%lu/%lu sat=%lu\n",
        (unsigned long)d->serial, (long)done->status, (unsigned long)t.tick_hz,
        (unsigned long)t.ticks[0], (unsigned long)t.calls[0],
        (unsigned long)t.ticks[1], (unsigned long)t.calls[1],
        (unsigned long)t.ticks[2], (unsigned long)t.calls[2],
        (unsigned long)t.ticks[3], (unsigned long)t.calls[3],
        (unsigned long)t.ticks[4], (unsigned long)t.calls[4], (unsigned long)t.saturated);
}

static bool resource_valid(const document_resource_t *r)
{
    return r->path[0] && storage_path_available(r->path) &&
        (strcmp(storage_path_root(r->path), STORAGE_SD_ROOT) || r->media == storage_card_session()) && r->revision == storage_path_revision(r->path) &&
        !storage_file_writers(r->path);
}

#include "document_entry.inc"
#include "document_metadata.inc"

static int remote_output(document_remote_host_t *host, const uint8_t *data, size_t length)
{
    document_t *d = host->document;
    size_t at = 0;
    while (at < length)
    {
        book_document_event_t e;
        if (length - at < sizeof(e)) return BOOK_ERROR;
        memcpy(&e, data + at, sizeof(e));
        if (e.bytes < sizeof(e) || (e.bytes & 3) || e.bytes > length - at ||
            e.length1 > e.bytes - sizeof(e) || e.length2 > e.bytes - sizeof(e) - e.length1)
            return BOOK_ERROR;
        const uint8_t *payload = data + at + sizeof(e);
        char first[DOCUMENT_TEXT_LIMIT + 1], second[DOCUMENT_TEXT_LIMIT + 1];
        if (e.type != BOOK_DOCUMENT_IMAGE_DATA)
        {
            if (e.length1 > DOCUMENT_TEXT_LIMIT || e.length2 > DOCUMENT_TEXT_LIMIT ||
                memchr(payload, 0, e.length1 + e.length2)) return BOOK_ERROR;
            if (e.type != BOOK_DOCUMENT_TEXT) {
                memcpy(first, payload, e.length1); first[e.length1] = 0;
                memcpy(second, payload + e.length1, e.length2); second[e.length2] = 0;
            }
        }
        int rc = BOOK_ERROR;
        bool navigation_event = e.type == BOOK_DOCUMENT_CHAPTER ||
            (e.type == BOOK_DOCUMENT_METADATA && e.a == BOOK_DOCUMENT_META_TOC);
        if (d->nav_state == DOCUMENT_NAV_ACTIVE && !navigation_event &&
            !(e.type == BOOK_DOCUMENT_METADATA && e.a == BOOK_DOCUMENT_META_BOOK)) return BOOK_ERROR;
        if (d->metadata_only) {
            rc = metadata_output(host, &e, payload, first);
            if (rc != BOOK_OK) return rc;
            at += e.bytes; continue;
        }
        if (d->note) {
            document_note_request_t *note = d->note;
            if (e.type == BOOK_DOCUMENT_TEXT) {
                if (e.length1 > DOCUMENT_NOTE_BYTES - note->used) return BOOK_ERROR;
                memcpy(note->text + note->used, payload, e.length1); note->used += e.length1;
                note->text[note->used] = 0;
            } else if (e.type == BOOK_DOCUMENT_NOTE_END) {
                note->truncated = e.a != 0; note->received = true;
            } else return BOOK_ERROR;
            at += e.bytes; continue;
        }
        if (host->image_id && e.type != BOOK_DOCUMENT_IMAGE_DATA && e.type != BOOK_DOCUMENT_IMAGE_END)
            return BOOK_ERROR;
        switch (e.type)
        {
        case BOOK_DOCUMENT_TEXT: rc = sink_text(d, (const char *)payload, e.length1, e.a); break;
        case BOOK_DOCUMENT_PARAGRAPH: rc = sink_paragraph(d); break;
        case BOOK_DOCUMENT_ANCHOR: rc = sink_anchor(d, first, e.b ? second : NULL, e.a); break;
        case BOOK_DOCUMENT_LINK: rc = sink_link(d, first, e.a != 0); break;
        case BOOK_DOCUMENT_METADATA:
            if (e.a == BOOK_DOCUMENT_META_BOOK) { rc = sink_metadata(d, first, second); break; }
            /* fall through */
        case BOOK_DOCUMENT_CHAPTER:
        case BOOK_DOCUMENT_SOURCE_ANCHOR:
            rc = metadata_apply(d, data + at, e.bytes, d->end);
            if (rc == BOOK_OK && d->nav_state != DOCUMENT_NAV_ACTIVE)
                rc = record_write(d, DOCUMENT_METADATA, 0, data + at, e.bytes);
            break;
        case BOOK_DOCUMENT_IMAGE_BEGIN:
        {
            if (!e.a || !e.b || !e.c || e.b > DOCUMENT_IMAGE_WIDTH || e.c > DOCUMENT_IMAGE_HEIGHT ||
                e.d != (e.b + 1) / 2 || host->image_id) return BOOK_ERROR;
            document_image_t dimensions = {e.b, e.c};
            document_record_t record = {DOCUMENT_IMAGE, sizeof(dimensions) + e.d * e.c, 0, d->link};
            rc = cache_room(d, sizeof(record) + record.length);
            if (rc == BOOK_OK && d->segmented && !location_append(d, d->end, 0)) rc = BOOK_NO_MEMORY;
            if (rc == BOOK_OK) rc = write_all(d, &record, sizeof(record));
            if (rc == BOOK_OK) rc = write_all(d, &dimensions, sizeof(dimensions));
            host->image_id = e.a; host->image_row = 0;
            host->image_width = e.b; host->image_height = e.c; host->image_stride = e.d;
            break;
        }
        case BOOK_DOCUMENT_IMAGE_DATA:
            if (!host->image_id || e.a != host->image_id || e.b != host->image_row || !e.c ||
                e.c > host->image_height - host->image_row || e.d != host->image_stride ||
                e.length1 != e.c * e.d || e.length2) return BOOK_ERROR;
            rc = write_all(d, payload, e.length1);
            host->image_row += e.c;
            break;
        case BOOK_DOCUMENT_IMAGE_END:
            if (!host->image_id || e.a != host->image_id || host->image_row != host->image_height)
                return BOOK_ERROR;
            host->image_id = 0;
            source_profile_content(d, d->end);
            rc = record_complete(d);
            break;
        default: return BOOK_ERROR;
        }
        if (rc != BOOK_OK) return rc;
        if (d->nav_state == DOCUMENT_NAV_ACTIVE && navigation_event) {
            ++d->nav_cursor;
            if (d->toc_context && d->nav_cursor > d->toc_context->cursor) d->toc_context->cursor = d->nav_cursor;
        }
        at += e.bytes;
    }
    return BOOK_OK;
}

static bool resource_path_valid(const char *path)
{
    if (!path || path[0] != '/' ||
        (strncmp(path, STORAGE_FLASH_ROOT "/", sizeof(STORAGE_FLASH_ROOT)) &&
         strncmp(path, STORAGE_SD_ROOT "/", sizeof(STORAGE_SD_ROOT)))) return false;
    for (const char *p = path; *p; ++p)
    {
        if ((unsigned char)*p < 32 || *p == '\\' || *p == ':') return false;
        if (*p == '/' && p[1] == '.' && (p[2] == '/' || !p[2] ||
            (p[2] == '.' && (p[3] == '/' || !p[3])))) return false;
    }
    return true;
}

static int custom_resource(document_remote_host_t *host, epd_compute_pipe_message_t *m,
                           document_resource_t *r, uint8_t *data)
{
    document_t *d = host->document;
    if (m->operation == EPD_PIPE_CLOSE)
    {
        r->stream.close(r->stream.user); memset(&r->stream, 0, sizeof(r->stream));
        r->custom = false; r->path[0] = 0; return BOOK_OK;
    }
    if (m->operation == EPD_PIPE_READ)
    {
        if (m->length > EPD_COMPUTE_PIPE_BYTES || m->offset > r->stream.size ||
            r->stream.seek(r->stream.user, m->offset) < 0) return BOOK_IO_ERROR;
        int n = r->stream.read(r->stream.user, data, m->length);
        if (n < 0) return n;
        if ((uint32_t)n > m->length) return BOOK_IO_ERROR;
        m->length = n; host->read_bytes += n; return BOOK_OK;
    }
    book_stream_t stream = {0};
    int rc = d->source.resources.open(d->source.resources.user, d->file.path, m->path, &stream);
    if (rc != BOOK_OK) return rc;
    if (!stream.read || !stream.seek || !stream.close)
    { if (stream.close) stream.close(stream.user); return BOOK_IO_ERROR; }
    d->custom_resources = true;
    m->size = stream.size; m->flags = BOOK_DOCUMENT_REGULAR;
    if (m->operation == EPD_PIPE_STAT) { stream.close(stream.user); return BOOK_OK; }
    unsigned slot = 0;
    while (slot < DOCUMENT_RESOURCE_LIMIT && host->resources[slot].path[0]) ++slot;
    if (slot == DOCUMENT_RESOURCE_LIMIT) { stream.close(stream.user); return BOOK_NO_MEMORY; }
    r = &host->resources[slot]; r->stream = stream; r->custom = true;
    snprintf(r->path, sizeof(r->path), "%s", m->path);
    m->handle = slot + 1;
    return BOOK_OK;
}

static int remote_service(void *user, epd_compute_pipe_message_t *m, uint8_t *data)
{
    document_remote_host_t *host = user;
    document_t *d = host->document;
    if (cancelled(d)) return BOOK_CANCELLED;
    ++host->calls;
    if (m->operation == EPD_PIPE_OUTPUT)
    {
        rt_tick_t started = rt_tick_get();
        host->profile.output_start = started; host->profile.output_active = true;
        host->write_bytes += m->length;
        int rc = m->flags == BOOK_DOCUMENT_ENTRY_OUTPUT ? entry_cache_receive(d, data, m->length) :
            remote_output(host, data, m->length);
        host->output_ticks += rt_tick_get() - started;
        host->profile.output_active = false;
        return rc;
    }
    bool named = m->operation == EPD_PIPE_OPEN || m->operation == EPD_PIPE_STAT ||
        m->operation == EPD_PIPE_DIR_OPEN;
    if (named && (!memchr(m->path, 0, sizeof(m->path)) || !resource_path_valid(m->path))) return BOOK_IO_ERROR;
    document_resource_t *r = m->handle && m->handle <= DOCUMENT_RESOURCE_LIMIT ?
        &host->resources[m->handle - 1] : NULL;
    if (!named && (!r || !r->path[0])) return BOOK_IO_ERROR;
    if ((!named && r->custom) || (named && d->source.resources.open &&
        m->operation != EPD_PIPE_DIR_OPEN && strcmp(m->path, d->file.path)))
        return custom_resource(host, m, r, data);
    rt_tick_t started = rt_tick_get();
    storage_lock();
    host->wait_ticks += rt_tick_get() - started;
    started = rt_tick_get();
    int rc = BOOK_IO_ERROR, observed = -1;
    const char *phase = "validate";
    errno = 0;
    if (cancelled(d)) rc = BOOK_CANCELLED;
    else if (named && storage_path_available(m->path) && !storage_file_writers(m->path))
    {
        struct stat st;
        phase = "stat";
        if (stat(m->path, &st) == 0)
        {
            m->size = st.st_size;
            m->flags = S_ISREG(st.st_mode) ? BOOK_DOCUMENT_REGULAR :
                       S_ISDIR(st.st_mode) ? BOOK_DOCUMENT_DIRECTORY : 0;
            if (m->operation == EPD_PIPE_STAT) rc = m->flags ? BOOK_OK : BOOK_UNSUPPORTED;
            else
            {
                unsigned slot = 0;
                while (slot < DOCUMENT_RESOURCE_LIMIT && host->resources[slot].path[0]) ++slot;
                if (slot == DOCUMENT_RESOURCE_LIMIT) rc = BOOK_NO_MEMORY;
                else
                {
                    r = &host->resources[slot];
                    phase = "open";
                    if (m->operation == EPD_PIPE_OPEN && S_ISREG(st.st_mode))
                        r->descriptor = reader_file_open(d, m->path, O_RDONLY);
                    else if (m->operation == EPD_PIPE_DIR_OPEN && S_ISDIR(st.st_mode))
                        r->directory = opendir(m->path);
                    if (r->descriptor >= 0 || r->directory)
                    {
                        snprintf(r->path, sizeof(r->path), "%s", m->path);
                        r->media = storage_card_session(); r->revision = storage_path_revision(r->path);
                        m->handle = slot + 1; rc = BOOK_OK;
                    }
                }
            }
            if (S_ISREG(st.st_mode)) observed = rc == BOOK_OK ? BOOK_FILE_OPENED : BOOK_FILE_UNAVAILABLE;
        }
        else observed = errno == ENOENT || errno == -ENOENT ? BOOK_FILE_MISSING : BOOK_FILE_UNAVAILABLE;
    }
    else if (!named)
    {
        if (m->operation == EPD_PIPE_CLOSE && r->descriptor >= 0)
        { phase = "close"; rc = close(r->descriptor) == 0 ? BOOK_OK : BOOK_IO_ERROR; r->descriptor = -1; r->path[0] = 0; }
        else if (m->operation == EPD_PIPE_DIR_CLOSE && r->directory)
        { phase = "closedir"; rc = closedir(r->directory) == 0 ? BOOK_OK : BOOK_IO_ERROR; r->directory = NULL; r->path[0] = 0; }
        else if (resource_valid(r))
        {
            if (m->operation == EPD_PIPE_READ && r->descriptor >= 0 && m->offset <= INT32_MAX &&
                m->length <= EPD_COMPUTE_PIPE_BYTES)
            {
                phase = "seek";
                if (lseek(r->descriptor, (off_t)m->offset, SEEK_SET) >= 0)
                {
                    phase = "read";
                    int n = read(r->descriptor, data, m->length);
                    if (n >= 0) { m->length = n; host->read_bytes += n; rc = BOOK_OK; }
                }
            }
            else if (m->operation == EPD_PIPE_DIR_NEXT && r->directory)
            {
                errno = 0;
                phase = "readdir";
                struct dirent *entry = readdir(r->directory);
                if (!entry) { m->length = 0; rc = errno ? BOOK_IO_ERROR : BOOK_OK; }
                else if (strlen(entry->d_name) < EPD_COMPUTE_PIPE_BYTES)
                { m->length = strlen(entry->d_name) + 1; memcpy(data, entry->d_name, m->length); rc = BOOK_OK; }
            }
        }
    }
    int io_error = errno;
    unsigned cache_fds = 0, resource_fds = 0;
    if (rc == BOOK_IO_ERROR)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        for (document_t *p = documents; p; p = p->next)
            cache_fds += (p->input >= 0) + (p->output >= 0);
        rt_hw_interrupt_enable(key);
        for (unsigned i = 0; i < DOCUMENT_RESOURCE_LIMIT; ++i)
            resource_fds += (host->resources[i].descriptor >= 0) + (host->resources[i].directory != NULL);
    }
    host->read_ticks += rt_tick_get() - started;
    storage_unlock();
    if (rc == BOOK_IO_ERROR)
        rt_kprintf("[document] resource_io id=%lu op=%u phase=%s errno=%d handle=%lu offset=%lu length=%lu cache_fds=%u resource_fds=%u path=%s\n",
            (unsigned long)d->serial, (unsigned)m->operation, phase, io_error, (unsigned long)m->handle,
            (unsigned long)m->offset, (unsigned long)m->length, cache_fds, resource_fds,
            named ? m->path : r->path);
    if (observed >= 0) document_cache_observe_resource(d->cached, m->path, observed);
    return rc;
}

#define DOCUMENT_OPEN_BUSY_MS 2000u

static int convert_remote_open(document_t *d)
{
    rt_tick_t started = rt_tick_get(), admitted;
    bool retry = d->open_pending;
    d->open_pending = false;
    if (!retry) { d->open_started = started; d->open_backoff = 20; }
    document_remote_host_t *host = allocate(sizeof(*host));
    epd_compute_pipe_message_t *initial = allocate(sizeof(*initial));
    if (!host || !initial) { epd_app_free(host); epd_app_free(initial); return BOOK_NO_MEMORY; }
    memset(host, 0, sizeof(*host)); memset(initial, 0, sizeof(*initial));
    d->remote = host;
    host->document = d;
    for (unsigned i = 0; i < DOCUMENT_RESOURCE_LIMIT; ++i) host->resources[i].descriptor = -1;
    initial->flags = d->format == DOCUMENT_XHTML ? 6u : 0u;
    initial->values[0] = BOOK_DOCUMENT_WIRE_VERSION;
    initial->values[1] = d->note ? BOOK_DOCUMENT_NOTE_BUDGET : BOOK_DOCUMENT_DEFAULT_BUDGET;
    initial->values[2] = DOCUMENT_IMAGE_WIDTH; initial->values[3] = DOCUMENT_IMAGE_HEIGHT;
    if (d->segmented)
    {
        initial->values[4] = d->requested.chapter;
        initial->values[5] = d->requested.offset;
        initial->values[6] = BOOK_DOCUMENT_SELECT_CHAPTER | BOOK_DOCUMENT_CONTEXT;
        if (d->full_source) initial->values[6] |= BOOK_DOCUMENT_FULL_SOURCE;
        initial->values[7] = 64u * 1024u;
        initial->offset = 24u * 1024u;
        initial->size = d->requested.skip_utf8;
        if (d->body_first) {
            initial->values[6] |= d->nav_state == DOCUMENT_NAV_ACTIVE ? BOOK_DOCUMENT_NAV_ONLY : BOOK_DOCUMENT_BODY_ONLY;
            if (d->nav_state == DOCUMENT_NAV_ACTIVE) {
                initial->values[4] = d->toc_context ? d->toc_context->nav_chapter : d->chapter;
                initial->values[5] = 0;
                initial->size = d->nav_cursor;
            } else if (d->first_body) initial->values[6] |= BOOK_DOCUMENT_FIRST_BODY;
        }
    }
    if (d->note) initial->values[6] = BOOK_DOCUMENT_NOTE;
    if (d->metadata_only) {
        initial->values[6] = BOOK_DOCUMENT_METADATA_ONLY;
        initial->values[2] = DOCUMENT_COVER_WIDTH;
        initial->values[3] = DOCUMENT_COVER_HEIGHT;
    }
    size_t entry_bytes = 0;
    const void *entry = entry_cache_request(d, &entry_bytes);
    if (d->entry_source_valid) initial->values[6] |= BOOK_DOCUMENT_ENTRY_CACHE;
    snprintf(initial->path, sizeof(initial->path), "%s", d->file.path);
    epd_compute_pipe_host_t pipe = NULL;
    epd_compute_wait_t wait = NULL;
    admitted = rt_tick_get();
    int status = epd_compute_wait_create(&wait);
    if (status == EPD_COMPUTE_OK)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        d->compute_wait = wait;
        if (cancelled(d)) epd_compute_wait_cancel(wait);
        rt_hw_interrupt_enable(key);
        status = epd_compute_pipe_open_wait(BOOK_DOCUMENT_COMPONENT, BOOK_DOCUMENT_VERSION,
            d->owners && !d->metadata_only && d->nav_state != DOCUMENT_NAV_ACTIVE ? EPD_COMPUTE_FOREGROUND : EPD_COMPUTE_BACKGROUND, initial, d->note ? d->note->target : entry,
            d->note ? strlen(d->note->target) + 1 : entry_bytes, wait, &pipe);
        key = rt_hw_interrupt_disable();
        d->compute_wait = NULL; d->pipe = pipe;
        if (cancelled(d) && pipe) epd_compute_pipe_cancel(pipe);
        rt_hw_interrupt_enable(key);
        epd_compute_wait_destroy(wait);
    }
    epd_app_free(initial);
    if (status == EPD_COMPUTE_OK) {
        host->profile.enabled = !d->note && !d->metadata_only && d->nav_state != DOCUMENT_NAV_ACTIVE;
        host->profile.profile_start = host->profile.slice_end = host->profile.step_end = rt_tick_get();
    }
    if (!retry || status != EPD_COMPUTE_BUSY)
    rt_kprintf("[document] convert_open id=%lu result=%d elapsed_ms=%u chapter=%lu anchor=%lu skip=%lu budget=%u note=%u open_ms=%u admission_ms=%u\n",
        (unsigned long)d->serial, status, elapsed_ms(d->opened_at),
        (unsigned long)d->requested.chapter, (unsigned long)d->requested.offset,
        (unsigned long)d->requested.skip_utf8,
        d->note ? BOOK_DOCUMENT_NOTE_BUDGET : BOOK_DOCUMENT_DEFAULT_BUDGET, d->note != NULL,
        elapsed_ms(started), elapsed_ms(admitted));
    if (status == EPD_COMPUTE_BUSY && !cancelled(d)) {
        d->open_pending = true;
        unsigned elapsed = elapsed_ms(d->open_started);
        unsigned remaining = elapsed < DOCUMENT_OPEN_BUSY_MS ? DOCUMENT_OPEN_BUSY_MS - elapsed : 0;
        unsigned delay = d->open_backoff < remaining ? d->open_backoff : remaining;
        d->open_retry = rt_tick_get() + rt_tick_from_millisecond(delay) + 1;
        if (d->open_backoff < 500) {
            d->open_backoff *= 2;
            if (d->open_backoff > 500) d->open_backoff = 500;
        }
        return BOOK_OK;
    }
    if (status == EPD_COMPUTE_UNAVAILABLE)
        d->failure_code = READER_ERROR_UNAVAILABLE;
    if (status != EPD_COMPUTE_OK)
        snprintf(d->work_error, sizeof(d->work_error), "阅读计算组件不可用 (%d)", status);
    return status == EPD_COMPUTE_OK ? BOOK_OK :
           status == EPD_COMPUTE_NO_MEMORY ? BOOK_NO_MEMORY :
           status == EPD_COMPUTE_UNSUPPORTED ? BOOK_UNSUPPORTED :
           status == EPD_COMPUTE_CANCELLED ? BOOK_CANCELLED : BOOK_ERROR;
}

enum { DOCUMENT_FINISH_PREPARE = 1, DOCUMENT_FINISH_RENAME, DOCUMENT_FINISH_COMMIT };

/* A failed CLOSE retains the entire conversion until the next worker step. */
static bool convert_finish(document_t *d, int result)
{
    bool navigation = d->nav_state == DOCUMENT_NAV_ACTIVE;
    document_remote_host_t *host = d->remote;
    if (host) { host->terminal = true; host->result = result; }
    document_source_profile_t profile = {0};
    rt_base_t key = rt_hw_interrupt_disable();
    epd_compute_pipe_host_t pipe = d->pipe; d->pipe = NULL;
    rt_hw_interrupt_enable(key);
    int closed = epd_compute_pipe_shutdown(&pipe);
    if (closed != EPD_COMPUTE_OK)
    {
        key = rt_hw_interrupt_disable();
        d->pipe = pipe;
        if (!navigation || cancelled(d)) result_locked(d, cancelled(d) ? BOOK_CANCELLED : BOOK_ERROR);
        d->status.complete = true;
        rt_hw_interrupt_enable(key);
        if (host) host->result = BOOK_ERROR;
        if (host && host->profile.enabled && !host->profile.done) {
            host->profile.done = true;
            source_profile_snapshot(host);
            source_profile_log(d, &host->profile, "stop", BOOK_ERROR);
        }
        log_cleanup_pending(d, 1u, closed); log_terminal(d);
        return false;
    }
    if (host)
    {
        for (unsigned i = 0; i < DOCUMENT_RESOURCE_LIMIT; ++i)
        {
            document_resource_t *r = &host->resources[i];
            if (r->custom && r->stream.close) r->stream.close(r->stream.user);
            storage_lock();
            if (r->descriptor >= 0) close(r->descriptor);
            if (r->directory) closedir(r->directory);
            storage_unlock();
        }
        rt_kprintf("[document] convert_done id=%lu result=%d steps=%lu slices=%lu read_bytes=%lu output_bytes=%lu elapsed_ms=%u\n",
            (unsigned long)d->serial, result, (unsigned long)host->steps, (unsigned long)host->slices,
            (unsigned long)host->read_bytes, (unsigned long)host->write_bytes, elapsed_ms(d->opened_at));
        /* Preserve scalar diagnostics through the final spool flush without
         * retaining the pipe, resources, or remote allocation. */
        source_profile_snapshot(host);
        profile = host->profile;
        epd_app_free(host); d->remote = NULL;
    }
    rt_tick_t started = rt_tick_get();
    bool observing = d->observing;
    unsigned finish_stage = 0;
    if (cancelled(d)) result = BOOK_CANCELLED;
    bool paused = navigation && d->nav_pausing && !cancelled(d);
    if (paused) result = BOOK_OK;
    if (d->body_first && !navigation && result == BOOK_OK &&
        (!d->body_end || d->body_end != d->end)) result = BOOK_ERROR;
    if (d->output >= 0)
    {
        if (result == BOOK_OK) {
            result = cache_publish(d, true);
            if (result == BOOK_OK) source_profile_published(d, &profile);
        }
        storage_lock();
        errno = 0;
        closed = close(d->output);
        if (closed != 0 && result == BOOK_OK) result = cache_failure(d, "close", errno);
        d->output = -1;
        storage_unlock();
        epd_app_free(d->write_buffer); d->write_buffer = NULL; d->write_used = 0;
        rt_kprintf("[document] finalize id=%lu stage=0 saved=%u elapsed_ms=%u\n",
            (unsigned long)d->serial, result == BOOK_OK, elapsed_ms(started));
        if (result == BOOK_OK && observing && !d->custom_resources && !cancelled(d))
        {
            finish_stage = DOCUMENT_FINISH_PREPARE;
        }
    }
    if (result != BOOK_OK && d->cache_result != BOOK_OK) result = d->cache_result;
    if (cancelled(d)) result = BOOK_CANCELLED;
    if (profile.enabled && !profile.done)
        source_profile_log(d, &profile, "stop", result);
    if (observing) { document_cache_observe_end(d->cached); d->observing = false; }
    key = rt_hw_interrupt_disable();
    if (navigation && !cancelled(d)) {
        d->nav_result = result;
        d->nav_state = paused ? DOCUMENT_NAV_PENDING : result == BOOK_OK ? DOCUMENT_NAV_DONE : DOCUMENT_NAV_FAILED;
        if (d->nav_state == DOCUMENT_NAV_DONE) toc_finish(d->toc_context);
    } else {
        result_locked(d, result);
        if (d->body_first && result == BOOK_OK) d->nav_state = DOCUMENT_NAV_PENDING;
    }
    d->nav_pausing = false;
    /* Readers and chapter promotion need only the closed spool. Persistence
     * remains independent of the conversion slot and its complete status. */
    if (!navigation || cancelled(d)) d->finish_stage = result == BOOK_OK ? finish_stage : 0;
    d->converting = d->open_pending = false; d->status.complete = true;
    worker_changed();
    if (result == BOOK_OK && !d->note && !d->metadata_only) d->adjacent_pending = true;
    if (conversion_document == d) conversion_document = NULL;
    if (note_conversion_document == d) note_conversion_document = NULL;
    rt_hw_interrupt_enable(key);
    if (worker_service) epd_service_ui_request(worker_service, 0);
    log_terminal(d);
    if (d->metadata_only && worker_service) epd_service_ui_request(worker_service, 0);
    return true;
}

static void convert_finalize(document_t *d)
{
    if (cancelled(d) || d->status.result != BOOK_OK) { d->finish_stage = 0; return; }
    unsigned stage = d->finish_stage;
    rt_tick_t started = rt_tick_get();
    bool saved = false;
    if (stage == DOCUMENT_FINISH_PREPARE)
        saved = document_cache_prepare(d->cached);
    else if (stage == DOCUMENT_FINISH_RENAME)
    {
        storage_lock();
        readers_close(d);
        const char *binary = document_cache_binary(d->cached);
        saved = !cancelled(d) && storage_file_commit(d->cache, binary);
        if (saved)
        {
            rt_base_t key = rt_hw_interrupt_disable();
            snprintf(d->cache, sizeof(d->cache), "%s", binary);
            rt_hw_interrupt_enable(key);
        }
        storage_unlock();
    }
    else if (stage == DOCUMENT_FINISH_COMMIT)
        saved = d->persistent = document_cache_commit(d->cached, d->status.title, d->status.end);
    if (stage == DOCUMENT_FINISH_COMMIT && saved) worker_changed();
    rt_kprintf("[document] finalize id=%lu stage=%u saved=%u elapsed_ms=%u\n",
        (unsigned long)d->serial, stage, saved, elapsed_ms(started));
    d->finish_stage = !saved || stage == DOCUMENT_FINISH_COMMIT || cancelled(d) ? 0 : stage + 1;
}

/* The backend has one BODY/NAV slot; only a note can nest inside a BODY.
 * Keep the slot until CLOSE succeeds, including failed partial OPENs. */
static bool conversion_available(document_t *d)
{
    return d->note ? !note_conversion_document &&
        (!conversion_document || conversion_document->nav_state != DOCUMENT_NAV_ACTIVE) :
        !conversion_document && !note_conversion_document;
}

static void convert_begin(document_t *d)
{
    rt_base_t key = rt_hw_interrupt_disable();
    if (!conversion_available(d)) { rt_hw_interrupt_enable(key); return; }
    if (d->note) note_conversion_document = d;
    else conversion_document = d;
    d->converting = true;
    rt_hw_interrupt_enable(key);
    if (!d->note && !d->metadata_only && !cache_open(d)) { convert_finish(d, BOOK_IO_ERROR); return; }
    d->observing = d->cached && document_cache_observe_begin(d->cached);
    int result = convert_remote_open(d);
    if (result != BOOK_OK) convert_finish(d, result);
}

static bool navigation_allowed(document_t *d)
{
    /* A retained neighbor may own the one active NAV pipe for this shared TOC.
     * Let it finish when the current reader has returned the optional I/O gate. */
    rt_base_t key = rt_hw_interrupt_disable();
    document_t *owner = flow_owner();
    bool shared = d->nav_state == DOCUMENT_NAV_ACTIVE && owner && owner->prefetch_enabled &&
        owner->toc_context && owner->toc_context == d->toc_context;
    rt_hw_interrupt_enable(key);
    return d->body_first && (d->owners || shared) && (!toc_complete(d) || d->nav_state == DOCUMENT_NAV_ACTIVE) &&
        !d->nav_deferred && !cancelled(d) && !epd_reader_suspended() && flow_allows(d) &&
        (d->nav_requested || ((d->prefetch_enabled || shared) && persistence_allowed()));
}

/* The committed spool ends at SOURCE_END. Optional navigation lives only in
 * the bounded index and resumes after the last accepted complete event. */
static void navigation_begin(document_t *d)
{
    rt_base_t key = rt_hw_interrupt_disable();
    if (!conversion_available(d)) { rt_hw_interrupt_enable(key); return; }
    if (d->toc_context) {
        /* NAV_ONLY omits the selected spine; its cursor belongs to that stream. */
        if (!d->toc_context->cursor) d->toc_context->nav_chapter = d->chapter;
        d->nav_cursor = d->toc_context->cursor;
    }
    d->nav_state = DOCUMENT_NAV_ACTIVE; d->converting = true;
    conversion_document = d;
    rt_hw_interrupt_enable(key);
    int result = convert_remote_open(d);
    if (result != BOOK_OK) convert_finish(d, result);
}

static void navigation_pause_locked(document_t *d)
{
    d->nav_pausing = true;
    if (d->pipe) epd_compute_pipe_cancel(d->pipe);
}

/* The reader disables speculative work while waiting for the visible target
 * or pausing. Keep the suspended parser, advancing only for necessary input. */
static document_t *flow_owner(void)
{
    document_t *latest = NULL;
    for (document_t *p = documents; p; p = p->next)
        if (p->owners && !cancelled(p) && p->demand_controlled &&
            (!latest || (int32_t)(p->flow_order - latest->flow_order) > 0)) latest = p;
    return latest;
}

/* A paused current/near page can still demand its own input. Other chapters
 * wait until that owner releases the optional-I/O gate. Notes are explicit. */
static bool flow_allows(document_t *d)
{
    rt_base_t key = rt_hw_interrupt_disable();
    document_t *owner = flow_owner();
    bool allowed = !owner || owner->prefetch_enabled || owner == d || d->note;
    rt_hw_interrupt_enable(key);
    return allowed;
}

static bool conversion_wanted(document_t *d)
{
    if (cancelled(d) || (d->remote && d->remote->terminal)) return d->converting;
    if (d->nav_state == DOCUMENT_NAV_ACTIVE) return d->nav_pausing || navigation_allowed(d);
    if (d->status.complete && !d->converting) return false;
    if (d->metadata_only) {
        for (document_t *p = documents; p; p = p->next)
            if (!p->metadata_only && p->owners && !cancelled(p)) return false;
        return d->owners != 0;
    }
    if (epd_reader_suspended()) return false;
    if (d->note) return d->owners != 0;
    if (!flow_allows(d)) return false;
    if (note_conversion_document) return false;
    if (d->owners && d->target_required) return true;
    if (d->body_first && d->nav_requested) return true;
    if (d == conversion_document)
        for (document_t *p = documents; p; p = p->next)
            if (p->note && !p->status.complete && !cancelled(p) && p->owners) return false;
    if (!d->owners)
    {
        for (document_t *p = documents; p; p = p->next)
            if (p->owners && !cancelled(p) && p->prefetch_enabled &&
                (p->previous == d || p->following == d)) return true;
        return false;
    }
    if (!d->demand_controlled || d->prefetch_enabled) return true;
    if (!d->status.end || (d->segmented && (!d->target_ready || d->target_record >= d->status.end)) ||
        (d->requested.unit == DOCUMENT_SOURCE_SPOOL && d->requested.offset >= d->status.end)) return true;
    for (document_record_request_t *r = record_requests; r; r = r->next)
        if (r->document == d && r->foreground && !r->released && !r->done &&
            (r->offset >= d->status.end || d->status.end - r->offset < sizeof(document_record_t))) return true;
    for (document_image_request_t *r = image_requests; r; r = r->next)
        if (r->document == d && !r->released && !r->done &&
            (r->offset >= d->status.end || d->status.end - r->offset < sizeof(document_record_t))) return true;
    return false;
}

static bool conversion_runnable(document_t *d)
{
    return conversion_wanted(d) && (cancelled(d) || d->nav_pausing || !d->open_pending ||
        (int32_t)(rt_tick_get() - d->open_retry) >= 0);
}

static int32_t conversion_wait(void)
{
    int32_t wait = RT_WAITING_FOREVER;
    document_t *slots[] = {conversion_document, note_conversion_document};
    for (unsigned i = 0; i < 2; ++i) {
        document_t *d = slots[i];
        if (!d || !d->open_pending || !conversion_wanted(d)) continue;
        int32_t left = (int32_t)(d->open_retry - rt_tick_get());
        if (left < 1) left = 1;
        if (wait == RT_WAITING_FOREVER || left < wait) wait = left;
    }
    return wait;
}

/* Caller holds the document publication critical section. The reader worker
 * reports flow for the context it is advancing, including glyph-only waits.
 * Its latest owned context controls optional writes; old paused neighbors
 * must not veto a subsequent foreground resume. */
static bool persistence_allowed(void)
{
    if (epd_reader_suspended()) return false;
    document_t *latest = flow_owner();
    return !latest || latest->prefetch_enabled;
}

static void convert_step_work(document_t *d)
{
    if (!d->converting) return;
    document_remote_host_t *host = d->remote;
    if (cancelled(d)) { convert_finish(d, BOOK_CANCELLED); return; }
    if (d->nav_pausing) { convert_finish(d, BOOK_CANCELLED); return; }
    if (d->open_pending) {
        if (!conversion_runnable(d)) return;
        rt_base_t key = rt_hw_interrupt_disable();
        epd_compute_pipe_host_t pipe = d->pipe; d->pipe = NULL;
        rt_hw_interrupt_enable(key);
        int closed = epd_compute_pipe_shutdown(&pipe);
        key = rt_hw_interrupt_disable(); d->pipe = pipe; rt_hw_interrupt_enable(key);
        if (closed != EPD_COMPUTE_OK) {
            d->open_retry = rt_tick_get() + rt_tick_from_millisecond(500) + 1;
            log_cleanup_pending(d, 1u, closed); return;
        }
        /* OPEN has not serviced any resource requests yet. */
        epd_app_free(d->remote); d->remote = NULL;
        if (elapsed_ms(d->open_started) >= DOCUMENT_OPEN_BUSY_MS) {
            rt_kprintf("[document] open_busy_limit id=%lu elapsed_ms=%u navigation=%u owners=%u\n",
                (unsigned long)d->serial, elapsed_ms(d->open_started),
                d->nav_state == DOCUMENT_NAV_ACTIVE, d->owners);
            if (d->nav_state == DOCUMENT_NAV_ACTIVE) {
                d->nav_deferred = d->nav_pausing = true;
                convert_finish(d, BOOK_CANCELLED);
            } else if (!d->owners) {
                d->cancelled = d->released = true; d->retained = false;
                convert_finish(d, BOOK_CANCELLED);
            } else {
                d->failure_code = READER_ERROR_UNAVAILABLE;
                snprintf(d->work_error, sizeof(d->work_error), "阅读计算组件忙，请重试");
                convert_finish(d, BOOK_ERROR);
            }
            return;
        }
        int result = convert_remote_open(d);
        if (result != BOOK_OK) convert_finish(d, result);
        return;
    }
    if (!host || !d->pipe) { convert_finish(d, BOOK_ERROR); return; }
    if (host->terminal) { convert_finish(d, host->result); return; }
    rt_tick_t started = rt_tick_get();
    ++host->slices;
    for (unsigned i = 0; i < DOCUMENT_CONVERT_STEPS; ++i)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        bool runnable = conversion_runnable(d);
        rt_hw_interrupt_enable(key);
        if (!runnable) return;
        if (cancelled(d)) { convert_finish(d, BOOK_CANCELLED); return; }
        ++host->steps;
        bool measure = host->profile.enabled && !host->profile.done;
        if (measure) {
            rt_tick_t now = rt_tick_get(), gap = now - host->profile.step_end;
            if (gap > host->profile.step_gap_max) host->profile.step_gap_max = gap;
            host->profile.step_start = now; host->profile.step_active = true;
        }
        int status = epd_compute_pipe_step(d->pipe, remote_service, host);
        if (measure) {
            host->profile.step_end = rt_tick_get();
            host->profile.step_ticks += host->profile.step_end - host->profile.step_start;
            host->profile.step_active = false;
        }
        if (status != EPD_COMPUTE_MORE)
        {
            int result = BOOK_ERROR;
            const epd_compute_pipe_message_t *done = epd_compute_pipe_result(d->pipe);
            log_stage_timing(d, done);
            if ((status == EPD_COMPUTE_OK || status == EPD_COMPUTE_COMPONENT_ERROR) &&
                done && done->operation == EPD_PIPE_DONE &&
                done->values[0] == BOOK_DOCUMENT_WIRE_VERSION && done->length <= EPD_COMPUTE_PIPE_BYTES)
            {
                result = host->image_id || (status == EPD_COMPUTE_COMPONENT_ERROR && done->status >= 0) ?
                    BOOK_ERROR : done->status;
                if (d->note && result == BOOK_OK && !d->note->received) result = BOOK_ERROR;
                if (d->segmented && result == BOOK_OK && d->nav_state != DOCUMENT_NAV_ACTIVE) d->source_next = done->values[7];
                if (done->length) snprintf(d->work_error, sizeof(d->work_error), "%.*s",
                    (int)done->length, (const char *)epd_compute_pipe_payload(d->pipe));
            }
            else snprintf(d->work_error, sizeof(d->work_error), "阅读计算组件执行失败 (%d)", status);
            convert_finish(d, result); return;
        }
        if ((uint64_t)(rt_tick_get() - started) * 1000u >= DOCUMENT_CONVERT_MS * RT_TICK_PER_SECOND) break;
    }
}

static void convert_step(document_t *d)
{
    document_remote_host_t *host = d->remote;
    bool profile = host && host->profile.enabled && !host->profile.done;
    if (profile) {
        rt_tick_t now = rt_tick_get(), gap = now - host->profile.slice_end;
        host->profile.gap_ticks += gap;
        if (gap > host->profile.gap_max) host->profile.gap_max = gap;
        host->profile.slice_start = now; host->profile.slice_active = true;
    }
    convert_step_work(d);
    /* DONE/cancel may release the remote object inside convert_step_work. */
    if (profile && d->remote == host) {
        host->profile.slice_end = rt_tick_get();
        host->profile.slice_ticks += host->profile.slice_end - host->profile.slice_start; host->profile.slice_active = false;
    }
}

/* New foreground requests supersede an unfinished older conversion. A failed
 * CLOSE keeps conversion_document occupied, so a second parser cannot open. */
static void conversion_preempt_locked(document_t *target)
{
    if (target->note) return;
    document_t *old = conversion_document;
    if (!old || old == target || cancelled(old) ||
        (int32_t)(target->demand - old->demand) <= 0) return;
    if (old->nav_state == DOCUMENT_NAV_ACTIVE) { navigation_pause_locked(old); return; }
    old->cancelled = true; old->retained = false;
    if (!old->owners) old->released = true;
    compute_cancel(old);
}

static bool adjacent_runnable(document_t *d)
{
    return !epd_reader_suspended() && !cancelled(d) && flow_allows(d) && d->prefetch_enabled && d->owners &&
           d->adjacent_pending && d->status.complete && !d->chapter_restore;
}

/* Called under the publication lock. Required page input and the existing
 * flow gate stay ahead of navigation, including warm-cache index restore. */
static document_t *navigation_candidate(void)
{
    for (document_record_request_t *r = record_requests; r; r = r->next)
        if (r->foreground && !r->released && !r->done && !cancelled(r->document)) return NULL;
    document_t *owner = flow_owner(), *best = NULL;
    for (document_t *p = documents; p; p = p->next) {
        if (!p->claimed || !p->status.complete || p->status.result != BOOK_OK || p->cache_bad ||
            !navigation_allowed(p)) continue;
        bool runnable = p->chapter_restore ||
            (p->nav_state == DOCUMENT_NAV_PENDING && conversion_available(p)) ||
            (p->nav_state == DOCUMENT_NAV_ACTIVE && conversion_document == p && conversion_runnable(p));
        if (!runnable) continue;
        if (!best || (p->nav_requested != best->nav_requested ? p->nav_requested :
            (!p->nav_requested && (p == owner || best == owner) ? p == owner :
             (int32_t)(p->demand - best->demand) > 0))) best = p;
    }
    return best;
}

bool document_worker_step(epd_service_t *service)
{
    static unsigned background_rounds;
    if (worker_stopping) return false;
    worker_service = service;
    if (reclaim_work()) return true;
    for (document_t *p = documents; p; p = p->next)
        if ((p->note || p->metadata_only) && !p->status.complete && !p->cancelled && elapsed_ms(p->opened_at) > 10000u) {
            rt_base_t note_key = rt_hw_interrupt_disable();
            p->cancelled = true; compute_cancel(p); rt_hw_interrupt_enable(note_key);
        }
    if (record_work_priority(true)) return true;
    rt_base_t key = rt_hw_interrupt_disable();
    document_t *d = NULL;
    bool persist = persistence_allowed();
    bool retired_work = false;
    /* Close cancelled documents before entering another backend instance. */
    for (document_t *p = documents; p; p = p->next)
        if ((!p->stopped && cancelled(p)) || (p->released && !p->cleanup_done)) { d = p; break; }
    if (!d && conversion_document && cancelled(conversion_document)) d = conversion_document;
    if (!d && conversion_document && conversion_document->nav_state == DOCUMENT_NAV_ACTIVE)
        for (document_t *p = documents; p; p = p->next)
            if (p->note && !p->status.complete && !cancelled(p) && p->owners) {
                navigation_pause_locked(conversion_document); d = conversion_document; break;
            }
    /* Notes run on a bounded child fiber. The body pipe stays suspended with
     * its existing input cursor and output batch intact. */
    if (!d && note_conversion_document && conversion_runnable(note_conversion_document))
        d = note_conversion_document;
    if (!d && !note_conversion_document)
        for (document_t *p = documents; p; p = p->next)
            if (p->note && !p->status.complete && !cancelled(p) && conversion_runnable(p))
                { d = p; break; }
    document_t *navigation = navigation_candidate();
    /* At most eight background reads/index loads between bounded navigation
     * slices. Foreground reads, cancellation and notes retain precedence. */
    if (!d && (!navigation || background_rounds < 8u)) {
        rt_hw_interrupt_enable(key);
        if (record_work() || index_work()) {
            if (background_rounds < 8u) ++background_rounds;
            return true;
        }
        key = rt_hw_interrupt_disable();
        navigation = navigation_candidate();
    }
    document_t *target = NULL;
    bool nav_yield = false;
    uint32_t nav_serial = 0, body_serial = 0, nav_demand = 0, body_demand = 0;
    for (document_t *p = documents; p; p = p->next)
        if (!note_conversion_document && !p->note && !cancelled(p) && p->owners &&
            ((!p->status.complete && conversion_runnable(p)) ||
             (p == navigation && p->nav_requested && p->chapter_restore) ||
             (p->nav_state == DOCUMENT_NAV_PENDING && p->nav_requested && navigation_allowed(p))) &&
            (!target || (int32_t)(p->demand - target->demand) > 0)) target = p;
    if (!d && target && !target->chapter_restore && conversion_document && target != conversion_document)
    {
        /* An optional NAV can retain the slot while its own input gate is
         * paused. Even an older/background BODY must be able to close it. */
        document_t *active = conversion_document;
        if (!target->status.complete && !target->metadata_only &&
            active->nav_state == DOCUMENT_NAV_ACTIVE && !active->nav_requested) {
            if (!active->nav_pausing) {
                nav_yield = true;
                nav_serial = active->serial; body_serial = target->serial;
                nav_demand = active->demand; body_demand = target->demand;
                navigation_pause_locked(active);
            }
        } else conversion_preempt_locked(target);
        if (conversion_runnable(conversion_document)) d = conversion_document;
        else target = NULL;
    }
    if (!d)
        for (document_t *p = documents; p; p = p->next)
            if (!epd_reader_suspended() && p->claimed && !cancelled(p) && image_pending(p)) { d = p; break; }
    if (!d && target) d = target;
    if (!d && navigation) d = navigation;
    if (!d && conversion_document && conversion_runnable(conversion_document)) d = conversion_document;
    if (!d)
        for (document_t *p = documents; p; p = p->next)
            if (compute_retired_pending(p)) { d = p; retired_work = true; break; }
    if (!d)
        for (document_t *p = documents; p; p = p->next)
            if (p->cache_bad || (persist && (p->finish_stage || (p->prefetch_enabled && p->cache_touch))) || content_pending(p) ||
                adjacent_runnable(p) ||
                ((!p->claimed && conversion_available(p) && conversion_runnable(p)) ||
                 (!p->stopped && p->claimed && !epd_reader_suspended() && image_pending(p))))
            { d = p; break; }
    if (!d && !conversion_document && !note_conversion_document)
        for (document_t *p = documents; p; p = p->next)
            if (p->status.complete && p->nav_state == DOCUMENT_NAV_PENDING && navigation_allowed(p))
                { d = p; break; }
    if (d && d == navigation) background_rounds = 0;
    worker_document = d;
    rt_hw_interrupt_enable(key);
    if (nav_yield)
        rt_kprintf("[document] nav_yield id=%lu target=%lu demand=%lu:%lu\n",
            (unsigned long)nav_serial, (unsigned long)body_serial,
            (unsigned long)nav_demand, (unsigned long)body_demand);
    if (persist) entry_cache_queue();
    if (!d)
    {
        if (saves && persist) { save_work(); return true; }
        return false;
    }
    if (retired_work) compute_cleanup(d, false);
    if (!cancelled(d) && !d->claimed && conversion_available(d))
    {
        d->claimed = true;
        if (d->note || d->metadata_only) convert_begin(d);
        else {
        document_cache_t *cached;
        if (d->segmented)
        {
            char identity[104];
            snprintf(identity, sizeof(identity), "%08lx-%lu-%lu-%lu-%lu-%lu-%lu%s",
                (unsigned long)hash_bytes(2166136261u, d->file.path, strlen(d->file.path)),
                (unsigned long)d->file.size, (unsigned long)d->file.modified,
                (unsigned long)d->requested.unit, (unsigned long)d->requested.chapter,
                (unsigned long)d->requested.offset, (unsigned long)d->requested.skip_utf8,
                d->full_source ? "-full" : "");
            cached = document_cache_create_source(&d->file, "books", NULL, identity);
        }
        else cached = d->source.external ?
                document_cache_create_source(&d->file, d->app_id, d->directory, d->identity) :
                document_cache_create(&d->file);
        key = rt_hw_interrupt_disable();
        d->cached = cached;
        if (cancelled(d)) document_cache_cancel(cached);
        rt_hw_interrupt_enable(key);
        if (cache_lookup(d))
        {
            const char *binary = document_cache_binary(d->cached);
            uint32_t generation = storage_card_session();
            key = rt_hw_interrupt_disable();
            snprintf(d->cache, sizeof(d->cache), "%s", binary);
            d->cache_generation = generation;
            d->status.end = d->pending_end = d->end;
            d->status.publication = 1; d->status.complete = true;
            worker_changed();
            d->persistent = true; d->cache_touch = true; d->chapter_restore = true; d->adjacent_pending = true;
            if (d->body_first) d->nav_state = DOCUMENT_NAV_DONE;
            rt_hw_interrupt_enable(key);
            log_readable(d);
            if (worker_service) epd_service_ui_request(worker_service, 0);
        }
        else convert_begin(d);
        }
    }
    if (!d->converting && d->status.complete && d->nav_state == DOCUMENT_NAV_PENDING &&
        !conversion_document && !note_conversion_document && navigation_allowed(d)) navigation_begin(d);
    if (d->converting) convert_step(d);
    if (d->finish_stage && persist && persistence_allowed() && !cancelled(d)) convert_finalize(d);
    if (cancelled(d) && !d->converting)
    {
        key = rt_hw_interrupt_disable();
        d->finish_stage = 0;
        result_locked(d, BOOK_CANCELLED); d->status.complete = true;
        rt_hw_interrupt_enable(key);
        if (d->metadata_only && worker_service) epd_service_ui_request(worker_service, 0);
    }
    if (d->cache_bad)
    {
        document_cache_invalidate(d->cached);
        d->cache_bad = d->cache_touch = false;
    }
    else if (persist && d->prefetch_enabled && d->cache_touch)
    {
        d->cache_touch = false;
        if (!cancelled(d)) document_cache_touch(d->cached);
    }
    image_work(d);
    content_work(d);
    if (adjacent_runnable(d)) adjacent_work(d);
    if (d->released) cleanup_work(d);
    log_terminal(d);
    key = rt_hw_interrupt_disable();
    /* A paused cursor needs no work until resume, but must remain eligible
     * for the cancellation pass that releases its worker-owned reader. */
    d->stopped = cancelled(d) || (!image_pending(d) && !content_pending(d) && !d->cache_bad &&
                                 !(d->converting && conversion_runnable(d)) &&
                                 !compute_retired_pending(d) && !(persist && (d->finish_stage || (d->prefetch_enabled && d->cache_touch))) &&
                                 !adjacent_runnable(d) &&
                                 prefetch_document != d);
    worker_document = NULL;
    rt_hw_interrupt_enable(key);
    return true;
}

void document_worker_cancel(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    worker_stopping = true;
    reclaim.ticket = 0; reclaim.resume = false;
    for (document_t *d = documents; d; d = d->next)
    {
        d->cancelled = true;
        compute_cancel(d);
    }
    rt_hw_interrupt_enable(key);
}

void document_worker_shutdown(void)
{
    /* The caller has returned from its last worker_step before draining. */
    document_worker_cancel();
    rt_base_t key;
    for (;;)
    {
        key = rt_hw_interrupt_disable();
        document_t *d = documents;
        while (d && d->stopped && !d->cache_bad && d->cancelled) d = d->next;
        worker_document = d;
        rt_hw_interrupt_enable(key);
        if (!d) break;
        key = rt_hw_interrupt_disable();
        d->cancelled = true; compute_cancel(d);
        rt_hw_interrupt_enable(key);
        prefetch_release(d);
        if (d->cache_bad)
        {
            document_cache_invalidate(d->cached);
            d->cache_bad = false;
        }
        key = rt_hw_interrupt_disable();
        d->cache_touch = false;
        d->stopped = true;
        worker_document = NULL;
        rt_hw_interrupt_enable(key);
    }
    while (saves) save_work();
    while (record_work()) {}
    unsigned retries = 0;
    for (;;)
    {
        /* Pin each pending object while cleanup drops locks. UI disposal can
         * remove completed objects between attempts, so restart from the list. */
        uint32_t after = 0;
        for (;;)
        {
            key = rt_hw_interrupt_disable();
            document_t *d = NULL;
            for (document_t *p = documents; p; p = p->next)
                if (!p->cleanup_done && p->serial > after && (!d || p->serial < d->serial)) d = p;
            worker_document = d;
            if (d) after = d->serial;
            rt_hw_interrupt_enable(key);
            if (!d) break;
            cleanup_work(d);
            key = rt_hw_interrupt_disable(); worker_document = NULL; rt_hw_interrupt_enable(key);
        }
        entry_cache_forget();
        document_cache_release_memory();
        key = rt_hw_interrupt_disable();
        bool pending = false;
        for (document_t *d = documents; d; d = d->next) pending |= !d->cleanup_done;
        rt_hw_interrupt_enable(key);
        if (!pending && !document_cache_cleanup_pending()) break;
        if (!retries) rt_kprintf("[document] shutdown_wait cleanup_pending=1\n");
        rt_thread_mdelay(retries++ < 3 ? 20 : 250);
    }
    if (retries) rt_kprintf("[document] shutdown_done retries=%u\n", retries);
    worker_service = NULL;
}

void document_worker(epd_service_t *service)
{
    worker_stopping = false;
    worker_service = service;
    while (!epd_service_cancelled(service))
        if (!document_worker_step(service)) epd_service_wait(service, conversion_wait());
    document_worker_shutdown();
}

static bool document_input_format(document_format_t format)
{
    return format == DOCUMENT_TXT || format == DOCUMENT_MD || format == DOCUMENT_EPUB ||
           format == DOCUMENT_SPLIT_EPUB || format == DOCUMENT_MOBI || format == DOCUMENT_XHTML;
}

document_t *document_open_source(const document_source_t *source, char *error, size_t size)
{
    rt_tick_t started = rt_tick_get();
    document_service_process();
    open_error = READER_ERROR_NONE;
    if (!source || !source->path || !source->app_id ||
        !source->cache_path || !source->identity || !*source->app_id || !*source->identity ||
        source->path[0] != '/' || source->cache_path[0] != '/' ||
        strlen(source->path) >= BOOKSHELF_PATH_MAX || strlen(source->app_id) >= 64 ||
        strlen(source->cache_path) + 32 >= STORAGE_PATH_MAX ||
        strlen(source->app_id) + strlen(source->identity) + 2 >= BOOKSHELF_PATH_MAX)
    { open_error = READER_ERROR_INPUT; snprintf(error, size, "文档输入或缓存身份无效"); return NULL; }
    if (!document_input_format(source->format))
    { open_error = READER_ERROR_TYPE; snprintf(error, size, "不支持的文档类型"); return NULL; }
    for (const unsigned char *p = (const unsigned char *)source->app_id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
        { open_error = READER_ERROR_APP_ID; snprintf(error, size, "应用标识无效"); return NULL; }
    if (worker_stopping)
    {
        if (documents || worker_service || worker_document)
        { open_error = READER_ERROR_STOPPING; snprintf(error, size, "阅读服务正在停止"); return NULL; }
        worker_stopping = false;
    }
    document_t *d = allocate(sizeof(*d));
    if (!d) { open_error = READER_ERROR_SESSION_MEMORY; snprintf(error, size, "文档会话内存不足"); return NULL; }
    memset(d, 0, sizeof(*d));
    d->opened_at = started;
    d->input = d->output = -1;
    d->generation = d->cache_generation = storage_card_session();
    d->format = source->format;
    d->owners = 1;
    snprintf(d->file.path, sizeof(d->file.path), "%s", source->path);
    snprintf(d->file.name, sizeof(d->file.name), "%s", source->title ? source->title : "");
    d->file.format = source->format;
    snprintf(d->app_id, sizeof(d->app_id), "%s", source->app_id);
    snprintf(d->directory, sizeof(d->directory), "%s", source->cache_path);
    snprintf(d->identity, sizeof(d->identity), "%s", source->identity);
    d->source = *source;
    d->source.external = true;
    d->source.app_id = d->app_id; d->source.cache_path = d->directory;
    d->source.identity = d->identity; d->source.path = d->file.path; d->source.title = d->file.name;
    snprintf(d->status.title, sizeof(d->status.title), "%s", d->file.name);
    rt_base_t key = rt_hw_interrupt_disable();
    if (worker_stopping)
    {
        rt_hw_interrupt_enable(key);
        open_error = READER_ERROR_STOPPING; snprintf(error, size, "阅读服务正在停止");
        log_open(d);
        document_cache_destroy(d->cached); epd_app_free(d); return NULL;
    }
    d->demand = d->serial = ++sequence;
    d->next = documents; documents = d;
    conversion_preempt_locked(d);
    rt_hw_interrupt_enable(key);
    log_open(d);
    wake_worker();
    return d;
}

document_t *document_open(const bookshelf_item_t *file, char *error, size_t size)
{ return document_open_at(file, NULL, error, size); }

static document_toc_t *toc_acquire_locked(const document_t *d)
{
    for (document_t *p = documents; p; p = p->next)
        if (p->toc_context && !p->released && !cancelled(p) && p->generation == d->generation &&
            p->source_revision == d->source_revision && p->file.size == d->file.size &&
            p->file.modified == d->file.modified && !strcmp(p->file.path, d->file.path)) {
            ++p->toc_context->references; return p->toc_context;
        }
    return NULL;
}

static document_t *document_find_locked(const bookshelf_item_t *file,
                                        const document_anchor_t *wanted, bool segmented, bool full_source)
{
    for (document_t *p = documents; p; p = p->next)
        if (!p->note && !p->metadata_only && !p->source.external && !p->released && !cancelled(p) && p->segmented == segmented && p->full_source == full_source &&
            !strcmp(p->file.path, file->path) && p->file.size == file->size && p->file.modified == file->modified &&
            !memcmp(&p->requested, wanted, sizeof(*wanted))) return p;
    return NULL;
}

static void document_acquire_locked(document_t *d, bool foreground)
{
    ++d->owners;
    if (foreground)
    {
        d->demand = ++sequence;
        if (!d->status.complete) conversion_preempt_locked(d);
    }
}

static document_t *document_open_at_internal(const bookshelf_item_t *file, const document_anchor_t *anchor,
                                             char *error, size_t size, bool foreground, bool full_source)
{
    rt_tick_t started = rt_tick_get();
    open_error = READER_ERROR_NONE;
    if (!file || !memchr(file->path, 0, sizeof(file->path)) ||
        !memchr(file->name, 0, sizeof(file->name)) || !resource_path_valid(file->path))
    { open_error = READER_ERROR_INPUT; snprintf(error, size, "文档输入路径无效"); return NULL; }
    if (!document_input_format((document_format_t)file->format))
    { open_error = READER_ERROR_TYPE; snprintf(error, size, "不支持的文档类型"); return NULL; }
    document_anchor_t wanted = {0};
    bool segmented = file->format == BOOK_FORMAT_TXT || file->format == BOOK_FORMAT_EPUB ||
                     file->format == BOOK_FORMAT_SPLIT_EPUB;
    if (segmented)
    {
        wanted.unit = file->format == BOOK_FORMAT_TXT ? BOOK_DOCUMENT_SOURCE_FILE_BYTE : BOOK_DOCUMENT_SOURCE_BODY_UTF8;
        wanted.chapter = file->format == BOOK_FORMAT_SPLIT_EPUB ? 0 : UINT32_MAX;
        if (anchor && anchor->unit == wanted.unit) wanted = *anchor;
    }
    else if (file->format == BOOK_FORMAT_MARKDOWN || file->format == BOOK_FORMAT_MOBI)
    {
        wanted.unit = DOCUMENT_SOURCE_SPOOL;
        if (anchor && anchor->unit == DOCUMENT_SOURCE_SPOOL && !anchor->chapter &&
            anchor->offset <= INT32_MAX && anchor->skip_utf8 <= DOCUMENT_TEXT_LIMIT) wanted = *anchor;
    }
    rt_base_t key = rt_hw_interrupt_disable();
    if (worker_stopping && !documents && !worker_service && !worker_document) worker_stopping = false;
    document_t *existing = worker_stopping ? NULL : document_find_locked(file, &wanted, segmented, full_source);
    if (existing) document_acquire_locked(existing, foreground);
    bool stopping = worker_stopping;
    rt_hw_interrupt_enable(key);
    if (existing) { wake_worker(); return existing; }
    if (stopping) { open_error = READER_ERROR_STOPPING; snprintf(error, size, "阅读服务正在停止"); return NULL; }
    document_t *d = allocate(sizeof(*d));
    if (!d) { open_error = READER_ERROR_SESSION_MEMORY; snprintf(error, size, "文档会话内存不足"); return NULL; }
    memset(d, 0, sizeof(*d));
    d->opened_at = started;
    d->input = d->output = -1;
    d->file = *file;
    d->owners = 1; d->segmented = segmented; d->requested = wanted; d->full_source = full_source;
    d->body_first = file->format == BOOK_FORMAT_EPUB;
    d->first_body = d->body_first && !(anchor && anchor->unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8);
    if (d->body_first) { d->nav_state = DOCUMENT_NAV_PENDING; d->source_revision = storage_path_revision(file->path); }
    d->chapter = wanted.chapter; d->source_total = file->format == BOOK_FORMAT_TXT ? file->size : 0;
    d->format = (document_format_t)file->format;
    d->generation = storage_card_session();
    snprintf(d->status.title, sizeof(d->status.title), "%s", file->name);
    document_toc_t *spare = NULL;
    if (d->body_first) {
        key = rt_hw_interrupt_disable(); spare = toc_acquire_locked(d); rt_hw_interrupt_enable(key);
        if (!spare) {
            spare = allocate(sizeof(*spare));
            if (!spare) { epd_app_free(d); open_error = READER_ERROR_SESSION_MEMORY;
                snprintf(error, size, "目录上下文内存不足"); return NULL; }
            memset(spare, 0, sizeof(*spare)); spare->references = 1; spare->bytes = sizeof(*spare);
        }
    }
    key = rt_hw_interrupt_disable();
    existing = worker_stopping ? NULL : document_find_locked(file, &wanted, segmented, full_source);
    if (existing) document_acquire_locked(existing, foreground);
    stopping = worker_stopping;
    if (!existing && !stopping)
    {
        if (spare) {
            d->toc_context = toc_acquire_locked(d);
            if (!d->toc_context) { d->toc_context = spare; spare = NULL; }
        }
        d->serial = ++sequence; d->demand = foreground ? d->serial : 0;
        d->next = documents; documents = d;
        if (foreground) conversion_preempt_locked(d);
    }
    rt_hw_interrupt_enable(key);
    toc_release(spare);
    if (existing || stopping)
    {
        epd_app_free(d);
        if (stopping) { open_error = READER_ERROR_STOPPING; snprintf(error, size, "阅读服务正在停止"); }
        else wake_worker();
        return existing;
    }
    log_open(d);
    wake_worker();
    return d;
}

document_t *document_open_at(const bookshelf_item_t *file, const document_anchor_t *anchor,
                            char *error, size_t size)
{
    return document_open_at_internal(file, anchor, error, size, true, false);
}

document_t *document_pagination_open(document_t *d, uint32_t chapter)
{
    if (!d) return NULL;
    rt_base_t key = rt_hw_interrupt_disable();
    if (cancelled(d) || d->released || worker_stopping) { rt_hw_interrupt_enable(key); return NULL; }
    if (d->source.external) {
        if (chapter) { rt_hw_interrupt_enable(key); return NULL; }
        document_acquire_locked(d, false);
        rt_hw_interrupt_enable(key); wake_worker(); return d;
    }
    bool chapters = d->format == DOCUMENT_EPUB || d->format == DOCUMENT_SPLIT_EPUB;
    bool valid = chapters ? chapter < d->chapter_count : chapter == 0;
    bookshelf_item_t file = d->file;
    rt_hw_interrupt_enable(key);
    if (!valid) return NULL;
    document_anchor_t anchor = {chapters ? BOOK_DOCUMENT_SOURCE_BODY_UTF8 :
        file.format == BOOK_FORMAT_TXT ? BOOK_DOCUMENT_SOURCE_FILE_BYTE : DOCUMENT_SOURCE_SPOOL,
        chapters ? chapter : file.format == BOOK_FORMAT_TXT ? UINT32_MAX : 0, 0, 0};
    char error[96];
    return document_open_at_internal(&file, &anchor, error, sizeof(error), false, file.format == BOOK_FORMAT_TXT);
}

void document_close(document_t *d)
{
    if (!d) return;
    rt_base_t key = rt_hw_interrupt_disable();
    if (d->owners) --d->owners;
    bool owned = false;
    for (document_t *p = documents; p; p = p->next)
        if (p->owners && !p->released && !strcmp(p->file.path, d->file.path)) owned = true;
    for (document_t *p = documents; p; p = p->next)
        if ((p == d && !p->owners && (!p->retained || !p->status.complete)) ||
            (!owned && !strcmp(p->file.path, d->file.path)))
        {
            p->cancelled = p->released = true;
            compute_cancel(p);
            for (document_image_request_t *j = image_requests; j; j = j->next)
                if (j->document == p) j->released = true;
            for (document_record_request_t *j = record_requests; j; j = j->next)
                if (j->document == p) j->released = true;
        }
    rt_hw_interrupt_enable(key);
    wake_worker();
}

void document_service_process(void)
{
    image_collect();
    record_collect();
    document_t *d = documents;
    while (d)
    {
        if (!d->cancelled && cancelled(d))
        {
            rt_base_t cancel_key = rt_hw_interrupt_disable();
            d->cancelled = true;
            compute_cancel(d);
            rt_hw_interrupt_enable(cancel_key);
            wake_worker();
        }
        rt_base_t key = rt_hw_interrupt_disable();
        document_t *next = d->next;
        /* Superseded requests that never reached the worker own no backend. */
        if (d->released && !d->claimed && worker_document != d) d->stopped = true;
        bool dispose = d->cleanup_done && d->stopped && d->released && !d->index_active && worker_document != d && prefetch_document != d && !image_referenced(d) && !record_referenced(d) &&
                       (!worker_service || !d->cache_bad);
        if (dispose)
        {
            document_t **link = &documents;
            while (*link && *link != d) link = &(*link)->next;
            *link = d->next;
            for (document_t *p = documents; p; p = p->next)
            { if (p->previous == d) p->previous = NULL; if (p->following == d) p->following = NULL; }
        }
        rt_hw_interrupt_enable(key);
        if (dispose)
        {
            if (d->source.release_user) d->source.release_user(d->source.user);
            epd_app_free(d->note);
            epd_app_free(d->cover.memory);
            epd_app_free(d);
        }
        d = next;
    }
}

static void cleanup_work(document_t *d)
{
    if (!d || d->cleanup_done || d->index_active) return;
    for (document_record_request_t *j = record_requests; j; j = j->next)
        if (j->document == d && j->active) return;
    for (document_image_request_t *j = image_requests; j; j = j->next)
        if (j->document == d && j->active) return;
    if (d->converting) { convert_step(d); if (d->converting) return; }
    if (d->observing) { document_cache_observe_end(d->cached); d->observing = false; }
    if (!compute_cleanup(d, true)) return;
    prefetch_release(d);
    storage_lock(); readers_close(d); storage_unlock();
    while (d->readers) document_reader_destroy(d->readers);
    content_dispose(d);
    if (d->output >= 0) { storage_lock(); close(d->output); storage_unlock(); d->output = -1; }
    if (d->cache[0] && !d->persistent && d->temp_owned)
    {
        storage_lock();
        if (media_valid(d->cache, d->cache_generation)) unlink(d->cache);
        storage_unlock();
    }
    rt_base_t key = rt_hw_interrupt_disable();
    document_cache_t *cached = d->cached; d->cached = NULL;
    rt_hw_interrupt_enable(key);
    document_cache_destroy(cached);
    epd_app_free(d->write_buffer); d->write_buffer = NULL;
    epd_app_free(d->index_blob); d->index_blob = NULL;
    while (d->navigation) { document_navigation_t *n = d->navigation; d->navigation = n->next; epd_app_free(n); }
    while (d->locations) { document_location_t *n = d->locations; d->locations = n->next; epd_app_free(n); }
    while (d->parts) { document_part_t *n = d->parts; d->parts = n->next; epd_app_free(n); }
    toc_release(d->toc_context); d->toc_context = NULL;
    rt_kprintf("[document] cleanup_done id=%lu elapsed_ms=%u\n",
               (unsigned long)d->serial, elapsed_ms(d->opened_at));
    d->cleanup_done = true;
    worker_changed();
}

void document_service_shutdown(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    worker_stopping = true;
    document_save_t *pending = saves;
    saves = NULL;
    rt_hw_interrupt_enable(key);
    while (pending)
    {
        document_save_t *next = pending->next;
        save_release(pending);
        pending = next;
    }
    for (document_t *d = documents; d; d = d->next) document_close(d);
    document_service_process();
}
void document_status(document_t *d, document_status_t *status)
{
    rt_base_t key = rt_hw_interrupt_disable();
    *status = d->status;
    status->stopped = d->stopped;
    rt_hw_interrupt_enable(key);
    if (cancelled(d) && !status->error[0])
    { status->error_code = READER_ERROR_CANCELLED; status->result = BOOK_CANCELLED; snprintf(status->error, sizeof(status->error), "文档读取已取消或存储状态已变化，请重新打开"); }
}
static void document_spool_progress(document_t *d, document_progress_t *progress)
{
    rt_base_t key = rt_hw_interrupt_disable();
    progress->end = d->status.end;
    progress->publication = d->status.publication;
    progress->complete = d->status.complete;
    progress->result = d->status.result;
    progress->source_begin = d->source_begin; progress->source_next = d->source_next;
    progress->source_total = d->source_total; progress->chapter = d->chapter;
    progress->chapters = d->chapter_count;
    rt_hw_interrupt_enable(key);
    if (cancelled(d)) progress->result = BOOK_CANCELLED;
}
void document_progress(document_t *d, document_progress_t *progress)
{
    document_spool_progress(d, progress);
    rt_base_t key = rt_hw_interrupt_disable();
    uint32_t end = d->body_end;
    rt_hw_interrupt_enable(key);
    if (end && end <= progress->end) {
        progress->end = end;
        if (d->body_publication) progress->publication = d->body_publication;
    }
}

unsigned document_toc_count(document_t *d)
{
    if (!d || cancelled(d)) return 0;
    if (d->toc_context && d->toc_context->count) return d->toc_context->count;
    if (d->body_first && (!toc_complete(d) || d->chapter_restore)) return d->explicit_toc_count;
    return d->explicit_toc_count ? d->explicit_toc_count : d->toc_count;
}

int document_target_require(document_t *d, bool required)
{
    if (!d) return -1;
    rt_base_t key = rt_hw_interrupt_disable();
    bool valid = !cancelled(d) && d->status.result == BOOK_OK;
    bool changed = required && valid && !d->target_required;
    d->target_required = required && valid;
    if (changed) { d->demand = ++sequence; d->stopped = false; }
    int ready = !valid ? -1 : d->status.complete && (!d->chapter_restore ||
        (d->body_first && d->body_end && d->chapter_scan >= d->body_end)) ? 1 : 0;
    rt_hw_interrupt_enable(key);
    if (changed && !ready) wake_worker();
    return ready;
}

int document_navigation_request(document_t *d)
{
    if (!d || cancelled(d) || d->status.result != BOOK_OK) return -1;
    rt_base_t key = rt_hw_interrupt_disable();
    /* Polling the same failed request must not restart a BUSY retry burst.
     * An automatic attempt may be retried by the first explicit request. */
    if (d->nav_deferred && !d->nav_requested) d->nav_deferred = false;
    int ready = d->toc_context && d->toc_context->complete ? 1 :
        d->nav_state == DOCUMENT_NAV_FAILED || d->nav_deferred ? -1 :
        d->status.complete && (!d->body_first || toc_complete(d)) && !d->chapter_restore ? 1 : 0;
    bool changed = !ready && !d->nav_requested;
    if (changed) {
        d->demand = ++sequence;
        d->nav_requested = true; d->stopped = false;
    }
    rt_hw_interrupt_enable(key);
    if (changed) wake_worker();
    return ready;
}

void document_navigation_release(document_t *d)
{
    if (!d) return;
    rt_base_t key = rt_hw_interrupt_disable();
    d->nav_requested = false;
    rt_hw_interrupt_enable(key);
}

bool document_source_position(document_t *d, uint32_t record, uint32_t byte, document_anchor_t *anchor)
{
    if (!d || !anchor || cancelled(d)) return false;
    if (!d->segmented && (d->format == DOCUMENT_MD || d->format == DOCUMENT_MOBI))
    {
        if (record > d->status.end || byte > DOCUMENT_TEXT_LIMIT || (record == d->status.end && byte)) return false;
        *anchor = (document_anchor_t){DOCUMENT_SOURCE_SPOOL, 0, record, byte};
        return true;
    }
    document_location_t *found = NULL;
    for (document_location_t *n = d->locations; n && n->record <= record; n = n->next) found = n;
    if (!found || found->record >= d->status.end) return false;
    *anchor = found->anchor;
    uint32_t skip = found->record == record ? (byte > found->bytes ? found->bytes : byte) : found->bytes;
    if (skip > UINT32_MAX - anchor->skip_utf8) return false;
    anchor->skip_utf8 += skip;
    if (anchor->unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8)
    {
        if (anchor->skip_utf8 > UINT32_MAX - anchor->offset) return false;
        anchor->offset += anchor->skip_utf8; anchor->skip_utf8 = 0;
    }
    return true;
}

int document_target_cursor(document_t *d, uint32_t *record, uint32_t *byte)
{
    if (!d || !record || !byte || cancelled(d) || d->status.result) return -1;
    if (!d->segmented)
    {
        if (d->requested.unit != DOCUMENT_SOURCE_SPOOL) { *record = *byte = 0; return 1; }
        if (d->requested.offset < d->status.end ||
            (d->status.complete && d->requested.offset == d->status.end && !d->requested.skip_utf8))
        { *record = d->requested.offset; *byte = d->requested.skip_utf8; return 1; }
        return d->status.complete ? -1 : 0;
    }
    if (d->target_ready && d->target_record <= d->status.end)
    { *record = d->target_record; *byte = d->target_byte; return 1; }
    return d->status.complete && !d->chapter_restore ? -1 : 0;
}

static bool adjacent_anchor(document_t *d, int direction, document_anchor_t *anchor)
{
    if (!d->segmented || !direction) return false;
    *anchor = (document_anchor_t){d->requested.unit, d->chapter, 0, 0};
    if (d->format == DOCUMENT_TXT)
    {
        if (d->full_source && direction < 0) return false;
        if (direction > 0)
        {
            if (!d->status.complete || !d->source_next || d->source_next >= d->file.size) return false;
            anchor->offset = d->source_next;
        }
        else
        {
            if (!d->source_begin) return false;
            anchor->offset = d->source_begin;
        }
    }
    else
    {
        if (direction < 0 && d->source_begin)
        { anchor->offset = d->source_begin; return true; }
        if (!d->chapter_count || (direction > 0 && d->chapter + 1 >= d->chapter_count) ||
            (direction < 0 && !d->chapter)) return false;
        anchor->chapter = direction > 0 ? d->chapter + 1 : d->chapter - 1;
        anchor->offset = direction < 0 ? UINT32_MAX : 0;
    }
    return true;
}

/* A complete spine can satisfy either chapter boundary without converting a
 * new tail slice. Intra-chapter and TXT boundaries still identify exact slices. */
static document_t *adjacent_complete_locked(document_t *d, const document_anchor_t *anchor)
{
    if (!chapter_format(d) || anchor->chapter == d->chapter || anchor->skip_utf8 ||
        (anchor->offset && anchor->offset != UINT32_MAX)) return NULL;
    for (document_t *p = documents; p; p = p->next)
        if (!p->note && !p->source.external && !p->released && !cancelled(p) && p->segmented &&
            p->format == d->format && p->requested.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8 &&
            p->chapter == anchor->chapter && !p->source_begin && !p->chapter_restore &&
            p->status.complete && p->status.result == BOOK_OK &&
            p->file.size == d->file.size && p->file.modified == d->file.modified &&
            !strcmp(p->file.path, d->file.path)) return p;
    return NULL;
}
static void adjacent_link_locked(document_t *d, document_t *p, int direction)
{
    if ((direction > 0 ? d->following : d->previous) != p) worker_changed();
    d->retained = p->retained = true;
    if (direction > 0) { d->following = p; p->previous = d; }
    else { d->previous = p; p->following = d; }
}
static document_t *document_adjacent_internal(document_t *d, int direction, bool foreground)
{
    if (!d) return NULL;
    document_anchor_t anchor;
    rt_base_t key = rt_hw_interrupt_disable();
    if (cancelled(d) || !adjacent_anchor(d, direction, &anchor))
    { rt_hw_interrupt_enable(key); return NULL; }
    document_t *complete = adjacent_complete_locked(d, &anchor);
    document_t *p = complete ? complete : direction > 0 ? d->following : d->previous;
    if (p && !cancelled(p) && !p->released &&
        (complete || !memcmp(&p->requested, &anchor, sizeof(anchor))))
    {
        document_acquire_locked(p, foreground);
        adjacent_link_locked(d, p, direction);
        rt_hw_interrupt_enable(key); wake_worker(); return p;
    }
    rt_hw_interrupt_enable(key);
    char error[96];
    p = document_open_at_internal(&d->file, &anchor, error, sizeof(error), foreground, false);
    if (!p) return NULL;
    key = rt_hw_interrupt_disable();
    complete = adjacent_complete_locked(d, &anchor);
    document_t *linked = complete ? complete : direction > 0 ? d->following : d->previous;
    if (d->released || cancelled(p))
    { rt_hw_interrupt_enable(key); document_close(p); return NULL; }
    if (linked && linked != p && !cancelled(linked) && !linked->released &&
        (complete || !memcmp(&linked->requested, &anchor, sizeof(anchor))))
    {
        document_acquire_locked(linked, foreground);
        adjacent_link_locked(d, linked, direction);
        rt_hw_interrupt_enable(key); document_close(p); wake_worker(); return linked;
    }
    adjacent_link_locked(d, p, direction);
    rt_hw_interrupt_enable(key);
    return p;
}

document_t *document_adjacent(document_t *d, int direction)
{ return document_adjacent_internal(d, direction, true); }

static void adjacent_work(document_t *d)
{
    rt_base_t key = rt_hw_interrupt_disable();
    d->adjacent_pending = false;
    bool ready = d->segmented && !d->source.external && d->target_ready && d->status.result == BOOK_OK &&
                 d->status.complete && !cancelled(d);
    rt_hw_interrupt_enable(key);
    if (!ready) return;
    for (unsigned i = 0; i < 2; ++i)
    {
        int direction = i ? -1 : 1;
        key = rt_hw_interrupt_disable();
        document_t *p = i ? d->previous : d->following;
        bool missing = !p || cancelled(p) || p->released;
        rt_hw_interrupt_enable(key);
        if (missing && epd_app_psram_available() > DOCUMENT_CONTENT_CACHE_RESERVE + 256u * 1024u)
        {
            p = document_adjacent_internal(d, direction, false);
            if (p) { key = rt_hw_interrupt_disable(); --p->owners; rt_hw_interrupt_enable(key); }
        }
    }
    key = rt_hw_interrupt_disable();
    for (document_t *p = documents; p; p = p->next)
        if (p != d && !p->owners && !p->released && !strcmp(p->file.path, d->file.path) &&
            p != d->previous && p != d->following)
        {
            p->retained = false;
            document_close(p);
        }
    rt_hw_interrupt_enable(key);
}

document_t *document_toc_open(document_t *d, unsigned index, char *error, size_t size)
{
    if (!d || cancelled(d)) return NULL;
    for (document_navigation_t *n = toc_head(d); n; n = n->next)
        if (navigation_is_toc(d, n) && !index--)
        {
            if (n->chapter == UINT32_MAX) return NULL;
            document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, n->chapter, 0, 0};
            return document_open_at(&d->file, &anchor, error, size);
        }
    return NULL;
}
bool document_navigation_ready(document_t *d)
{ return d && !cancelled(d) && d->status.result == BOOK_OK &&
    ((d->toc_context && d->toc_context->complete) ||
     (d->status.complete && (!d->body_first || toc_complete(d)) && !d->chapter_restore)); }
bool document_toc_entry(document_t *d, unsigned index, char *title, size_t title_size,
                        char *target, size_t target_size, unsigned *level)
{
    if (!d || cancelled(d)) return false;
    for (document_navigation_t *n = toc_head(d); n; n = n->next)
        if (navigation_is_toc(d, n) && !index--)
        {
            if (title && title_size) snprintf(title, title_size, "%s", n->text + n->title_at);
            if (target && target_size) snprintf(target, target_size, "%s", n->text);
            if (level) *level = n->level;
            return true;
        }
    return false;
}
bool document_anchor_find(document_t *d, const char *target, uint32_t *record)
{
    if (!d || !target || !record || cancelled(d)) return false;
    for (document_navigation_t *n = d->navigation; n; n = n->next)
        if (n->kind == DOCUMENT_ANCHOR && !n->text[n->title_at] && n->record < d->status.end &&
            (!strcmp(n->text, target) || (target[0] == '#' && !strcmp(n->text, target + 1))))
        { *record = n->record; return true; }
    return false;
}
bool document_link_target(document_t *d, uint32_t record, char *target, size_t capacity)
{
    if (!d || !target || !capacity || cancelled(d)) return false;
    for (document_navigation_t *n = d->navigation; n; n = n->next)
        if (n->kind == DOCUMENT_LINK && n->record == record)
        { snprintf(target, capacity, "%s", n->text); return true; }
    return false;
}
#include "document_note.inc"

document_t *document_link_open(document_t *d, uint32_t record, char *error, size_t size)
{
    char target[DOCUMENT_TEXT_LIMIT + 1];
    if (!document_link_target(d, record, target, sizeof(target)) || !target[0] ||
        strstr(target, "://") || !strncmp(target, "mailto:", 7)) goto missing;
    const char *fragment = strchr(target, '#');
    size_t root = fragment ? (size_t)(fragment - target) : strlen(target);
    if (!root || !d->segmented) {
        rt_base_t key = rt_hw_interrupt_disable();
        if (cancelled(d) || d->released) { rt_hw_interrupt_enable(key); goto missing; }
        document_acquire_locked(d, true);
        rt_hw_interrupt_enable(key); wake_worker(); return d;
    }
    for (document_part_t *part = d->toc_context ? d->toc_context->parts : d->parts; part; part = part->next)
        if (strlen(part->text) == root && !strncmp(part->text, target, root)) {
            document_anchor_t anchor = {BOOK_DOCUMENT_SOURCE_BODY_UTF8, part->number, 0, 0};
            return document_open_at(&d->file, &anchor, error, size);
        }
missing:
    if (d && !cancelled(d) && document_navigation_request(d) == 0) {
        if (error && size) error[0] = 0;
        return NULL;
    }
    if (error && size) snprintf(error, size, "链接目标不存在");
    return NULL;
}
static bool toc_current_title(document_t *d, uint32_t record, char *title, size_t capacity, unsigned *index)
{
    document_toc_t *toc = d->toc_context;
    uint32_t revision = toc->revision, local_revision = d->navigation_revision, end = d->status.end;
    title[0] = 0;
    if (!d->title_valid || d->title_revision != local_revision ||
        d->title_toc_revision != revision || d->title_record != record || d->title_end != end) {
        const char *root = NULL;
        for (document_navigation_t *n = d->navigation; n; n = n->next)
            if (n->kind == DOCUMENT_ANCHOR && !n->text[n->title_at] && !strchr(n->text, '#') &&
                n->record <= record && n->record < end) root = n->text;
        document_navigation_t *best = NULL, *first = NULL;
        uint32_t best_record = 0;
        unsigned at = 0, chosen = 0, first_index = 0;
        size_t length = root ? strlen(root) : 0;
        for (document_navigation_t *n = toc->first; root && n; n = n->next, ++at) {
            if (!strncmp(root, n->text, length) && (!n->text[length] || n->text[length] == '#')) {
                if (!first) { first = n; first_index = at; }
                uint32_t local;
                if (document_anchor_find(d, n->text, &local) && local <= record &&
                    (!best || local > best_record || (local == best_record && n->level > best->level))) {
                    best = n; best_record = local; chosen = at;
                }
            }
        }
        /* The first declared title also covers the prefix before its heading. */
        if (!best) { best = first; chosen = first_index; }
        d->title_node = best; d->title_index = chosen;
        d->title_revision = local_revision; d->title_toc_revision = revision;
        d->title_record = record; d->title_end = end; d->title_valid = true;
    }
    if (!d->title_node) return false;
    snprintf(title, capacity, "%s", d->title_node->text + d->title_node->title_at);
    if (index) *index = d->title_index;
    return true;
}

bool document_current_title(document_t *d, uint32_t record, char *title, size_t capacity, unsigned *index)
{
    if (!d || !title || !capacity || cancelled(d)) return false;
    if (d->toc_context && d->toc_context->count) return toc_current_title(d, record, title, capacity, index);
    if (d->toc_context && d->title_toc_revision != d->toc_context->revision) {
        d->title_valid = false; d->title_toc_revision = d->toc_context->revision;
    }
    uint32_t revision = d->navigation_revision, end = d->status.end;
    if (!d->title_valid || d->title_revision != revision || d->title_record != record || d->title_end != end) {
        document_navigation_t *best = NULL;
        const char *root = NULL;
        unsigned at = 0, chosen = 0;
        for (document_navigation_t *n = d->navigation; n; n = n->next) {
            if (n->kind != DOCUMENT_ANCHOR) continue;
            if (!n->text[n->title_at]) {
                if (n->record <= record && n->record < end && !strchr(n->text, '#')) root = n->text;
                continue;
            }
            if (!navigation_is_toc(d, n)) continue;
            if (n->title_record <= record && n->title_record < end &&
                (!best || n->title_record > best->title_record ||
                 (n->title_record == best->title_record && n->level > best->level))) {
                best = n; chosen = at;
            }
            ++at;
        }
        size_t length = root ? strlen(root) : 0;
        if (best && root && (strncmp(root, best->text, length) ||
            (best->text[length] && best->text[length] != '#'))) best = NULL;
        /* A published body heading does not depend on the optional TOC. */
        if (!best && root) {
            for (document_navigation_t *n = d->navigation; n; n = n->next)
                if (n->kind == DOCUMENT_ANCHOR && !n->explicit_toc && n->text[n->title_at] &&
                    n->title_record <= record && n->title_record < end &&
                    !strncmp(root, n->text, length) && (!n->text[length] || n->text[length] == '#') &&
                    (!best || n->title_record > best->title_record ||
                     (n->title_record == best->title_record && n->level > best->level))) {
                    best = n; chosen = UINT_MAX;
                }
        }
        if (!best && root) {
            at = 0;
            for (document_navigation_t *n = d->navigation; n; n = n->next)
                if (navigation_is_toc(d, n)) {
                    if (!strncmp(root, n->text, length) && (!n->text[length] || n->text[length] == '#')) {
                        best = n; chosen = at; break;
                    }
                    ++at;
                }
        }
        d->title_node = best; d->title_index = chosen;
        d->title_revision = revision; d->title_record = record; d->title_end = end; d->title_valid = true;
    }
    title[0] = 0;
    if (!d->title_node) return false;
    snprintf(title, capacity, "%s", d->title_node->text + d->title_node->title_at);
    if (index) *index = d->title_index;
    return true;
}

bool document_chapter_range(document_t *d, uint32_t record, document_chapter_range_t *range)
{
    if (!d || !range) return false;
    memset(range, 0, sizeof(*range));
    if (cancelled(d)) return false;
    rt_base_t key = rt_hw_interrupt_disable();
    uint32_t visible = d->status.end;
    if (d->body_end && d->body_end <= visible) visible = d->body_end;
    if (record >= visible || d->status.result != BOOK_OK)
    { rt_hw_interrupt_enable(key); return false; }
    range->end = range->next_end = visible;
    range->publication = d->body_publication ? d->body_publication : d->status.publication;
    range->end_known = range->next_end_known = d->status.complete;
    if (d->chapter_restore && !(d->body_end && d->body_end <= d->status.end))
        range->end_known = range->next_end_known = false;
    else if (chapter_format(d) && !d->chapter_failed && d->chapters)
    {
        range->indexed = true;
        unsigned following = 0;
        for (document_chapters_t *c = d->chapters; c && following < 2; c = c->next)
        {
            if (c->count && c->starts[c->count - 1] <= record)
            {
                range->previous_begin = c->count > 1 ? c->starts[c->count - 2] : range->begin;
                range->begin = c->starts[c->count - 1];
                continue;
            }
            for (unsigned i = 0; i < c->count && following < 2; ++i)
            {
                uint32_t start = c->starts[i];
                if (start >= visible) break;
                if (start <= record)
                { range->previous_begin = range->begin; range->begin = start; }
                else if (++following == 1)
                { range->end = start; range->end_known = true; }
                else { range->next_end = start; range->next_end_known = true; }
            }
        }
    }
    rt_hw_interrupt_enable(key);
    return true;
}

void document_prefetch_enable(document_t *d, bool enabled)
{
    if (!d) return;
    rt_base_t key = rt_hw_interrupt_disable();
    document_t *previous_owner = flow_owner();
    bool was_allowed = persistence_allowed();
    bool suspended = epd_reader_suspended();
    bool changed = !d->cancelled && (!d->demand_controlled || d->prefetch_enabled != enabled ||
                                    d->suspend_seen != suspended);
    if (changed)
    {
        d->prefetch_enabled = enabled;
        d->demand_controlled = true;
        d->suspend_seen = suspended;
        d->adjacent_pending = enabled;
        d->stopped = false;
    }
    if (!d->cancelled) d->flow_order = ++flow_sequence;
    bool transferred = previous_owner != flow_owner();
    bool resumed = !was_allowed && persistence_allowed();
    rt_hw_interrupt_enable(key);
    if (changed)
    {
        rt_kprintf("[document] flow id=%lu prefetch=%u suspended=%u end=%lu complete=%u\n",
            (unsigned long)d->serial, (unsigned)enabled, (unsigned)suspended, (unsigned long)d->status.end,
            (unsigned)d->status.complete);
    }
    if (changed || resumed || transferred) wake_worker();
}

void document_chapter_prefetch(document_t *d, uint32_t record)
{
    if (!d) return;
    rt_base_t key = rt_hw_interrupt_disable();
    if (!d->cancelled && (!d->prefetch_request ||
        d->prefetch_record / DOCUMENT_IO_WINDOW != record / DOCUMENT_IO_WINDOW))
    {
        d->prefetch_record = record;
        if (!++d->prefetch_request) ++d->prefetch_request;
        d->stopped = false;
    }
    rt_hw_interrupt_enable(key);
    wake_worker();
}

document_reader_t *document_reader_create(document_t *d)
{
    if (!d || cancelled(d)) return NULL;
    document_reader_t *r = allocate(sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->document = d;
    r->window = allocate(DOCUMENT_IO_WINDOW);
    storage_lock();
    rt_base_t key = rt_hw_interrupt_disable();
    r->next = d->readers; d->readers = r;
    rt_hw_interrupt_enable(key);
    storage_unlock();
    return r;
}

void document_reader_destroy(document_reader_t *r)
{
    if (!r) return;
    document_t *d = r->document;
    storage_lock();
    rt_base_t key = rt_hw_interrupt_disable();
    document_reader_t **link = &d->readers;
    while (*link && *link != r) link = &(*link)->next;
    if (*link) *link = r->next;
    if (d->prefetch_reader == r) d->prefetch_reader = NULL;
    if (d->record_reader == r) d->record_reader = NULL;
    if (d->default_reader == r) d->default_reader = NULL;
    rt_hw_interrupt_enable(key);
    if (!d->readers && d->input >= 0) { close(d->input); d->input = -1; }
    storage_unlock();
    epd_app_free(r->window); epd_app_free(r);
}

/* storage_lock serializes cursor I/O and the completed-cache name switch. */
static void readers_close(document_t *d)
{
    if (d->input >= 0) close(d->input);
    d->input = -1;
    for (document_reader_t *r = d->readers; r; r = r->next)
        r->window_used = 0;
}

/* Memory windows remain valid; a later miss reopens the shared cursor. */
static unsigned cache_inputs_release(void)
{
    unsigned released = 0;
    for (;;) {
        rt_base_t key = rt_hw_interrupt_disable();
        document_t *d = documents;
        while (d && d->input < 0) d = d->next;
        rt_hw_interrupt_enable(key);
        if (!d || close(d->input) != 0) break;
        key = rt_hw_interrupt_disable();
        d->input = -1;
        d->input_position = d->input_visible = 0;
        rt_hw_interrupt_enable(key);
        ++released;
    }
    return released;
}

static bool reader_fill(document_reader_t *r, uint32_t offset, void *data, size_t length)
{
    document_t *d = r->document;
    if (d->input_position != offset)
    {
        if (lseek(d->input, offset, SEEK_SET) != (off_t)offset) return false;
        d->input_position = offset;
    }
    uint8_t *p = data;
    while (length)
    {
        if (cancelled(d)) return false;
        size_t chunk = length > DOCUMENT_IO_WINDOW ? DOCUMENT_IO_WINDOW : length;
        int n = read(d->input, p, chunk);
        if (n <= 0) return false;
        d->input_position += n;
        p += n; length -= n;
    }
    return !cancelled(d);
}

static bool navigation_restore_tail(document_t *d)
{
    return d->body_first && d->chapter_restore && d->status.complete &&
        d->status.result == BOOK_OK && d->body_end && d->body_end <= d->status.end &&
        d->chapter_scan >= d->body_end;
}

static void cache_read_failed(document_reader_t *r, uint32_t offset)
{
    document_t *d = r->document;
    if (!d->persistent || cancelled(d)) return;
    /* The restore worker reports tail failures separately from body errors. */
    if (r == d->chapter_reader && navigation_restore_tail(d) && offset >= d->body_end) {
        struct stat st;
        if (d->input >= 0 && !fstat(d->input, &st) && st.st_size >= (off_t)d->body_end && !cancelled(d)) return;
    }
    rt_base_t key = rt_hw_interrupt_disable();
    d->cache_bad = true;
    d->stopped = false;
    result_locked(d, BOOK_IO_ERROR);
    rt_hw_interrupt_enable(key);
    wake_worker();
}

/* Called under storage_lock, including from the bounded read-through path. */
static document_content_t *content_find(document_t *d, uint32_t start)
{
    for (unsigned i = 0; i < DOCUMENT_CONTENT_SLOTS; ++i)
        if (content_slots[i].document == d && content_slots[i].start == start)
            return &content_slots[i];
    return NULL;
}

static bool content_read(document_t *d, uint32_t offset, void *data, size_t length)
{
    /* Keep validated slot indices while storage_lock prevents eviction.
     * A miss leaves both the destination and every recency stamp unchanged. */
    uint16_t slots[DOCUMENT_CONTENT_SLOTS];
    unsigned count = 0;
    uint32_t at = offset;
    size_t remaining = length;
    while (remaining)
    {
        if (count == DOCUMENT_CONTENT_SLOTS) return false;
        uint32_t start = at / DOCUMENT_IO_WINDOW * DOCUMENT_IO_WINDOW;
        document_content_t *slot = content_find(d, start);
        size_t n = DOCUMENT_IO_WINDOW - (at - start);
        if (n > remaining) n = remaining;
        if (!slot || !slot->data || at - start > slot->used || n > slot->used - (at - start)) return false;
        slots[count++] = (uint16_t)(slot - content_slots);
        at += (uint32_t)n; remaining -= n;
    }
    uint8_t *out = data;
    for (unsigned i = 0; i < count; ++i)
    {
        document_content_t *slot = &content_slots[slots[i]];
        uint32_t start = offset / DOCUMENT_IO_WINDOW * DOCUMENT_IO_WINDOW;
        size_t n = DOCUMENT_IO_WINDOW - (offset - start);
        if (n > length) n = length;
        memcpy(out, slot->data + offset - start, n);
        slot->stamp = ++content_stamp;
        out += n; offset += (uint32_t)n; length -= n;
    }
    return true;
}

static uint32_t content_distance(uint32_t offset)
{
    uint32_t focus = content_target ? content_target->prefetch_focus : 0;
    return offset > focus ? offset - focus : focus - offset;
}

static unsigned content_priority(document_t *d, uint32_t offset)
{
    if (!content_target || d != content_target || offset < d->prefetch_low || offset >= d->prefetch_high)
        return 0;
    if (content_distance(offset) <= 64u * 1024u) return 3;
    if (offset + DOCUMENT_IO_WINDOW > d->prefetch_range.begin && offset < d->prefetch_range.end) return 2;
    return 1;
}

static document_content_t *content_victim(void)
{
    document_content_t *victim = NULL;
    for (unsigned i = 0; i < DOCUMENT_CONTENT_SLOTS; ++i)
    {
        document_content_t *slot = &content_slots[i];
        if (!slot->data) continue;
        unsigned priority = content_priority(slot->document, slot->start);
        unsigned previous = victim ? content_priority(victim->document, victim->start) : 0;
        if (!victim || priority < previous ||
            (priority == previous && priority && content_distance(slot->start) > content_distance(victim->start)) ||
            (priority == previous && (!priority || content_distance(slot->start) == content_distance(victim->start)) &&
             slot->stamp < victim->stamp)) victim = slot;
    }
    return victim;
}

static bool content_room(size_t extra)
{
    return epd_app_psram_available() >= DOCUMENT_CONTENT_CACHE_RESERVE + extra;
}

/* Worker-only reclamation; mandatory page/glyph allocations take precedence. */
static void content_trim(size_t extra)
{
    while (!content_room(extra))
    {
        document_content_t *victim = content_victim();
        if (!victim) break;
        epd_app_free(victim->data); memset(victim, 0, sizeof(*victim));
        content_payload -= DOCUMENT_IO_WINDOW;
    }
}

static bool content_can_store(document_t *d, uint32_t offset)
{
    if (content_memory_held()) return false;
    unsigned priority = content_priority(d, offset);
    if (!priority) return false;
    if (content_find(d, offset)) return true;
    size_t extra = DOCUMENT_IO_WINDOW + 64u;
    if (!d->prefetch_reader) extra += sizeof(document_reader_t) + DOCUMENT_IO_WINDOW + 128u;
    if (content_room(extra))
        for (unsigned i = 0; i < DOCUMENT_CONTENT_SLOTS; ++i)
            if (!content_slots[i].data) return true;
    document_content_t *victim = content_victim();
    if (!victim) return false;
    unsigned previous = content_priority(victim->document, victim->start);
    return previous < priority || (previous == priority && content_distance(victim->start) >= content_distance(offset));
}

static void content_store(document_t *d, uint32_t offset, const void *data, uint32_t length, bool grow)
{
    if (content_memory_held()) return;
    unsigned priority = content_priority(d, offset);
    if (!priority || offset % DOCUMENT_IO_WINDOW || !length || length > DOCUMENT_IO_WINDOW) return;
    document_content_t *slot = content_find(d, offset);
    if (slot && slot->used >= length) return;
    if (!slot && grow && content_room(DOCUMENT_IO_WINDOW + 64u))
        for (unsigned i = 0; i < DOCUMENT_CONTENT_SLOTS; ++i)
            if (!content_slots[i].data)
            {
                content_slots[i].data = allocate(DOCUMENT_IO_WINDOW);
                if (content_slots[i].data) { slot = &content_slots[i]; content_payload += DOCUMENT_IO_WINDOW; }
                break;
            }
    if (!slot)
    {
        slot = content_victim();
        if (!slot) return;
        unsigned previous = content_priority(slot->document, slot->start);
        if (previous > priority || (previous == priority && content_distance(slot->start) < content_distance(offset)))
            return;
    }
    memcpy(slot->data, data, length);
    slot->document = d; slot->start = offset; slot->used = length;
    slot->stamp = ++content_stamp;
}

static void prefetch_release(document_t *d)
{
    document_reader_destroy(d->prefetch_reader);
    epd_app_free(d->prefetch_scratch); d->prefetch_scratch = NULL;
    rt_base_t key = rt_hw_interrupt_disable();
    if (prefetch_document == d) prefetch_document = NULL;
    rt_hw_interrupt_enable(key);
}

/* Runs between worker jobs; slots contain only copies of published spool data. */
static bool reclaim_work(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    uint32_t ticket = reclaim.ticket;
    if (!ticket && reclaim.resume)
    {
        for (document_t *d = documents; d; d = d->next)
            d->prefetch_active = d->prefetch_done = 0;
        reclaim.resume = false;
        rt_hw_interrupt_enable(key);
        return true;
    }
    bool pending = ticket && !reclaim.result;
    rt_hw_interrupt_enable(key);
    if (!pending) return false;

    size_t released = entry_cache_forget(), available = epd_app_psram_available();
    bool exhausted = false;
    for (unsigned i = 0; i < 8; ++i)
    {
        key = rt_hw_interrupt_disable();
        bool current = ticket == reclaim.ticket && !worker_stopping;
        size_t target = reclaim.target;
        rt_hw_interrupt_enable(key);
        if (!current || available >= target) break;
        if (prefetch_document)
        {
            document_t *d = prefetch_document;
            released += d->prefetch_scratch ? DOCUMENT_IO_WINDOW : 0;
            released += d->prefetch_reader ? sizeof(document_reader_t) : 0;
            prefetch_release(d);
        }
        else
        {
            storage_lock();
            document_content_t *victim = content_victim();
            void *memory = victim ? victim->data : NULL;
            if (memory) { memset(victim, 0, sizeof(*victim)); content_payload -= DOCUMENT_IO_WINDOW; }
            storage_unlock();
            if (!memory) { exhausted = true; break; }
            epd_app_free(memory);
            released += DOCUMENT_IO_WINDOW;
        }
        available = epd_app_psram_available();
    }
    key = rt_hw_interrupt_disable();
    int result = 0;
    size_t target = reclaim.target, total = 0;
    if (ticket == reclaim.ticket && !worker_stopping)
    {
        reclaim.released += released; reclaim.available = available;
        result = available >= target ? 1 : exhausted ? 2 : 0;
        reclaim.result = result; total = reclaim.released;
    }
    rt_hw_interrupt_enable(key);
    if (result)
    {
        worker_changed();
        rt_kprintf("[document] reclaim ticket=%lu target=%lu released=%lu available=%lu result=%d\n",
            (unsigned long)ticket, (unsigned long)target, (unsigned long)total, (unsigned long)available, result);
        if (worker_service) epd_service_ui_request(worker_service, 0);
    }
    return true;
}

static void content_dispose(document_t *d)
{
    storage_lock();
    if (content_target == d) content_target = NULL;
    for (unsigned i = 0; i < DOCUMENT_CONTENT_SLOTS; ++i)
        if (content_slots[i].document == d)
        {
            epd_app_free(content_slots[i].data);
            content_payload -= DOCUMENT_IO_WINDOW;
            memset(&content_slots[i], 0, sizeof(content_slots[i]));
        }
    storage_unlock();
    epd_app_free(d->prefetch_scratch); d->prefetch_scratch = NULL;
    while (d->chapters)
    {
        document_chapters_t *next = d->chapters->next;
        epd_app_free(d->chapters); d->chapters = next;
    }
}

static bool chapter_restore_runnable(document_t *d)
{
    /* Segmented sources need their target marker before layout can start.
     * Once it is available, restore the remaining index with optional work. */
    bool body_pending = d->segmented && d->requested.unit == BOOK_DOCUMENT_SOURCE_BODY_UTF8 && !d->body_end;
    bool target_pending = d->target_required &&
        (!d->body_first || !d->body_end || d->chapter_scan < d->body_end);
    return d->chapter_restore && (d->prefetch_enabled || d->nav_requested || target_pending ||
        body_pending || (d->segmented && !d->target_ready));
}

static bool content_pending(document_t *d)
{
    return !epd_reader_suspended() && !cancelled(d) && flow_allows(d) && d->status.result == BOOK_OK &&
        (chapter_restore_runnable(d) || (!content_memory_held() && d->prefetch_enabled &&
        ((content_target == d && content_payload && !content_room(0)) || (d->prefetch_request &&
         (d->prefetch_request != d->prefetch_done || d->prefetch_publication != d->status.publication)))));
}

/* Reopening a persistent spool scans record headers and small anchors only.
 * Image payloads are skipped. A pending index never claims a stable chapter. */
static void chapter_restore_work(document_t *d)
{
    bool navigation = navigation_restore_tail(d);
    if (!d->chapter_reader)
    {
        size_t extra = sizeof(document_reader_t) + DOCUMENT_IO_WINDOW + 128u;
        storage_lock(); content_trim(extra); storage_unlock();
        if (content_room(extra)) d->chapter_reader = document_reader_create(d);
    }
    bool failed = !d->chapter_reader;
    int failure = BOOK_NO_MEMORY;
    rt_tick_t started = rt_tick_get(), slice = rt_tick_from_millisecond(2);
    if (!slice) slice = 1;
    uint32_t window = d->chapter_reader ? d->chapter_reader->window_start : 0;
    for (unsigned i = 0; !failed && i < 16 && d->chapter_scan < d->status.end &&
         chapter_restore_runnable(d); ++i)
    {
        navigation = navigation_restore_tail(d);
        document_record_t record;
        if (!document_reader_record(d->chapter_reader, d->chapter_scan, &record))
        { failure = BOOK_IO_ERROR; failed = true; break; }
        if (record.kind == DOCUMENT_METADATA)
        {
            uint8_t *meta = record.length <= 2 * DOCUMENT_TEXT_LIMIT + sizeof(book_document_event_t) + 4 ?
                            allocate(record.length) : NULL;
            int rc = !meta ? BOOK_NO_MEMORY :
                !document_reader_read(d->chapter_reader, d->chapter_scan + sizeof(record), meta, record.length) ?
                BOOK_IO_ERROR : metadata_apply(d, meta, record.length, d->chapter_scan);
            epd_app_free(meta);
            if (rc != BOOK_OK) { failure = rc; failed = true; break; }
            /* Cache metadata records are not offsets in the NAV_ONLY event stream. */
        }
        if (d->segmented && record.kind == DOCUMENT_TEXT && !location_append(d, d->chapter_scan, record.length))
        { failed = true; break; }
        if (d->segmented && record.kind == DOCUMENT_IMAGE && !location_append(d, d->chapter_scan, 0))
        { failed = true; break; }
        if (record.kind == DOCUMENT_ANCHOR || record.kind == DOCUMENT_LINK)
        {
            char anchor[DOCUMENT_TEXT_LIMIT + 1];
            if (!record.length || record.length > DOCUMENT_TEXT_LIMIT ||
                !document_reader_read(d->chapter_reader, d->chapter_scan + sizeof(record), anchor, record.length))
            { failure = BOOK_IO_ERROR; failed = true; break; }
            if (!navigation_append(d, d->chapter_scan, record.kind, record.style, anchor, record.length, false))
            { failed = true; break; }
            const char *nul = memchr(anchor, 0, record.length);
            if (record.kind == DOCUMENT_ANCHOR && nul && nul == anchor + record.length - 2 && !nul[1] &&
                chapter_root(anchor, nul + 1, record.style))
                chapter_append(d, d->chapter_scan);
        }
        d->chapter_scan += sizeof(record) + record.length;
        if (d->chapter_failed) { failed = true; break; }
        if (rt_tick_get() - started >= slice || d->chapter_reader->window_start != window) break;
    }
    if (failed || d->chapter_scan == d->status.end)
    {
        document_reader_destroy(d->chapter_reader); d->chapter_reader = NULL;
        rt_base_t key = rt_hw_interrupt_disable();
        d->chapter_failed |= failed && !navigation;
        d->chapter_restore = false;
        if (!failed && d->body_first && d->body_end == d->status.end)
            d->nav_state = DOCUMENT_NAV_PENDING;
        if (!failed && d->toc_context && d->body_end && d->body_end < d->status.end &&
            !cancelled(d) && d->status.result == BOOK_OK) {
            toc_finish(d->toc_context);
            if (conversion_document && conversion_document->toc_context == d->toc_context &&
                conversion_document->nav_state == DOCUMENT_NAV_ACTIVE) navigation_pause_locked(conversion_document);
        }
        if (failed && !cancelled(d) && d->status.result == BOOK_OK)
        {
            if (navigation) {
                d->nav_result = failure; d->nav_state = DOCUMENT_NAV_FAILED;
                d->nav_requested = false; d->finish_stage = 0;
            } else result_locked(d, failure);
            if (failure != BOOK_NO_MEMORY) d->cache_bad = true;
        }
        /* Boundary consumers must notice index readiness even though the
         * persistent spool length itself has not changed. */
        ++d->status.publication;
        worker_changed();
        d->prefetch_active = 0;
        d->prefetch_done = 0;
        rt_hw_interrupt_enable(key);
        rt_kprintf("[document] restore_done id=%lu result=%d scanned=%lu end=%lu elapsed_ms=%u\n",
            (unsigned long)d->serial, d->status.result, (unsigned long)d->chapter_scan,
            (unsigned long)d->status.end, elapsed_ms(d->opened_at));
    }
}

/* Aligned blocks keep repeated page hints/publications from copying the same
 * bytes again. Even an unindexed book selects at most one budget-sized window. */
static void prefetch_plan(document_t *d, uint32_t record, const document_chapter_range_t *range)
{
    uint32_t focus = record / DOCUMENT_IO_WINDOW * DOCUMENT_IO_WINDOW;
    uint32_t low = focus, high = focus + DOCUMENT_IO_WINDOW;
    d->prefetch_focus = focus; d->prefetch_range = *range;
    d->prefetch_cursor = 0; d->prefetch_count = 1; d->prefetch_blocks[0] = focus;
    for (unsigned part = 0; part < 2 && d->prefetch_count < DOCUMENT_CONTENT_SLOTS; ++part)
    {
        uint32_t begin = part ? range->previous_begin : range->begin;
        uint32_t end = part ? range->next_end : range->end;
        begin = begin / DOCUMENT_IO_WINDOW * DOCUMENT_IO_WINDOW;
        while (d->prefetch_count < DOCUMENT_CONTENT_SLOTS && (low > begin || high < end))
        {
            if (high < end) { d->prefetch_blocks[d->prefetch_count++] = high; high += DOCUMENT_IO_WINDOW; }
            if (low > begin && d->prefetch_count < DOCUMENT_CONTENT_SLOTS)
            { low -= DOCUMENT_IO_WINDOW; d->prefetch_blocks[d->prefetch_count++] = low; }
        }
    }
    d->prefetch_low = low; d->prefetch_high = high;
}

static void content_work(document_t *d)
{
    if (epd_reader_suspended()) return;
    if (cancelled(d) || d->status.result != BOOK_OK) { prefetch_release(d); return; }
    if (!flow_allows(d)) return;
    if (chapter_restore_runnable(d)) chapter_restore_work(d);
    if (!d->prefetch_enabled || content_memory_held()) return;
    if (prefetch_document && prefetch_document != d) prefetch_release(prefetch_document);
    storage_lock(); content_trim(0); storage_unlock();
    if (!content_room(0)) { prefetch_release(d); }
    rt_base_t key = rt_hw_interrupt_disable();
    uint32_t request = d->prefetch_request, record = d->prefetch_record;
    uint32_t publication = d->status.publication;
    bool done = request == d->prefetch_done && publication == d->prefetch_publication;
    rt_hw_interrupt_enable(key);
    if (!request || done) { prefetch_release(d); return; }
    if (content_target != d || request != d->prefetch_active || publication != d->prefetch_publication)
    {
        d->prefetch_active = request; d->prefetch_publication = publication;
        document_chapter_range_t range;
        if (!document_chapter_range(d, record, &range))
        { d->prefetch_done = request; prefetch_release(d); return; }
        storage_lock();
        content_target = d;
        prefetch_plan(d, record, &range);
        storage_unlock();
    }
    /* Resident blocks need neither a reader nor a scratch copy. One missing
     * block per step bounds the delay for a newly queued compute/image job. */
    while (d->prefetch_cursor < d->prefetch_count)
    {
        uint32_t at = d->prefetch_blocks[d->prefetch_cursor++];
        uint32_t end = d->status.end;
        if (d->body_first && d->body_end && d->body_end < end) end = d->body_end;
        if (at >= end) continue;
        uint32_t n = end - at;
        if (n > DOCUMENT_IO_WINDOW) n = DOCUMENT_IO_WINDOW;
        storage_lock();
        document_content_t *slot = content_find(d, at);
        bool resident = slot && slot->used >= n;
        bool admitted = resident || content_can_store(d, at);
        storage_unlock();
        if (resident || !admitted) continue;
        if (!d->prefetch_reader)
        {
            size_t extra = sizeof(document_reader_t) + DOCUMENT_IO_WINDOW + 128u;
            storage_lock(); content_trim(extra); storage_unlock();
            if (content_room(extra)) d->prefetch_reader = document_reader_create(d);
            if (!d->prefetch_reader || !d->prefetch_reader->window)
            { d->prefetch_done = request; prefetch_release(d); return; }
            d->prefetch_scratch = d->prefetch_reader->window;
            d->prefetch_reader->window = NULL;
            key = rt_hw_interrupt_disable(); prefetch_document = d; rt_hw_interrupt_enable(key);
        }
        storage_lock(); admitted = content_can_store(d, at); storage_unlock();
        if (!admitted) continue;
        if (!document_reader_read(d->prefetch_reader, at, d->prefetch_scratch, n))
        { d->prefetch_done = request; prefetch_release(d); return; }
        storage_lock(); content_store(d, at, d->prefetch_scratch, n, true); storage_unlock();
        break;
    }
    if (d->prefetch_cursor == d->prefetch_count)
    { d->prefetch_done = request; prefetch_release(d); }
}

bool document_reader_read(document_reader_t *r, uint32_t offset, void *data, size_t length)
{
    if (!r || (!data && length)) return false;
    document_t *d = r->document;
    document_progress_t status;
    document_spool_progress(d, &status);
    if (status.result != BOOK_OK || offset > status.end || length > status.end - offset) return false;
    if (!length) return true;
    storage_lock();
    bool ok = false;
    if (cancelled(d)) goto done;
    if (content_read(d, offset, data, length)) { ok = true; goto done; }
    if (r->window && offset >= r->window_start && offset - r->window_start <= r->window_used &&
        length <= r->window_used - (offset - r->window_start))
    {
        memcpy(data, r->window + offset - r->window_start, length);
        ok = true; goto done;
    }
    /* All cache readers share one descriptor under storage_lock. Their memory
     * windows remain independent; the file cursor and visible size do not. */
    if (d->input >= 0 && (offset > d->input_visible || length > d->input_visible - offset))
    { close(d->input); d->input = -1; }
    if (d->input < 0)
    {
        d->input = reader_file_open(d, d->cache, O_RDONLY);
        if (d->input < 0) goto done;
        d->input_position = 0; d->input_visible = status.end;
    }
    if (r->window && length <= DOCUMENT_IO_WINDOW)
    {
        uint32_t start = offset / DOCUMENT_IO_WINDOW * DOCUMENT_IO_WINDOW;
        if (length > DOCUMENT_IO_WINDOW - (offset - start)) start = offset;
        if (r == d->chapter_reader && navigation_restore_tail(d) &&
            offset >= d->body_end && start < d->body_end) start = d->body_end;
        uint32_t n = d->input_visible - start;
        if (n > DOCUMENT_IO_WINDOW) n = DOCUMENT_IO_WINDOW;
        /* A body read must not depend on optional navigation read-ahead. */
        if (d->body_first && d->body_end && offset < d->body_end &&
            length <= d->body_end - offset && n > d->body_end - start) n = d->body_end - start;
        r->window_used = 0;
        if (!reader_fill(r, start, r->window, n)) goto done;
        r->window_start = start; r->window_used = n;
        content_store(d, start, r->window, n, false);
        memcpy(data, r->window + offset - start, length); ok = true;
    }
    else ok = reader_fill(r, offset, data, length);
done:
    if (!ok) cache_read_failed(r, offset);
    storage_unlock();
    return ok;
}

bool document_reader_record(document_reader_t *r, uint32_t offset, document_record_t *record)
{
    if (!r || !document_reader_read(r, offset, record, sizeof(*record))) return false;
    document_progress_t status;
    document_spool_progress(r->document, &status);
    bool ok = status.result == BOOK_OK && offset <= status.end && sizeof(*record) <= status.end - offset &&
           record->length <= status.end - offset - sizeof(*record) &&
           record->kind >= DOCUMENT_TEXT && record->kind <= DOCUMENT_METADATA &&
           (record->kind == DOCUMENT_IMAGE ||
            (record->kind == DOCUMENT_METADATA && record->length <= 2 * DOCUMENT_TEXT_LIMIT + sizeof(book_document_event_t) + 4) ||
            record->length <= DOCUMENT_TEXT_LIMIT);
    if (!ok) cache_read_failed(r, offset);
    return ok;
}

bool document_reader_image_info(document_reader_t *r, uint32_t offset, document_image_t *image)
{
    document_record_t record;
    if (!image || !document_reader_record(r, offset, &record)) return false;
    if (record.kind != DOCUMENT_IMAGE || record.length < sizeof(*image))
    { cache_read_failed(r, offset); return false; }
    if (!document_reader_read(r, offset + sizeof(record), image, sizeof(*image))) return false;
    if (!image->width || !image->height || image->width > DOCUMENT_IMAGE_WIDTH ||
        image->height > DOCUMENT_IMAGE_HEIGHT ||
        record.length != sizeof(*image) + ((image->width + 1) / 2) * image->height)
    { cache_read_failed(r, offset); return false; }
    return true;
}

bool document_read(document_t *d, uint32_t offset, void *data, size_t length)
{
    if (!d->default_reader) d->default_reader = document_reader_create(d);
    return document_reader_read(d->default_reader, offset, data, length);
}
bool document_record(document_t *d, uint32_t offset, document_record_t *record)
{
    if (!d->default_reader) d->default_reader = document_reader_create(d);
    return document_reader_record(d->default_reader, offset, record);
}

typedef struct { document_t *document; document_image_request_t *job; } document_image_cancel_t;
static bool image_cancelled(void *user)
{
    document_image_cancel_t *c = user;
    return epd_reader_suspended() || (c->job && c->job->released) || cancelled(c->document);
}
static bool image_load(document_reader_t *r, uint32_t offset, unsigned width, unsigned height,
                       document_bitmap_t *bitmap, document_image_request_t *job)
{
    document_t *d = r->document;
    document_image_cancel_t cancellation = {d, job};
    if (!image_cleanup(d) || image_cancelled(&cancellation)) return false;
    document_image_t source;
    if (!document_reader_image_info(r, offset, &source)) return false;
    if (!bitmap_allocate(bitmap, width, height)) return false;
    unsigned bytes = ((source.width + 1) / 2) * source.height;
    epd_compute_buffer_t packed = {0};
    epd_compute_buffer_t frame = {(void *)bitmap->image.data, bitmap->memory,
        RT_ALIGN(bitmap->image.data_size, EPD_COMPUTE_CACHE_LINE)};
    epd_reader_render_t render = NULL;
    rr_frame format = {width, height, bitmap->image.header.stride, RR_FORMAT_RGB565};
    int rc = EPD_COMPUTE_NO_MEMORY;
    if (epd_compute_buffer_alloc(&packed, bytes) &&
        document_reader_read(r, offset + sizeof(document_record_t) + sizeof(source), packed.data, bytes))
    {
        epd_compute_wait_t wait = NULL;
        rc = epd_compute_wait_create(&wait);
        if (rc == EPD_COMPUTE_OK)
        {
            rt_base_t key = rt_hw_interrupt_disable();
            d->compute_wait = wait;
            if (image_cancelled(&cancellation)) epd_compute_wait_cancel(wait);
            rt_hw_interrupt_enable(key);
            rc = epd_reader_render_open_wait(&frame, format, EPD_COMPUTE_FOREGROUND, wait, &render);
            key = rt_hw_interrupt_disable();
            d->compute_wait = NULL; d->image_render = render;
            if (image_cancelled(&cancellation) && render) epd_reader_render_cancel(render);
            rt_hw_interrupt_enable(key);
            epd_compute_wait_destroy(wait);
        }
    }
    if (rc == EPD_COMPUTE_OK)
    {
        rr_command command;
        rr_command_init(&command, RR_INIT, format);
        rc = epd_reader_render_command(render, &command, NULL, 0, image_cancelled, &cancellation, NULL);
        if (rc == EPD_COMPUTE_OK)
        {
            rr_command_init(&command, RR_SCALE_GRAY4_TO_RGB565, format);
            command.width = width; command.height = height;
            command.src_width = source.width; command.src_height = source.height;
            command.src_stride = (source.width + 1) / 2;
            rc = epd_reader_render_command_shared(render, &command, &packed, bytes,
                                                  image_cancelled, &cancellation, NULL);
        }
        if (rc == EPD_COMPUTE_OK)
        {
            rr_command_init(&command, RR_FINISH, format);
            rc = epd_reader_render_command(render, &command, NULL, 0, image_cancelled, &cancellation, NULL);
        }
    }
    rt_base_t key = rt_hw_interrupt_disable(); d->image_render = NULL; rt_hw_interrupt_enable(key);
    int closed = epd_reader_render_shutdown(&render);
    if (closed != EPD_COMPUTE_OK)
    {
        rt_kprintf("reader image close failed=%d; shared buffers retained\n", closed);
        key = rt_hw_interrupt_disable();
        d->image_render = render; d->image_packed = packed; d->image_retained = *bitmap;
        rt_hw_interrupt_enable(key);
        memset(bitmap, 0, sizeof(*bitmap));
        return false;
    }
    epd_compute_buffer_free(&packed);
    if (rc == EPD_COMPUTE_OK) return true;
    epd_app_free(bitmap->memory); memset(bitmap, 0, sizeof(*bitmap));
    return false;
}

bool document_image_load(document_t *d, uint32_t offset, unsigned width, unsigned height, document_bitmap_t *bitmap)
{
    document_reader_t *r = document_reader_create(d);
    bool ok = r && image_load(r, offset, width, height, bitmap, NULL);
    document_reader_destroy(r);
    if (ok) bitmap_clean(bitmap);
    return ok;
}

document_record_request_t *document_record_request(document_t *d, uint32_t offset, bool foreground)
{
    const char *reason = "cancelled";
    document_record_request_t *job = NULL;
    rt_base_t key = rt_hw_interrupt_disable();
    if (!d || cancelled(d) || worker_stopping) goto rejected_locked;
    unsigned count = record_allocating;
    for (document_record_request_t *p = record_requests; p; p = p->next) {
        ++count;
        if (!job && p->released && !p->active) job = p;
    }
    if (job) {
        /* The worker has relinquished this record; UI collection may lag. */
        document_record_request_t *next = job->next;
        memset(job, 0, sizeof(*job)); job->next = next;
    } else {
        if (count >= 16) { reason = "queue_full"; goto rejected_locked; }
        ++record_allocating;
        rt_hw_interrupt_enable(key);
        job = allocate(sizeof(*job));
        if (job) memset(job, 0, sizeof(*job));
        key = rt_hw_interrupt_disable();
        --record_allocating;
        if (cancelled(d) || worker_stopping || !job) {
            if (!job) reason = "no_memory";
            rt_hw_interrupt_enable(key);
            epd_app_free(job);
            goto rejected;
        }
        document_record_request_t **tail = &record_requests;
        while (*tail) tail = &(*tail)->next;
        *tail = job;
    }
    job->document = d; job->offset = offset; job->foreground = foreground;
    d->stopped = false;
    rt_hw_interrupt_enable(key);
    wake_worker();
    return job;
rejected_locked:
    rt_hw_interrupt_enable(key);
rejected:
    rt_kprintf("[document] record_rejected id=%lu offset=%lu foreground=%u reason=%s\n",
        (unsigned long)(d ? d->serial : 0), (unsigned long)offset, (unsigned)foreground, reason);
    return NULL;
}

int document_record_take(document_record_request_t *job, document_record_data_t *out)
{
    if (!job || !out || job->released || cancelled(job->document)) return -1;
    rt_base_t key = rt_hw_interrupt_disable();
    int status = job->done ? (job->result == BOOK_OK ? 1 : -1) : 0;
    rt_hw_interrupt_enable(key);
    if (status == 1) *out = job->data;
    return status;
}

void document_record_cancel(document_record_request_t *job)
{
    if (!job) return;
    rt_base_t key = rt_hw_interrupt_disable();
    job->released = true;
    rt_hw_interrupt_enable(key);
    wake_worker();
}

static bool record_referenced(document_t *d)
{
    for (document_record_request_t *p = record_requests; p; p = p->next)
        if (p->document == d) return true;
    return false;
}

static void record_collect(void)
{
    document_record_request_t **at = &record_requests;
    while (*at)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        document_record_request_t *job = *at;
        bool dispose = job->released && !job->active;
        if (dispose) *at = job->next;
        rt_hw_interrupt_enable(key);
        if (dispose) epd_app_free(job); else at = &job->next;
    }
}

static bool record_work_priority(bool foreground_only)
{
    rt_base_t key = rt_hw_interrupt_disable();
    document_record_request_t *job = NULL;
    for (document_record_request_t *p = record_requests; p; p = p->next)
    {
        if (p->released || p->done || p->active || (foreground_only && !p->foreground)) continue;
        document_t *d = p->document;
        if (epd_reader_suspended() && !cancelled(d)) continue;
        if (!p->foreground && !cancelled(d) && !flow_allows(d)) continue;
        if (!cancelled(d) && !d->status.complete && !d->status.result &&
            (p->offset >= d->status.end || d->status.end - p->offset < sizeof(document_record_t))) continue;
        if (!job || (!job->foreground && p->foreground)) job = p;
    }
    if (job) job->active = true;
    rt_hw_interrupt_enable(key);
    if (!job) return false;
    document_t *d = job->document;
    document_progress_t progress;
    document_progress(d, &progress);
    if (!progress.result && !d->record_reader) d->record_reader = document_reader_create(d);
    document_reader_t *reader = progress.result ? NULL : d->record_reader;
    document_record_t *record = &job->data.record;
    bool ok = reader && job->offset <= progress.end && sizeof(*record) <= progress.end - job->offset &&
        document_reader_record(reader, job->offset, record) &&
        record->length <= progress.end - job->offset - sizeof(*record) &&
        record->kind >= DOCUMENT_TEXT && record->kind <= DOCUMENT_METADATA;
    if (ok && record->kind == DOCUMENT_IMAGE)
        ok = document_reader_image_info(reader, job->offset, &job->data.image);
    else if (ok && record->kind != DOCUMENT_METADATA)
    {
        ok = record->length <= DOCUMENT_TEXT_LIMIT &&
            document_reader_read(reader, job->offset + sizeof(*record), job->data.text, record->length);
        if (ok) job->data.text[record->length] = 0;
    }
    if (!ok)
        rt_kprintf("[document] record_failed id=%lu offset=%lu end=%lu complete=%u result=%d reader=%u kind=%u length=%u\n",
            (unsigned long)d->serial, (unsigned long)job->offset, (unsigned long)progress.end,
            (unsigned)progress.complete, progress.result, reader != NULL, (unsigned)record->kind, (unsigned)record->length);
    key = rt_hw_interrupt_disable();
    job->result = ok ? BOOK_OK : BOOK_IO_ERROR;
    job->done = true; job->active = false;
    worker_changed();
    rt_hw_interrupt_enable(key);
    if (worker_service) epd_service_ui_request(worker_service, 0);
    return true;
}

static bool record_work(void) { return record_work_priority(false); }

document_image_request_t *document_image_request(document_t *d, uint32_t offset, unsigned width, unsigned height)
{
    if (!d || cancelled(d) || !width || !height) return NULL;
    document_image_request_t *job = allocate(sizeof(*job));
    if (!job) return NULL;
    memset(job, 0, sizeof(*job));
    job->document = d; job->offset = offset; job->width = width; job->height = height;
    rt_base_t key = rt_hw_interrupt_disable();
    if (cancelled(d) || worker_stopping)
    { rt_hw_interrupt_enable(key); epd_app_free(job); return NULL; }
    document_image_request_t **tail = &image_requests;
    while (*tail) tail = &(*tail)->next;
    *tail = job;
    d->stopped = false;
    rt_hw_interrupt_enable(key);
    wake_worker();
    return job;
}

int document_image_take(document_image_request_t *job, document_bitmap_t *bitmap)
{
    if (!job || !bitmap || cancelled(job->document)) return -1;
    rt_base_t key = rt_hw_interrupt_disable();
    int result = job->done ? (job->result == BOOK_OK && job->bitmap.memory ? 1 : -1) : 0;
    if (result == 1)
    {
        *bitmap = job->bitmap;
        memset(&job->bitmap, 0, sizeof(job->bitmap));
    }
    rt_hw_interrupt_enable(key);
    if (result == 1) bitmap_clean(bitmap);
    return result;
}

void document_image_cancel(document_image_request_t *job)
{
    if (!job) return;
    rt_base_t key = rt_hw_interrupt_disable();
    job->released = true;
    if (job->active)
    {
        epd_compute_wait_cancel(job->document->compute_wait);
        if (job->document->image_render) epd_reader_render_cancel(job->document->image_render);
    }
    rt_hw_interrupt_enable(key);
    wake_worker();
}

static bool image_pending(document_t *d)
{
    if (!cancelled(d) && !flow_allows(d)) return false;
    for (document_image_request_t *j = image_requests; j; j = j->next)
        if (j->document == d && !j->done && !j->released) return true;
    return false;
}

static bool image_referenced(document_t *d)
{
    for (document_image_request_t *j = image_requests; j; j = j->next)
        if (j->document == d) return true;
    return false;
}

static void image_collect(void)
{
    document_image_request_t **link = &image_requests;
    while (*link)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        document_image_request_t *job = *link;
        bool dispose = job->released && !job->active;
        if (dispose) *link = job->next;
        rt_hw_interrupt_enable(key);
        if (!dispose) { link = &job->next; continue; }
        epd_app_free(job->bitmap.memory); epd_app_free(job);
    }
}

static void image_work(document_t *only)
{
    if (epd_reader_suspended() && !cancelled(only)) return;
    if (!cancelled(only) && !flow_allows(only)) return;
    for (;;)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        document_image_request_t *job = image_requests;
        while (job && (job->document != only || job->released || job->done || job->active)) job = job->next;
        if (job) job->active = true;
        rt_hw_interrupt_enable(key);
        if (!job) return;
        document_progress_t progress;
        document_progress(only, &progress);
        /* An incomplete source is allowed to grow; never block its producer. */
        bool waiting = progress.result == BOOK_OK && !progress.complete &&
            (job->offset >= progress.end || sizeof(document_record_t) > progress.end - job->offset);
        bool ok = false;
        if (!waiting && progress.result == BOOK_OK)
        {
            document_reader_t *reader = document_reader_create(only);
            ok = reader && image_load(reader, job->offset, job->width, job->height, &job->bitmap, job);
            document_reader_destroy(reader);
        }
        key = rt_hw_interrupt_disable();
        job->result = ok ? BOOK_OK : progress.result != BOOK_OK ? progress.result : BOOK_IO_ERROR;
        job->done = !waiting;
        if (!waiting) worker_changed();
        job->active = false;
        rt_hw_interrupt_enable(key);
        if (waiting) return;
    }
}

static void save_release(document_save_t *job)
{
    if (!job) return;
    rt_base_t key = rt_hw_interrupt_disable();
    bool last = --job->references == 0;
    rt_hw_interrupt_enable(key);
    if (last) epd_app_free(job);
}

/* Payloads remain immutable while a reader copies/checks a pinned snapshot. */
static document_save_t *save_snapshot(const char *path)
{
    rt_base_t key = rt_hw_interrupt_disable();
    document_save_t *latest = NULL;
    for (document_save_t *job = saves; job; job = job->next)
        if (!strcmp(job->path, path)) latest = job;
    if (latest) ++latest->references;
    rt_hw_interrupt_enable(key);
    return latest;
}

bool document_save_snapshot(const char *path, void **data, size_t *size)
{
    if (!data || !size) return false;
    *data = NULL; *size = 0;
    if (!path) return false;
    document_save_t *job = save_snapshot(path);
    if (!job) return false;
    bool ok = job->length && media_valid(job->path, job->generation) &&
              (!worker_service || !epd_service_cancelled(worker_service));
    void *buffer = ok ? allocate(job->length) : NULL;
    if (!buffer) ok = false;
    if (ok)
    {
        memcpy(buffer, job->data, job->length);
        ok = media_valid(job->path, job->generation) &&
             (!worker_service || !epd_service_cancelled(worker_service));
    }
    if (ok) { *data = buffer; *size = job->length; }
    save_release(job);
    if (!ok) epd_app_free(buffer);
    return ok;
}

bool document_save_async(const char *path, const void *header, size_t header_size, const void *data, size_t size)
{
    if (!path || strlen(path) >= sizeof(((document_save_t *)0)->path) ||
        (!header && header_size) || (!data && size) || size > SIZE_MAX - header_size ||
        size + header_size > SIZE_MAX - sizeof(document_save_t)) return false;
    document_save_t *job = allocate(sizeof(*job) + header_size + size);
    if (!job) return false;
    memset(job, 0, sizeof(*job));
    memcpy(job->path, path, strlen(path) + 1);
    job->generation = storage_card_session(); job->length = header_size + size;
    job->references = 1;
    if (header_size) memcpy(job->data, header, header_size);
    if (size) memcpy(job->data + header_size, data, size);
    rt_base_t key = rt_hw_interrupt_disable();
    if (worker_stopping)
    {
        rt_hw_interrupt_enable(key);
        epd_app_free(job);
        return false;
    }
    document_save_t **tail = &saves;
    while (*tail && ((*tail)->active || strcmp((*tail)->path, path))) tail = &(*tail)->next;
    document_save_t *previous = *tail;
    job->next = previous ? previous->next : NULL;
    *tail = job;
    rt_hw_interrupt_enable(key);
    save_release(previous);
    wake_worker();
    return true;
}

static void save_work(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    document_save_t *job = saves;
    /* Keep the in-flight snapshot visible until its disk replacement finishes.
     * New checkpoints may replace only its queued successor, never this job. */
    if (job) { job->active = true; ++job->references; }
    rt_hw_interrupt_enable(key);
    if (!job) return;
    rt_tick_t started = rt_tick_get();
    const char *result = "skipped";
    if ((!worker_service || !epd_service_cancelled(worker_service)) && media_valid(job->path, job->generation))
    {
        char directory[STORAGE_PATH_MAX];
        memcpy(directory, job->path, strlen(job->path) + 1);
        char *slash = strrchr(directory, '/');
        if (slash) *slash = 0;
        result = "mkdir_failed";
        if (slash && storage_mkdirs(directory))
            result = storage_file_replace_session(job->path, job->data, job->length, job->generation) ? "saved" : "write_failed";
    }
    if (entry_save_job(job))
        rt_kprintf("[document] entry_persist generation=%lu bytes=%u result=%s elapsed_ms=%u\n",
            (unsigned long)job->generation, (unsigned)job->length, result, elapsed_ms(started));
    key = rt_hw_interrupt_disable();
    document_save_t **link = &saves;
    while (*link && *link != job) link = &(*link)->next;
    bool queued = *link == job;
    if (queued) *link = job->next;
    rt_hw_interrupt_enable(key);
    if (queued) save_release(job);
    save_release(job);
}

/* The opaque reader-index payload carries its own ACPU checksum. This v3
 * envelope protects only the document binding; it never scans that payload. */
static uint32_t index_header_checksum(const document_index_header_t *header)
{
    return hash_bytes(2166136261u, header, offsetof(document_index_header_t, checksum));
}
static bool index_path(document_t *d, bool counts, char *path, size_t capacity)
{
    const char *index = document_cache_index(d->cached);
    size_t length = strlen(index);
    if (!counts) return snprintf(path, capacity, "%s", index) < (int)capacity;
    if (length < 4 || strcmp(index + length - 4, ".idx")) return false;
    return snprintf(path, capacity, "%.*s.count", (int)(length - 4), index) < (int)capacity;
}
static bool index_save_kind(document_t *d, uint32_t layout, const void *data, size_t size, bool counts)
{
    if (!d || !d->persistent || !d->status.complete || d->status.result != BOOK_OK ||
        cancelled(d) || !size || !data || size > DOCUMENT_INDEX_BYTES) return false;
    uint32_t identity = document_cache_identity(d->cached);
    if (!identity) return false;
    document_index_header_t header = {DOCUMENT_INDEX_MAGIC, 3, identity,
        layout, d->status.end, (uint32_t)size, 0};
    header.checksum = index_header_checksum(&header);
    if (document_cache_identity(d->cached) != identity) return false;
    char path[STORAGE_PATH_MAX];
    return index_path(d, counts, path, sizeof(path)) && document_save_async(path, &header, sizeof(header), data, size);
}
bool document_index_save(document_t *d, uint32_t layout, const void *data, size_t size)
{ return index_save_kind(d, layout, data, size, false); }
bool document_count_save(document_t *d, uint32_t layout, const void *data, size_t size)
{ return size <= 64 && index_save_kind(d, layout, data, size, true); }

static bool index_header_valid(document_t *d, uint32_t layout,
                               const document_index_header_t *header, uint64_t length, bool counts)
{
    return header->magic == DOCUMENT_INDEX_MAGIC && header->version == 3 && header->content &&
        header->content == document_cache_identity(d->cached) && header->layout == layout &&
        header->end == d->status.end && header->bytes && header->bytes <= DOCUMENT_INDEX_BYTES &&
        (!counts || header->bytes <= 64) &&
        length == sizeof(*header) + header->bytes && header->checksum == index_header_checksum(header);
}

static bool index_load_queued(document_t *d, const char *path, uint32_t layout, void **data, size_t *size, bool counts)
{
    document_save_t *job = save_snapshot(path);
    if (!job) return false;
    document_index_header_t header;
    bool ok = job->length >= sizeof(header) && !cancelled(d) && media_valid(path, job->generation);
    if (ok)
    {
        memcpy(&header, job->data, sizeof(header));
        ok = index_header_valid(d, layout, &header, job->length, counts);
    }
    void *buffer = ok ? allocate(header.bytes) : NULL;
    if (!buffer) ok = false;
    if (ok) memcpy(buffer, job->data + sizeof(header), header.bytes);
    ok = ok && !cancelled(d) && media_valid(path, job->generation) &&
        index_header_valid(d, layout, &header, job->length, counts);
    save_release(job);
    if (!ok) { epd_app_free(buffer); return false; }
    *data = buffer; *size = header.bytes;
    return true;
}

static bool index_load_kind(document_t *d, uint32_t layout, void **data, size_t *size, bool counts)
{
    if (!data || !size) return false;
    *data = NULL; *size = 0;
    if (!d || !d->persistent || !d->status.complete || d->status.result != BOOK_OK || cancelled(d)) return false;
    char path[STORAGE_PATH_MAX];
    if (!index_path(d, counts, path, sizeof(path))) return false;
    if (index_load_queued(d, path, layout, data, size, counts)) return true;
    document_index_header_t header;
    struct stat st;
    storage_lock();
    int fd = !cancelled(d) && storage_file_recover(path) ? open(path, O_RDONLY) : -1;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && read(fd, &header, sizeof(header)) == sizeof(header) &&
        index_header_valid(d, layout, &header, (uint64_t)st.st_size, counts);
    storage_unlock();
    uint8_t *buffer = ok ? allocate(header.bytes) : NULL;
    if (!buffer) ok = false;
    uint32_t at = 0;
    while (ok && at < header.bytes)
    {
        if (cancelled(d)) { ok = false; break; }
        size_t count = header.bytes - at;
        if (count > DOCUMENT_IO_WINDOW) count = DOCUMENT_IO_WINDOW;
        storage_lock();
        int n = !cancelled(d) ? read(fd, buffer + at, count) : -1;
        storage_unlock();
        if (n <= 0) { ok = false; break; }
        at += n;
    }
    storage_lock();
    if (fd >= 0 && close(fd) != 0) ok = false;
    storage_unlock();
    if (ok) ok = !cancelled(d) && index_header_valid(d, layout, &header, (uint64_t)st.st_size, counts);
    if (!ok) { epd_app_free(buffer); return false; }
    *data = buffer; *size = header.bytes;
    return true;
}

bool document_index_load(document_t *d, uint32_t layout, void **data, size_t *size)
{ return index_load_kind(d, layout, data, size, false); }
static int index_load_async_kind(document_t *d, uint32_t layout, void **data, size_t *size, bool counts)
{
    if (!data || !size) return -1;
    *data = NULL; *size = 0;
    if (!d || cancelled(d) || !d->status.complete || !d->persistent) return -1;
    rt_base_t key = rt_hw_interrupt_disable();
    if (d->index_done && d->index_layout == layout && d->index_count == counts)
    {
        *data = d->index_blob; *size = d->index_bytes;
        d->index_blob = NULL; d->index_bytes = 0;
        rt_hw_interrupt_enable(key);
        return *data ? 1 : -1;
    }
    void *old = NULL;
    if (!d->index_requested || d->index_layout != layout || d->index_count != counts)
    {
        old = d->index_blob; d->index_blob = NULL; d->index_bytes = 0;
        d->index_layout = layout; d->index_count = counts; d->index_done = false; d->index_requested = true;
    }
    rt_hw_interrupt_enable(key);
    epd_app_free(old);
    wake_worker();
    return 0;
}
int document_index_load_async(document_t *d, uint32_t layout, void **data, size_t *size)
{ return index_load_async_kind(d, layout, data, size, false); }
int document_count_load_async(document_t *d, uint32_t layout, void **data, size_t *size)
{ return index_load_async_kind(d, layout, data, size, true); }

static bool index_work(void)
{
    if (epd_reader_suspended()) return false;
    rt_base_t key = rt_hw_interrupt_disable();
    document_t *d = documents;
    while (d && (cancelled(d) || !flow_allows(d) || !d->index_requested || d->index_active || d->index_done)) d = d->next;
    uint32_t layout = d ? d->index_layout : 0;
    bool counts = d && d->index_count;
    if (d) d->index_active = true;
    rt_hw_interrupt_enable(key);
    if (!d) return false;
    void *data = NULL; size_t size = 0;
    bool ok = index_load_kind(d, layout, &data, &size, counts);
    key = rt_hw_interrupt_disable();
    if (!cancelled(d) && d->index_layout == layout && d->index_count == counts)
    {
        if (ok) { d->index_blob = data; d->index_bytes = size; data = NULL; }
        d->index_done = true;
        worker_changed();
    }
    d->index_active = false;
    rt_hw_interrupt_enable(key);
    epd_app_free(data);
    if (worker_service) epd_service_ui_request(worker_service, 0);
    return true;
}
