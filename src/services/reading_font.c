#include "reading_font.h"
#include "reading_font_validate.h"
#include "storage_internal.h"
#include "storage_file.h"
#include "app_service.h"
#include "sleep_guard.h"
#include "system_reset_guard.h"
#include "epd_memory.h"
#include "ui_font.h"
#include "platform/epd_app.h"
#include <dfs_posix.h>
#include <rthw.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define FONT_RECORD "/flash/system/reading-fonts.bin"
#define FONT_RECORD_VERSION 2u
#define FONT_MAGIC 0x464e5431u
#define FONT_SERVICE "rd_font"
#define COPY_SIZE (32u * 1024u)
#define COMPARE_SIZE 4096u
#define UPLOAD_TIMEOUT_MS 30000u
#define RESULT_COUNT 8u
#define SCAN_RETRY_LIMIT 2u
#define SCAN_RETRY_DELAY_MS 1000u

typedef struct
{
    uint32_t id, signature, size, volume, proof;
    char name[READING_FONT_NAME_MAX];
} font_record_item_t;
typedef struct
{
    uint32_t next_id, preferred, count;
    font_record_item_t items[READING_FONT_LIMIT];
} font_record_t;
typedef struct
{
    uint32_t revision, session;
    bool mounted, writers, known;
} font_scan_stamp_t;
typedef struct
{
    uint32_t deadline;
    uint8_t failures;
    bool pending, installed_probe;
} font_scan_retry_t;
typedef struct
{
    font_record_item_t item;
    uint32_t session, revision, references, failure_revision;
    uint32_t checked_volume_revision;
    bool available, path_current;
    font_scan_stamp_t scanned;
    font_scan_retry_t retry;
} font_entry_t;
typedef struct
{
    uint32_t id, session;
    storage_volume_t volume;
} font_pin_t;
typedef enum { JOB_BOOT, JOB_SCAN, JOB_IMPORT, JOB_UPLOAD, JOB_SELECT, JOB_DELETE } font_job_t;
typedef struct
{
    uint32_t started, lock_ticks, read_ticks, write_ticks, sync_ticks, hash_ticks;
    uint32_t reads, writes;
} font_io_stats_t;
typedef struct
{
    font_job_t kind;
    storage_volume_t volume;
    uint32_t id, session, upload_tick;
    bool card, cancel, prepared, running, finished;
    bool available[READING_FONT_LIMIT];
    uint32_t failures[READING_FONT_LIMIT];
    uint32_t revisions[READING_FONT_LIMIT], sessions[READING_FONT_LIMIT];
    font_scan_stamp_t scanned[READING_FONT_LIMIT];
    font_scan_retry_t retries[READING_FONT_LIMIT];
    bool retry_failed;
    uint32_t volume_revisions[STORAGE_COUNT];
    bool volume_available[STORAGE_COUNT];
    rt_err_t error;
    const char *error_reason;
    int upload_fd;
    uint32_t signature, signature_revision;
    bool signature_valid, signature_ready;
    font_io_stats_t io;
    char source[STORAGE_PATH_MAX], temporary[STORAGE_PATH_MAX];
    char name[READING_FONT_NAME_MAX];
    font_record_t result;
} font_job_state_t;
typedef struct
{
    font_entry_t entries[READING_FONT_LIMIT];
    font_pin_t pins[READING_FONT_LIMIT];
    font_record_t record;
    font_job_state_t job;
    reading_font_status_t status;
    reading_font_status_t results[RESULT_COUNT];
    unsigned next_result;
    uint32_t render_revision, failed_id, next_ticket;
    uint32_t volume_revisions[STORAGE_COUNT], scan_session;
    bool volume_available[STORAGE_COUNT];
    bool card_blocked, rescan, loaded, restore_pending;
} font_state_t;

static font_state_t *fonts;
static struct rt_mutex control;
static epd_service_t *service;
static sleep_guard_owner_t awake = {.id = FONT_SERVICE};
static bool lock_ready;
static void request_scan(bool retry_failed);

static const char *directory(storage_volume_t volume)
{ return volume == STORAGE_SD ? "/sdcard/.epd/fonts" : "/flash/fonts"; }

static void content_path(char *path, size_t size, storage_volume_t volume, uint32_t id)
{ snprintf(path, size, "%s/%08lx.ttf", directory(volume), (unsigned long)id); }

/* Pins belong to the catalog, independently of temporary validator/source
 * descriptors. Their count is bounded by the installed-font limit. */
static void font_unpin(font_pin_t *pin)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!pin->id) { rt_hw_interrupt_enable(level); return; }
    /* A replacement mount has a new table: never decrement its owners. */
    if (pin->volume != STORAGE_SD || pin->session == storage_card_session())
    {
        char path[STORAGE_PATH_MAX];
        content_path(path, sizeof(path), pin->volume, pin->id);
        storage_path_unpin(path);
    }
    memset(pin, 0, sizeof(*pin));
    rt_hw_interrupt_enable(level);
}

static bool font_pin(const font_record_item_t *item)
{
    storage_volume_t volume = (storage_volume_t)item->volume;
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t session = volume == STORAGE_SD ? storage_card_session() : 0;
    bool ok = storage_session_valid(volume, session) &&
        (volume != STORAGE_SD || !fonts->card_blocked);
    font_pin_t *empty = NULL;
    for (unsigned i = 0; i < READING_FONT_LIMIT; ++i)
    {
        font_pin_t *pin = &fonts->pins[i];
        if (pin->id && pin->volume == STORAGE_SD && pin->session != storage_card_session())
            font_unpin(pin);
        if (pin->id == item->id && pin->volume == volume)
        {
            rt_hw_interrupt_enable(level);
            return ok;
        }
        if (!pin->id && !empty) empty = pin;
    }
    if (ok && empty)
    {
        char path[STORAGE_PATH_MAX];
        content_path(path, sizeof(path), volume, item->id);
        ok = storage_path_pin(path);
        if (ok) *empty = (font_pin_t){.id = item->id, .session = session, .volume = volume};
    }
    else ok = false;
    rt_hw_interrupt_enable(level);
    return ok;
}

static void font_pins_prune(const font_record_t *record)
{
    for (unsigned i = 0; i < READING_FONT_LIMIT; ++i)
    {
        font_pin_t *pin = &fonts->pins[i];
        bool keep = false;
        for (unsigned j = 0; pin->id && j < record->count; ++j)
            if (record->items[j].id == pin->id && record->items[j].volume == (uint32_t)pin->volume)
            { keep = true; break; }
        if (!keep) font_unpin(pin);
    }
}

/* Catalog pins keep these path tokens stable without opening the files. */
static font_scan_stamp_t scan_stamp(const font_record_item_t *item)
{
    storage_volume_t volume = (storage_volume_t)item->volume;
    font_scan_stamp_t stamp = {0};
    stamp.session = volume == STORAGE_SD ? storage_card_session() : 0;
    stamp.mounted = storage_available(volume) &&
        (volume != STORAGE_SD || !fonts->card_blocked);
    if (stamp.mounted)
    {
        char path[STORAGE_PATH_MAX];
        content_path(path, sizeof(path), volume, item->id);
        stamp.revision = storage_path_revision(path);
        stamp.writers = storage_file_writers(path) != 0;
    }
    stamp.known = true;
    return stamp;
}

static bool scan_stamp_equal(const font_scan_stamp_t *a, const font_scan_stamp_t *b)
{
    return a->known && b->known && a->revision == b->revision && a->session == b->session &&
        a->mounted == b->mounted && a->writers == b->writers;
}

static bool scan_retry_due(const font_scan_retry_t *retry)
{
    return retry->pending && (int32_t)((uint32_t)rt_tick_get() - retry->deadline) >= 0;
}

static void scan_retry_failed(font_scan_retry_t *retry, rt_err_t error)
{
    /* Format errors remain cached; resource and I/O failures get a bounded
     * chance to recover without a path write or a new mount. */
    if (error != -RT_ENOMEM && error != -RT_EBUSY && error != -RT_EIO)
    {
        memset(retry, 0, sizeof(*retry));
        return;
    }
    if (retry->failures <= SCAN_RETRY_LIMIT) ++retry->failures;
    retry->pending = retry->failures <= SCAN_RETRY_LIMIT;
    retry->deadline = (uint32_t)rt_tick_get() + rt_tick_from_millisecond(SCAN_RETRY_DELAY_MS);
}

static void scan_retry_request(void)
{
    if (fonts->card_blocked || epd_service_cancelled(service)) return;
    uint32_t now = (uint32_t)rt_tick_get(), delay = UINT32_MAX;
    for (unsigned i = 0; i < fonts->record.count; ++i)
    {
        const font_scan_retry_t *retry = &fonts->entries[i].retry;
        if (!retry->pending) continue;
        int32_t remaining = (int32_t)(retry->deadline - now);
        uint32_t ms = remaining > 0 ?
            (uint32_t)(((uint64_t)remaining * 1000u + RT_TICK_PER_SECOND - 1) / RT_TICK_PER_SECOND) : 0;
        if (ms < delay) delay = ms;
    }
    /* Reuse the service's one-shot wakeup; no periodic retry timer is needed. */
    if (delay != UINT32_MAX) epd_service_ui_request(service, delay);
}

static bool scan_needed(void)
{
    uint32_t revisions[STORAGE_COUNT], session = storage_card_session();
    bool available[STORAGE_COUNT], changed[STORAGE_COUNT];
    for (unsigned v = 0; v < STORAGE_COUNT; ++v)
    {
        revisions[v] = storage_volume_revision((storage_volume_t)v);
        available[v] = storage_available((storage_volume_t)v);
        changed[v] = revisions[v] != fonts->volume_revisions[v] ||
            available[v] != fonts->volume_available[v] || (v == STORAGE_SD && session != fonts->scan_session);
    }
    for (unsigned i = 0; i < fonts->record.count; ++i)
    {
        font_entry_t *entry = &fonts->entries[i];
        if (scan_retry_due(&entry->retry)) return true;
        /* A writer may retire after its last volume notification. */
        if (!changed[entry->item.volume] && !entry->scanned.writers) continue;
        font_scan_stamp_t current = scan_stamp(&entry->item);
        if (!scan_stamp_equal(&entry->scanned, &current)) return true;
    }
    /* Acknowledge only the tokens sampled before checking paths, so a write
     * racing the checks remains visible on the next poll. */
    memcpy(fonts->volume_revisions, revisions, sizeof(revisions));
    memcpy(fonts->volume_available, available, sizeof(available));
    fonts->scan_session = session;
    return false;
}

static int entry_index(uint32_t id)
{
    for (unsigned i = 0; fonts && i < fonts->record.count; ++i)
        if (fonts->entries[i].item.id == id) return (int)i;
    return -1;
}

static bool entry_available(font_entry_t *entry)
{
    rt_base_t level = rt_hw_interrupt_disable();
    storage_volume_t volume = (storage_volume_t)entry->item.volume;
    bool ok = entry->available && (volume != STORAGE_SD || !fonts->card_blocked) &&
        storage_session_valid(volume, entry->session);
    if (ok)
    {
        uint32_t revision = storage_volume_revision(volume);
        /* Catalog pins prevent path-token eviction. Every watched mutation
         * advances the volume token before touching content, so unchanged
         * volumes need no path normalization or slot lookup on glyph reads.
         * Retry negative checks: a writer can retire after its final event. */
        if (!entry->path_current || entry->checked_volume_revision != revision)
        {
            char path[STORAGE_PATH_MAX];
            content_path(path, sizeof(path), volume, entry->item.id);
            entry->path_current = !storage_file_writers(path) &&
                storage_path_revision(path) == entry->revision;
            entry->checked_volume_revision = revision;
        }
        ok = entry->path_current;
    }
    rt_hw_interrupt_enable(level);
    return ok;
}

static const char *unavailable_reason(uint32_t id)
{
    int index = entry_index(id);
    if (index < 0) return "missing";
    const font_entry_t *entry = &fonts->entries[index];
    if ((entry->item.volume == STORAGE_SD && fonts->card_blocked) ||
        !storage_session_valid((storage_volume_t)entry->item.volume, entry->session)) return "media";
    char path[STORAGE_PATH_MAX];
    content_path(path, sizeof(path), (storage_volume_t)entry->item.volume, id);
    if (storage_file_writers(path)) return "writer";
    if (storage_path_revision(path) != entry->revision) return "changed";
    return "unavailable";
}

static void log_selection(uint32_t previous, const char *reason)
{
    rt_kprintf("font select from=%lu to=%lu preferred=%lu reason=%s\n",
        (unsigned long)previous, (unsigned long)fonts->status.selected,
        (unsigned long)fonts->status.preferred, reason);
}

static void snapshot_item(unsigned index, reading_font_info_t *out)
{
    font_entry_t *entry = &fonts->entries[index];
    memset(out, 0, sizeof(*out));
    out->id = entry->item.id;
    out->signature = entry->item.signature;
    out->size = entry->item.size;
    out->volume = (storage_volume_t)entry->item.volume;
    out->available = entry_available(entry);
    out->selected = fonts->status.selected == out->id;
    out->preferred = fonts->status.preferred == out->id;
    memcpy(out->name, entry->item.name, sizeof(out->name));
}

void reading_font_get_status(reading_font_status_t *out)
{
    if (!out) return;
    rt_base_t level = rt_hw_interrupt_disable();
    if (fonts) *out = fonts->status;
    else memset(out, 0, sizeof(*out));
    rt_hw_interrupt_enable(level);
}

bool reading_font_job_status(uint32_t ticket, reading_font_status_t *out)
{
    if (!ticket || !out) return false;
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    if (fonts && fonts->status.ticket == ticket)
    {
        *out = fonts->status;
        found = true;
    }
    for (unsigned i = 0; fonts && !found && i < RESULT_COUNT; ++i)
        if (fonts->results[i].ticket == ticket)
        {
            *out = fonts->results[i];
            found = true;
        }
    rt_hw_interrupt_enable(level);
    return found;
}

bool reading_font_get(uint32_t id, reading_font_info_t *out)
{
    if (!out) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    bool found = false;
    if (!id)
    {
        memset(out, 0, sizeof(*out));
        strcpy(out->name, "MiSans");
        out->available = true;
        out->selected = !fonts || !fonts->status.selected;
        out->preferred = !fonts || !fonts->status.preferred;
        found = true;
    }
    else
    {
        int index = entry_index(id);
        if (index >= 0) { snapshot_item((unsigned)index, out); found = true; }
    }
    rt_hw_interrupt_enable(level);
    return found;
}

size_t reading_font_list(reading_font_info_t *items, size_t capacity)
{
    if (items && capacity) reading_font_get(0, items);
    rt_base_t level = rt_hw_interrupt_disable();
    unsigned count = fonts ? fonts->record.count : 0;
    for (unsigned i = 0; items && i < count && i + 1 < capacity; ++i)
        snapshot_item(i, &items[i + 1]);
    rt_hw_interrupt_enable(level);
    return count + 1;
}

uint32_t reading_font_selected(void) { return fonts ? fonts->status.selected : 0; }
uint32_t reading_font_revision(void) { return fonts ? fonts->render_revision : 0; }

bool reading_font_acquire(uint32_t id, reading_font_lease_t *lease)
{
    if (!lease || !id || !fonts) return false;
    memset(lease, 0, sizeof(*lease));
    rt_base_t level = rt_hw_interrupt_disable();
    int index = entry_index(id);
    bool ok = index >= 0 && entry_available(&fonts->entries[index]);
    if (ok)
    {
        font_entry_t *entry = &fonts->entries[index];
        ++entry->references;
        lease->id = id;
        lease->signature = entry->item.signature;
        lease->size = entry->item.size;
        lease->session = entry->session;
        lease->revision = entry->revision;
        lease->failure_revision = entry->failure_revision;
        lease->volume = (storage_volume_t)entry->item.volume;
    }
    rt_hw_interrupt_enable(level);
    if (ok) content_path(lease->path, sizeof(lease->path), lease->volume, id);
    return ok;
}

void reading_font_release(reading_font_lease_t *lease)
{
    if (!lease || !lease->id || !fonts) return;
    rt_base_t level = rt_hw_interrupt_disable();
    int index = entry_index(lease->id);
    if (index >= 0 && fonts->entries[index].references) --fonts->entries[index].references;
    lease->id = 0;
    rt_hw_interrupt_enable(level);
}

bool reading_font_lease_valid(const reading_font_lease_t *lease)
{
    if (!lease || !lease->id || !fonts) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    int index = entry_index(lease->id);
    bool ok = index >= 0 &&
        fonts->entries[index].item.signature == lease->signature &&
        fonts->entries[index].item.size == lease->size &&
        fonts->entries[index].item.volume == (uint32_t)lease->volume &&
        fonts->entries[index].revision == lease->revision &&
        fonts->entries[index].failure_revision == lease->failure_revision &&
        fonts->entries[index].session == lease->session &&
        entry_available(&fonts->entries[index]);
    rt_hw_interrupt_enable(level);
    return ok;
}

void reading_font_report_failure(uint32_t id)
{
    if (!fonts || !id) return;
    rt_base_t level = rt_hw_interrupt_disable();
    int index = entry_index(id);
    uint32_t previous = fonts->status.selected;
    if (index >= 0)
    {
        bool changed = fonts->entries[index].available;
        fonts->entries[index].available = false;
        ++fonts->entries[index].failure_revision;
        fonts->failed_id = id;
        if (fonts->status.selected == id)
        {
            fonts->status.selected = 0;
            ++fonts->render_revision;
        }
        if (changed) ++fonts->status.revision;
    }
    rt_hw_interrupt_enable(level);
    if (previous != fonts->status.selected) log_selection(previous, "stream");
    if (service) epd_service_ui_request(service, 0);
}

static bool stopped(void *context)
{
    font_job_state_t *job = &fonts->job;
    return job->cancel || !system_reset_write_allowed() ||
        (context && epd_service_cancelled(context)) ||
        (job->card && (fonts->card_blocked || storage_release_requested() ||
                      !storage_session_valid(STORAGE_SD, job->session)));
}

static bool persist(const font_record_t *record)
{ return storage_record_save(FONT_RECORD, FONT_MAGIC, FONT_RECORD_VERSION, record, sizeof(*record)); }

static bool record_valid(const font_record_t *record)
{
    if (!record->next_id || record->count > READING_FONT_LIMIT) return false;
    bool preferred = !record->preferred;
    for (unsigned i = 0; i < record->count; ++i)
    {
        const font_record_item_t *item = &record->items[i];
        if (!item->id || item->id >= record->next_id || item->volume >= STORAGE_COUNT ||
            !item->size || item->size > READING_FONT_FILE_MAX ||
            !memchr(item->name, 0, sizeof(item->name))) return false;
        for (unsigned j = 0; j < i; ++j) if (record->items[j].id == item->id) return false;
        if (record->preferred == item->id) preferred = true;
    }
    return preferred;
}

typedef struct
{
    epd_service_t *owner;
    storage_volume_t volume;
    uint32_t session;
} font_scan_context_t;

static bool scan_stopped(void *context)
{
    const font_scan_context_t *scan = context;
    return fonts->job.cancel || !system_reset_write_allowed() || epd_service_cancelled(scan->owner) ||
        (scan->volume == STORAGE_SD && (fonts->card_blocked || storage_release_requested() ||
         !storage_session_valid(scan->volume, scan->session)));
}

static void scan_fonts(epd_service_t *owner)
{
    font_job_state_t *job = &fonts->job;
    if (job->kind == JOB_BOOT)
    {
        if (!storage_available(STORAGE_FLASH)) { job->error = -RT_EIO; return; }
        if (!storage_record_load(FONT_RECORD, FONT_MAGIC, FONT_RECORD_VERSION, &job->result, sizeof(job->result)) ||
            !record_valid(&job->result))
        {
            memset(&job->result, 0, sizeof(job->result));
            job->result.next_id = 1;
        }
    }
    bool changed = false;
    fonts->status.phase = READING_FONT_VALIDATING;
    fonts->status.total = job->result.count;
    for (unsigned i = 0; i < job->result.count; ++i)
    {
        font_record_item_t *item = &job->result.items[i];
        if (job->cancel || epd_service_cancelled(owner)) break;
        fonts->status.completed = i + 1;
        char path[STORAGE_PATH_MAX];
        content_path(path, sizeof(path), (storage_volume_t)item->volume, item->id);
        font_scan_context_t scan = {owner, (storage_volume_t)item->volume, 0};
        /* Bind this validation to the current mount, not the session that was
         * present when the scan was queued before TF startup completed. */
        rt_base_t level = rt_hw_interrupt_disable();
        scan.session = item->volume == STORAGE_SD ? storage_card_session() : 0;
        bool pinned = font_pin(item);
        font_scan_stamp_t current = scan_stamp(item);
        rt_hw_interrupt_enable(level);
        font_scan_retry_t *retry = &job->retries[i];
        bool unchanged = scan_stamp_equal(&job->scanned[i], &current);
        bool initial_mount = job->kind == JOB_BOOT || !job->scanned[i].known ||
            !job->scanned[i].mounted || job->scanned[i].session != current.session;
        /* A delayed retry of a lightweight probe must not become a full scan. */
        bool quick = !job->failures[i] && (initial_mount ||
            (unchanged && !job->retry_failed && retry->pending && retry->installed_probe));
        if (job->retry_failed || !unchanged) memset(retry, 0, sizeof(*retry));
        bool unchanged_failure = !job->retry_failed && !job->available[i] &&
            unchanged && !scan_retry_due(retry);
        job->scanned[i] = current;
        if (unchanged_failure) continue;
        retry->installed_probe = quick;
        if (!pinned)
        {
            job->available[i] = false;
            scan_retry_failed(retry, current.mounted ? -RT_EBUSY : RT_EOK);
            continue;
        }
        uint32_t revision = current.revision;
        /* Validations belong to a path version and mount session. Other files
         * on the same volume, or a TF rescan, do not invalidate this result. */
        if (job->kind != JOB_BOOT && job->available[i] &&
            job->revisions[i] == revision && !storage_file_writers(path) &&
            storage_session_valid((storage_volume_t)item->volume, job->sessions[i])) continue;
        job->available[i] = false;
        if (!storage_available((storage_volume_t)item->volume) ||
            (item->volume == STORAGE_SD && (fonts->card_blocked || storage_release_requested()))) continue;
        if (storage_file_writers(path)) continue;
        reading_font_info_t info = {0};
        /* Each volume is checked separately: an absent TF card must not hide
         * valid internal fonts during startup. */
        uint32_t proof = item->proof;
        info.size = item->size; info.signature = item->signature;
        memcpy(info.name, item->name, sizeof(info.name));
        rt_err_t error = quick ? reading_font_probe_installed(path, &info, proof, scan_stopped, &scan) :
            reading_font_validate_install(path, &info, NULL, &proof, scan_stopped, &scan);
        if (error == RT_EOK && !scan_stopped(&scan) && !storage_file_writers(path) &&
            storage_path_revision(path) == revision)
        {
            memset(retry, 0, sizeof(*retry));
            job->available[i] = true;
            job->revisions[i] = revision;
            job->sessions[i] = scan.session;
            changed |= item->signature != info.signature || item->size != info.size || item->proof != proof;
            item->proof = proof;
            item->signature = info.signature;
            item->size = info.size;
            if (*info.name) memcpy(item->name, info.name, sizeof(item->name));
        }
        else
        {
            scan_retry_failed(retry, error);
            if (error == -RT_EINTR)
                rt_kprintf("font scan id=%lu reason=interrupted session=%lu current=%lu\n",
                    (unsigned long)item->id, (unsigned long)scan.session,
                    (unsigned long)storage_card_session());
        }
        fonts->status.completed = i + 1;
    }
    job->card = false;
    /* Availability is a runtime property; a missing/removable card does not
     * erase the saved preference or the installed catalog. */
    if (job->cancel || epd_service_cancelled(owner)) job->error = -RT_EINTR;
    else if (changed && !persist(&job->result)) job->error = -RT_EIO;
}

static bool room_for(storage_volume_t volume, uint32_t size)
{
    storage_info_t info;
    return storage_info(volume, &info) && info.mounted && info.total >= info.used &&
        info.total - info.used >= (uint64_t)size + 16384u;
}

static void font_io_lock(void)
{
    uint32_t start = (uint32_t)rt_tick_get();
    storage_lock();
    fonts->job.io.lock_ticks += (uint32_t)rt_tick_get() - start;
}

static uint32_t font_io_ms(uint32_t ticks)
{ return (uint32_t)((uint64_t)ticks * 1000u / RT_TICK_PER_SECOND); }

static void font_io_report(void)
{
    const font_job_state_t *job = &fonts->job;
    const font_io_stats_t *io = &job->io;
    rt_kprintf("font import ticket=%lu bytes=%lu/%lu buffer=%u total_ms=%lu result=%d\n",
        (unsigned long)fonts->status.ticket, (unsigned long)fonts->status.completed,
        (unsigned long)fonts->status.total, job->kind == JOB_IMPORT ? COPY_SIZE : 0u,
        (unsigned long)font_io_ms((uint32_t)rt_tick_get() - io->started), (int)job->error);
    rt_kprintf("font copy_io ticket=%lu read_ms=%lu write_ms=%lu lock_wait_ms=%lu\n",
        (unsigned long)fonts->status.ticket, (unsigned long)font_io_ms(io->read_ticks),
        (unsigned long)font_io_ms(io->write_ticks), (unsigned long)font_io_ms(io->lock_ticks));
    rt_kprintf("font copy_end ticket=%lu sync_ms=%lu hash_ms=%lu reads=%lu writes=%lu\n",
        (unsigned long)fonts->status.ticket, (unsigned long)font_io_ms(io->sync_ticks),
        (unsigned long)font_io_ms(io->hash_ticks), (unsigned long)io->reads, (unsigned long)io->writes);
}

static void font_signature_append(const void *data, size_t size)
{
    font_job_state_t *job = &fonts->job;
    const uint8_t *bytes = data;
    uint32_t start = (uint32_t)rt_tick_get();
    for (size_t i = 0; i < size; ++i)
        job->signature = (job->signature ^ bytes[i]) * 16777619u;
    job->io.hash_ticks += (uint32_t)rt_tick_get() - start;
}

static rt_err_t stage_open(void)
{
    font_job_state_t *job = &fonts->job;
    if (stopped(service)) return -RT_EINTR;
    if (!room_for(job->volume, fonts->status.total)) return -RT_EFULL;
    if (!storage_mkdirs(directory(job->volume))) return -RT_EIO;
    snprintf(job->temporary, sizeof(job->temporary), "%s/.incoming.ttf", directory(job->volume));
    font_io_lock();
    job->upload_fd = stopped(service) ? -1 : open(job->temporary, O_CREAT | O_TRUNC | O_WRONLY, 0);
    job->signature = 2166136261u;
    job->signature_revision = storage_path_revision(job->temporary);
    job->signature_valid = job->upload_fd >= 0 && storage_file_writers(job->temporary) == 1;
    job->signature_ready = false;
    storage_unlock();
    return job->upload_fd < 0 ? -RT_EIO : RT_EOK;
}

static bool write_all(int fd, const void *data, size_t size)
{
    font_job_state_t *job = &fonts->job;
    const uint8_t *p = data;
    job->signature_valid &= storage_path_revision(job->temporary) == job->signature_revision &&
        storage_file_writers(job->temporary) == 1;
    while (size)
    {
        uint32_t start = (uint32_t)rt_tick_get();
        int written = write(fd, p, size);
        job->io.write_ticks += (uint32_t)rt_tick_get() - start;
        ++job->io.writes;
        if (written <= 0) { job->signature_valid = false; return false; }
        p += written;
        size -= (unsigned)written;
    }
    job->signature_revision = storage_path_revision(job->temporary);
    return true;
}

static void stage_close(bool sync)
{
    font_job_state_t *job = &fonts->job;
    font_io_lock();
    if (job->upload_fd >= 0)
    {
        job->signature_valid &= storage_path_revision(job->temporary) == job->signature_revision &&
            storage_file_writers(job->temporary) == 1;
        uint32_t start = (uint32_t)rt_tick_get();
        if (sync && fsync(job->upload_fd) != 0) job->error = -RT_EIO;
        if (close(job->upload_fd) != 0) job->error = -RT_EIO;
        job->io.sync_ticks += (uint32_t)rt_tick_get() - start;
        job->upload_fd = -1;
        job->signature_revision = storage_path_revision(job->temporary);
        job->signature_ready = sync && !job->error && job->signature_valid &&
            fonts->status.completed == fonts->status.total && !storage_file_writers(job->temporary);
    }
    storage_unlock();
}

static void stage_remove(void)
{
    font_job_state_t *job = &fonts->job;
    storage_lock();
    if (*job->temporary && storage_session_valid(job->volume, job->session))
        (void)unlink(job->temporary);
    storage_unlock();
    job->temporary[0] = 0;
}

static void copy_source(epd_service_t *owner)
{
    font_job_state_t *job = &fonts->job;
    struct stat st;
    job->io.started = (uint32_t)rt_tick_get();
    font_io_lock();
    int fd = stopped(owner) ? -1 : open(job->source, O_RDONLY);
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > 0 && (uint64_t)st.st_size <= READING_FONT_FILE_MAX;
    storage_unlock();
    if (!ok) { job->error = -RT_EINVAL; goto done; }
    fonts->status.total = (uint32_t)st.st_size;
    job->error = stage_open();
    if (job->error != RT_EOK) goto done;
    uint8_t *buffer = epd_app_alloc(COPY_SIZE, EPD_APP_PSRAM);
    if (!buffer) { job->error = -RT_ENOMEM; goto done; }
    fonts->status.phase = READING_FONT_COPYING;
    while (fonts->status.completed < fonts->status.total)
    {
        if (stopped(owner)) { job->error = -RT_EINTR; break; }
        unsigned want = fonts->status.total - fonts->status.completed;
        if (want > COPY_SIZE) want = COPY_SIZE;
        font_io_lock();
        uint32_t start = (uint32_t)rt_tick_get();
        int n = stopped(owner) ? -1 : read(fd, buffer, want);
        job->io.read_ticks += (uint32_t)rt_tick_get() - start;
        ++job->io.reads;
        storage_unlock();
        if (n <= 0) { job->error = stopped(owner) ? -RT_EINTR : -RT_EIO; break; }
        font_io_lock();
        bool cancelled = stopped(owner);
        ok = !cancelled && write_all(job->upload_fd, buffer, (unsigned)n);
        storage_unlock();
        if (!ok) { job->error = cancelled ? -RT_EINTR : -RT_EIO; break; }
        font_signature_append(buffer, (unsigned)n);
        fonts->status.completed += (unsigned)n;
        rt_thread_yield();
    }
    epd_app_free(buffer);
done:
    font_io_lock();
    if (fd >= 0) close(fd);
    storage_unlock();
    stage_close(job->error == RT_EOK);
    font_io_report();
}

static bool same_content(const char *a, const char *b, epd_service_t *owner)
{
    uint8_t *bytes = epd_app_alloc(COMPARE_SIZE * 2, EPD_APP_PSRAM);
    if (!bytes) return false;
    storage_lock();
    int left = open(a, O_RDONLY), right = open(b, O_RDONLY);
    storage_unlock();
    bool same = left >= 0 && right >= 0;
    while (same)
    {
        if (stopped(owner)) { same = false; break; }
        storage_lock();
        int n = read(left, bytes, COMPARE_SIZE), m = read(right, bytes + COMPARE_SIZE, COMPARE_SIZE);
        storage_unlock();
        same = n >= 0 && n == m && !memcmp(bytes, bytes + COMPARE_SIZE, (unsigned)(n > 0 ? n : 0));
        if (n <= 0 || m <= 0) break;
        rt_thread_yield();
    }
    storage_lock();
    if (left >= 0) close(left);
    if (right >= 0) close(right);
    storage_unlock();
    epd_app_free(bytes);
    return same;
}

static void install_staged(epd_service_t *owner)
{
    font_job_state_t *job = &fonts->job;
    if (job->error || stopped(owner)) { if (!job->error) job->error = -RT_EINTR; return; }
    reading_font_info_t info = {0};
    fonts->status.phase = READING_FONT_VALIDATING;
    uint32_t staged_revision = storage_path_revision(job->temporary);
    reading_font_signature_t signature = {
        .path = job->temporary, .size = fonts->status.total, .signature = job->signature,
        .path_revision = job->signature_revision,
        .storage_session = job->volume == STORAGE_SD ? job->session : 0
    };
    uint32_t proof = 0;
    job->error = reading_font_validate_install(job->temporary, &info,
        job->signature_ready ? &signature : NULL, &proof, stopped, owner);
    if (job->error || stopped(owner)) { if (!job->error) job->error = -RT_EINTR; return; }
    if (storage_file_writers(job->temporary) || storage_path_revision(job->temporary) != staged_revision)
    { job->error = -RT_EIO; return; }
    if (info.size != fonts->status.total) { job->error = -RT_EIO; return; }
    char destination[STORAGE_PATH_MAX];
    for (unsigned i = 0; i < job->result.count; ++i)
    {
        font_record_item_t *item = &job->result.items[i];
        if (item->volume != (uint32_t)job->volume || item->signature != info.signature || item->size != info.size) continue;
        content_path(destination, sizeof(destination), job->volume, item->id);
        if (!font_pin(item)) continue;
        uint32_t revision = storage_path_revision(destination);
        if (!storage_file_writers(destination) && same_content(job->temporary, destination, owner) &&
            !storage_file_writers(destination) && storage_path_revision(destination) == revision &&
            !storage_file_writers(job->temporary) && storage_path_revision(job->temporary) == staged_revision)
        {
            job->available[i] = true;
            job->revisions[i] = revision;
            job->sessions[i] = job->volume == STORAGE_SD ? job->session : 0;
            fonts->status.result_id = item->id;
            return;
        }
    }
    if (job->result.count == READING_FONT_LIMIT) { job->error = -RT_EFULL; return; }
    struct stat st;
    bool chosen = false;
    for (unsigned attempt = 0; attempt < READING_FONT_LIMIT + 16u; ++attempt)
    {
        if (!job->result.next_id || job->result.next_id == UINT32_MAX) break;
        info.id = job->result.next_id++;
        content_path(destination, sizeof(destination), job->volume, info.id);
        storage_lock();
        bool exists = stat(destination, &st) == 0;
        storage_unlock();
        if (!exists) { chosen = true; break; }
    }
    if (!chosen) { job->error = -RT_EFULL; return; }
    font_record_item_t candidate = {.id = info.id, .volume = job->volume};
    if (!font_pin(&candidate)) { job->error = -RT_ENOMEM; return; }
    if (stopped(owner)) { job->error = -RT_EINTR; return; }
    fonts->status.phase = READING_FONT_COMMITTING;
    storage_lock();
    bool committed = !stopped(owner) && !storage_file_writers(job->temporary) &&
        storage_path_revision(job->temporary) == staged_revision &&
        storage_file_commit(job->temporary, destination);
    uint32_t installed_revision = storage_path_revision(destination);
    storage_unlock();
    if (!committed) { job->error = -RT_EIO; return; }
    job->temporary[0] = 0;
    unsigned index = job->result.count++;
    font_record_item_t *item = &job->result.items[index];
    memset(item, 0, sizeof(*item));
    item->id = info.id;
    item->signature = info.signature;
    item->proof = proof;
    item->size = info.size;
    item->volume = job->volume;
    snprintf(item->name, sizeof(item->name), "%s", *info.name ? info.name : job->name);
    if (stopped(owner) || !persist(&job->result))
    {
        storage_lock();
        if (storage_session_valid(job->volume, job->session)) (void)unlink(destination);
        storage_unlock();
        job->error = -RT_EIO;
        return;
    }
    job->available[index] = true;
    job->revisions[index] = installed_revision;
    job->sessions[index] = job->volume == STORAGE_SD ? job->session : 0;
    job->scanned[index] = (font_scan_stamp_t){installed_revision, job->sessions[index], true, false, true};
    memset(&job->retries[index], 0, sizeof(job->retries[index]));
    fonts->status.result_id = info.id;
}

static void delete_font(void)
{
    font_job_state_t *job = &fonts->job;
    unsigned index = 0;
    while (index < job->result.count && job->result.items[index].id != job->id) ++index;
    if (index == job->result.count) { job->error = -RT_EINVAL; return; }
    char path[STORAGE_PATH_MAX];
    content_path(path, sizeof(path), job->volume, job->id);
    if (storage_file_writers(path)) { job->error = -RT_EBUSY; return; }
    if (job->result.preferred == job->id) job->result.preferred = 0;
    for (unsigned i = index; i + 1 < job->result.count; ++i)
    {
        job->result.items[i] = job->result.items[i + 1];
        job->available[i] = job->available[i + 1];
        job->failures[i] = job->failures[i + 1];
        job->revisions[i] = job->revisions[i + 1];
        job->sessions[i] = job->sessions[i + 1];
        job->scanned[i] = job->scanned[i + 1];
        job->retries[i] = job->retries[i + 1];
    }
    memset(&job->result.items[--job->result.count], 0, sizeof(job->result.items[0]));
    if (stopped(service)) { job->error = -RT_EINTR; return; }
    /* Remove the durable reference before the file. A reset at either step
     * cannot leave a selected font pointing at an intentionally deleted file. */
    if (!persist(&job->result)) { job->error = -RT_EIO; return; }
    storage_lock();
    bool removed = false;
    bool writing = storage_file_writers(path) != 0;
    if (!writing && !stopped(service)) removed = unlink(path) == 0 || errno == ENOENT;
    storage_unlock();
    if (!removed)
    {
        (void)persist(&fonts->record);
        job->error = writing ? -RT_EBUSY : -RT_EIO;
    }
}

static void select_font(epd_service_t *owner)
{
    font_job_state_t *job = &fonts->job;
    job->result.preferred = job->id;
    reading_font_info_t selected;
    if (stopped(owner)) { job->error = -RT_EINTR; job->error_reason = "cancelled"; }
    else if (!reading_font_get(job->id, &selected) || !selected.available)
    { job->error = -RT_EIO; job->error_reason = "unavailable"; }
    else if (!persist(&job->result)) { job->error = -RT_EIO; job->error_reason = "persist"; }
}

static void worker(epd_service_t *owner)
{
    rt_int32_t wait_ticks = RT_WAITING_FOREVER;
    while (!epd_service_cancelled(owner))
    {
        (void)epd_service_wait(owner, wait_ticks);
        rt_mutex_take(&control, RT_WAITING_FOREVER);
        wait_ticks = RT_WAITING_FOREVER;
        if (!fonts->status.busy || fonts->job.finished)
        {
            rt_mutex_release(&control);
            continue;
        }
        font_job_state_t *job = &fonts->job;
        if (fonts->status.phase == READING_FONT_RECEIVING)
            wait_ticks = rt_tick_from_millisecond(1000);
        if (fonts->status.phase == READING_FONT_RECEIVING &&
            (uint32_t)(rt_tick_get() - job->upload_tick) >= rt_tick_from_millisecond(UPLOAD_TIMEOUT_MS))
            job->cancel = true;
        if (job->cancel && !job->running)
        {
            wait_ticks = RT_WAITING_FOREVER;
            stage_close(false);
            stage_remove();
            job->error = -RT_EINTR;
            job->finished = true;
            rt_mutex_release(&control);
            epd_service_ui_request(owner, 0);
            continue;
        }
        if (!job->prepared || job->running)
        {
            rt_mutex_release(&control);
            continue;
        }
        job->running = true;
        rt_mutex_release(&control);
        if (job->kind == JOB_BOOT || job->kind == JOB_SCAN) scan_fonts(owner);
        else if (job->kind == JOB_IMPORT || job->kind == JOB_UPLOAD)
        {
            if (job->kind == JOB_IMPORT) copy_source(owner);
            install_staged(owner);
            stage_remove();
        }
        else if (job->kind == JOB_SELECT) select_font(owner);
        else delete_font();
        job->finished = true;
        epd_service_ui_request(owner, 0);
    }
    rt_mutex_take(&control, RT_WAITING_FOREVER);
    if (fonts->status.busy)
    {
        stage_close(false);
        stage_remove();
        fonts->job.error = -RT_EINTR;
        fonts->job.finished = true;
    }
    rt_mutex_release(&control);
}

static bool prepare_font(uint32_t id)
{
    if (!id) return true;
    reading_font_info_t info;
    if (!reading_font_get(id, &info) || !info.available) return false;
    /* Catalog availability is bound to the validated content revision/session.
     * Creating an asynchronous shell neither validates nor prepares it here. */
    return true;
}

static void publish(void)
{
    font_job_state_t *job = &fonts->job;
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t old_selected = fonts->status.selected;
    uint32_t old_preferred = fonts->status.preferred;
    const char *reason = job->kind == JOB_SELECT ? "user" : job->kind == JOB_BOOT ? "boot" : "scan";
    int old_active = entry_index(old_selected);
    uint32_t old_signature = old_active >= 0 ? fonts->entries[old_active].item.signature : 0;
    uint32_t old_revision = old_active >= 0 ? fonts->entries[old_active].revision : 0;
    uint32_t old_session = old_active >= 0 ? fonts->entries[old_active].session : 0;
    if (job->error == RT_EOK)
    {
        /* Keep lease counters attached to immutable IDs when the list shrinks. */
        for (unsigned i = 0; i < job->result.count; ++i)
        {
            int old = entry_index(job->result.items[i].id);
            uint32_t refs = old >= 0 ? fonts->entries[old].references : 0;
            uint32_t failure = old >= 0 ? fonts->entries[old].failure_revision : 0;
            fonts->entries[i].item = job->result.items[i];
            fonts->entries[i].references = refs;
            fonts->entries[i].failure_revision = failure;
            fonts->entries[i].session = job->sessions[i];
            fonts->entries[i].revision = job->revisions[i];
            fonts->entries[i].scanned = job->scanned[i];
            fonts->entries[i].retry = job->retries[i];
            /* A scan may restore availability without changing the volume. */
            fonts->entries[i].path_current = false;
            fonts->entries[i].available = job->available[i] && failure == job->failures[i] &&
                (job->result.items[i].volume != STORAGE_SD || !fonts->card_blocked);
        }
        fonts->record = job->result;
        if (job->kind == JOB_BOOT || job->kind == JOB_SCAN)
        {
            memcpy(fonts->volume_revisions, job->volume_revisions, sizeof(fonts->volume_revisions));
            memcpy(fonts->volume_available, job->volume_available, sizeof(fonts->volume_available));
            fonts->scan_session = job->session;
        }
        fonts->status.preferred = job->result.preferred;
        if (job->kind == JOB_SELECT)
        {
            fonts->status.selected = job->id;
            fonts->restore_pending = false;
        }
        if (job->kind == JOB_BOOT) fonts->loaded = true;
        /* A delayed mount or retry can make the saved choice available after
         * boot, before any reader session has requested restoration. */
        if (job->kind == JOB_BOOT || job->kind == JOB_SCAN || fonts->restore_pending)
        {
            int index = entry_index(job->result.preferred);
            bool available = !job->result.preferred || (index >= 0 && entry_available(&fonts->entries[index]));
            if (available)
            {
                fonts->status.selected = job->result.preferred;
                if (fonts->restore_pending) reason = "begin-ready";
                fonts->restore_pending = false;
            }
            else
            {
                /* A mount/path change after the scan snapshot needs another
                 * scan before the new reading session can finish restoring. */
                bool changed = fonts->rescan;
                if (index >= 0)
                {
                    font_scan_stamp_t current = scan_stamp(&fonts->entries[index].item);
                    changed |= current.writers || job->retries[index].pending ||
                        !scan_stamp_equal(&job->scanned[index], &current);
                }
                fonts->restore_pending &= changed;
                reason = unavailable_reason(job->result.preferred);
            }
        }
    }
    font_pins_prune(&fonts->record);
    int active = entry_index(fonts->status.selected);
    if (fonts->status.selected && (active < 0 || !entry_available(&fonts->entries[active])))
    {
        reason = unavailable_reason(fonts->status.selected);
        fonts->status.selected = 0;
    }
    /* Persistence can succeed just before a stream or mount is invalidated.
     * Keep the saved preference, but do not report an unapplied selection as done. */
    if (!job->error && job->kind == JOB_SELECT && fonts->status.selected != job->id)
    { job->error = -RT_EIO; job->error_reason = "publish-unavailable"; }
    uint32_t signature = fonts->status.selected && active >= 0 ? fonts->entries[active].item.signature : 0;
    if (old_selected != fonts->status.selected || old_signature != signature ||
        (fonts->status.selected && active >= 0 &&
         (old_revision != fonts->entries[active].revision || old_session != fonts->entries[active].session)))
        ++fonts->render_revision;
    fonts->status.ready = fonts->loaded;
    if (job->error && (job->kind == JOB_DELETE || job->kind == JOB_BOOT)) fonts->rescan = true;
    fonts->status.error = job->error;
    fonts->status.phase = job->error == RT_EOK ? READING_FONT_DONE : READING_FONT_FAILED;
    fonts->status.busy = false;
    ++fonts->status.revision;
    fonts->results[fonts->next_result] = fonts->status;
    fonts->next_result = (fonts->next_result + 1) % RESULT_COUNT;
    rt_hw_interrupt_enable(level);
    if (old_selected != fonts->status.selected || old_preferred != fonts->status.preferred)
        log_selection(old_selected, reason);
    if (job->error && job->kind == JOB_SELECT)
        rt_kprintf("font select id=%lu result=%d reason=%s\n", (unsigned long)job->id, (int)job->error,
            job->error_reason ? job->error_reason : "cancelled");
    sleep_guard_release(&awake);
    scan_retry_request();
}

void reading_font_process(void)
{
    if (!fonts || !service || rt_mutex_take(&control, 0) != RT_EOK) return;
    font_job_state_t *job = &fonts->job;
    if (fonts->failed_id && epd_app_refresh_done())
    {
        uint32_t failed[READING_FONT_LIMIT];
        unsigned count = 0;
        rt_base_t level = rt_hw_interrupt_disable();
        for (unsigned i = 0; i < fonts->record.count; ++i)
            if (!fonts->entries[i].available)
                failed[count++] = fonts->entries[i].item.id;
        fonts->failed_id = 0;
        rt_hw_interrupt_enable(level);
        for (unsigned i = 0; i < count; ++i) ui_font_reader_revoke(failed[i]);
    }
    if (fonts->status.busy && job->finished && epd_app_refresh_done()) publish();
    else if (fonts->status.busy && !job->prepared && !job->running && !job->cancel &&
             fonts->status.phase == READING_FONT_QUEUED && epd_app_refresh_done())
    {
        if (job->kind == JOB_SELECT)
        {
            if (!prepare_font(job->id))
            { job->error = -RT_EIO; job->error_reason = "prepare"; job->finished = true; }
        }
        else if (job->kind == JOB_DELETE)
        {
            rt_base_t level = rt_hw_interrupt_disable();
            int index = entry_index(job->id);
            if (index >= 0) fonts->entries[index].available = false;
            bool selected = fonts->status.selected == job->id;
            if (selected)
            {
                fonts->status.selected = 0;
                ++fonts->render_revision;
            }
            ++fonts->status.revision;
            rt_hw_interrupt_enable(level);
            /* Publish fallback before draining streams. The platform may close
             * leases while drawing is drained; never hold control across it. */
            rt_mutex_release(&control);
            if (selected) log_selection(job->id, "delete");
            ui_font_reader_revoke(job->id);
            rt_mutex_take(&control, RT_WAITING_FOREVER);
            index = entry_index(job->id);
            if (index < 0 || fonts->entries[index].references)
            { job->error = -RT_EBUSY; job->finished = true; }
        }
        if (!job->finished)
        {
            fonts->status.phase = READING_FONT_COMMITTING;
            job->prepared = true;
            epd_service_wake(service);
        }
    }
    bool retry_failed = fonts->rescan || !fonts->loaded;
    bool rescan = !fonts->status.busy && !fonts->card_blocked && !storage_changing() &&
        !epd_service_cancelled(service) && (retry_failed || scan_needed());
    rt_mutex_release(&control);
    if (rescan) request_scan(retry_failed);
}

static bool ui_process(epd_service_t *owner, bool stopping)
{
    reading_font_process();
    if (fonts && (fonts->status.busy || fonts->failed_id)) epd_service_ui_request(owner, 50);
    return stopping && (!fonts || !fonts->status.busy);
}

static bool flush(void) { return !fonts || !fonts->status.busy; }
static const epd_background_t background = {
    .run = worker, .stack_size = 8192, .priority = 25, .ui_process = ui_process, .flush = flush
};

static uint32_t next_ticket(void)
{
    if (!++fonts->next_ticket) ++fonts->next_ticket;
    return fonts->next_ticket;
}

static rt_err_t accept(font_job_t kind, storage_volume_t volume, uint32_t id, uint32_t *ticket)
{
    if (!fonts || !service || fonts->status.busy || system_reset_blocks_apps() ||
        storage_changing() || storage_release_requested()) return -RT_EBUSY;
    if (kind != JOB_BOOT && !fonts->status.ready) return -RT_EBUSY;
    if (volume >= STORAGE_COUNT || (kind != JOB_BOOT && kind != JOB_SCAN && !storage_available(volume))) return -RT_EIO;
    if (!sleep_guard_acquire(&awake)) return -RT_EBUSY;
    font_job_state_t *job = &fonts->job;
    memset(job, 0, sizeof(*job));
    job->kind = kind;
    job->volume = volume;
    job->id = id;
    job->session = storage_card_session();
    job->card = volume == STORAGE_SD;
    job->upload_fd = -1;
    job->result = fonts->record;
    job->io.started = (uint32_t)rt_tick_get();
    for (unsigned v = 0; v < STORAGE_COUNT; ++v)
    {
        job->volume_revisions[v] = storage_volume_revision((storage_volume_t)v);
        job->volume_available[v] = storage_available((storage_volume_t)v);
    }
    /* Availability and failure generation must describe the same instant;
     * a draw worker may report a stream failure while a scan is accepted. */
    rt_base_t level = rt_hw_interrupt_disable();
    for (unsigned i = 0; i < fonts->record.count; ++i)
    {
        job->available[i] = entry_available(&fonts->entries[i]);
        job->failures[i] = fonts->entries[i].failure_revision;
        job->revisions[i] = fonts->entries[i].revision;
        job->sessions[i] = fonts->entries[i].session;
        job->scanned[i] = fonts->entries[i].scanned;
        job->retries[i] = fonts->entries[i].retry;
    }
    rt_hw_interrupt_enable(level);
    fonts->status.busy = true;
    fonts->status.phase = READING_FONT_QUEUED;
    fonts->status.error = 0;
    fonts->status.completed = fonts->status.total = fonts->status.result_id = 0;
    fonts->status.ticket = next_ticket();
    if (ticket) *ticket = fonts->status.ticket;
    return RT_EOK;
}

bool reading_font_init(void)
{
    if (service) return true;
    if (!lock_ready)
    {
        if (rt_mutex_init(&control, "font_ctl", RT_IPC_FLAG_PRIO) != RT_EOK) return false;
        lock_ready = true;
    }
    if (!fonts)
    {
        fonts = epd_app_alloc(sizeof(*fonts), EPD_APP_PSRAM);
        if (!fonts) return false;
        memset(fonts, 0, sizeof(*fonts));
        fonts->record.next_id = 1;
        fonts->status.revision = 1;
        fonts->render_revision = 1;
        fonts->job.upload_fd = -1;
    }
    service = app_service_start(FONT_SERVICE, &background, NULL);
    if (!service) return false;
    rt_mutex_take(&control, RT_WAITING_FOREVER);
    rt_err_t error = accept(JOB_BOOT, STORAGE_FLASH, 0, NULL);
    if (error == RT_EOK) { fonts->job.prepared = true; epd_service_wake(service); }
    rt_mutex_release(&control);
    return error == RT_EOK;
}

static bool ttf_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return n > 4 && name[n - 4] == '.' && (name[n - 3] == 't' || name[n - 3] == 'T') &&
        (name[n - 2] == 't' || name[n - 2] == 'T') && (name[n - 1] == 'f' || name[n - 1] == 'F');
}

rt_err_t reading_font_import(const char *source, storage_volume_t destination, uint32_t *ticket)
{
    if (!fonts || !source || strlen(source) >= STORAGE_PATH_MAX || !ttf_name(source) ||
        strncmp(source, "/sdcard/", 8) || strstr(source, "/../") || strstr(source, "/./")) return -RT_EINVAL;
    rt_mutex_take(&control, RT_WAITING_FOREVER);
    rt_err_t error = accept(JOB_IMPORT, destination, 0, ticket);
    if (error == RT_EOK)
    {
        strcpy(fonts->job.source, source);
        const char *name = strrchr(source, '/');
        snprintf(fonts->job.name, sizeof(fonts->job.name), "%s", name ? name + 1 : source);
        fonts->job.card = true;
        fonts->job.prepared = true;
        epd_service_wake(service);
    }
    rt_mutex_release(&control);
    return error;
}

static rt_err_t request_id(font_job_t kind, uint32_t id, uint32_t *ticket)
{
    if (ticket) *ticket = 0;
    if (!fonts || (!id && kind == JOB_DELETE)) return -RT_EINVAL;
    rt_mutex_take(&control, RT_WAITING_FOREVER);
    reading_font_info_t info;
    uint32_t accepted_ticket = 0;
    rt_err_t error = RT_EOK;
    bool reused = false;
    rt_base_t level = rt_hw_interrupt_disable();
    if (!reading_font_get(id, &info) || (kind == JOB_SELECT && !info.available)) error = -RT_EINVAL;
    else if (kind == JOB_SELECT && service && fonts->status.ready &&
        !system_reset_blocks_apps() && !storage_changing() && !storage_release_requested())
    {
        font_job_state_t *job = &fonts->job;
        if (fonts->status.busy && job->kind == JOB_SELECT && job->id == id && !job->cancel)
        {
            accepted_ticket = fonts->status.ticket;
            reused = true;
        }
        else if (fonts->status.selected == id && fonts->status.preferred == id &&
            (!fonts->status.busy || (job->kind == JOB_SCAN && !job->cancel)))
        {
            /* A completed no-op owns its result, never a background scan's
             * ticket/error. Leave the active job and render revision intact. */
            reading_font_status_t result = fonts->status;
            result.ticket = accepted_ticket = next_ticket();
            result.busy = false;
            result.phase = READING_FONT_DONE;
            result.error = RT_EOK;
            result.completed = result.total = 0;
            result.result_id = id;
            fonts->results[fonts->next_result] = result;
            fonts->next_result = (fonts->next_result + 1) % RESULT_COUNT;
            reused = true;
        }
    }
    rt_hw_interrupt_enable(level);
    if (error == RT_EOK && !reused) error = accept(kind, info.volume, id, &accepted_ticket);
    if (error == RT_EOK && ticket) *ticket = accepted_ticket;
    if (error == RT_EOK) epd_service_ui_request(service, 0);
    if (kind == JOB_SELECT)
        rt_kprintf("font select request id=%lu ticket=%lu result=%d\n", (unsigned long)id,
            (unsigned long)(error == RT_EOK ? accepted_ticket : 0), (int)error);
    rt_mutex_release(&control);
    return error;
}

rt_err_t reading_font_select(uint32_t id, uint32_t *ticket) { return request_id(JOB_SELECT, id, ticket); }
rt_err_t reading_font_delete(uint32_t id, uint32_t *ticket) { return request_id(JOB_DELETE, id, ticket); }

bool reading_font_begin(void)
{
    if (!fonts || rt_mutex_take(&control, 0) != RT_EOK) return false;
    bool ok = true;
    uint32_t previous = fonts->status.selected;
    const char *reason = "begin";
    int active = entry_index(previous);
    if (previous && (active < 0 || !entry_available(&fonts->entries[active])))
    {
        reason = unavailable_reason(previous);
        fonts->status.selected = 0;
        ++fonts->status.revision;
        ++fonts->render_revision;
    }
    int preferred_index = entry_index(fonts->status.preferred);
    fonts->restore_pending = !fonts->loaded || fonts->status.busy ||
        (preferred_index >= 0 && fonts->entries[preferred_index].retry.pending);
    if (fonts->loaded && fonts->status.selected != fonts->status.preferred &&
        (!fonts->status.busy || fonts->job.kind == JOB_SCAN))
    {
        reading_font_info_t preferred;
        bool available = reading_font_get(fonts->status.preferred, &preferred) && preferred.available;
        ok = !available || prepare_font(fonts->status.preferred);
        if (available && ok)
        {
            fonts->status.selected = fonts->status.preferred;
            ++fonts->status.revision;
            ++fonts->render_revision;
            fonts->restore_pending = false;
            reason = "begin";
        }
    }
    if (previous != fonts->status.selected)
    {
        log_selection(previous, reason);
        epd_service_ui_request(service, 0);
    }
    rt_mutex_release(&control);
    return ok;
}

rt_err_t reading_font_upload_begin(const char *name, storage_volume_t destination, uint32_t length, uint32_t *ticket)
{
    if (!fonts || !name || !*name || !ttf_name(name) || strlen(name) >= READING_FONT_NAME_MAX ||
        strchr(name, '/') || strchr(name, '\\') || !length || length > READING_FONT_FILE_MAX) return -RT_EINVAL;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) if (*p < 32 || *p == 127) return -RT_EINVAL;
    rt_mutex_take(&control, RT_WAITING_FOREVER);
    rt_err_t error = accept(JOB_UPLOAD, destination, 0, ticket);
    if (error == RT_EOK)
    {
        strcpy(fonts->job.name, name);
        fonts->status.total = length;
        fonts->job.upload_tick = (uint32_t)rt_tick_get();
        fonts->status.phase = READING_FONT_RECEIVING;
        error = stage_open();
        if (error != RT_EOK)
        {
            stage_close(false);
            stage_remove();
            fonts->job.error = error;
            fonts->job.finished = true;
        }
        epd_service_ui_request(service, 0);
        epd_service_wake(service);
    }
    rt_mutex_release(&control);
    return error;
}

rt_err_t reading_font_upload_write(uint32_t ticket, const void *bytes, size_t size)
{
    if (!fonts || !bytes || !size) return -RT_EINVAL;
    rt_mutex_take(&control, RT_WAITING_FOREVER);
    font_job_state_t *job = &fonts->job;
    rt_err_t error = RT_EOK;
    if (!fonts->status.busy || ticket != fonts->status.ticket || job->kind != JOB_UPLOAD ||
        fonts->status.phase != READING_FONT_RECEIVING || job->upload_fd < 0) error = -RT_EINVAL;
    else if (stopped(service)) error = -RT_EINTR;
    else if (size > fonts->status.total - fonts->status.completed) error = -RT_EFULL;
    else
    {
        font_io_lock();
        bool ok = !stopped(service) && write_all(job->upload_fd, bytes, size);
        storage_unlock();
        if (ok)
        {
            font_signature_append(bytes, size);
            fonts->status.completed += (uint32_t)size;
            job->upload_tick = (uint32_t)rt_tick_get();
        }
        else error = -RT_EIO;
    }
    rt_mutex_release(&control);
    return error;
}

rt_err_t reading_font_upload_finish(uint32_t ticket)
{
    if (!fonts) return -RT_EINVAL;
    rt_mutex_take(&control, RT_WAITING_FOREVER);
    font_job_state_t *job = &fonts->job;
    rt_err_t error = RT_EOK;
    if (!fonts->status.busy || ticket != fonts->status.ticket || job->kind != JOB_UPLOAD ||
        fonts->status.phase != READING_FONT_RECEIVING || job->upload_fd < 0) error = -RT_EINVAL;
    else
    {
        if (stopped(service) || fonts->status.completed != fonts->status.total) error = -RT_EIO;
        stage_close(error == RT_EOK);
        if (!error) error = job->error;
        job->error = error;
        job->cancel = error != RT_EOK;
        job->prepared = true;
        fonts->status.phase = READING_FONT_QUEUED;
        font_io_report();
        epd_service_wake(service);
    }
    rt_mutex_release(&control);
    return error;
}

void reading_font_upload_abort(uint32_t ticket)
{
    if (!fonts) return;
    rt_mutex_take(&control, RT_WAITING_FOREVER);
    if (fonts->status.busy && ticket == fonts->status.ticket && fonts->job.kind == JOB_UPLOAD &&
        !fonts->job.finished)
    {
        fonts->job.cancel = true;
        if (!fonts->job.running)
        {
            stage_close(false);
            stage_remove();
            fonts->job.error = -RT_EINTR;
            fonts->job.finished = true;
        }
        epd_service_wake(service);
        epd_service_ui_request(service, 0);
    }
    rt_mutex_release(&control);
}

bool reading_font_release_storage(void)
{
    if (!fonts) return true;
    if (!epd_app_refresh_done()) return false;
    if (!fonts->card_blocked)
    {
        uint32_t previous = fonts->status.selected;
        fonts->card_blocked = true;
        ui_font_reader_revoke_storage();
        int active = entry_index(fonts->status.selected);
        if (active >= 0 && fonts->entries[active].item.volume == STORAGE_SD)
        {
            fonts->status.selected = 0;
            ++fonts->render_revision;
        }
        for (unsigned i = 0; i < fonts->record.count; ++i)
            if (fonts->entries[i].item.volume == STORAGE_SD) fonts->entries[i].available = false;
        ++fonts->status.revision;
        if (previous != fonts->status.selected) log_selection(previous, "storage-release");
        epd_service_ui_request(service, 0);
    }
    if (fonts->status.busy)
    {
        if (!fonts->job.cancel)
        {
            fonts->job.cancel = true;
            epd_service_wake(service);
        }
        return false;
    }
    if (!ui_font_reader_release_storage(storage_release_request_id())) return false;
    for (unsigned i = 0; i < READING_FONT_LIMIT; ++i)
        if (fonts->pins[i].volume == STORAGE_SD) font_unpin(&fonts->pins[i]);
    return true;
}

static void request_scan(bool retry_failed)
{
    if (!fonts || !service) return;
    if (retry_failed) fonts->rescan = true;
    if (storage_changing() || storage_release_requested()) return;
    fonts->card_blocked = false;
    if (rt_mutex_take(&control, 0) != RT_EOK) return;
    if (accept(fonts->loaded ? JOB_SCAN : JOB_BOOT, STORAGE_FLASH, 0, NULL) == RT_EOK)
    {
        fonts->job.retry_failed = fonts->rescan;
        fonts->rescan = false;
        fonts->job.prepared = true;
        epd_service_wake(service);
    }
    rt_mutex_release(&control);
}

void reading_font_storage_changed(void) { request_scan(true); }
