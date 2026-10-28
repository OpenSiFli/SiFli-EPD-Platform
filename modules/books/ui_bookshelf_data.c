#include "app_service.h"
#include "epd_memory.h"
#include "ui_bookshelf_data.h"
#include "storage.h"
#include "storage_file.h"
#include "platform/epd_app.h"
#include "document/document.h"
#include "file_version.h"
#include <rtdevice.h>
#include <rthw.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#define HISTORY_MAGIC 0x45505232u
#define HISTORY_LEGACY_MAGIC 0x45505231u

typedef struct
{
    uint32_t magic, sequence, size, modified;
    char path[BOOKSHELF_PATH_MAX];
    struct { uint32_t offset, current_page, total_pages, layout, progress, last_read; } position;
    uint32_t checksum;
} legacy_history_t;

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
static bool storage_ready;

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

static struct
{
    struct rt_workqueue *queue;
    struct rt_work work;
    history_t pending;
    char path[64];
    unsigned index;
    bool dirty, suspended;
} writer;

/* Only the snapshot crosses threads; the worker never reads UI book objects. */
static bool write_pending(void)
{
    if (!storage_app_available("books")) return false;
    history_t record;
    char path[64];
    unsigned index, slot;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!writer.dirty) { rt_hw_interrupt_enable(level); return true; }
    record = writer.pending;
    memcpy(path, writer.path, sizeof(path));
    index = writer.index;
    slot = saved_slots[index] ^ 1u;
    rt_hw_interrupt_enable(level);
    path[strlen(path) - 1] = slot ? 'b' : 'a';
    bool ok = storage_file_replace(path, &record, sizeof(record));
    level = rt_hw_interrupt_disable();
    if (ok)
    {
        saved_slots[index] = slot;
        if (writer.pending.sequence == record.sequence) writer.dirty = false;
    }
    rt_hw_interrupt_enable(level);
    return ok;
}

static void history_work(struct rt_work *work, void *parameter)
{
    (void)parameter;
    bool ok = write_pending();
    bool available = storage_app_available("books");
    rt_base_t level = rt_hw_interrupt_disable();
    if (writer.dirty && !writer.suspended && available)
        rt_workqueue_submit_work(writer.queue, work, rt_tick_from_millisecond(ok ? 100 : 1000));
    rt_hw_interrupt_enable(level);
}

bool ui_bookshelf_flush(void)
{
    if (!writer.queue) return !writer.dirty;
    rt_base_t level = rt_hw_interrupt_disable();
    writer.suspended = true;
    rt_hw_interrupt_enable(level);
    rt_workqueue_cancel_work_sync(writer.queue, &writer.work);
    /* A previously running callback may also have had a delayed retry queued. */
    rt_workqueue_cancel_work(writer.queue, &writer.work);
    bool ok = write_pending();
    bool available = storage_app_available("books");
    level = rt_hw_interrupt_disable();
    writer.suspended = false;
    if (writer.dirty && available)
        rt_workqueue_submit_work(writer.queue, &writer.work, rt_tick_from_millisecond(1000));
    rt_hw_interrupt_enable(level);
    return ok;
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
    bool legacy = false;
    if (fd < 0 && storage_app_available("books") && !strcmp(storage_path_root(book->file.path), STORAGE_SD_ROOT) &&
        storage_available(STORAGE_SD))
    {
        const char *relative = storage_relative_path(book->file.path);
        snprintf(path, sizeof(path), STORAGE_SD_ROOT "/.epd_reader/%08lx.%s",
                 (unsigned long)hash(relative, strlen(relative)), slot);
        fd = open(path, O_RDONLY);
        if (failed && fd < 0 && errno != ENOENT && errno != -ENOENT) *failed = true;
        legacy = true;
    }
    if (fd < 0) { storage_unlock(); return false; }
    int n = read(fd, record, sizeof(*record));
    if (close(fd) != 0 || n < 0) { if (failed) *failed = true; }
    bool integrity = n == sizeof(*record) && record->magic == HISTORY_MAGIC &&
                     record->checksum == hash(record, offsetof(history_t, checksum));
    if (n == sizeof(legacy_history_t) && record->magic == HISTORY_LEGACY_MAGIC && book->file.format == 0)
    {
        legacy_history_t old;
        memcpy(&old, record, sizeof(old));
        integrity = old.checksum == hash(&old, offsetof(legacy_history_t, checksum));
        memset(record, 0, sizeof(*record));
        record->magic = HISTORY_MAGIC; record->sequence = old.sequence;
        record->size = old.size; record->modified = old.modified;
        memcpy(record->path, old.path, sizeof(record->path));
        memcpy(&record->position, &old.position, sizeof(old.position));
        record->checksum = hash(record, offsetof(history_t, checksum));
        legacy = true;
    }
    bool ok = integrity &&
           record->size == book->file.size && record->modified == book->file.modified &&
           memchr(record->path, 0, sizeof(record->path)) &&
           !strcmp(storage_relative_path(record->path), storage_relative_path(book->file.path)) &&
           (book->file.format != 0 || record->position.offset <= record->size) &&
           record->position.progress <= 100 &&
           (book->file.format == 0 || (record->position.document_version == DOCUMENT_CONVERSION_VERSION &&
             record->position.document_kind == book->file.format));
    if (ok && legacy && (!snapshot || !scan_cancelled(snapshot)))
    {
        cache_path(book, path, sizeof(path), slot);
        storage_file_replace(path, record, sizeof(*record));
    }
    storage_unlock();
    return ok;
}

static void format_last_read(ui_bookshelf_book_t *book)
{
    book->last_read_kind = book->position.current_page ? BOOK_LAST_READ_READ : BOOK_LAST_READ_NONE;
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
    if (!writer.queue)
    {
        writer.queue = rt_workqueue_sysq();
        if (!writer.queue) return -RT_ENOMEM;
        rt_work_init(&writer.work, history_work, NULL);
    }
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

bool ui_bookshelf_process(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    /* History completion updates slots by index. Keep those indices stable
     * until the writer has committed, including any retry after a failure. */
    if (scan.stopping || !scan.ready || writer.dirty)
    { rt_hw_interrupt_enable(level); return false; }
    shelf_snapshot_t *snapshot = scan.ready;
    scan.ready = NULL;
    scan.requested = false;
    rt_hw_interrupt_enable(level);
    if (scan_cancelled(snapshot)) { epd_app_free(snapshot); return false; }
    for (unsigned i = 0; i < snapshot->count; ++i)
    {
        ui_bookshelf_book_t *book = &snapshot->books[i];
        for (unsigned j = 0; j <= count; ++j)
        {
            const ui_bookshelf_book_t *previous = j < count ? &books[j] :
                                                 external_valid ? &external_book : NULL;
            if (previous && !strcmp(previous->file.path, book->file.path) &&
                previous->file.size == book->file.size && previous->file.modified == book->file.modified &&
                previous->position.current_page)
            {
                book->position = previous->position;
                book->order = previous->order;
                snapshot->slots[i] = saved_slots[j < count ? j : BOOKSHELF_MAX_ITEMS];
                break;
            }
        }
        format_last_read(book);
    }
    if (scan_cancelled(snapshot)) { epd_app_free(snapshot); return false; }
    epd_app_free(visible_snapshot);
    visible_snapshot = snapshot;
    books = snapshot->books;
    count = snapshot->count;
    memcpy(saved_slots, snapshot->slots, sizeof(snapshot->slots));
    storage_ready = true;
    const ui_bookshelf_book_t *recent = NULL;
    for (unsigned i = 0; i < count; ++i)
    {
        if (books[i].order && (!order || (int32_t)(books[i].order - order) > 0)) order = books[i].order;
        if (books[i].position.current_page && (!recent || (int32_t)(books[i].order - recent->order) > 0))
            recent = &books[i];
    }
    if (recent) epd_app_set_recent_reading(recent->file.name, recent->position.progress);
    return true;
}

void ui_bookshelf_worker_shutdown(void)
{
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
bool ui_bookshelf_storage_ready(void) { return storage_ready; }
const ui_bookshelf_book_t *ui_bookshelf_book(unsigned index)
{
    if (index == BOOKSHELF_MAX_ITEMS) return external_valid ? &external_book : NULL;
    return index < count ? &books[index] : NULL;
}

bool ui_bookshelf_include_path(const char *path, unsigned *index)
{
    if (!path || !index || strlen(path) >= sizeof(external_book.file.path) ||
        !ui_bookshelf_flush()) return false;
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
        ok = file.format != DOCUMENT_UNKNOWN;
    }
    storage_unlock();
    if (ok && !strcmp(storage_path_root(path), STORAGE_FLASH_ROOT))
        ok = bookshelf_file_signature(path, file.size, &file.modified);
    ok = ok && book_file_version_valid(path, &version);
    if (!ok) return false;
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(books[i].file.path, path) && books[i].file.size == file.size &&
            books[i].file.modified == file.modified)
        { *index = i; return true; }
    if (external_valid && !strcmp(external_book.file.path, path) &&
        external_book.file.size == file.size && external_book.file.modified == file.modified)
    { *index = BOOKSHELF_MAX_ITEMS; return true; }
    if (!writer.queue)
    {
        writer.queue = rt_workqueue_sysq();
        if (!writer.queue) return false;
        rt_work_init(&writer.work, history_work, NULL);
    }
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
    memset(&external_book, 0, sizeof(external_book));
    external_book.file = file;
    external_valid = true;
    *index = BOOKSHELF_MAX_ITEMS;
    history_t a, b;
    bool a_ok = load_slot(&external_book, "a", &a, NULL, NULL);
    bool b_ok = load_slot(&external_book, "b", &b, NULL, NULL);
    history_t *latest = a_ok ? &a : NULL;
    if (b_ok && (!latest || (int32_t)(b.sequence - latest->sequence) > 0)) latest = &b;
    saved_slots[*index] = latest == &b ? 1 : 0;
    if (latest)
    {
        external_book.position = latest->position;
        external_book.order = latest->sequence;
        if (!order || (int32_t)(latest->sequence - order) > 0) order = latest->sequence;
    }
    format_last_read(&external_book);
    return true;
}

bool ui_bookshelf_save(unsigned index, const ui_reading_position_t *position)
{
    ui_bookshelf_book_t *book = index == BOOKSHELF_MAX_ITEMS && external_valid ? &external_book :
                              index < count ? &books[index] : NULL;
    if (!book) return false;
    book->position = *position;
    format_last_read(book);
    rt_base_t level = rt_hw_interrupt_disable();
    bool switching = writer.dirty && writer.index != index;
    rt_hw_interrupt_enable(level);
    if (switching && !ui_bookshelf_flush()) return false;
    history_t record;
    memset(&record, 0, sizeof(record));
    record.magic = HISTORY_MAGIC;
    record.sequence = ++order;
    book->order = order;
    if (!storage_app_available("books")) return false;
    record.size = book->file.size;
    record.modified = book->file.modified;
    memcpy(record.path, book->file.path, sizeof(record.path));
    record.position = *position;
    record.checksum = hash(&record, offsetof(history_t, checksum));
    char path[64];
    if (!writer.queue) return false;
    /* Snapshot publication waits for this write before changing indices. */
    ui_bookshelf_cache_path(index, path, sizeof(path), "a");
    level = rt_hw_interrupt_disable();
    writer.pending = record;
    memcpy(writer.path, path, sizeof(path));
    writer.index = index;
    writer.dirty = true;
    rt_workqueue_submit_work(writer.queue, &writer.work, rt_tick_from_millisecond(100));
    rt_hw_interrupt_enable(level);
    return true;
}

void ui_bookshelf_close(void)
{
    /* The application service has joined its worker before UI teardown. */
    ui_bookshelf_worker_shutdown();
    if (!ui_bookshelf_flush()) rt_kprintf("books: history flush failed\n");
    if (writer.queue)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        writer.suspended = true;
        rt_hw_interrupt_enable(level);
        rt_workqueue_cancel_work_sync(writer.queue, &writer.work);
        rt_workqueue_cancel_work(writer.queue, &writer.work);
    }
    memset(&writer, 0, sizeof(writer));
    epd_app_free(visible_snapshot);
    visible_snapshot = NULL;
    books = NULL;
    memset(&external_book, 0, sizeof(external_book));
    external_valid = false;
    count = order = 0;
    storage_ready = false;
    memset(&scan, 0, sizeof(scan));
}
