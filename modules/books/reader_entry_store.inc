/* SPDX-License-Identifier: Apache-2.0 */
#define ENTRY_STORE_MAGIC 0x45504652u
#define ENTRY_STORE_VERSION 2u
#define ENTRY_DISK_SLOTS 8u
#define ENTRY_IO_BYTES 4096u
#define ENTRY_DISK_RESERVE (2u * 1024u * 1024u)
/* Catalog size/mtime (SD) or existing content tag (Flash) identify the source.
 * Runtime tokens guard queued work. Cache validation never scans source files. */
typedef struct {
    char path[BOOKSHELF_PATH_MAX];
    uint32_t size, modified, format, conversion, layout_version;
    document_anchor_t source;
    uint32_t font_id, font_fingerprint;
    unsigned settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    reader_geometry_t area;
} entry_store_key_t;
typedef struct {
    uint32_t magic, version, bytes, checksum;
    entry_store_key_t key;
    uint32_t width, height, stride, page, pages, percent, margin, line_space;
} entry_store_header_t;
typedef enum { ENTRY_OPEN, ENTRY_HEADER, ENTRY_PIXELS, ENTRY_SEAL, ENTRY_SYNC,
               ENTRY_CLOSE, ENTRY_COMMIT, ENTRY_ABORT } entry_phase_t;
typedef struct {
    entry_store_header_t header;
    entry_store_key_t key;
    book_file_version_t source, cached;
    entry_pixels_t *pixels;
    char path[STORAGE_PATH_MAX], temporary[STORAGE_PATH_MAX];
    size_t offset;
    uint32_t serial, epoch, session, checksum;
    storage_volume_t volume;
    entry_phase_t phase;
    int fd;
    bool writing, temporary_owned;
} entry_store_job_t;
static struct {
    epd_service_t *service;
    entry_store_job_t *pending, *job;
    entry_store_key_t keys[ENTRY_SLOTS];
    book_file_version_t versions[ENTRY_SLOTS];
    reader_entry_t *ready;
    uint32_t serial, epoch, cancel_epoch;
    unsigned count, next;
    bool active, cancel_requested, write_allowed;
} entry_store;

static uint32_t entry_checksum(uint32_t value, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    while (bytes--) value = (value ^ *p++) * 16777619u;
    return value;
}
static bool entry_store_path(const entry_store_key_t *key, storage_volume_t volume, char *out, size_t capacity)
{
    unsigned slot = entry_checksum(2166136261u, key->path, strlen(key->path)) % ENTRY_DISK_SLOTS;
    int n = snprintf(out, capacity, "%s/entry-%u.frame",
        volume == STORAGE_SD ? STORAGE_SD_ROOT "/.epd/cache/books/resume" : STORAGE_FLASH_ROOT "/cache/books/resume", slot);
    return n > 0 && (size_t)n < capacity;
}
static void entry_store_key(entry_store_key_t *key, const reader_entry_t *entry)
{
    memset(key, 0, sizeof(*key));
    memcpy(key->path, entry->file.path, sizeof(key->path));
    key->size = entry->file.size; key->modified = entry->file.modified; key->format = entry->file.format;
    key->conversion = DOCUMENT_CONVERSION_VERSION; key->layout_version = EPD_READER_LAYOUT_CACHE_VERSION;
    key->source = entry->source; key->font_id = entry->font_id; key->font_fingerprint = entry->font_fingerprint;
    memcpy(key->settings, entry->settings, sizeof(key->settings)); key->area = entry->area;
}
static void entry_job_free(entry_store_job_t *job)
{
    if (!job) return;
    entry_pixels_release(job->pixels); epd_app_free(job);
}
static void entry_store_allow_write(bool allowed)
{
    allowed = allowed && !entry_cache_constrained();
    rt_base_t level = rt_hw_interrupt_disable();
    if (entry_store.write_allowed && !allowed) ++entry_store.epoch;
    bool changed = allowed != entry_store.write_allowed;
    entry_store.write_allowed = allowed;
    if (changed) epd_service_wake(entry_store.service);
    rt_hw_interrupt_enable(level);
}
static void entry_store_save(const reader_entry_t *entry)
{
    if (!entry->pixels || epd_app_psram_available() < sizeof(entry_store_job_t) + ENTRY_RESERVE) return;
    entry_store_job_t *job = epd_app_alloc(sizeof(*job), EPD_APP_PSRAM);
    if (!job) return;
    memset(job, 0, sizeof(*job)); job->fd = -1; job->writing = true;
    job->source = entry->version; job->pixels = entry->pixels; entry_pixels_ref(job->pixels);
    job->header.magic = ENTRY_STORE_MAGIC; job->header.version = ENTRY_STORE_VERSION;
    job->header.bytes = entry->bitmap.image.data_size;
    entry_store_key(&job->key, entry); job->header.key = job->key;
    job->header.width = entry->bitmap.image.header.w; job->header.height = entry->bitmap.image.header.h;
    job->header.stride = entry->bitmap.image.header.stride;
    job->header.page = entry->view.page; job->header.pages = entry->view.pages; job->header.percent = entry->view.percent;
    job->header.margin = entry->view.margin; job->header.line_space = entry->view.line_space;
    rt_base_t level = rt_hw_interrupt_disable();
    entry_store_job_t *old = entry_store.pending;
    entry_store.pending = job;
    ++entry_store.epoch; /* Only the latest optional save may complete. */
    epd_service_wake(entry_store.service);
    rt_hw_interrupt_enable(level);
    entry_job_free(old);
}

void ui_reader_cache_preload(unsigned first, unsigned count)
{
    if (entry_cache_constrained()) return;
    entry_store_key_t keys[ENTRY_SLOTS] = {0};
    book_file_version_t versions[ENTRY_SLOTS] = {0};
    unsigned length = 0;
    for (unsigned n = 0; n < count && n < ENTRY_SLOTS; ++n) {
        const ui_bookshelf_book_t *book = ui_bookshelf_book(first + n);
        if (!book || book->position.document_version != DOCUMENT_CONVERSION_VERSION ||
            book->position.document_kind != book->file.format || !book->position.source.unit) continue;
        bool resident = false;
        for (unsigned i = 0; i < ENTRY_SLOTS; ++i) if (entry_matches(&entries[i], book)) { resident = true; break; }
        if (resident) continue;
        reader_entry_t entry = {0}; entry.file = book->file; entry.source = book->position.source;
        entry.font_id = epd_app_reading_font_id(); entry.area = reader_geometry();
        if (count == 1 && entry.area.width > 48 && entry.area.bottom > entry.area.top) {
            uint64_t bytes = (uint64_t)(((unsigned)entry.area.width - 48u + 3u) & ~3u) *
                (unsigned)(entry.area.bottom - entry.area.top) + sizeof(entry_pixels_t);
            entry_make_room(bytes < ENTRY_PIXEL_BUDGET ? (size_t)bytes : ENTRY_PIXEL_BUDGET);
        }
        if (!epd_reader_font_fingerprint(entry.font_id, &entry.font_fingerprint)) continue;
        for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
            entry.settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
        entry_store_key(&keys[length], &entry); versions[length++] = book_file_version(book->file.path);
    }
    bool same = entry_store.count == length && !memcmp(entry_store.keys, keys, sizeof(keys));
    for (unsigned i = 0; same && i < length; ++i) {
        const book_file_version_t *old = &entry_store.versions[i];
        same = book_file_version_valid(keys[i].path, old) && old->session == versions[i].session &&
            (keys[i].format != DOCUMENT_SPLIT_EPUB || old->storage == versions[i].storage);
    }
    rt_base_t level = rt_hw_interrupt_disable();
    if (!same) {
        ++entry_store.serial; entry_store.count = length; entry_store.next = 0;
        memcpy(entry_store.keys, keys, sizeof(keys)); memcpy(entry_store.versions, versions, sizeof(versions));
        epd_service_wake(entry_store.service);
    }
    rt_hw_interrupt_enable(level);
}

/* Queued work owns no descriptor and may be discarded immediately. An active
 * transaction remains worker-owned until its fd is returned to resident storage. */
static void entry_store_cancel_queue(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    entry_store_job_t *pending = entry_store.pending; entry_store.pending = NULL;
    reader_entry_t *ready = entry_store.ready; entry_store.ready = NULL;
    bool cancel = pending || ready || entry_store.next < entry_store.count ||
        (entry_store.active && !entry_store.cancel_requested);
    if (cancel) {
        ++entry_store.serial; ++entry_store.epoch; ++entry_store.cancel_epoch;
        if (entry_store.active) entry_store.cancel_requested = true;
    }
    entry_store.count = entry_store.next = 0;
    entry_store.write_allowed = false;
    if (cancel) epd_service_wake(entry_store.service);
    rt_hw_interrupt_enable(level);
    entry_job_free(pending);
    if (ready) { entry_pixels_release(ready->pixels); epd_app_free(ready); }
}
void ui_reader_cache_quiesce(void)
{
    entry_dirty = false;
    entry_store_cancel_queue();
}
bool ui_reader_cache_busy(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool active = entry_store.active;
    rt_hw_interrupt_enable(level); return active;
}
static bool entry_job_source_valid(const entry_store_job_t *job)
{
    return book_file_version_valid(job->key.path, &job->source) &&
        (job->key.format != DOCUMENT_SPLIT_EPUB || book_file_version(job->key.path).storage == job->source.storage);
}
/* Under storage_lock, account only for our own filesystem mutation. */
static void entry_job_own_change(entry_store_job_t *job)
{
    if (job->key.format == DOCUMENT_SPLIT_EPUB)
        job->source.storage = book_file_version(job->key.path).storage;
}
/* Called by the worker with interrupts disabled; UI never mutates an active job. */
static bool entry_read_current(entry_store_job_t *job)
{
    if (job->serial == entry_store.serial) return true;
    const book_file_version_t *version = &entry_store.versions[0];
    if (job->phase == ENTRY_ABORT || job->epoch != entry_store.cancel_epoch ||
        entry_store.count != 1 || entry_store.next ||
        memcmp(&job->key, &entry_store.keys[0], sizeof(job->key)) ||
        job->source.path != version->path || job->source.session != version->session ||
        (job->key.format == DOCUMENT_SPLIT_EPUB && job->source.storage != version->storage)) return false;
    /* The selected book already owns this descriptor and partially read frame. */
    job->serial = entry_store.serial;
    entry_store.next = 1;
    return true;
}
static bool entry_job_cancelled(entry_store_job_t *job, epd_service_t *service)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool cancelled = job->writing ? job->epoch != entry_store.epoch || !entry_store.write_allowed :
        !entry_read_current(job);
    rt_hw_interrupt_enable(level);
    return cancelled || entry_cache_constrained() || (service && epd_service_cancelled(service));
}
static bool entry_job_cleanup(entry_store_job_t *job)
{
    storage_lock();
    if (job->fd >= 0) {
        bool closed = storage_file_owned_close(job->fd);
        job->fd = -1;
        if (!closed) job->temporary_owned = false;
    }
    if (job->temporary_owned && storage_session_valid(job->volume, job->session))
        (void)unlink(job->temporary);
    job->temporary_owned = false;
    storage_unlock(); return true;
}
static bool entry_job_header_valid(const entry_store_job_t *job)
{
    const entry_store_header_t *h = &job->header;
    return h->magic == ENTRY_STORE_MAGIC && h->version == ENTRY_STORE_VERSION &&
        !memcmp(&h->key, &job->key, sizeof(job->key)) && h->bytes && h->bytes <= ENTRY_BYTES_MAX &&
        h->width && h->height && h->stride >= h->width && h->stride <= ENTRY_BYTES_MAX &&
        h->height <= ENTRY_BYTES_MAX / h->stride && h->bytes == h->height * h->stride && h->percent <= 100 &&
        h->width <= (unsigned)job->key.area.width &&
        h->height <= (unsigned)(job->key.area.bottom - job->key.area.top) &&
        h->margin <= (unsigned)job->key.area.width - h->width;
}
static reader_entry_t *entry_job_result(entry_store_job_t *job)
{
    if (epd_app_psram_available() < sizeof(reader_entry_t) + ENTRY_RESERVE) return NULL;
    reader_entry_t *entry = epd_app_alloc(sizeof(*entry), EPD_APP_PSRAM);
    if (!entry) return NULL;
    memset(entry, 0, sizeof(*entry));
    const entry_store_key_t *key = &job->key;
    const entry_store_header_t *h = &job->header;
    memcpy(entry->file.path, key->path, sizeof(entry->file.path));
    entry->file.size = key->size; entry->file.modified = key->modified; entry->file.format = key->format;
    entry->source = key->source; entry->version = job->source;
    entry->font_id = key->font_id; entry->font_fingerprint = key->font_fingerprint;
    memcpy(entry->settings, key->settings, sizeof(entry->settings)); entry->area = key->area;
    entry->view.margin = h->margin; entry->view.top = key->area.top; entry->view.bottom = key->area.bottom;
    entry->view.content_width = h->width; entry->view.page = h->page; entry->view.pages = h->pages;
    entry->view.percent = h->percent; entry->view.line_space = h->line_space;
    entry->view.revision = 1; entry->view.ready = true;
    entry->pixels = job->pixels; job->pixels = NULL;
    entry->bitmap.memory = entry->pixels->data;
    entry->bitmap.image.header.magic = LV_IMAGE_HEADER_MAGIC;
    entry->bitmap.image.header.cf = LV_COLOR_FORMAT_L8;
    entry->bitmap.image.header.w = h->width; entry->bitmap.image.header.h = h->height;
    entry->bitmap.image.header.stride = h->stride;
    entry->bitmap.image.data_size = h->bytes; entry->bitmap.image.data = entry->pixels->data;
    return entry;
}
/* Called with interrupts disabled. Only the selected, still unreadable book
 * may borrow a bounded read slice ahead of document preparation. */
static bool entry_priority_read(const entry_store_job_t *job)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book(opened_book);
    if (!book || preview || document_view_get()->ready || entry_store.ready || entry_store.count != 1)
        return false;
    const entry_store_key_t *key = &entry_store.keys[0];
    const book_file_version_t *version = &entry_store.versions[0];
    if (strcmp(key->path, book->file.path) || key->size != book->file.size ||
        key->modified != book->file.modified || key->format != book->file.format ||
        memcmp(&key->source, &book->position.source, sizeof(key->source)) ||
        version->path != opened_version.path || version->session != opened_version.session ||
        (key->format == DOCUMENT_SPLIT_EPUB && version->storage != opened_version.storage)) return false;
    return job ? !job->writing && job->serial == entry_store.serial &&
        !memcmp(&job->key, key, sizeof(*key)) : entry_store.next < entry_store.count;
}
/* At most one 4 KiB data chunk or one transaction phase per call. Optional
 * reads/writes yield to the reader; cleanup retains its existing priority. */
static bool entry_worker_step(epd_service_t *service, bool reader_worked, bool priority_only)
{
    rt_base_t level = rt_hw_interrupt_disable(); entry_store.service = service;
    entry_store_job_t *job = entry_store.job;
    bool need_load = !job && !entry_store.ready && entry_store.next < entry_store.count;
    uint32_t load_serial = entry_store.serial;
    rt_hw_interrupt_enable(level);
    if (job && entry_job_cancelled(job, service)) job->phase = ENTRY_ABORT;
    if (job && job->phase == ENTRY_ABORT) goto abort;
    if (entry_cache_constrained() || (service && epd_service_cancelled(service))) return false;
    if (priority_only) {
        level = rt_hw_interrupt_disable();
        bool priority = entry_priority_read(job);
        rt_hw_interrupt_enable(level);
        if (!priority) return false;
    } else if (reader_worked) return false;
    if (!job) {
        entry_store_job_t *allocated = need_load && epd_app_psram_available() >= sizeof(*allocated) + ENTRY_RESERVE ?
            epd_app_alloc(sizeof(*allocated), EPD_APP_PSRAM) : NULL;
        level = rt_hw_interrupt_disable();
        if (priority_only && !entry_priority_read(NULL)) {
            /* Selection may change while allocating. Leave the new queue intact. */
        } else if (!priority_only && entry_store.pending && entry_store.write_allowed) {
            job = entry_store.pending; entry_store.pending = NULL; job->epoch = entry_store.epoch;
        } else if (!entry_store.ready && entry_store.next < entry_store.count) {
            job = allocated; allocated = NULL;
            if (job) {
                memset(job, 0, sizeof(*job)); job->fd = -1;
                job->key = entry_store.keys[entry_store.next]; job->source = entry_store.versions[entry_store.next++];
                job->serial = entry_store.serial; job->epoch = entry_store.cancel_epoch;
            } else if (need_load && load_serial == entry_store.serial) {
                /* Only retire a generation whose allocation was considered.
                 * A UI request arriving during the unlocked window is new work. */
                entry_store.next = entry_store.count;
            }
        }
        if (job) { entry_store.job = job; entry_store.active = true; }
        rt_hw_interrupt_enable(level);
        epd_app_free(allocated);
        if (!job) return false;
    }
    if (entry_job_cancelled(job, service) || !entry_job_source_valid(job)) goto abort;
    if (job->phase == ENTRY_OPEN) {
        job->volume = storage_available(STORAGE_SD) ? STORAGE_SD : STORAGE_FLASH;
        job->session = storage_card_session();
        storage_info_t info;
        if (job->writing && (!storage_info(job->volume, &info) || info.used > info.total ||
            info.total - info.used < (uint64_t)job->header.bytes + sizeof(job->header) + ENTRY_DISK_RESERVE)) goto abort;
        if (!entry_store_path(&job->key, job->volume, job->path, sizeof(job->path))) goto abort;
        storage_lock();
        bool ok = entry_job_source_valid(job) && storage_session_valid(job->volume, job->session);
        if (ok && job->writing) {
            char directory[STORAGE_PATH_MAX]; strcpy(directory, job->path); *strrchr(directory, '/') = 0;
            int n = snprintf(job->temporary, sizeof(job->temporary), "%s/staging.tmp", directory);
            ok = n > 0 && n < (int)sizeof(job->temporary) && storage_mkdirs(directory) && storage_file_recover(job->path);
            if (ok) { job->fd = storage_file_owned_open(job->temporary, O_CREAT | O_TRUNC | O_RDWR, true); job->temporary_owned = job->fd >= 0; }
            entry_job_own_change(job);
        } else if (ok) {
            if (storage_file_recover(job->path)) job->fd = storage_file_owned_open(job->path, O_RDONLY, false);
            job->cached = book_file_version(job->path);
        }
        storage_unlock();
        if (!ok || job->fd < 0) goto abort;
        job->phase = ENTRY_HEADER; return true;
    }
    storage_lock();
    bool valid = entry_job_source_valid(job) && storage_session_valid(job->volume, job->session) &&
        (job->writing || book_file_version_valid(job->path, &job->cached));
    if (!valid) { storage_unlock(); goto abort; }
    bool ok = true, done = false;
    switch (job->phase) {
    case ENTRY_HEADER:
        if (job->writing) ok = write(job->fd, &job->header, sizeof(job->header)) == sizeof(job->header);
        else ok = read(job->fd, &job->header, sizeof(job->header)) == sizeof(job->header) && entry_job_header_valid(job);
        if (ok && !job->writing) { job->pixels = entry_pixels_new(job->header.bytes); ok = job->pixels != NULL; }
        if (ok) {
            uint32_t checksum = job->header.checksum; job->header.checksum = 0;
            job->checksum = entry_checksum(2166136261u, &job->header, sizeof(job->header));
            job->header.checksum = checksum; job->phase = ENTRY_PIXELS;
        }
        break;
    case ENTRY_PIXELS: {
        size_t bytes = job->header.bytes - job->offset;
        if (bytes > ENTRY_IO_BYTES) bytes = ENTRY_IO_BYTES;
        uint8_t *pixels = job->pixels->data + job->offset;
        ok = job->writing ? write(job->fd, pixels, bytes) == (int)bytes : read(job->fd, pixels, bytes) == (int)bytes;
        if (ok) { job->checksum = entry_checksum(job->checksum, pixels, bytes); job->offset += bytes; }
        if (ok && job->offset == job->header.bytes) job->phase = ENTRY_SEAL;
        break;
    }
    case ENTRY_SEAL:
        if (job->writing) {
            job->header.checksum = job->checksum;
            ok = lseek(job->fd, 0, SEEK_SET) == 0 && write(job->fd, &job->header, sizeof(job->header)) == sizeof(job->header);
            job->phase = ENTRY_SYNC;
        } else {
            uint8_t extra; ok = read(job->fd, &extra, 1) == 0 && job->checksum == job->header.checksum;
            job->phase = ENTRY_CLOSE;
        }
        break;
    case ENTRY_SYNC: ok = fsync(job->fd) == 0; job->phase = ENTRY_CLOSE; break;
    case ENTRY_CLOSE:
        ok = storage_file_owned_close(job->fd);
        job->fd = -1;
        if (!ok) { job->temporary_owned = false; break; }
        if (job->writing) job->phase = ENTRY_COMMIT; else done = true;
        break;
    case ENTRY_COMMIT:
        ok = !entry_job_cancelled(job, service) && storage_file_commit(job->temporary, job->path);
        if (ok) { job->temporary_owned = false; done = true; }
        break;
    default: ok = false; break;
    }
    if (job->writing) entry_job_own_change(job);
    storage_unlock();
    if (!ok) goto abort;
    if (!done) return true;
    if (!job->writing && !entry_job_cancelled(job, service) && entry_job_source_valid(job)) {
        reader_entry_t *ready = entry_job_result(job);
        level = rt_hw_interrupt_disable();
        if (entry_read_current(job) && !entry_store.ready) { entry_store.ready = ready; ready = NULL; }
        rt_hw_interrupt_enable(level);
        if (ready) { entry_pixels_release(ready->pixels); epd_app_free(ready); }
    }
    goto finished;
abort:
    job->phase = ENTRY_ABORT;
    entry_pixels_release(job->pixels); job->pixels = NULL;
    (void)entry_job_cleanup(job);
finished:
    entry_job_free(job);
    level = rt_hw_interrupt_disable();
    entry_store.job = NULL; entry_store.active = entry_store.cancel_requested = false;
    rt_hw_interrupt_enable(level);
    epd_service_ui_request(service, 0);
    return true;
}

bool ui_reader_cache_worker_step(epd_service_t *service, bool reader_worked)
{
    return entry_worker_step(service, reader_worked, false);
}
bool ui_reader_cache_worker_priority(epd_service_t *service)
{
    rt_tick_t started = rt_tick_get();
    const rt_tick_t budget = rt_tick_from_millisecond(8);
    bool worked = false;
    /* Sixteen phases cap pixel I/O at 64 KiB even within one clock tick.
     * Storage calls are synchronous; the time budget is checked between them. */
    for (unsigned steps = 0; steps < 16; ++steps) {
        if (!entry_worker_step(service, true, true)) break;
        worked = true;
        if ((rt_tick_t)(rt_tick_get() - started) >= budget) break;
    }
    return worked;
}

void ui_reader_cache_worker_shutdown(void)
{
    entry_store_cancel_queue();
    if (ui_reader_cache_busy()) ui_reader_cache_worker_step(entry_store.service, true);
}
bool ui_reader_cache_process(void)
{
    bool changed = entry_cache_pressure();
    rt_base_t level = rt_hw_interrupt_disable();
    reader_entry_t *ready = entry_store.ready; entry_store.ready = NULL;
    if (ready) epd_service_wake(entry_store.service);
    rt_hw_interrupt_enable(level);
    if (!ready) return changed;
    unsigned index;
    const ui_bookshelf_book_t *book = NULL;
    for (index = 0; index <= BOOKSHELF_MAX_ITEMS; ++index) {
        book = ui_bookshelf_book(index);
        if (book && !strcmp(book->file.path, ready->file.path)) break;
    }
    ready->font_revision = epd_app_reading_font_revision();
    if (book && entry_matches(ready, book)) {
        reader_entry_t *slot = NULL;
        for (unsigned i = 0; i < ENTRY_SLOTS; ++i) {
            if (&entries[i] == preview) continue;
            if (!strcmp(entries[i].file.path, ready->file.path)) { slot = &entries[i]; break; }
            if (!slot || !entries[i].bitmap.memory || entries[i].age < slot->age) slot = &entries[i];
        }
        if (slot) {
            entry_release(slot); *slot = *ready; ready->pixels = NULL;
            slot->file = book->file; slot->age = ++entry_age;
            slot->view.title = slot->file.name; slot->view.text = ""; slot->view.font = epd_app_font_role(EPD_FONT_BODY);
            if (opened_book == index && !document_view_get()->ready && !preview) { preview = slot; changed = true; }
        }
    }
    entry_pixels_release(ready->pixels); epd_app_free(ready);
    return changed;
}
static void entry_store_reset(void)
{
    /* The worker has returned descriptor ownership to resident storage. */
    entry_job_free(entry_store.pending);
    if (entry_store.ready) { entry_pixels_release(entry_store.ready->pixels); epd_app_free(entry_store.ready); }
    memset(&entry_store, 0, sizeof(entry_store));
}
