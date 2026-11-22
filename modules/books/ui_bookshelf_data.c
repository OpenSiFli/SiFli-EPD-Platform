#include "app_service.h"
#include "epd_memory.h"
#include "epd_resource_runtime.h"
#include "ui_bookshelf_data.h"
#include "storage.h"
#include "storage_file.h"
#include "platform/epd_app.h"
#include "document/document.h"
#include "document/document_cache.h"
#include "file_version.h"
#include "cover_cache.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <rtdevice.h>
#include <rthw.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#define HISTORY_MAGIC 0x45505233u

typedef struct
{
    uint32_t magic, sequence, size, modified;
    char path[BOOKSHELF_PATH_MAX];
    ui_reading_position_t position;
    uint32_t checksum;
} history_t;

static ui_bookshelf_book_t *books;
static uint32_t order;
static unsigned char saved_slots[BOOKSHELF_MAX_ITEMS + 1];
static ui_bookshelf_book_t external_book;
static bool external_valid;
static unsigned count;
static uint32_t shelf_generation;
static epd_resource_cache_t shelf_resource;
static size_t shelf_cover_bytes;
static bool shelf_resource_changed;
static int shelf_resource_shrink(void *, uint32_t, size_t, size_t *);
static void shelf_resource_publish(void)
{
    if (shelf_cover_bytes) epd_resource_cache_open(&shelf_resource, EPD_RESOURCE_UI, shelf_resource_shrink, NULL);
    epd_resource_cache_update_region(&shelf_resource, EPD_MEMORY_REGION_HCPU_PSRAM, shelf_cover_bytes,
        epd_app_refresh_done() ? shelf_cover_bytes : 0, 3, 0);
}
void ui_bookshelf_resources_refresh(void)
{
    shelf_resource_publish();
}
static bool storage_ready;
static book_file_version_t metadata_versions[BOOKSHELF_MAX_ITEMS];
static struct {
    document_t *document;
    unsigned first, count, index;
    book_file_version_t version;
    bool lookup;
} metadata;

void ui_bookshelf_metadata_cancel(void)
{
    if (metadata.document) document_close(metadata.document);
    metadata.document = NULL;
    metadata.lookup = false;
    books_cover_cache_cancel();
    metadata.count = 0;
}

void ui_bookshelf_metadata_request(unsigned first, unsigned visible_count)
{
    if (visible_count > BOOKSHELF_COVER_LIMIT) visible_count = BOOKSHELF_COVER_LIMIT;
    if (metadata.first != first || metadata.count != visible_count)
        ui_bookshelf_metadata_cancel();
    metadata.first = first;
    metadata.count = visible_count;
}

static bool metadata_version_valid(const ui_bookshelf_book_t *book, const book_file_version_t *version)
{
    return book_file_version_valid(book->file.path, version) &&
        (book->file.format != DOCUMENT_SPLIT_EPUB ||
         book_file_version(book->file.path).storage == version->storage);
}

static bool metadata_needed(unsigned index)
{
    const ui_bookshelf_book_t *book = &books[index];
    return (book->file.format == DOCUMENT_EPUB || book->file.format == DOCUMENT_SPLIT_EPUB) &&
        (!book->metadata_checked || book->cover_evicted);
}

bool ui_bookshelf_covers_ready(unsigned first, unsigned length)
{
    if (length > BOOKSHELF_COVER_LIMIT) length = BOOKSHELF_COVER_LIMIT;
    for (unsigned i = first; i < count && i - first < length; ++i)
        if (metadata_needed(i)) return false;
    return true;
}

static bool metadata_process(void)
{
    bool ready = ui_bookshelf_covers_ready(metadata.first, metadata.count);
    if (metadata.document || metadata.lookup) {
        char title[BOOKSHELF_NAME_MAX] = {0};
        document_bitmap_t cover = {0};
        bool extracted = metadata.document != NULL;
        int result = extracted ? document_metadata_take(metadata.document, title, sizeof(title), &cover) :
            books_cover_cache_take(title, sizeof(title), &cover);
        if (!result) return false;
        ui_bookshelf_book_t *book = &books[metadata.index];
        if (metadata.lookup && result < 0) {
            metadata.lookup = false;
            if (metadata_version_valid(book, &metadata.version)) {
                metadata.document = document_metadata_open(&book->file);
                if (metadata.document) return false;
            }
        }
        metadata.lookup = false;
        if (metadata_version_valid(book, &metadata.version)) {
            book->metadata_checked = true;
            book->cover_evicted = false;
            metadata_versions[metadata.index] = metadata.version;
            if (result > 0) {
                if (extracted) books_cover_cache_save(&book->file, &metadata.version, title, &cover);
                if (title[0]) snprintf(book->file.name, sizeof(book->file.name), "%s", title);
                book->cover = cover;
                shelf_cover_bytes += cover.image.data_size;
                memset(&cover, 0, sizeof(cover));
            }
        }
        epd_app_free(cover.memory);
        if (metadata.document) document_close(metadata.document);
        metadata.document = NULL;
    }
    /* One probe in flight. Foreground book conversion preempts this worker. */
    for (unsigned priority = 0; metadata.count && priority < 3; ++priority) {
        if (priority == 2 && metadata.first < BOOKSHELF_COVER_LIMIT) continue;
        unsigned first = priority == 0 ? metadata.first : priority == 1 ?
            metadata.first + BOOKSHELF_COVER_LIMIT : metadata.first - BOOKSHELF_COVER_LIMIT;
        for (unsigned i = first; i < count && i - first < BOOKSHELF_COVER_LIMIT; ++i) {
            if (!metadata_needed(i)) continue;
            metadata.version = book_file_version(books[i].file.path);
            /* A runtime mutation invalidates old pixels before the catalog is rescanned. */
            bool stale = books[i].cover_evicted && !books[i].metadata_checked;
            metadata.lookup = !stale && books_cover_cache_request(&books[i].file, &metadata.version);
            metadata.document = metadata.lookup ? NULL : document_metadata_open(&books[i].file);
            metadata.index = i;
            if (!metadata.document && !metadata.lookup) {
                books[i].metadata_checked = true;
                books[i].cover_evicted = false;
                metadata_versions[i] = metadata.version;
                continue;
            }
            goto queued;
        }
    }
queued:
    return !ready && ui_bookshelf_covers_ready(metadata.first, metadata.count);
}

static void covers_release(ui_bookshelf_book_t *items, unsigned length)
{
    for (unsigned i = 0; i < length; ++i) {
        if (items[i].cover.memory && books && (uintptr_t)&items[i] >= (uintptr_t)books &&
            (uintptr_t)&items[i] < (uintptr_t)(books + count)) shelf_cover_bytes -= items[i].cover.image.data_size;
        if (items[i].cover.memory) lv_image_cache_drop(&items[i].cover.image);
        epd_app_free(items[i].cover.memory);
        memset(&items[i].cover, 0, sizeof(items[i].cover));
    }
}

static bool covers_prune(void)
{
    bool changed = false;
    unsigned first = metadata.first >= BOOKSHELF_COVER_LIMIT ?
        metadata.first - BOOKSHELF_COVER_LIMIT : 0;
    unsigned end = metadata.first + 2u * BOOKSHELF_COVER_LIMIT;
    for (unsigned i = 0; i < count; ++i) {
        bool valid = !books[i].metadata_checked ||
            metadata_version_valid(&books[i], &metadata_versions[i]);
        if (!valid) { books[i].metadata_checked = false; books[i].cover_evicted = true; changed = true; }
        if (!books[i].cover.memory || (valid && i >= first && i < end)) continue;
        covers_release(&books[i], 1);
        books[i].cover_evicted = true;
        changed = true;
    }
    return changed;
}

typedef struct
{
    ui_bookshelf_book_t books[BOOKSHELF_MAX_ITEMS];
    unsigned char slots[BOOKSHELF_MAX_ITEMS];
    unsigned count;
    uint32_t revision, session, request;
} shelf_snapshot_t;

static shelf_snapshot_t *visible_snapshot;
static struct
{
    epd_service_t *service;
    shelf_snapshot_t *ready;
    uint32_t revision, session, request;
    bool requested, pending, stopping;
    int error;
} scan;
static bool scan_cancelled(void *user);

/* One latest record per path, with room for the shelf and two external books.
 * No save-time allocation and no UI-owned pointer crosses into the worker. */
#define HISTORY_PENDING_MAX (BOOKSHELF_MAX_ITEMS + 2u)
#define HISTORY_COALESCE_MS 2000u
typedef struct {
    history_t record;
    char path[64];
    uint32_t generation, ticket, deadline, session;
    unsigned index, slot;
    bool used, failed, blocked, retried;
} history_pending_t;
static struct {
    history_pending_t pending[HISTORY_PENDING_MAX];
    uint32_t generations[BOOKSHELF_MAX_ITEMS + 1], ticket;
    unsigned count;
    bool active, stopping;
} writer;

static history_pending_t *history_find(const char *path)
{
    for (unsigned i = 0; i < HISTORY_PENDING_MAX; ++i)
        if (writer.pending[i].used && !strcmp(writer.pending[i].record.path, path))
            return &writer.pending[i];
    return NULL;
}

bool ui_bookshelf_save_failed(unsigned index)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book(index);
    rt_base_t level = rt_hw_interrupt_disable();
    history_pending_t *pending = book ? history_find(book->file.path) : NULL;
    bool failed = pending && pending->failed;
    rt_hw_interrupt_enable(level);
    return failed;
}

bool ui_bookshelf_history_failed(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool failed = false;
    for (unsigned i = 0; i < HISTORY_PENDING_MAX; ++i)
        failed |= writer.pending[i].used && writer.pending[i].failed;
    rt_hw_interrupt_enable(level);
    return failed;
}

void ui_bookshelf_history_retry(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!writer.stopping) {
        for (unsigned i = 0; i < HISTORY_PENDING_MAX; ++i)
            if (writer.pending[i].used) {
                writer.pending[i].blocked = writer.pending[i].retried = false;
                writer.pending[i].deadline = (uint32_t)rt_tick_get();
            }
        if (writer.count) epd_service_wake(scan.service);
    }
    rt_hw_interrupt_enable(level);
}

rt_int32_t ui_bookshelf_worker_delay(void)
{
    uint32_t now = (uint32_t)rt_tick_get();
    rt_int32_t delay = RT_WAITING_FOREVER;
    rt_base_t level = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < HISTORY_PENDING_MAX; ++i) {
        history_pending_t *p = &writer.pending[i];
        if (!p->used || p->blocked) continue;
        int32_t remaining = (int32_t)(p->deadline - now);
        if (remaining < 0) remaining = 0;
        if (delay == RT_WAITING_FOREVER || remaining < delay) delay = remaining;
    }
    rt_hw_interrupt_enable(level);
    return delay;
}

/* Worker only. A local copy permits the UI to coalesce a newer position while
 * this write is in progress. Completion must not acknowledge that newer save. */
static bool history_write(epd_service_t *service, history_pending_t *selected)
{
    uint32_t now = (uint32_t)rt_tick_get();
    rt_base_t level = rt_hw_interrupt_disable();
    history_pending_t *pending = selected;
    for (unsigned i = 0; !selected && i < HISTORY_PENDING_MAX; ++i) {
        history_pending_t *p = &writer.pending[i];
        if (!p->used || p->blocked || (int32_t)(p->deadline - now) > 0) continue;
        if (!pending || (int32_t)(p->ticket - pending->ticket) < 0) pending = p;
    }
    if (!pending) { rt_hw_interrupt_enable(level); return false; }
    history_pending_t copy = *pending;
    writer.active = true;
    rt_hw_interrupt_enable(level);
    unsigned slot = copy.slot ^ 1u;
    copy.path[strlen(copy.path) - 1] = slot ? 'b' : 'a';
    storage_volume_t volume = !strcmp(storage_path_root(copy.path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    bool available = storage_session_valid(volume, copy.session) && storage_app_available("books");
    errno = 0;
    bool ok = available && storage_file_replace(copy.path, &copy.record, sizeof(copy.record));
    int error = errno;
    level = rt_hw_interrupt_disable();
    /* DFS also reports ENOMEM when its file descriptors are temporarily busy. */
    bool retry = !ok && available && !selected && !writer.stopping && !copy.retried &&
        (error == -ENOMEM || error == ENOMEM || error == -EBUSY || error == EBUSY);
    bool report = !ok && !retry && !pending->failed;
    writer.active = false;
    if (ok) {
        pending->slot = slot;
        if (pending->generation == writer.generations[pending->index])
            saved_slots[pending->index] = slot;
        if (pending->record.sequence == copy.record.sequence) {
            memset(pending, 0, sizeof(*pending));
            --writer.count;
        } else pending->ticket = ++writer.ticket;
    } else if (retry) {
        pending->retried = true;
        pending->deadline = (uint32_t)rt_tick_get() + rt_tick_from_millisecond(100);
    } else {
        pending->failed = true;
        pending->blocked = true;
    }
    rt_hw_interrupt_enable(level);
    if (report) rt_kprintf("books: history save failed: %s available=%u errno=%d\n",
                           copy.record.path, (unsigned)available, error);
    epd_service_ui_request(service, 0);
    return true;
}

bool ui_bookshelf_flush(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool done = !writer.active;
    bool wake = false;
    uint32_t now = (uint32_t)rt_tick_get();
    for (unsigned i = 0; i < HISTORY_PENDING_MAX; ++i)
        if (writer.pending[i].used && !writer.pending[i].blocked) {
            writer.pending[i].deadline = now;
            done = false;
            wake = true;
        }
    if (wake) epd_service_wake(scan.service);
    rt_hw_interrupt_enable(level);
    return done;
}

static uint32_t hash(const void *data, size_t length)
{
    const unsigned char *p = data;
    uint32_t h = 2166136261u;
    while (length--) h = (h ^ *p++) * 16777619u;
    return h;
}

static void cache_path(const ui_bookshelf_book_t *book, char *out, size_t size, const char *suffix)
{
    if (!book) { if (size) out[0] = 0; return; }
    const char *path = book->file.path;
    const char *relative = storage_relative_path(path);
    char relative_name[32];
    snprintf(relative_name, sizeof(relative_name), "%s-%08lx.%s",
             !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? "sd" : "flash",
             (unsigned long)hash(relative, strlen(relative)), suffix);
    storage_app_path(out, size, "books", !strcmp(suffix, "idx") ? STORAGE_APP_CACHE : STORAGE_APP_DATA,
                     relative_name);
}

void ui_bookshelf_cache_path(unsigned index, char *out, size_t size, const char *suffix)
{
    cache_path(ui_bookshelf_book(index), out, size, suffix);
}

static bool load_slot(const ui_bookshelf_book_t *book, const char *slot, history_t *record,
                      bool *failed, shelf_snapshot_t *snapshot)
{
    if (!book) return false;
    char path[64];
    cache_path(book, path, sizeof(path), slot);
    storage_lock();
    if (snapshot && scan_cancelled(snapshot))
    {
        if (failed) *failed = true;
        storage_unlock();
        return false;
    }
    bool recovered = storage_file_recover(path);
    int fd = recovered ? open(path, O_RDONLY) : -1;
    if (failed && (!recovered || (fd < 0 && errno != ENOENT && errno != -ENOENT))) *failed = true;
    if (fd < 0) { storage_unlock(); return false; }
    int n = read(fd, record, sizeof(*record));
    if (close(fd) != 0 || n < 0) { if (failed) *failed = true; }
    bool integrity = n == sizeof(*record) && record->magic == HISTORY_MAGIC &&
                     record->checksum == hash(record, offsetof(history_t, checksum));
    bool ok = integrity &&
           record->size == book->file.size && record->modified == book->file.modified &&
           memchr(record->path, 0, sizeof(record->path)) &&
           !strcmp(storage_relative_path(record->path), storage_relative_path(book->file.path)) &&
           record->position.progress <= 100 &&
           document_history_version(record->position.document_version) &&
           record->position.document_kind == book->file.format &&
           (!record->position.source.unit ||
            (book->file.format == BOOK_FORMAT_TXT && record->position.source.unit == 1 &&
             record->position.source.offset <= book->file.size) ||
            ((book->file.format == BOOK_FORMAT_EPUB || book->file.format == BOOK_FORMAT_SPLIT_EPUB) &&
             record->position.source.unit == 2) ||
            ((book->file.format == BOOK_FORMAT_MARKDOWN || book->file.format == BOOK_FORMAT_MOBI) &&
             record->position.source.unit == DOCUMENT_SOURCE_SPOOL && !record->position.source.chapter &&
             record->position.source.offset <= INT32_MAX &&
             record->position.source.skip_utf8 <= DOCUMENT_TEXT_LIMIT));
    storage_unlock();
    return ok;
}

static bool has_reading_position(const ui_bookshelf_book_t *book)
{
    return document_history_version(book->position.document_version) &&
           book->position.document_kind == book->file.format;
}

static void format_last_read(ui_bookshelf_book_t *book)
{
    book->last_read_kind = has_reading_position(book) ? BOOK_LAST_READ_READ : BOOK_LAST_READ_NONE;
    book->last_read[0] = 0;
    time_t stamp = book->position.last_read, now = time(NULL);
    struct tm then, today;
    if (stamp < 1704067200 || !localtime_r(&stamp, &then)) return;
    if (localtime_r(&now, &today) && then.tm_year == today.tm_year && then.tm_yday == today.tm_yday)
        book->last_read_kind = BOOK_LAST_READ_TODAY;
    else
    {
        book->last_read_kind = BOOK_LAST_READ_DATE;
        strftime(book->last_read, sizeof(book->last_read), "%Y-%m-%d", &then);
    }
}

int ui_bookshelf_refresh(void)
{
    if (storage_changing()) return -RT_EIO;
    uint32_t revision = storage_revision(), session = storage_card_session();
    rt_base_t level = rt_hw_interrupt_disable();
    if (scan.stopping) { rt_hw_interrupt_enable(level); return -RT_EIO; }
    if (scan.requested && scan.revision == revision && scan.session == session)
    {
        rt_hw_interrupt_enable(level);
        return RT_EOK;
    }
    shelf_snapshot_t *discard = scan.ready;
    scan.ready = NULL;
    scan.revision = revision;
    scan.session = session;
    ++scan.request;
    scan.error = 0;
    scan.requested = scan.pending = true;
    epd_service_wake(scan.service);
    rt_hw_interrupt_enable(level);
    epd_app_free(discard);
    return RT_EOK;
}

static bool scan_cancelled(void *user)
{
    shelf_snapshot_t *snapshot = user;
    rt_base_t level = rt_hw_interrupt_disable();
    bool stopped = scan.stopping || snapshot->request != scan.request;
    epd_service_t *service = scan.service;
    rt_hw_interrupt_enable(level);
    return stopped || (service && epd_service_cancelled(service)) || storage_changing() ||
           snapshot->revision != storage_revision() || snapshot->session != storage_card_session();
}

bool ui_bookshelf_worker_step(epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    scan.service = service;
    rt_hw_interrupt_enable(level);
    if (history_write(service, NULL)) return true;
    level = rt_hw_interrupt_disable();
    if (scan.stopping || !scan.pending) { rt_hw_interrupt_enable(level); return false; }
    uint32_t revision = scan.revision, session = scan.session, request = scan.request;
    scan.pending = false;
    rt_hw_interrupt_enable(level);

    shelf_snapshot_t *snapshot = epd_app_alloc(sizeof(*snapshot), EPD_APP_PSRAM);
    bookshelf_item_t *files = epd_app_alloc(sizeof(*files) * BOOKSHELF_MAX_ITEMS, EPD_APP_PSRAM);
    int result = -RT_ENOMEM;
    if (!snapshot || !files) goto done;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->revision = revision;
    snapshot->session = session;
    snapshot->request = request;
    result = bookshelf_scan(files, scan_cancelled, snapshot);
    if (result < 0) goto done;
    snapshot->count = (unsigned)result;
    for (unsigned i = 0; i < snapshot->count; ++i)
    {
        if (scan_cancelled(snapshot)) { result = -RT_EIO; goto done; }
        ui_bookshelf_book_t *book = &snapshot->books[i];
        book->file = files[i];
        history_t a, b;
        bool failed = false;
        bool a_ok = load_slot(book, "a", &a, &failed, snapshot);
        if (scan_cancelled(snapshot)) { result = -RT_EIO; goto done; }
        bool b_ok = load_slot(book, "b", &b, &failed, snapshot);
        if (failed) { result = -RT_EIO; goto done; }
        history_t *latest = a_ok ? &a : NULL;
        if (b_ok && (!latest || (int32_t)(b.sequence - latest->sequence) > 0)) latest = &b;
        snapshot->slots[i] = latest == &b ? 1 : 0;
        if (latest) { book->order = latest->sequence; book->position = latest->position; }
        format_last_read(book);
    }
    if (scan_cancelled(snapshot)) { result = -RT_EIO; goto done; }
    level = rt_hw_interrupt_disable();
    if (!scan.stopping && request == scan.request)
    {
        scan.ready = snapshot;
        snapshot = NULL;
    }
    rt_hw_interrupt_enable(level);
done:
    epd_app_free(files);
    epd_app_free(snapshot);
    if (result < 0)
    {
        bool cancelled = epd_service_cancelled(service) || storage_changing() ||
                         revision != storage_revision() || session != storage_card_session();
        level = rt_hw_interrupt_disable();
        if (request == scan.request)
        {
            scan.requested = false;
            if (!cancelled && !scan.stopping) scan.error = result;
        }
        rt_hw_interrupt_enable(level);
        rt_kprintf("books: shelf scan failed (%d)\n", result);
    }
    epd_service_ui_request(service, 0);
    return true;
}

int ui_bookshelf_take_error(void)
{
    uint32_t revision = storage_revision(), session = storage_card_session();
    bool changing = storage_changing();
    rt_base_t level = rt_hw_interrupt_disable();
    int error = !scan.stopping && !changing && scan.revision == revision && scan.session == session ? scan.error : 0;
    scan.error = 0;
    rt_hw_interrupt_enable(level);
    return error;
}

static bool process_scan(void)
{
    book_file_version_t versions[BOOKSHELF_MAX_ITEMS] = {0};
    rt_base_t level = rt_hw_interrupt_disable();
    /* Keep history indices stable until every accepted record is committed. */
    if (scan.stopping || !scan.ready || writer.count || writer.active)
    { rt_hw_interrupt_enable(level); return false; }
    shelf_snapshot_t *snapshot = scan.ready;
    scan.ready = NULL;
    scan.requested = false;
    rt_hw_interrupt_enable(level);
    if (scan_cancelled(snapshot)) { epd_app_free(snapshot); return false; }
    unsigned first = metadata.first, visible_count = metadata.count;
    ui_bookshelf_metadata_cancel();
    metadata.first = first; metadata.count = visible_count;
    for (unsigned i = 0; i < snapshot->count; ++i)
    {
        ui_bookshelf_book_t *book = &snapshot->books[i];
        for (unsigned j = 0; j <= count; ++j)
        {
            ui_bookshelf_book_t *previous = j < count ? &books[j] :
                                                 external_valid ? &external_book : NULL;
            if (previous && !strcmp(previous->file.path, book->file.path) &&
                previous->file.size == book->file.size && previous->file.modified == book->file.modified)
            {
                if (has_reading_position(previous)) {
                    book->position = previous->position;
                    book->order = previous->order;
                    snapshot->slots[i] = saved_slots[j < count ? j : BOOKSHELF_MAX_ITEMS];
                }
                if (j < count && previous->metadata_checked &&
                    metadata_version_valid(book, &metadata_versions[j])) {
                    snprintf(book->file.name, sizeof(book->file.name), "%s", previous->file.name);
                    book->metadata_checked = true;
                    book->cover_evicted = previous->cover_evicted;
                    versions[i] = metadata_versions[j];
                }
                break;
            }
        }
        format_last_read(book);
    }
    if (scan_cancelled(snapshot)) { covers_release(snapshot->books, snapshot->count); epd_app_free(snapshot); return false; }
    for (unsigned i = 0; i < snapshot->count; ++i)
        for (unsigned j = 0; j < count; ++j)
            if (snapshot->books[i].metadata_checked && !strcmp(snapshot->books[i].file.path, books[j].file.path)) {
                if (books[j].cover.memory) lv_image_cache_drop(&books[j].cover.image);
                snapshot->books[i].cover = books[j].cover;
                memset(&books[j].cover, 0, sizeof(books[j].cover));
                break;
            }
    covers_release(books, count);
    epd_app_free(visible_snapshot);
    visible_snapshot = snapshot;
    books = snapshot->books;
    count = snapshot->count;
    memcpy(metadata_versions, versions, sizeof(versions));
    memcpy(saved_slots, snapshot->slots, sizeof(snapshot->slots));
    storage_ready = true;
    const ui_bookshelf_book_t *recent = NULL;
    for (unsigned i = 0; i < count; ++i)
    {
        if (books[i].order && (!order || (int32_t)(books[i].order - order) > 0)) order = books[i].order;
        if (has_reading_position(&books[i]) && (!recent || (int32_t)(books[i].order - recent->order) > 0))
            recent = &books[i];
    }
    if (recent) epd_app_set_recent_reading(recent->file.name, recent->position.progress);
    ++shelf_generation;
    return true;
}

static int shelf_resource_shrink(void *context, uint32_t backend, size_t requested, size_t *released)
{
    (void)context; (void)backend; *released = 0;
    if (!epd_app_refresh_done()) { shelf_resource_publish(); return EPD_RESOURCE_BUSY; }
    for (unsigned i = 0; i < count && *released < requested; ++i) {
        if (!books[i].cover.memory) continue;
        *released += books[i].cover.image.data_size;
        covers_release(&books[i], 1); books[i].cover_evicted = true; shelf_resource_changed = true;
    }
    shelf_resource_publish();
    if (*released) epd_service_ui_request(scan.service, 0);
    return EPD_RESOURCE_OK;
}

bool ui_bookshelf_process(void)
{
    shelf_resource_publish();
    if (!epd_app_refresh_done()) return false;
    bool changed = shelf_resource_changed; shelf_resource_changed = false;
    changed |= process_scan();
    changed |= covers_prune();
    if (!storage_changing()) changed |= metadata_process();
    shelf_resource_publish();
    return changed;
}

void ui_bookshelf_worker_shutdown(void)
{
    rt_base_t guard = rt_hw_interrupt_disable();
    bool final = !writer.stopping;
    writer.stopping = true;
    rt_hw_interrupt_enable(guard);
    if (final) {
        /* Cancellation drains unattempted work, never retries a terminal error. */
        for (unsigned i = 0; i < HISTORY_PENDING_MAX; ++i)
            if (writer.pending[i].used && !writer.pending[i].blocked)
                history_write(scan.service, &writer.pending[i]);
    }
    ui_bookshelf_metadata_cancel();
    books_cover_cache_shutdown();
    rt_base_t level = rt_hw_interrupt_disable();
    scan.stopping = true;
    scan.pending = false;
    scan.service = NULL;
    shelf_snapshot_t *discard = scan.ready;
    scan.ready = NULL;
    rt_hw_interrupt_enable(level);
    epd_app_free(discard);
}

unsigned ui_bookshelf_count(void) { return count; }
uint32_t ui_bookshelf_generation(void) { return shelf_generation; }
bool ui_bookshelf_storage_ready(void) { return storage_ready; }
const ui_bookshelf_book_t *ui_bookshelf_book(unsigned index)
{
    if (index == BOOKSHELF_MAX_ITEMS) return external_valid ? &external_book : NULL;
    return index < count ? &books[index] : NULL;
}

bool ui_bookshelf_include_path(const char *path, unsigned *index)
{
    if (!path || !index || strlen(path) >= sizeof(external_book.file.path)) return false;
    bookshelf_item_t file = {0};
    storage_lock();
    book_file_version_t version = book_file_version(path);
    struct stat st = {0};
    bool ok = !storage_changing() && storage_path_available(path) &&
              stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
              (uint64_t)st.st_size <= UINT32_MAX;
    if (ok)
    {
        file.format = document_format(path, false);
        file.size = (uint32_t)st.st_size;
        file.modified = (uint32_t)st.st_mtime;
        ok = file.format != (uint32_t)DOCUMENT_UNKNOWN;
    }
    storage_unlock();
    if (ok && !strcmp(storage_path_root(path), STORAGE_FLASH_ROOT))
        ok = document_cache_file_signature(path, &file.modified);
    ok = ok && book_file_version_valid(path, &version);
    if (!ok) return false;
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(books[i].file.path, path) && books[i].file.size == file.size &&
            books[i].file.modified == file.modified)
        { *index = i; return true; }
    if (external_valid && !strcmp(external_book.file.path, path) &&
        external_book.file.size == file.size && external_book.file.modified == file.modified)
    { *index = BOOKSHELF_MAX_ITEMS; return true; }
    /* Reserve shelf capacity even when external files are opened rapidly.
     * Refuse a third unsaved external identity without replacing the old slot. */
    rt_base_t level = rt_hw_interrupt_disable();
    unsigned external_pending = 0;
    for (unsigned i = 0; i < HISTORY_PENDING_MAX; ++i) {
        history_pending_t *p = &writer.pending[i];
        if (p->used && p->index == BOOKSHELF_MAX_ITEMS) ++external_pending;
    }
    bool room = !writer.stopping && (history_find(path) || external_pending < 2);
    rt_hw_interrupt_enable(level);
    if (!room) { rt_kprintf("books: history queue full; external open deferred\n"); return false; }
    memcpy(file.path, path, strlen(path) + 1);
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *dot = strrchr(name, '.');
    size_t length = dot ? (size_t)(dot - name) : strlen(name);
    if (length >= sizeof(file.name))
    {
        length = sizeof(file.name) - 1;
        while (length && ((unsigned char)name[length] & 0xc0) == 0x80) --length;
    }
    memcpy(file.name, name, length);
    level = rt_hw_interrupt_disable();
    ++writer.generations[BOOKSHELF_MAX_ITEMS];
    rt_hw_interrupt_enable(level);
    memset(&external_book, 0, sizeof(external_book));
    external_book.file = file;
    external_valid = true;
    *index = BOOKSHELF_MAX_ITEMS;
    level = rt_hw_interrupt_disable();
    history_pending_t *pending = history_find(path);
    if (pending) {
        pending->index = *index;
        pending->generation = writer.generations[*index];
        saved_slots[*index] = pending->slot;
        if (pending->record.size == file.size && pending->record.modified == file.modified &&
            pending->record.position.document_kind == file.format) {
            external_book.position = pending->record.position;
            external_book.order = pending->record.sequence;
        }
    }
    rt_hw_interrupt_enable(level);
    if (!pending) {
        history_t a, b;
        bool a_ok = load_slot(&external_book, "a", &a, NULL, NULL);
        bool b_ok = load_slot(&external_book, "b", &b, NULL, NULL);
        history_t *latest = a_ok ? &a : NULL;
        if (b_ok && (!latest || (int32_t)(b.sequence - latest->sequence) > 0)) latest = &b;
        saved_slots[*index] = latest == &b ? 1 : 0;
        if (latest) {
            external_book.position = latest->position;
            external_book.order = latest->sequence;
            if (!order || (int32_t)(latest->sequence - order) > 0) order = latest->sequence;
        }
    }
    format_last_read(&external_book);
    return true;
}

bool ui_bookshelf_save(unsigned index, const ui_reading_position_t *position)
{
    ui_bookshelf_book_t *book = index == BOOKSHELF_MAX_ITEMS && external_valid ? &external_book :
                              index < count ? &books[index] : NULL;
    if (!book) return false;
    history_t record = {0};
    record.magic = HISTORY_MAGIC;
    record.sequence = ++order;
    record.size = book->file.size;
    record.modified = book->file.modified;
    memcpy(record.path, book->file.path, sizeof(record.path));
    record.position = *position;
    record.checksum = hash(&record, offsetof(history_t, checksum));
    char path[64];
    ui_bookshelf_cache_path(index, path, sizeof(path), "a");
    if (!path[0]) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    history_pending_t *pending = history_find(record.path);
    if (!pending)
        for (unsigned i = 0; i < HISTORY_PENDING_MAX; ++i)
            if (!writer.pending[i].used) { pending = &writer.pending[i]; break; }
    if (!pending || writer.stopping) { rt_hw_interrupt_enable(level); return false; }
    if (!pending->used) {
        memset(pending, 0, sizeof(*pending));
        pending->used = true;
        pending->ticket = ++writer.ticket;
        pending->slot = saved_slots[index];
        pending->session = storage_card_session();
        pending->deadline = (uint32_t)rt_tick_get() + rt_tick_from_millisecond(HISTORY_COALESCE_MS);
        ++writer.count;
    } else if (pending->blocked &&
               memcmp(&pending->record.position.source, &position->source, sizeof(position->source))) {
        /* A newly committed reading position can retry a previously failed
         * write. Keep the bounded policy for repeated attempts at one anchor. */
        pending->blocked = pending->retried = false;
        pending->deadline = (uint32_t)rt_tick_get() + rt_tick_from_millisecond(HISTORY_COALESCE_MS);
    }
    pending->record = record;
    memcpy(pending->path, path, sizeof(path));
    pending->index = index;
    pending->generation = writer.generations[index];
    epd_service_wake(scan.service);
    rt_hw_interrupt_enable(level);
    book->position = *position;
    book->order = record.sequence;
    format_last_read(book);
    return true;
}

void ui_bookshelf_save_soon(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t now = (uint32_t)rt_tick_get();
    for (unsigned i = 0; i < HISTORY_PENDING_MAX; ++i)
        if (writer.pending[i].used && !writer.pending[i].blocked) writer.pending[i].deadline = now;
    epd_service_wake(scan.service);
    rt_hw_interrupt_enable(level);
}

void ui_bookshelf_close(void)
{
    ui_bookshelf_metadata_cancel();
    /* The application service has joined its worker before UI teardown. */
    if (writer.count) rt_kprintf("books: history not persisted on close: %u records\n", writer.count);
    memset(&writer, 0, sizeof(writer));
    covers_release(books, count);
    epd_app_free(visible_snapshot);
    visible_snapshot = NULL;
    books = NULL;
    memset(&external_book, 0, sizeof(external_book));
    external_valid = false;
    count = order = 0;
    shelf_resource_publish(); epd_resource_cache_close(&shelf_resource);
    storage_ready = false;
    epd_app_free(scan.ready);
    memset(&scan, 0, sizeof(scan));
    memset(&metadata, 0, sizeof(metadata));
    memset(metadata_versions, 0, sizeof(metadata_versions));
    ++shelf_generation;
}
