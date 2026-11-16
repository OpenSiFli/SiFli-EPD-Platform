#include "gallery.h"
#include "storage_file.h"
#include "epd_memory.h"
#include <dfs_posix.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* The catalog and the selected view live on TF. Only one directory and one
 * page are resident, regardless of the number of photographs. */
typedef struct { char path[STORAGE_PATH_MAX]; uint32_t directory; } catalog_item_t;
typedef bool (*catalog_visit_t)(const catalog_item_t *, void *);
typedef struct {
    catalog_item_t item;
    unsigned position[STORAGE_PATH_MAX / 2];
    bool album_seen[STORAGE_PATH_MAX / 2];
    char parent[STORAGE_PATH_MAX];
    DIR *dir;
    unsigned depth, offset;
    bool done;
} walk_t;
typedef struct { walk_t walk; gallery_job_t job; gallery_result_t result; unsigned requested_first; bool anchor_fallback; } readonly_t;
static readonly_t *readonly_view;
static void readonly_step(void);
static int catalog_fd = -1, view_fd = -1;
static unsigned count, directories;
static uint32_t revision, session;
static char view_root[STORAGE_PATH_MAX];
static bool valid, view_valid, recursive, albums;
/* Directory records also form the disk-backed breadth-first work queue.
 * Two marks an enumerated directory; four stores an album's cover image. */
static DIR *scan_dir;
static char scan_root[STORAGE_PATH_MAX];
static unsigned records, queue_next, priority_record = UINT_MAX;
static bool scanning, view_complete, scan_album;
static gallery_status_t scan_error;
static volatile uint32_t catalog_revision, catalog_generation;
static uint32_t view_revision, view_epoch;
static const gallery_job_t background_job;
static struct { char path[3][STORAGE_PATH_MAX]; unsigned index; uint32_t epoch; bool valid; } location;

uint32_t gallery_catalog_revision(void) { return catalog_revision; }
uint32_t gallery_catalog_generation(void) { return catalog_generation; }

void gallery_catalog_clear(void)
{
    if (scan_dir) { storage_lock(); closedir(scan_dir); storage_unlock(); scan_dir = NULL; }
    if (readonly_view) {
        if (readonly_view->walk.dir) { storage_lock(); closedir(readonly_view->walk.dir); storage_unlock(); }
        epd_app_free(readonly_view);
        readonly_view = NULL;
    }
    gallery_file_close(catalog_fd);
    gallery_file_close(view_fd);
    catalog_fd = view_fd = -1;
    valid = view_valid = false;
    location.valid = false;
    count = directories = 0;
    scanning = false;
    scan_error = GALLERY_STATUS_NONE;
    records = queue_next = 0;
    priority_record = UINT_MAX;
    scan_root[0] = 0;
    ++catalog_revision;
    ++catalog_generation;
}

static bool image_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext || strlen(ext) > 5) return false;
    char lower[6];
    unsigned i;
    for (i = 0; ext[i]; ++i)
        lower[i] = ext[i] >= 'A' && ext[i] <= 'Z' ? ext[i] + ('a' - 'A') : ext[i];
    lower[i] = 0;
    return !strcmp(lower, ".png") || !strcmp(lower, ".jpg") || !strcmp(lower, ".jpeg");
}

static bool current(const gallery_job_t *job)
{
    return !gallery_cancelled(job->serial) && storage_session_valid(STORAGE_SD, session) &&
           revision == gallery_storage_revision();
}

/* Reopen the parent after visiting a child. The fixed stack contains directory
 * offsets, not open handles or paths; its depth is bounded by STORAGE_PATH_MAX. */
/* Return one entry, or yield after 32 directory operations. Reopening a
 * parent also resumes skipping its old entries across bounded turns. */
static bool walk_next(walk_t *state, const gallery_job_t *job, gallery_result_t *result)
{
    for (unsigned budget = 0; budget < 32 && !state->done && current(job); ++budget) {
        if (!state->dir) {
            storage_lock();
            state->dir = storage_session_valid(STORAGE_SD, session) ? opendir(state->parent) : NULL;
            storage_unlock();
            state->offset = 0;
            if (!state->dir) { result->error_code = GALLERY_STATUS_SCAN; break; }
        }
        storage_lock();
        errno = 0;
        struct dirent *entry = readdir(state->dir);
        if (!entry) {
            int error = errno;
            closedir(state->dir);
            state->dir = NULL;
            storage_unlock();
            if (error) { result->error_code = GALLERY_STATUS_SCAN; break; }
            if (!state->depth) state->done = true;
            else { *strrchr(state->parent, '/') = 0; --state->depth; }
            continue;
        }
        if (state->offset++ < state->position[state->depth]) { storage_unlock(); continue; }
        state->position[state->depth] = state->offset;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) { storage_unlock(); continue; }
        int n = snprintf(state->item.path, sizeof(state->item.path), "%s/%s", state->parent, entry->d_name);
        struct stat st;
        bool fits = n > 0 && (size_t)n < sizeof(state->item.path);
        bool skip = fits && !strcmp(state->item.path, GALLERY_STORE);
        bool found = fits && !skip && stat(state->item.path, &st) == 0;
        storage_unlock();
        if (skip) continue;
        if (!fits || !found) { result->error_code = fits ? GALLERY_STATUS_SCAN : GALLERY_STATUS_PATH; break; }
        state->item.directory = S_ISDIR(st.st_mode);
        if (!state->item.directory && (!S_ISREG(st.st_mode) || !image_name(state->item.path))) continue;
        if (state->item.directory && (job->recursive || job->kind == GALLERY_ALBUMS)) {
            if (state->depth + 1 >= sizeof(state->position) / sizeof(state->position[0])) {
                result->error_code = GALLERY_STATUS_PATH; break;
            }
            storage_lock(); closedir(state->dir); storage_unlock(); state->dir = NULL;
            memcpy(state->parent, state->item.path, sizeof(state->parent));
            state->position[++state->depth] = 0;
            state->album_seen[state->depth] = false;
        } else if (!state->item.directory && job->kind == GALLERY_ALBUMS) {
            if (state->album_seen[state->depth]) continue;
            state->album_seen[state->depth] = true;
            state->item.directory = 4;
        }
        return true;
    }
    if (!current(job) && !gallery_cancelled(job->serial)) result->error_code = GALLERY_STATUS_REMOVED;
    if (result->error_code) {
        if (state->dir) { storage_lock(); closedir(state->dir); storage_unlock(); state->dir = NULL; }
        state->done = true;
    }
    return false;
}

static bool rewind_file(int fd, unsigned index, const gallery_job_t *job)
{
    uint64_t offset = (uint64_t)index * sizeof(catalog_item_t);
    if (offset > LONG_MAX || !current(job)) return false;
    storage_lock();
    bool ok = storage_session_valid(STORAGE_SD, session) && lseek(fd, (off_t)offset, SEEK_SET) == (off_t)offset;
    storage_unlock();
    return ok;
}

static bool catalog_append(const catalog_item_t *item)
{
    if (records == UINT_MAX || !rewind_file(catalog_fd, records, &background_job) ||
        !gallery_file_io(catalog_fd, (void *)item, sizeof(*item), true, 0, session)) return false;
    ++records;
    ++catalog_revision;
    return true;
}

/* Only the service thread touches files. Each turn performs at most 32
 * directory/queue operations, then gives pending image requests a turn. */
bool gallery_catalog_step(void)
{
    if (!valid) return false;
    if (!current(&background_job)) { gallery_catalog_clear(); return false; }
    if (catalog_fd < 0) { readonly_step(); return scanning; }
    if (!scanning) return false;
    for (unsigned budget = 0; budget < 32 && scanning; ++budget)
    {
        catalog_item_t item;
        if (!current(&background_job)) { gallery_catalog_clear(); return false; }
        if (!scan_dir && !scan_root[0])
        {
            if (queue_next == records && priority_record == UINT_MAX)
            { scanning = false; ++catalog_revision; break; }
            unsigned index = priority_record != UINT_MAX ? priority_record : queue_next++;
            priority_record = UINT_MAX;
            if (!rewind_file(catalog_fd, index, &background_job) ||
                !gallery_file_io(catalog_fd, &item, sizeof(item), false, 0, session)) goto unwritable;
            if (item.directory != 1) continue;
            /* Claim the directory so priority requests cannot scan it twice. */
            item.directory = 3;
            if (!rewind_file(catalog_fd, index, &background_job) ||
                !gallery_file_io(catalog_fd, &item, sizeof(item), true, 0, session)) goto unwritable;
            memcpy(scan_root, item.path, sizeof(scan_root));
        }
        if (!scan_dir)
        {
            scan_album = false;
            storage_lock();
            scan_dir = storage_session_valid(STORAGE_SD, session) ? opendir(scan_root) : NULL;
            storage_unlock();
            if (!scan_dir) goto failed;
        }
        storage_lock();
        errno = 0;
        struct dirent *entry = storage_session_valid(STORAGE_SD, session) ? readdir(scan_dir) : NULL;
        int error = errno;
        if (!entry)
        {
            storage_unlock();
            if (error || !current(&background_job)) goto failed;
            storage_lock(); closedir(scan_dir); storage_unlock(); scan_dir = NULL;
            memcpy(item.path, scan_root, sizeof(item.path));
            item.directory = 2;
            if (!catalog_append(&item)) goto unwritable;
            scan_root[0] = 0;
            continue;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        { storage_unlock(); continue; }
        int n = snprintf(item.path, sizeof(item.path), "%s/%s", scan_root, entry->d_name);
        storage_unlock();
        if (n < 0 || (size_t)n >= sizeof(item.path)) { scan_error = GALLERY_STATUS_PATH; goto failed; }
        if (!strcmp(item.path, GALLERY_STORE)) continue;
        struct stat st;
        storage_lock();
        bool found = storage_session_valid(STORAGE_SD, session) && stat(item.path, &st) == 0;
        storage_unlock();
        if (!found) goto failed;
        item.directory = S_ISDIR(st.st_mode);
        if (!item.directory && (!S_ISREG(st.st_mode) || !image_name(item.path))) continue;
        if (!catalog_append(&item)) goto unwritable;
        if (!item.directory && !scan_album) {
            /* One cover record per image-owning directory, including TF root. */
            item.directory = 4;
            if (!catalog_append(&item)) goto unwritable;
            scan_album = true;
            break;
        }
        /* Publish the first usable entry without waiting for a full batch. */
        if (records == 1) break;
    }
    return scanning;
unwritable:
    if (!current(&background_job)) { gallery_catalog_clear(); return false; }
    if (scan_dir) { storage_lock(); closedir(scan_dir); storage_unlock(); scan_dir = NULL; }
    gallery_file_close(catalog_fd);
    gallery_file_close(view_fd);
    catalog_fd = view_fd = -1;
    view_valid = false;
    scanning = false;
    ++catalog_generation;
    ++catalog_revision;
    return false;
failed:
    if (scan_dir) { storage_lock(); closedir(scan_dir); storage_unlock(); scan_dir = NULL; }
    scanning = false;
    if (!scan_error) scan_error = current(&background_job) ? GALLERY_STATUS_SCAN : GALLERY_STATUS_REMOVED;
    ++catalog_revision;
    return false;
}

static bool catalog_ready(const gallery_job_t *job, gallery_result_t *result)
{
    if (gallery_cancelled(job->serial)) return false;
    if (!gallery_sd_path(job->path)) { result->error_code = GALLERY_STATUS_PATH; return false; }
    if (valid && current(&background_job)) return !gallery_cancelled(job->serial);
    gallery_catalog_clear();
    revision = gallery_storage_revision();
    session = storage_card_session();
    if (!storage_path_available(STORAGE_SD_ROOT)) { result->error_code = GALLERY_STATUS_STORAGE; return false; }
    if (gallery_store_ready(session))
        catalog_fd = gallery_file_open(GALLERY_STORE "/catalog", O_CREAT | O_TRUNC | O_RDWR, session);
    if (catalog_fd >= 0)
    {
        memcpy(scan_root, STORAGE_SD_ROOT, sizeof(STORAGE_SD_ROOT));
        scanning = true;
    }
    valid = true;
    (void)gallery_catalog_step();
    return true;
}

typedef struct {
    const gallery_job_t *job;
    gallery_result_t *result;
    unsigned first, limit, seen;
    int output;
    bool ordinal;
} selection_t;

static bool select_item(const catalog_item_t *item, void *context)
{
    selection_t *select = context;
    const gallery_job_t *job = select->job;
    bool album = job->kind == GALLERY_ALBUMS;
    if (item->directory == 2) {
        if (!album && !job->recursive && !strcmp(item->path, job->path)) select->result->complete = true;
        return true;
    }
    if (item->directory != (album ? 4u : 0u)) return true;
    size_t n = strlen(job->path);
    if (strncmp(item->path, job->path, n) || item->path[n] != '/' ||
        (!album && !job->recursive && strchr(item->path + n + 1, '/'))) return true;
    if (job->kind == GALLERY_LOCATE) {
        gallery_result_t *result = select->result;
        if (!result->located) {
            if (!strcmp(item->path, job->anchor)) {
                result->located = true;
                result->index = select->seen;
                memcpy(result->path, item->path, sizeof(result->path));
            } else memcpy(result->previous, item->path, sizeof(result->previous));
        } else if (!result->next[0]) memcpy(result->next, item->path, sizeof(result->next));
        ++select->seen;
        return true;
    }
    if (select->seen == UINT_MAX) { select->result->error_code = GALLERY_STATUS_PATH; return false; }
    if (select->output >= 0 && !gallery_file_io(select->output, (void *)item, sizeof(*item), true, job->serial, session))
    { gallery_file_close(select->output); select->output = -1; }
    unsigned first = select->first;
    if (job->anchor[0] && !select->ordinal) {
        gallery_result_t *result = select->result;
        if (!result->located) {
            /* Keep just the current nine-entry block until the path is found. */
            if (!(select->seen % GALLERY_ROWS)) result->count = 0;
            size_t length = album ? (size_t)(strrchr(item->path, '/') - item->path) : strlen(item->path);
            if (strlen(job->anchor) == length && !strncmp(item->path, job->anchor, length)) {
                result->located = true;
                result->index = select->seen / GALLERY_ROWS * GALLERY_ROWS;
            }
        }
        first = result->located ? result->index : select->seen / GALLERY_ROWS * GALLERY_ROWS;
    }
    if (select->seen >= first && select->result->count < select->limit)
    {
        gallery_entry_t *entry = &select->result->entries[select->result->count++];
        memcpy(entry->path, item->path, sizeof(entry->path));
        entry->cover[0] = 0;
        if (album) {
            memcpy(entry->cover, item->path, sizeof(entry->cover));
            *strrchr(entry->path, '/') = 0;
        }
        snprintf(entry->name, sizeof(entry->name), "%s", strrchr(entry->path, '/') + 1);
        entry->directory = album;
    }
    ++select->seen;
    if (album) ++select->result->directories;
    return true;
}

static bool select_pass(selection_t *select)
{
    if (catalog_fd < 0) return false;
    if (!rewind_file(catalog_fd, 0, select->job)) return false;
    catalog_item_t item;
    unsigned index = 0;
    while (current(select->job))
    {
        storage_lock();
        int n = storage_session_valid(STORAGE_SD, session) ? read(catalog_fd, &item, sizeof(item)) : -1;
        storage_unlock();
        if (!n) return true;
        if (n != sizeof(item) || !memchr(item.path, 0, sizeof(item.path))) return false;
        if (!select->job->recursive && item.directory == 1 && !strcmp(item.path, select->job->path))
            priority_record = index;
        ++index;
        if (!select_item(&item, select)) return false;
    }
    return false;
}

static void readonly_step(void)
{
    if (!readonly_view || readonly_view->walk.done) return;
    readonly_t *view = readonly_view;
    bool entry = walk_next(&view->walk, &view->job, &view->result);
    if (entry) {
        selection_t select = {&view->job, &view->result, view->job.first, GALLERY_ROWS, view->result.total, -1};
        select.ordinal = view->anchor_fallback;
        (void)select_item(&view->walk.item, &select);
        view->result.total = select.seen;
    }
    scanning = !view->walk.done;
    view->result.complete = view->walk.done && !view->result.error_code;
    view->result.catalog_revision = ++catalog_revision;
    view->result.catalog_generation = catalog_generation;
}

static bool readonly_page(const gallery_job_t *job, gallery_result_t *result)
{
    bool same = readonly_view && readonly_view->job.kind == job->kind &&
        (readonly_view->requested_first == job->first || readonly_view->job.first == job->first) && readonly_view->job.recursive == job->recursive &&
        !strcmp(readonly_view->job.path, job->path) && !strcmp(readonly_view->job.anchor, job->anchor);
    if (!same) {
        if (readonly_view) {
            if (readonly_view->walk.dir) { storage_lock(); closedir(readonly_view->walk.dir); storage_unlock(); }
            epd_app_free(readonly_view);
        }
        readonly_view = epd_app_alloc(sizeof(*readonly_view), EPD_APP_PSRAM);
        if (!readonly_view) { result->error_code = GALLERY_STATUS_MEMORY; return false; }
        memset(readonly_view, 0, sizeof(*readonly_view));
        readonly_view->job = *job;
        readonly_view->requested_first = job->first;
        readonly_view->job.serial = 0;
        memcpy(readonly_view->walk.parent, job->path, sizeof(readonly_view->walk.parent));
        scanning = true;
        readonly_step();
    }
    unsigned references = result->references;
    uint32_t serial = result->serial;
    *result = readonly_view->result;
    result->references = references;
    result->serial = serial;
    result->kind = job->kind;
    if (job->kind != GALLERY_LOCATE && !result->located) result->index = readonly_view->job.first;
    bool missing_anchor = job->kind != GALLERY_LOCATE && job->anchor[0] && !result->located && !readonly_view->anchor_fallback;
    if (missing_anchor) result->count = 0;
    result->more = result->index + result->count < result->total;
    if (job->kind == GALLERY_LOCATE && !result->located) result->previous[0] = 0;
    if (result->complete && job->kind != GALLERY_LOCATE &&
        (missing_anchor || (readonly_view->job.first && readonly_view->job.first >= result->total))) {
        unsigned first = readonly_view->job.first;
        if (first >= result->total) first = result->total ? (result->total - 1) / GALLERY_ROWS * GALLERY_ROWS : 0;
        memset(&readonly_view->walk, 0, sizeof(readonly_view->walk));
        memset(&readonly_view->result, 0, sizeof(readonly_view->result));
        readonly_view->job.first = first;
        readonly_view->anchor_fallback = true;
        memcpy(readonly_view->walk.parent, job->path, sizeof(readonly_view->walk.parent));
        scanning = true;
        readonly_step();
        return readonly_page(job, result);
    }
    return !result->error_code;
}

static bool select_page(const gallery_job_t *job, gallery_result_t *result, unsigned limit)
{
    if (!catalog_ready(job, result)) return false;
    if (catalog_fd < 0) return readonly_page(job, result);
    bool cached = !job->anchor[0] && view_valid && (view_complete || view_revision == catalog_revision) &&
                  recursive == job->recursive && albums == (job->kind == GALLERY_ALBUMS) && !strcmp(view_root, job->path);
    unsigned first = job->first;
    if (cached)
    {
        if (limit > 1 && view_complete && first >= count) first = count ? (count - 1) / GALLERY_ROWS * GALLERY_ROWS : 0;
        if (!rewind_file(view_fd, first, job)) cached = false;
    }
    if (cached)
    {
        result->total = count;
        result->directories = directories;
        result->complete = view_complete;
        selection_t select = {job, result, first, limit, first, -1};
        while (select.seen < count && result->count < limit)
        {
            catalog_item_t item;
            if (!gallery_file_io(view_fd, &item, sizeof(item), false, job->serial, session))
            { view_valid = false; result->error_code = GALLERY_STATUS_SCAN; return false; }
            if (!select_item(&item, &select)) return false;
        }
        result->directories = directories;
    }
    else
    {
        gallery_file_close(view_fd);
        view_fd = -1;
        view_valid = false;
        ++view_epoch;
        location.valid = false;
        selection_t select = {job, result, first, limit, 0, -1};
        if (catalog_fd >= 0) select.output = gallery_file_open(GALLERY_STORE "/view", O_CREAT | O_TRUNC | O_RDWR, session);
        bool ok = select_pass(&select);
        if (!ok)
        {
            gallery_file_close(select.output);
            if (!result->error_code) result->error_code = current(job) ? GALLERY_STATUS_SCAN : GALLERY_STATUS_REMOVED;
            return false;
        }
        count = result->total = select.seen;
        directories = result->directories;
        view_fd = select.output;
        view_valid = view_fd >= 0;
        result->complete = catalog_fd < 0 || ((job->recursive || job->kind == GALLERY_ALBUMS) ? !scanning && !scan_error : result->complete);
        view_complete = result->complete;
        view_revision = catalog_revision;
        recursive = job->recursive;
        albums = job->kind == GALLERY_ALBUMS;
        strcpy(view_root, job->path);
        if (job->anchor[0] && result->located) first = result->index;
        if (job->anchor[0] && !result->located) result->count = 0;
        if (limit > 1 && result->complete && ((first >= count && first) || (job->anchor[0] && !result->located)))
        {
            gallery_job_t retry = *job;
            retry.anchor[0] = 0;
            if (first >= count) retry.first = count ? (count - 1) / GALLERY_ROWS * GALLERY_ROWS : 0;
            result->count = result->directories = 0;
            return select_page(&retry, result, limit);
        }
    }
    result->index = first;
    result->more = first + result->count < result->total;
    result->catalog_revision = catalog_revision;
    result->catalog_generation = catalog_generation;
    if (scan_error && !result->complete) result->error_code = scan_error;
    if (catalog_fd >= 0 && !scanning && !result->complete && !result->error_code)
        result->error_code = GALLERY_STATUS_SCAN;
    if (!current(job)) { result->error_code = GALLERY_STATUS_REMOVED; return false; }
    return true;
}

void gallery_catalog_page(const gallery_job_t *job, gallery_result_t *result)
{
    (void)select_page(job, result, GALLERY_ROWS);
}

bool gallery_catalog_picture(const gallery_job_t *job, gallery_result_t *result, gallery_job_t *image)
{
    if (!select_page(job, result, 1)) return false;
    if (!result->count || result->entries[0].directory)
    { result->error_code = GALLERY_STATUS_UNAVAILABLE; return false; }
    memcpy(image->path, result->entries[0].path, sizeof(image->path));
    memcpy(result->path, result->entries[0].path, sizeof(result->path));
    return true;
}

void gallery_catalog_locate(const gallery_job_t *job, gallery_result_t *result)
{
    if (!gallery_sd_path(job->anchor)) { result->error_code = GALLERY_STATUS_PATH; return; }
    if (!catalog_ready(job, result)) return;
    if (catalog_fd < 0) { (void)readonly_page(job, result); return; }
    gallery_job_t view = *job;
    view.kind = GALLERY_SCAN;
    view.first = 0;
    view.anchor[0] = 0;
    if (!select_page(&view, result, 0)) return;
    if (view_fd < 0) {
        selection_t select = {job, result, 0, 0, 0, -1};
        if (!select_pass(&select) && !result->error_code)
            result->error_code = current(job) ? GALLERY_STATUS_SCAN : GALLERY_STATUS_REMOVED;
        if (!result->located) result->previous[0] = 0;
        return;
    }
    unsigned index = 0;
    bool hinted = false;
    if (location.valid && location.epoch == view_epoch)
        for (unsigned i = 0; i < 3; ++i)
            if (location.path[i][0] && !strcmp(location.path[i], job->anchor)) {
                index = location.index + i - 1;
                hinted = true;
                break;
            }
    if (!rewind_file(view_fd, index, job)) goto failed;
    catalog_item_t item;
    for (; index < count; ++index) {
        if (!gallery_file_io(view_fd, &item, sizeof(item), false, job->serial, session)) goto failed;
        if (!strcmp(item.path, job->anchor)) { result->located = true; break; }
        if (hinted) goto failed;
    }
    if (!result->located) return;
    result->index = index;
    memcpy(result->path, job->anchor, sizeof(result->path));
    if (index) {
        if (!rewind_file(view_fd, index - 1, job) || !gallery_file_io(view_fd, &item, sizeof(item), false, job->serial, session)) goto failed;
        memcpy(result->previous, item.path, sizeof(result->previous));
    }
    if (index + 1 < count) {
        if (!rewind_file(view_fd, index + 1, job) || !gallery_file_io(view_fd, &item, sizeof(item), false, job->serial, session)) goto failed;
        memcpy(result->next, item.path, sizeof(result->next));
    }
    location.valid = true;
    location.epoch = view_epoch;
    location.index = index;
    memcpy(location.path[0], result->previous, sizeof(location.path[0]));
    memcpy(location.path[1], result->path, sizeof(location.path[1]));
    memcpy(location.path[2], result->next, sizeof(location.path[2]));
    return;
failed:
    location.valid = false;
    result->error_code = current(job) ? GALLERY_STATUS_SCAN : GALLERY_STATUS_REMOVED;
}
