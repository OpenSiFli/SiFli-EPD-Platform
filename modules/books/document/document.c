/* SPDX-License-Identifier: Apache-2.0 */
#include "app_service.h"
#include "storage_file.h"
#include "storage.h"
#include "epd_memory.h"
#include "platform/epd_app_library.h"
#include "document.h"
#include "document_cache.h"
#include "reader_compute_client.h"
#include "../formats/formats.h"
#include "../image/book_svg.h"
#include "../image/book_image.h"
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

#define DOCUMENT_IO_WINDOW 8192u
#define DOCUMENT_CHAPTER_BATCH 64u
#define DOCUMENT_CONTENT_SLOTS (DOCUMENT_CONTENT_CACHE_BUDGET / DOCUMENT_IO_WINDOW)
typedef struct document_chapters {
    struct document_chapters *next;
    unsigned count;
    uint32_t starts[DOCUMENT_CHAPTER_BATCH];
} document_chapters_t;

typedef struct {
    document_t *document;
    uint8_t *data;
    uint32_t start, used;
    uint64_t stamp;
} document_content_t;
struct document_reader {
    struct document_reader *next;
    document_t *document;
    int descriptor;
    uint32_t visible, position, window_start, window_used;
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
#define DOCUMENT_INDEX_MAGIC 0x44584942u

struct document {
    struct document *next;
    bookshelf_item_t file;
    document_source_t source;
    char app_id[64], directory[STORAGE_PATH_MAX], identity[BOOKSHELF_PATH_MAX];
    document_format_t format;
    uint32_t generation, serial, end, link;
    uint32_t cache_generation;
    uint32_t pending_end, input_publication;
    rt_tick_t last_publish, opened_at;
    bool readable_logged;
    int output, input, cache_result;
    uint8_t *write_buffer;
    unsigned write_used;
    document_reader_t *readers, *default_reader;
    document_chapters_t *chapters, *chapters_tail;
    document_reader_t *chapter_reader;
    uint32_t chapter_scan;
    bool chapter_failed, chapter_restore, prefetch_enabled;
    uint32_t prefetch_record, prefetch_request, prefetch_done, prefetch_active;
    uint32_t prefetch_publication, prefetch_focus, prefetch_low, prefetch_high;
    uint32_t prefetch_blocks[DOCUMENT_CONTENT_SLOTS];
    unsigned prefetch_cursor, prefetch_count;
    document_reader_t *prefetch_reader;
    uint8_t *prefetch_scratch;
    document_chapter_range_t prefetch_range;
    bool persistent, space_retried, cache_bad, cache_touch, temp_owned;
    document_cache_t *cached;
    char cache[STORAGE_PATH_MAX];
    char work_error[96];
    char cache_error[96];
    void *library;
    const book_convert_api_t *converter;
    bool cancelled, claimed, stopped, released;
    document_status_t status;
};
static document_t *documents;
static document_t *worker_document;
static epd_service_t *worker_service;
static bool worker_stopping;
static uint32_t sequence;
static bool cache_initialized[STORAGE_COUNT];
static document_image_request_t *image_requests;
static document_save_t *saves;
/* Payloads are allocated by the document worker only. storage_lock protects
 * slot ownership and copies; readers never retain pointers into these slots. */
static document_content_t content_slots[DOCUMENT_CONTENT_SLOTS];
static uint64_t content_stamp;
static document_t *content_target;
static document_t *prefetch_document;
static uint32_t content_payload;
static void image_work(document_t *only);
static void save_work(void);
static void save_release(document_save_t *job);
static bool image_pending(document_t *d);
static bool image_referenced(document_t *d);
static void image_collect(void);
static void readers_close(document_t *d);
static void content_work(document_t *d);
static bool content_pending(document_t *d);
static void content_dispose(document_t *d);
static void prefetch_release(document_t *d);

static void *allocate(size_t bytes) { return epd_app_alloc(bytes, EPD_APP_PSRAM); }
static void *resize(void *p, size_t bytes) { return epd_app_realloc(p, bytes, EPD_APP_PSRAM); }
static const book_allocator_t allocator = {allocate, resize, epd_app_free};

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
            !storage_file_signature(child, &content)) { valid = false; break; }
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
static bool cancelled(void *user)
{
    document_t *d = user;
    return d->cancelled || (worker_service && epd_service_cancelled(worker_service)) ||
           (d->source.cancelled && d->source.cancelled(d->source.user)) ||
           !media_valid(d->file.path, d->generation) ||
           (d->source.convert && !media_valid(d->directory, d->cache_generation)) ||
           !media_valid(d->cache, d->cache_generation);
}

static void wake_worker(void)
{
    if (worker_service) epd_service_wake(worker_service);
}

static reader_error_t open_error;
reader_error_t document_open_error(void) { return open_error; }

static unsigned elapsed_ms(rt_tick_t since)
{
    return (unsigned)((uint64_t)(rt_tick_t)(rt_tick_get() - since) * 1000 / RT_TICK_PER_SECOND);
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
    if (!force && bytes < batch && now - d->last_publish < rt_tick_from_millisecond(50)) return BOOK_OK;
    int flushed = write_flush(d);
    if (flushed != BOOK_OK) return flushed;
    storage_lock();
    int result = BOOK_CANCELLED;
    if (!cancelled(d))
    {
        errno = 0;
        int synced = fsync(d->output);
        if (synced && (errno == ENOSPC || errno == -ENOSPC) && !d->space_retried && d->cached)
        {
            d->space_retried = true;
            if (document_cache_reclaim(d->cached)) synced = fsync(d->output);
        }
        result = synced == 0 ? BOOK_OK : cache_failure(d, "sync", errno);
    }
    storage_unlock();
    if (result != BOOK_OK) return result;
    rt_base_t key = rt_hw_interrupt_disable();
    d->status.end = d->pending_end;
    ++d->status.publication;
    rt_hw_interrupt_enable(key);
    d->last_publish = now;
    if (worker_document == d && worker_service) epd_service_ui_request(worker_service, 0);
    log_readable(d);
    /* Only complete, synced records are visible to cooperative pixel jobs. */
    if (worker_document == d)
    {
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
        int result = record_write(d, DOCUMENT_TEXT, style, text, n);
        if (result != BOOK_OK) return result;
        text += n; length -= n;
    }
    return BOOK_OK;
}
static int sink_paragraph(void *user) { return record_write(user, DOCUMENT_PARAGRAPH, 0, NULL, 0); }
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
    return record_write(user, DOCUMENT_ANCHOR, level, data, a + b + 2);
}
static int sink_link(void *user, const char *target, bool begin)
{
    document_t *d = user;
    if (!begin) { d->link = 0; return BOOK_OK; }
    if (!target || strlen(target) >= 1024) return BOOK_UNSUPPORTED;
    uint32_t offset = d->end;
    int result = record_write(d, DOCUMENT_LINK, 0, target, strlen(target) + 1);
    if (result == BOOK_OK) d->link = offset + 1;
    return result;
}
static int sink_metadata(void *user, const char *title, const char *author)
{
    (void)author;
    document_t *d = user;
    if (title && *title)
    {
        rt_base_t key = rt_hw_interrupt_disable();
        snprintf(d->status.title, sizeof(d->status.title), "%s", title);
        rt_hw_interrupt_enable(key);
    }
    return cancelled(d) ? BOOK_CANCELLED : BOOK_OK;
}
static int missing_image(uint8_t **gray, unsigned *width, unsigned *height)
{
    *width = 48; *height = 32;
    *gray = allocate(*width * *height);
    if (!*gray) return BOOK_NO_MEMORY;
    /* A crossed frame uses the same packed-pixel record and UI as other images. */
    for (unsigned y = 0; y < *height; ++y)
        for (unsigned x = 0; x < *width; ++x)
        {
            unsigned diagonal = x * (*height - 1) / (*width - 1);
            bool ink = !x || !y || x == *width - 1 || y == *height - 1 ||
                       y == diagonal || y + diagonal == *height - 1;
            (*gray)[y * *width + x] = ink ? 0 : 255;
        }
    return BOOK_OK;
}
static int image_record(document_t *d, uint8_t *gray, unsigned width, unsigned height)
{
    if (cancelled(d)) { epd_app_free(gray); return BOOK_CANCELLED; }
    document_image_t dimensions = {width, height};
    size_t pixels = width * height, bytes = (pixels + 1) / 2;
    for (size_t i = 0; i < pixels; i += 2)
        gray[i / 2] = (uint8_t)((((gray[i] + 8) / 17) << 4) |
                       (i + 1 < pixels ? (gray[i + 1] + 8) / 17 : 15));
    document_record_t record = {DOCUMENT_IMAGE, sizeof(dimensions) + bytes, 0, d->link};
    int result = cache_room(d, sizeof(record) + record.length);
    if (result == BOOK_OK) result = write_all(d, &record, sizeof(record));
    if (result == BOOK_OK) result = write_all(d, &dimensions, sizeof(dimensions));
    if (result == BOOK_OK) result = write_all(d, gray, bytes);
    epd_app_free(gray);
    if (result == BOOK_OK) result = record_complete(d);
    return result;
}

static int sink_image(void *user, const char *id, book_stream_t *stream)
{
    (void)id;
    document_t *d = user;
    if (cancelled(d)) return BOOK_CANCELLED;
    uint8_t *gray = NULL;
    unsigned width = 0, height = 0;
    int result = stream ? book_image_decode(stream, DOCUMENT_IMAGE_WIDTH, DOCUMENT_IMAGE_HEIGHT,
                          cancelled, d, &gray, &width, &height, d->work_error, sizeof(d->work_error)) :
                          missing_image(&gray, &width, &height);
    return result == BOOK_OK ? image_record(d, gray, width, height) : result;
}

static int sink_svg(void *user, const char *xml, size_t length, const char *base,
                    const book_resources_t *resources)
{
    document_t *d = user;
    uint8_t *gray = NULL;
    unsigned width = 0, height = 0;
    int result = book_svg_decode(xml, length, base, resources,
                                 DOCUMENT_IMAGE_WIDTH, DOCUMENT_IMAGE_HEIGHT,
                                 cancelled, d, &gray, &width, &height);
    if (result == BOOK_OK) return image_record(d, gray, width, height);
    if (result != BOOK_CANCELLED && result != BOOK_NO_MEMORY && result != BOOK_NO_SPACE)
        snprintf(d->work_error, sizeof(d->work_error), "SVG 图像或其引用的图片无法解析");
    return result;
}

static void result_locked(document_t *d, int result)
{
    d->status.result = result;
    d->status.error_code = result == BOOK_OK ? READER_ERROR_NONE :
        result == BOOK_CANCELLED ? READER_ERROR_CANCELLED :
        result == BOOK_NO_MEMORY ? READER_ERROR_CONVERT_MEMORY :
        result == BOOK_NO_SPACE ? READER_ERROR_NO_SPACE :
        result == BOOK_UNSUPPORTED ? (d->cache_error[0] ? READER_ERROR_CACHE_RANGE : READER_ERROR_UNSUPPORTED) :
        result == BOOK_IO_ERROR ? (d->cache_error[0] ? READER_ERROR_CACHE_WRITE : READER_ERROR_IO) :
        READER_ERROR_PARSE;
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
    result->memory = allocate(stride * height + 31);
    if (!result->memory) return false;
    result->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    result->image.header.cf = LV_COLOR_FORMAT_RGB565;
    result->image.header.w = width;
    result->image.header.h = height;
    result->image.header.stride = stride;
    result->image.data = (uint8_t *)RT_ALIGN((uintptr_t)result->memory, 32);
    result->image.data_size = stride * height;
    memset((void *)result->image.data, 255, result->image.data_size);
    return true;
}
static uint16_t rgb565(unsigned gray)
{
    gray = ((gray + 8) / 17) * 17;
    return ((gray & 0xf8) << 8) | ((gray & 0xfc) << 3) | (gray >> 3);
}
static void bitmap_clean(document_bitmap_t *bitmap)
{
    lv_draw_buf_t buffer = {.data = (uint8_t *)bitmap->image.data, .data_size = bitmap->image.data_size};
    epd_app_clean_draw_buffer(&buffer);
}
static bool cache_open(document_t *d)
{
    storage_lock();
    if (cancelled(d)) { storage_unlock(); return false; }
    char directory[STORAGE_PATH_MAX];
    if (d->source.convert) snprintf(directory, sizeof(directory), "%s", d->directory);
    else if (!document_cache_directory(&d->file, "books", directory, sizeof(directory)))
    { storage_unlock(); return false; }
    storage_volume_t volume = !strcmp(storage_path_root(directory), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    if (!d->source.convert) d->cache_generation = storage_card_session();
    int named = d->source.convert ?
        snprintf(d->cache, sizeof(d->cache), "%s/tmp-%08lx-%08lx.bin", directory,
                 (unsigned long)hash_bytes(2166136261u, d->app_id, strlen(d->app_id)), (unsigned long)d->serial) :
        snprintf(d->cache, sizeof(d->cache), "%s/doc-%08lx.bin", directory, (unsigned long)d->serial);
    if (named < 0 || (size_t)named >= sizeof(d->cache)) { storage_unlock(); return false; }
    if (cancelled(d)) { storage_unlock(); return false; }
    errno = 0;
    bool ok = storage_mkdirs(directory);
    if (!ok) cache_failure(d, "mkdir", errno);
    if (ok && !d->source.convert && !cache_initialized[volume])
    {
        DIR *dir = opendir(directory);
        struct dirent *entry;
        while (dir && (entry = readdir(dir)) != NULL)
        {
            if (cancelled(d)) { ok = false; break; }
            size_t n = strlen(entry->d_name);
            if (n == 16 && !strncmp(entry->d_name, "doc-", 4) && !strcmp(entry->d_name + 12, ".bin"))
            {
                char path[STORAGE_PATH_MAX];
                size_t prefix = strlen(directory);
                if (prefix + 18 > sizeof(path)) { ok = false; break; }
                memcpy(path, directory, prefix); path[prefix] = '/';
                memcpy(path + prefix + 1, entry->d_name, 17);
                unlink(path);
            }
        }
        if (dir) closedir(dir);
        cache_initialized[volume] = true;
    }
    if (ok && !cancelled(d))
    {
        do
        {
            errno = 0;
            d->output = open(d->cache, O_CREAT | O_WRONLY | (d->source.convert ? O_EXCL : O_TRUNC), 0);
            if (d->output >= 0) { d->temp_owned = true; break; }
            if (!d->source.convert || (errno != EEXIST && errno != -EEXIST) || cancelled(d)) break;
            d->serial = ++sequence;
            snprintf(d->cache, sizeof(d->cache), "%.*s/tmp-%08lx-%08lx.bin",
                     (int)sizeof(d->cache) - 32, directory,
                     (unsigned long)hash_bytes(2166136261u, d->app_id, strlen(d->app_id)), (unsigned long)d->serial);
        } while (!cancelled(d));
        if (d->output < 0) cache_failure(d, "open", errno);
    }
    if (d->output >= 0) d->write_buffer = allocate(DOCUMENT_IO_WINDOW);
    d->last_publish = rt_tick_get();
    storage_unlock();
    return d->output >= 0;
}

static void convert(document_t *d)
{
    int result = BOOK_IO_ERROR;
    bool cacheable = true;
    if (cache_open(d))
    {
        book_sink_t sink = {d, cancelled, sink_text, sink_paragraph, sink_anchor,
                           sink_image, sink_metadata, sink_link, book_markup_convert, sink_svg};
        bool observing = d->cached && document_cache_observe_begin(d->cached);
        if (d->source.convert)
            result = d->source.convert(&d->source, &sink, &allocator, d->work_error, sizeof(d->work_error), &cacheable);
        else if (d->converter)
        {
            result = book_markup_begin(&allocator);
            if (result == BOOK_OK)
            {
                result = d->converter->convert(d->file.path, &sink, &allocator,
                                               d->work_error, sizeof(d->work_error));
                book_markup_end();
            }
        }
        else result = book_format_convert(d->file.path, &sink, &allocator,
                                           d->work_error, sizeof(d->work_error));
        if (observing) document_cache_observe_end(d->cached);
        if (result == BOOK_OK) result = cache_publish(d, true);
        storage_lock();
        errno = 0;
        int closed = close(d->output);
        if (closed != 0 && result == BOOK_OK) result = cache_failure(d, "close", errno);
        d->output = -1;
        storage_unlock();
        epd_app_free(d->write_buffer); d->write_buffer = NULL; d->write_used = 0;
        if (result == BOOK_OK && cacheable && observing && !cancelled(d))
        {
            storage_lock();
            if (!cancelled(d) && document_cache_prepare(d->cached))
            {
                readers_close(d);
                const char *binary = document_cache_binary(d->cached);
                if (storage_file_commit(d->cache, binary))
                {
                    snprintf(d->cache, sizeof(d->cache), "%s", binary);
                    storage_unlock();
                    d->persistent = document_cache_commit(d->cached, d->status.title, d->status.end);
                    storage_lock();
                }
            }
            storage_unlock();
        }
    }
    if (result != BOOK_OK && d->cache_result != BOOK_OK) result = d->cache_result;
    if (cancelled(d)) result = BOOK_CANCELLED;
    rt_base_t key = rt_hw_interrupt_disable();
    result_locked(d, result);
    d->status.complete = true;
    rt_hw_interrupt_enable(key);
}

bool document_worker_step(epd_service_t *service)
{
    if (worker_stopping) return false;
    worker_service = service;
    rt_base_t key = rt_hw_interrupt_disable();
    document_t *d = NULL;
    /* Close cancelled documents before entering another backend instance. */
    for (document_t *p = documents; p; p = p->next)
        if (!p->stopped && cancelled(p)) { d = p; break; }
    if (!d)
        for (document_t *p = documents; p; p = p->next)
            if (!cancelled(p) && image_pending(p)) { d = p; break; }
    if (!d)
        for (document_t *p = documents; p; p = p->next)
            if (p->cache_bad || (p->prefetch_enabled && p->cache_touch) || content_pending(p) ||
                (!p->stopped && (!p->claimed || image_pending(p))))
            { d = p; break; }
    worker_document = d;
    rt_hw_interrupt_enable(key);
    if (!d)
    {
        if (saves) { save_work(); return true; }
        return false;
    }
    if (!cancelled(d) && !d->claimed) { d->claimed = true; convert(d); }
    if (d->cache_bad)
    {
        document_cache_invalidate(d->cached);
        d->cache_bad = d->cache_touch = false;
    }
    else if (d->prefetch_enabled && d->cache_touch)
    {
        d->cache_touch = false;
        if (!cancelled(d)) document_cache_touch(d->cached);
    }
    image_work(d);
    content_work(d);
    key = rt_hw_interrupt_disable();
    /* A paused cursor needs no work until resume, but must remain eligible
     * for the cancellation pass that releases its worker-owned reader. */
    d->stopped = cancelled(d) || (!image_pending(d) && !content_pending(d) && !d->cache_bad &&
                                 !(d->prefetch_enabled && d->cache_touch) && prefetch_document != d);
    worker_document = NULL;
    rt_hw_interrupt_enable(key);
    return true;
}

void document_worker_shutdown(void)
{
    /* The caller has returned from its last worker_step before draining. */
    rt_base_t key = rt_hw_interrupt_disable();
    worker_stopping = true;
    rt_hw_interrupt_enable(key);
    for (;;)
    {
        key = rt_hw_interrupt_disable();
        document_t *d = documents;
        while (d && d->stopped && !d->cache_bad && d->cancelled) d = d->next;
        worker_document = d;
        rt_hw_interrupt_enable(key);
        if (!d) break;
        d->cancelled = true;
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
    worker_service = NULL;
}

void document_worker(epd_service_t *service)
{
    worker_stopping = false;
    worker_service = service;
    while (!epd_service_cancelled(service))
        if (!document_worker_step(service)) epd_service_wait(service, RT_WAITING_FOREVER);
    document_worker_shutdown();
}

document_t *document_open_source(const document_source_t *source, char *error, size_t size)
{
    rt_tick_t started = rt_tick_get();
    document_service_process();
    open_error = READER_ERROR_NONE;
    if (!source || !source->convert || !source->path || !source->app_id ||
        !source->cache_path || !source->identity || !*source->app_id || !*source->identity ||
        source->path[0] != '/' || source->cache_path[0] != '/' ||
        strlen(source->path) >= BOOKSHELF_PATH_MAX || strlen(source->app_id) >= 64 ||
        strlen(source->cache_path) + 32 >= STORAGE_PATH_MAX ||
        strlen(source->app_id) + strlen(source->identity) + 2 >= BOOKSHELF_PATH_MAX)
    { open_error = READER_ERROR_INPUT; snprintf(error, size, "文档输入或缓存身份无效"); return NULL; }
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
    d->serial = ++sequence; d->format = source->format;
    snprintf(d->file.path, sizeof(d->file.path), "%s", source->path);
    snprintf(d->file.name, sizeof(d->file.name), "%s", source->title ? source->title : "");
    d->file.format = source->format;
    snprintf(d->app_id, sizeof(d->app_id), "%s", source->app_id);
    snprintf(d->directory, sizeof(d->directory), "%s", source->cache_path);
    snprintf(d->identity, sizeof(d->identity), "%s", source->identity);
    d->source = *source;
    d->source.app_id = d->app_id; d->source.cache_path = d->directory;
    d->source.identity = d->identity; d->source.path = d->file.path; d->source.title = d->file.name;
    snprintf(d->status.title, sizeof(d->status.title), "%s", d->file.name);
    d->cached = document_cache_create_source(&d->file, d->app_id, d->directory, d->identity);
    if (cache_lookup(d))
    {
        snprintf(d->cache, sizeof(d->cache), "%s", document_cache_binary(d->cached));
        d->status.end = d->pending_end = d->end;
        d->status.publication = 1; d->status.complete = true;
        d->persistent = d->claimed = d->stopped = true; d->cache_touch = true;
        d->chapter_restore = chapter_format(d);
        log_readable(d);
    }
    rt_base_t key = rt_hw_interrupt_disable();
    if (worker_stopping)
    {
        rt_hw_interrupt_enable(key);
        open_error = READER_ERROR_STOPPING; snprintf(error, size, "阅读服务正在停止");
        log_open(d);
        document_cache_destroy(d->cached); epd_app_free(d); return NULL;
    }
    d->next = documents; documents = d;
    rt_hw_interrupt_enable(key);
    log_open(d);
    wake_worker();
    return d;
}

document_t *document_open(const bookshelf_item_t *file, char *error, size_t size)
{
    rt_tick_t started = rt_tick_get();
    document_service_process();
    open_error = READER_ERROR_NONE;
    document_t *d = allocate(sizeof(*d));
    if (!d) { open_error = READER_ERROR_SESSION_MEMORY; snprintf(error, size, "文档会话内存不足"); return NULL; }
    memset(d, 0, sizeof(*d));
    d->opened_at = started;
    d->input = d->output = -1;
    d->file = *file;
    d->format = (document_format_t)file->format;
    d->generation = storage_card_session();
    d->serial = ++sequence;
    snprintf(d->status.title, sizeof(d->status.title), "%s", file->name);
    d->cached = document_cache_create(file);
    if (cache_lookup(d))
    {
        snprintf(d->cache, sizeof(d->cache), "%s", document_cache_binary(d->cached));
        d->cache_generation = storage_card_session();
        d->status.end = d->pending_end = d->end;
        d->status.publication = 1; d->status.complete = true;
        d->persistent = d->claimed = d->stopped = true;
        d->cache_touch = true;
        d->chapter_restore = chapter_format(d);
        log_readable(d);
    }
    const char *name = !d->persistent && d->format == DOCUMENT_MOBI ? "bk_mobi" : NULL;
    if (name)
    {
        d->library = epd_app_library_open("books", name);
        if (!d->library)
        {
            open_error = READER_ERROR_CODEC_LOAD;
            snprintf(error, size, "无法加载 %s 解码库", name);
            log_open(d);
            document_cache_destroy(d->cached); epd_app_free(d); return NULL;
        }
        d->converter = epd_app_library_symbol(d->library, "book_converter");
        if (!d->converter || d->converter->abi != BOOK_CODEC_ABI ||
            d->converter->size < sizeof(*d->converter) || !d->converter->convert) goto incompatible;
    }
    rt_base_t key = rt_hw_interrupt_disable();
    d->next = documents; documents = d;
    rt_hw_interrupt_enable(key);
    log_open(d);
    wake_worker();
    return d;
incompatible:
    open_error = READER_ERROR_CODEC_ABI; snprintf(error, size, "解码库版本与书架不匹配");
    log_open(d);
    epd_app_library_close(d->library);
    document_cache_destroy(d->cached);
    epd_app_free(d);
    return NULL;
}

void document_close(document_t *d)
{
    if (!d) return;
    rt_base_t key = rt_hw_interrupt_disable();
    d->cancelled = d->released = true;
    for (document_image_request_t *j = image_requests; j; j = j->next)
        if (j->document == d) j->released = true;
    rt_hw_interrupt_enable(key);
    wake_worker();
}

void document_service_process(void)
{
    image_collect();
    document_t **link = &documents;
    while (*link)
    {
        document_t *d = *link;
        if (!media_valid(d->file.path, d->generation) || !media_valid(d->cache, d->cache_generation))
        {
            d->cancelled = true;
            cache_initialized[STORAGE_SD] = false;
            wake_worker();
        }
        rt_base_t key = rt_hw_interrupt_disable();
        /* Superseded requests that never reached the worker own no backend. */
        if (d->released && !d->claimed && worker_document != d) d->stopped = true;
        bool dispose = d->stopped && d->released && worker_document != d && prefetch_document != d && !image_referenced(d) &&
                       (!worker_service || !d->cache_bad);
        if (dispose) *link = d->next;
        rt_hw_interrupt_enable(key);
        if (!dispose) { link = &d->next; continue; }
        storage_lock(); readers_close(d); storage_unlock();
        while (d->readers) document_reader_destroy(d->readers);
        content_dispose(d);
        if (d->input >= 0) { storage_lock(); close(d->input); storage_unlock(); d->input = -1; }
        if (d->cache[0] && !d->persistent && (!d->source.convert || d->temp_owned))
        {
            storage_lock();
            if (media_valid(d->cache, d->cache_generation)) unlink(d->cache);
            storage_unlock();
        }
        if (d->library) { epd_app_library_close(d->library); d->library = NULL; }
        epd_app_free(d->write_buffer);
        document_cache_destroy(d->cached);
        if (d->source.release_user) d->source.release_user(d->source.user);
        epd_app_free(d);
    }
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
    memset(cache_initialized, 0, sizeof(cache_initialized));
    document_cache_release_memory();
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
void document_progress(document_t *d, document_progress_t *progress)
{
    rt_base_t key = rt_hw_interrupt_disable();
    progress->end = d->status.end;
    progress->publication = d->status.publication;
    progress->complete = d->status.complete;
    progress->result = d->status.result;
    rt_hw_interrupt_enable(key);
    if (cancelled(d)) progress->result = BOOK_CANCELLED;
}

bool document_chapter_range(document_t *d, uint32_t record, document_chapter_range_t *range)
{
    if (!d || !range) return false;
    memset(range, 0, sizeof(*range));
    if (cancelled(d)) return false;
    rt_base_t key = rt_hw_interrupt_disable();
    uint32_t visible = d->status.end;
    if (record >= visible || d->status.result != BOOK_OK)
    { rt_hw_interrupt_enable(key); return false; }
    range->end = range->next_end = visible;
    range->publication = d->status.publication;
    range->end_known = range->next_end_known = d->status.complete;
    if (d->chapter_restore)
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
    bool changed = !d->cancelled && d->prefetch_enabled != enabled;
    if (changed)
    {
        d->prefetch_enabled = enabled;
        d->stopped = false;
    }
    rt_hw_interrupt_enable(key);
    if (changed) wake_worker();
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
    r->document = d; r->descriptor = -1;
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
    if (r->descriptor >= 0) close(r->descriptor);
    rt_base_t key = rt_hw_interrupt_disable();
    document_reader_t **link = &d->readers;
    while (*link && *link != r) link = &(*link)->next;
    if (*link) *link = r->next;
    if (d->prefetch_reader == r) d->prefetch_reader = NULL;
    if (d->default_reader == r) { d->default_reader = NULL; d->input = -1; }
    rt_hw_interrupt_enable(key);
    storage_unlock();
    epd_app_free(r->window); epd_app_free(r);
}

/* storage_lock serializes cursor I/O and the completed-cache name switch. */
static void readers_close(document_t *d)
{
    for (document_reader_t *r = d->readers; r; r = r->next)
    {
        if (r->descriptor >= 0) close(r->descriptor);
        r->descriptor = -1; r->visible = r->window_used = 0;
    }
    d->input = -1;
}

static bool reader_fill(document_reader_t *r, uint32_t offset, void *data, size_t length)
{
    document_t *d = r->document;
    if (r->position != offset)
    {
        if (lseek(r->descriptor, offset, SEEK_SET) != (off_t)offset) return false;
        r->position = offset;
    }
    uint8_t *p = data;
    while (length)
    {
        if (cancelled(d)) return false;
        size_t chunk = length > DOCUMENT_IO_WINDOW ? DOCUMENT_IO_WINDOW : length;
        int n = read(r->descriptor, p, chunk);
        if (n <= 0) return false;
        r->position += n;
        p += n; length -= n;
    }
    return !cancelled(d);
}

static void cache_read_failed(document_t *d)
{
    if (!d->persistent || cancelled(d)) return;
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

static bool content_pending(document_t *d)
{
    return d->prefetch_enabled && !cancelled(d) && d->status.result == BOOK_OK &&
        ((content_target == d && content_payload && !content_room(0)) || d->chapter_restore || (d->prefetch_request &&
         (d->prefetch_request != d->prefetch_done || d->prefetch_publication != d->status.publication)));
}

/* Reopening a persistent spool scans record headers and small anchors only.
 * Image payloads are skipped. A pending index never claims a stable chapter. */
static void chapter_restore_work(document_t *d)
{
    if (!d->chapter_reader)
    {
        size_t extra = sizeof(document_reader_t) + DOCUMENT_IO_WINDOW + 128u;
        storage_lock(); content_trim(extra); storage_unlock();
        if (content_room(extra)) d->chapter_reader = document_reader_create(d);
    }
    bool failed = !d->chapter_reader;
    rt_tick_t started = rt_tick_get(), slice = rt_tick_from_millisecond(2);
    if (!slice) slice = 1;
    uint32_t window = d->chapter_reader ? d->chapter_reader->window_start : 0;
    for (unsigned i = 0; !failed && i < 16 && d->chapter_scan < d->status.end; ++i)
    {
        document_record_t record;
        if (!document_reader_record(d->chapter_reader, d->chapter_scan, &record))
        { failed = true; break; }
        if (record.kind == DOCUMENT_ANCHOR && record.length >= 2 && !record.style)
        {
            char anchor[DOCUMENT_TEXT_LIMIT];
            if (!document_reader_read(d->chapter_reader, d->chapter_scan + sizeof(record), anchor, record.length))
            { failed = true; break; }
            const char *nul = memchr(anchor, 0, record.length);
            if (nul && nul == anchor + record.length - 2 && !nul[1] && chapter_root(anchor, nul + 1, record.style))
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
        d->chapter_failed |= failed;
        d->chapter_restore = false;
        /* Boundary consumers must notice index readiness even though the
         * persistent spool length itself has not changed. */
        ++d->status.publication;
        d->prefetch_active = 0;
        d->prefetch_done = 0;
        rt_hw_interrupt_enable(key);
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
    if (cancelled(d) || d->status.result != BOOK_OK) { prefetch_release(d); return; }
    if (!d->prefetch_enabled) return;
    if (prefetch_document && prefetch_document != d) prefetch_release(prefetch_document);
    storage_lock(); content_trim(0); storage_unlock();
    if (!content_room(0)) { prefetch_release(d); }
    if (d->chapter_restore) chapter_restore_work(d);
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
        uint32_t n = d->status.end - at;
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
    document_progress(d, &status);
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
    if (r->descriptor >= 0 && (offset > r->visible || length > r->visible - offset))
    { close(r->descriptor); r->descriptor = -1; }
    if (r->descriptor < 0)
    {
        r->descriptor = open(d->cache, O_RDONLY);
        if (r->descriptor < 0) goto done;
        r->position = 0; r->visible = status.end;
        if (d->default_reader == r) { d->input = r->descriptor; d->input_publication = status.publication; }
    }
    if (r->window && length <= DOCUMENT_IO_WINDOW)
    {
        uint32_t start = offset / DOCUMENT_IO_WINDOW * DOCUMENT_IO_WINDOW;
        if (length > DOCUMENT_IO_WINDOW - (offset - start)) start = offset;
        uint32_t n = r->visible - start;
        if (n > DOCUMENT_IO_WINDOW) n = DOCUMENT_IO_WINDOW;
        r->window_used = 0;
        if (!reader_fill(r, start, r->window, n)) goto done;
        r->window_start = start; r->window_used = n;
        content_store(d, start, r->window, n, false);
        memcpy(data, r->window + offset - start, length); ok = true;
    }
    else ok = reader_fill(r, offset, data, length);
done:
    if (!ok) cache_read_failed(d);
    storage_unlock();
    return ok;
}

bool document_reader_record(document_reader_t *r, uint32_t offset, document_record_t *record)
{
    if (!r || !document_reader_read(r, offset, record, sizeof(*record))) return false;
    document_progress_t status;
    document_progress(r->document, &status);
    bool ok = status.result == BOOK_OK && offset <= status.end && sizeof(*record) <= status.end - offset &&
           record->length <= status.end - offset - sizeof(*record) &&
           record->kind >= DOCUMENT_TEXT && record->kind <= DOCUMENT_IMAGE &&
           (record->kind == DOCUMENT_IMAGE || record->length <= DOCUMENT_TEXT_LIMIT);
    if (!ok) cache_read_failed(r->document);
    return ok;
}

bool document_reader_image_info(document_reader_t *r, uint32_t offset, document_image_t *image)
{
    document_record_t record;
    if (!image || !document_reader_record(r, offset, &record)) return false;
    if (record.kind != DOCUMENT_IMAGE || record.length < sizeof(*image))
    { cache_read_failed(r->document); return false; }
    if (!document_reader_read(r, offset + sizeof(record), image, sizeof(*image))) return false;
    if (!image->width || !image->height || image->width > DOCUMENT_IMAGE_WIDTH ||
        image->height > DOCUMENT_IMAGE_HEIGHT ||
        record.length != sizeof(*image) + (image->width * image->height + 1) / 2)
    { cache_read_failed(r->document); return false; }
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

static bool image_load(document_reader_t *r, uint32_t offset, unsigned width, unsigned height, document_bitmap_t *bitmap)
{
    document_image_t source;
    if (!document_reader_image_info(r, offset, &source)) return false;
    if (!bitmap_allocate(bitmap, width, height)) return false;
    unsigned bytes = (source.width * source.height + 1) / 2;
    uint8_t *packed = allocate(bytes);
    if (!packed || !document_reader_read(r, offset + sizeof(document_record_t) + sizeof(source), packed, bytes))
    { epd_app_free(packed); epd_app_free(bitmap->memory); bitmap->memory = NULL; return false; }
    for (unsigned y = 0; y < height; ++y)
    {
        if (cancelled(r->document))
        { epd_app_free(packed); epd_app_free(bitmap->memory); memset(bitmap, 0, sizeof(*bitmap)); return false; }
        unsigned pixel = (y * source.height / height) * source.width;
        uint16_t *out = (uint16_t *)(bitmap->image.data + y * bitmap->image.header.stride);
        for (unsigned x = 0; x < width; ++x)
        {
            unsigned sx = pixel + x * source.width / width;
            unsigned nibble = (packed[sx / 2] >> (sx & 1 ? 0 : 4)) & 15;
            out[x] = rgb565(nibble * 17);
        }
    }
    epd_app_free(packed);
    return true;
}

bool document_image_load(document_t *d, uint32_t offset, unsigned width, unsigned height, document_bitmap_t *bitmap)
{
    document_reader_t *r = document_reader_create(d);
    bool ok = r && image_load(r, offset, width, height, bitmap);
    document_reader_destroy(r);
    if (ok) bitmap_clean(bitmap);
    return ok;
}

document_image_request_t *document_image_request(document_t *d, uint32_t offset, unsigned width, unsigned height)
{
    if (!d || cancelled(d) || !width || !height) return NULL;
    document_image_request_t *job = allocate(sizeof(*job));
    if (!job) return NULL;
    memset(job, 0, sizeof(*job));
    job->document = d; job->offset = offset; job->width = width; job->height = height;
    rt_base_t key = rt_hw_interrupt_disable();
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
    rt_hw_interrupt_enable(key);
    wake_worker();
}

static bool image_pending(document_t *d)
{
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
            ok = reader && image_load(reader, job->offset, job->width, job->height, &job->bitmap);
            document_reader_destroy(reader);
        }
        key = rt_hw_interrupt_disable();
        job->result = ok ? BOOK_OK : progress.result != BOOK_OK ? progress.result : BOOK_IO_ERROR;
        job->done = !waiting;
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
    if (job) job->active = true;
    rt_hw_interrupt_enable(key);
    if (!job) return;
    storage_lock();
    if ((!worker_service || !epd_service_cancelled(worker_service)) && media_valid(job->path, job->generation))
    {
        char directory[STORAGE_PATH_MAX];
        memcpy(directory, job->path, strlen(job->path) + 1);
        char *slash = strrchr(directory, '/');
        if (slash) *slash = 0;
        if (slash && storage_mkdirs(directory)) storage_file_replace(job->path, job->data, job->length);
    }
    storage_unlock();
    key = rt_hw_interrupt_disable();
    saves = job->next;
    rt_hw_interrupt_enable(key);
    save_release(job);
}

bool document_index_save(document_t *d, uint32_t layout, const void *data, size_t size)
{
    if (!d || !d->persistent || !d->status.complete || d->status.result != BOOK_OK ||
        cancelled(d) || !size || !data || size > INT32_MAX - sizeof(document_index_header_t)) return false;
    document_index_header_t header = {DOCUMENT_INDEX_MAGIC, 1, document_cache_identity(d->cached),
        layout, d->status.end, (uint32_t)size, hash_bytes(2166136261u, data, size)};
    if (!header.content) return false;
    return document_save_async(document_cache_index(d->cached), &header, sizeof(header), data, size);
}

static bool index_header_valid(document_t *d, uint32_t layout,
                               const document_index_header_t *header, uint64_t length)
{
    return header->magic == DOCUMENT_INDEX_MAGIC && header->version == 1 &&
        header->content == document_cache_identity(d->cached) && header->layout == layout &&
        header->end == d->status.end && header->bytes && header->bytes <= INT32_MAX - sizeof(*header) &&
        length == sizeof(*header) + header->bytes;
}

static bool index_load_queued(document_t *d, const char *path, uint32_t layout, void **data, size_t *size)
{
    void *buffer = NULL;
    size_t length = 0;
    if (!document_save_snapshot(path, &buffer, &length)) return false;
    document_index_header_t header;
    bool ok = length >= sizeof(header) && !cancelled(d);
    if (ok)
    {
        memcpy(&header, buffer, sizeof(header));
        ok = index_header_valid(d, layout, &header, length);
    }
    if (ok) ok = hash_bytes(2166136261u, (uint8_t *)buffer + sizeof(header), header.bytes) == header.checksum &&
                 !cancelled(d);
    if (!ok) { epd_app_free(buffer); return false; }
    memmove(buffer, (uint8_t *)buffer + sizeof(header), header.bytes);
    *data = buffer; *size = header.bytes;
    return true;
}

bool document_index_load(document_t *d, uint32_t layout, void **data, size_t *size)
{
    if (!data || !size) return false;
    *data = NULL; *size = 0;
    if (!d || !d->persistent || !d->status.complete || d->status.result != BOOK_OK || cancelled(d)) return false;
    const char *path = document_cache_index(d->cached);
    if (index_load_queued(d, path, layout, data, size)) return true;
    document_index_header_t header;
    struct stat st;
    storage_lock();
    int fd = !cancelled(d) && storage_file_recover(path) ? open(path, O_RDONLY) : -1;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && read(fd, &header, sizeof(header)) == sizeof(header) &&
        index_header_valid(d, layout, &header, (uint64_t)st.st_size);
    uint8_t *buffer = ok ? allocate(header.bytes) : NULL;
    if (!buffer) ok = false;
    uint32_t at = 0;
    while (ok && at < header.bytes)
    {
        if (cancelled(d)) { ok = false; break; }
        size_t count = header.bytes - at;
        if (count > DOCUMENT_IO_WINDOW) count = DOCUMENT_IO_WINDOW;
        int n = read(fd, buffer + at, count);
        if (n <= 0) { ok = false; break; }
        at += n;
    }
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (ok) ok = !cancelled(d) && hash_bytes(2166136261u, buffer, header.bytes) == header.checksum;
    storage_unlock();
    if (!ok) { epd_app_free(buffer); return false; }
    *data = buffer; *size = header.bytes;
    return true;
}
