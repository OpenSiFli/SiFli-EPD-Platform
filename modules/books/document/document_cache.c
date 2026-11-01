/* SPDX-License-Identifier: Apache-2.0 */
#include "storage.h"
#include "epd_memory.h"
#include "document_cache.h"
#include "document.h"
#include "services/storage_file.h"
#include "../file_version.h"
#include "../../../components/compute_reading_data/include/epd_reading_data.h"
#include <rthw.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

#define CACHE_MAGIC 0x434f444du
#define CACHE_VERSION 3u
#define CACHE_PREFIX "dc4-"
#define CACHE_IO_CHUNK 4096u
#define CACHE_RECLAIM_ENTRIES 512u
#define CACHE_RECLAIM_METADATA 16u

/* Frame v3 payload; fixed-width fields, zeroed padding. */
typedef struct {
    uint32_t complete, volume, format, size, modified, conversion;
    uint32_t image_width, image_height, end, identity, dependencies, lastuse;
    char source[BOOKSHELF_PATH_MAX], title[BOOKSHELF_NAME_MAX];
} cache_meta_t;
typedef struct { uint32_t volume, present, size, modified, length; } cache_dep_t;
typedef struct { uint32_t magic, version, size, checksum; } cache_frame_t;
typedef struct { uint32_t revision[STORAGE_COUNT], session; } cache_epoch_t;
typedef char cache_meta_wire_size[(sizeof(cache_meta_t) == 48u + BOOKSHELF_PATH_MAX + BOOKSHELF_NAME_MAX) ? 1 : -1];

struct document_cache {
    struct document_cache *next;
    cache_meta_t source;
    char directory[STORAGE_PATH_MAX], base[STORAGE_PATH_MAX];
    char binary[STORAGE_PATH_MAX], meta[STORAGE_PATH_MAX], index[STORAGE_PATH_MAX];
    uint8_t *dependencies;
    book_file_version_t *dependency_versions;
    book_file_version_t source_version;
    book_file_version_t meta_version;
    char source_path[BOOKSHELF_PATH_MAX];
    size_t dependency_bytes;
    uint32_t generation, identity, dependency_count;
    uint16_t root_length;
    uint8_t reclaim_next;
    bool prepared, observed, observing, dependencies_ok, invalid, explicit_identity;
    bool cancelled, retiring, shared_directory;
    epd_reading_data_t data;
};
static document_cache_t *active;
static bool reuse_disabled;
/* Keep verified metadata and dependency tokens across document closes. The
 * payload, tokens and path share one budget; tokens are never persisted. */
#define DEPENDENCY_MEMORY_BUDGET (128u * 1024u)
static struct {
    void *memory;
    size_t bytes, charge;
    uint32_t count;
    uint64_t used;
    book_file_version_t version;
} dependency_cache[4];
static uint64_t dependency_use;
static cache_meta_t *recalled_metadata(document_cache_t *, const char *, size_t *, book_file_version_t *);

/* A failed CLOSE retains its client for the next signature call or shutdown.
 * Slots also bound concurrent standalone signatures without blocking a caller
 * that already owns a document compute session. */
static struct { epd_reading_data_t data; bool busy; } signature_slots[4];

static bool signature_update(void *user, const void *data, size_t bytes, uint32_t *value)
{
    epd_reading_data_t *client = user;
    if (!*client && epd_reading_data_create(EPD_COMPUTE_BACKGROUND, client) != EPD_COMPUTE_OK) return false;
    return epd_reading_data_try_open(*client) == EPD_COMPUTE_OK &&
        epd_reading_data_signature_update(*client, data, bytes, value);
}

bool document_cache_file_signature(const char *path, uint32_t *signature)
{
    if (!path || !signature) return false;
    unsigned slot;
    rt_base_t key = rt_hw_interrupt_disable();
    for (slot = 0; slot < 4 && signature_slots[slot].busy; ++slot) {}
    if (slot == 4) { rt_hw_interrupt_enable(key); return false; }
    signature_slots[slot].busy = true;
    rt_hw_interrupt_enable(key);
    epd_reading_data_t *client = &signature_slots[slot].data;
    uint32_t value;
    bool ok = epd_reading_data_close(client) == EPD_COMPUTE_OK &&
        storage_file_signature_with(path, &value, signature_update, client);
    if (epd_reading_data_close(client) != EPD_COMPUTE_OK) ok = false;
    key = rt_hw_interrupt_disable();
    signature_slots[slot].busy = false;
    rt_hw_interrupt_enable(key);
    if (ok) *signature = value;
    return ok;
}

void document_cache_cancel(document_cache_t *c)
{
    if (!c) return;
    rt_base_t key = rt_hw_interrupt_disable();
    c->cancelled = true;
    if (c->data) epd_reading_data_cancel(c->data);
    rt_hw_interrupt_enable(key);
}

static bool cache_stopped(const document_cache_t *c)
{
    rt_base_t key = rt_hw_interrupt_disable();
    bool stopped = c->cancelled;
    rt_hw_interrupt_enable(key);
    return stopped;
}

static bool cache_data_open(document_cache_t *c)
{
    if (cache_stopped(c)) return false;
    if (!c->data) {
        epd_reading_data_t created = NULL;
        if (epd_reading_data_create(EPD_COMPUTE_BACKGROUND, &created) != EPD_COMPUTE_OK) return false;
        rt_base_t key = rt_hw_interrupt_disable();
        c->data = created;
        if (c->cancelled) epd_reading_data_cancel(created);
        rt_hw_interrupt_enable(key);
    }
    return epd_reading_data_try_open(c->data) == EPD_COMPUTE_OK && !cache_stopped(c);
}
static bool cache_hash_update(void *user, const void *data, size_t bytes, uint32_t *value)
{
    document_cache_t *c = user;
    return cache_data_open(c) &&
        epd_reading_data_signature_update(c->data, data, bytes, value) && !cache_stopped(c);
}
static bool metadata_check(document_cache_t *c, const void *data, size_t bytes, uint32_t *checksum)
{
    epd_reading_cache_spec_t spec = {BOOKSHELF_PATH_MAX, BOOKSHELF_NAME_MAX,
        c->explicit_identity ? EPD_READING_CACHE_EXPLICIT_ID : 0};
    return bytes <= DOCUMENT_CACHE_METADATA_MAX && cache_data_open(c) &&
        epd_reading_data_cache_check(c->data, data, bytes, &spec, checksum) == EPD_COMPUTE_OK && !cache_stopped(c);
}

/* The worker has returned from its hash before detaching the cancel target. */
static bool cache_data_close(document_cache_t *c)
{
    rt_base_t key = rt_hw_interrupt_disable();
    epd_reading_data_t data = c->data;
    c->data = NULL;
    rt_hw_interrupt_enable(key);
    if (epd_reading_data_close(&data) == EPD_COMPUTE_OK) return true;
    key = rt_hw_interrupt_disable();
    c->data = data;
    if (c->cancelled) epd_reading_data_cancel(data);
    rt_hw_interrupt_enable(key);
    return false;
}

void document_cache_release_memory(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        rt_base_t key = rt_hw_interrupt_disable();
        bool available = !signature_slots[i].busy;
        if (available) signature_slots[i].busy = true;
        rt_hw_interrupt_enable(key);
        if (!available) continue;
        (void)epd_reading_data_close(&signature_slots[i].data);
        key = rt_hw_interrupt_disable(); signature_slots[i].busy = false; rt_hw_interrupt_enable(key);
    }
    for (document_cache_t *c = active, *next; c; c = next) {
        next = c->next;
        if (c->retiring) document_cache_destroy(c);
    }
    for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i)
        epd_app_free(dependency_cache[i].memory);
    memset(dependency_cache, 0, sizeof(dependency_cache));
    dependency_use = 0;
}

static void *allocate(size_t size) { return epd_app_alloc(size, EPD_APP_PSRAM); }
bool document_cache_cleanup_pending(void)
{
    storage_lock();
    rt_base_t key = rt_hw_interrupt_disable();
    bool pending = false;
    for (unsigned i = 0; i < 4; ++i)
        pending |= signature_slots[i].busy || signature_slots[i].data != NULL;
    for (document_cache_t *c = active; c; c = c->next)
        pending |= c->retiring || c->data != NULL;
    rt_hw_interrupt_enable(key);
    storage_unlock();
    return pending;
}
static uint32_t hash(uint32_t value, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size--) value = (value ^ *p++) * 16777619u;
    return value;
}
static uint32_t source_hash(const cache_meta_t *m, uint32_t seed)
{
    uint32_t fields[] = {m->volume, m->format, m->size, m->modified,
                        m->conversion, m->image_width, m->image_height};
    return hash(hash(seed, fields, sizeof(fields)), m->source, strlen(m->source));
}
static bool content_hash(document_cache_t *c, const cache_meta_t *m, const void *deps, size_t bytes, uint32_t *out)
{
    uint32_t value = source_hash(m, 2166136261u);
    value = hash(value, &m->end, sizeof(m->end));
    if (bytes && !cache_hash_update(c, deps, bytes, &value)) return false;
    if (cache_stopped(c)) return false;
    *out = value ? value : 1;
    return true;
}
static bool relative_valid(const char *path)
{
    if (!path || !*path) return false;
    const char *start = path;
    for (const char *p = path; ; ++p)
    {
        if (*p && ((unsigned char)*p < 32 || *p == '\\' || *p == ':')) return false;
        if (*p == '/' || !*p)
        {
            size_t n = (size_t)(p - start);
            if (!n || (n == 1 && start[0] == '.') ||
                (n == 2 && start[0] == '.' && start[1] == '.')) return false;
            start = p + 1;
        }
        if (!*p) break;
    }
    return true;
}
static bool split_path(const char *path, uint32_t *volume, const char **relative)
{
    const char *roots[] = {STORAGE_FLASH_ROOT, STORAGE_SD_ROOT};
    if (!path) return false;
    for (unsigned i = 0; i < 2; ++i)
    {
        size_t n = strlen(roots[i]);
        if (!strncmp(path, roots[i], n) && path[n] == '/' && relative_valid(path + n + 1))
        { *volume = i; *relative = path + n + 1; return true; }
    }
    return false;
}
static bool path_valid(const document_cache_t *c, const char *path)
{
    return storage_path_available(path) &&
        (strcmp(storage_path_root(path), STORAGE_SD_ROOT) ||
         (c->generation == storage_card_session() && !storage_changing()));
}
static bool valid(const document_cache_t *c)
{
    return c && path_valid(c, c->base) &&
        storage_session_valid((storage_volume_t)c->source.volume, c->generation) &&
        (c->source.volume != STORAGE_SD || !storage_changing());
}
static bool same_source(const cache_meta_t *a, const cache_meta_t *b)
{
    return a->volume == b->volume && a->format == b->format && a->size == b->size &&
        a->modified == b->modified && a->conversion == b->conversion &&
        a->image_width == b->image_width && a->image_height == b->image_height &&
        !strcmp(a->source, b->source);
}
static bool same_base_active(const document_cache_t *c)
{
    for (document_cache_t *p = active; p; p = p->next)
        if (p != c && !strcmp(p->base, c->base) && valid(p)) return true;
    return false;
}
static bool name_paths(document_cache_t *c)
{
    uint32_t hash = source_hash(&c->source, 2166136261u);
    size_t root = strlen(c->directory);
    if (root > UINT16_MAX || root + 5 >= sizeof(c->directory)) return false;
    c->root_length = (uint16_t)root;
    c->reclaim_next = (uint8_t)(hash >> 24);
    /* Both routing levels have at most sixteen children. The complete key
     * remains in the basename and metadata, including chapter/source anchor. */
    snprintf(c->directory + root, sizeof(c->directory) - root, "/%x/%x",
             (unsigned)(hash >> 28), (unsigned)((hash >> 24) & 15));
    int n = snprintf(c->base, sizeof(c->base), "%s/" CACHE_PREFIX "%08lx%08lx", c->directory,
                     (unsigned long)hash,
                     (unsigned long)source_hash(&c->source, 0x9e3779b9u));
    if (n < 0 || (size_t)n + sizeof(".meta.old") >= sizeof(c->base)) return false;
    size_t length = (size_t)n;
    memcpy(c->binary, c->base, length); memcpy(c->binary + length, ".bin", sizeof(".bin"));
    memcpy(c->meta, c->base, length); memcpy(c->meta + length, ".meta", sizeof(".meta"));
    memcpy(c->index, c->base, length); memcpy(c->index + length, ".idx", sizeof(".idx"));
    return true;
}
bool document_cache_directory(const bookshelf_item_t *file, const char *app_id,
                              char *directory, size_t capacity)
{
    if (!file || !memchr(file->path, 0, sizeof(file->path)) ||
        !storage_app_path(directory, capacity, app_id, STORAGE_APP_CACHE, "doccache")) return false;
    if (!strcmp(storage_path_root(file->path), STORAGE_SD_ROOT))
    {
        int n = snprintf(directory, capacity, STORAGE_SD_ROOT "/.epd/cache/%s/doccache", app_id);
        if (n <= 0 || (size_t)n >= capacity) { directory[0] = 0; return false; }
    }
    return storage_path_available(directory);
}

document_cache_t *document_cache_create(const bookshelf_item_t *file)
{
    return document_cache_create_source(file, "books", NULL, NULL);
}
document_cache_t *document_cache_create_source(const bookshelf_item_t *file,
    const char *app_id, const char *directory, const char *identity)
{
    if (!file || !memchr(file->path, 0, sizeof(file->path)) ||
        !app_id || !*app_id ||
        (!identity && file->format != BOOK_FORMAT_MARKDOWN && file->format != BOOK_FORMAT_EPUB &&
         file->format != BOOK_FORMAT_SPLIT_EPUB && file->format != BOOK_FORMAT_MOBI &&
         file->format != BOOK_FORMAT_TXT)) return NULL;
    document_cache_t *c = allocate(sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    const char *relative;
    storage_lock();
    c->generation = storage_card_session();
    c->explicit_identity = identity != NULL;
    c->shared_directory = directory != NULL;
    bool ok = split_path(file->path, &c->source.volume, &relative);
    if (ok)
    {
        memcpy(c->source_path, file->path, strlen(file->path) + 1);
        if (identity)
        {
            int n = snprintf(c->source.source, sizeof(c->source.source), "@%s:%s", app_id, identity);
            ok = *identity && n > 0 && (size_t)n < sizeof(c->source.source);
        }
        else memcpy(c->source.source, relative, strlen(relative) + 1);
        c->source.format = file->format; c->source.size = file->size;
        c->source.modified = file->modified;
        c->source.conversion = DOCUMENT_CONVERSION_VERSION;
        c->source.image_width = DOCUMENT_IMAGE_WIDTH;
        c->source.image_height = DOCUMENT_IMAGE_HEIGHT;
        if (directory)
        {
            int n = snprintf(c->directory, sizeof(c->directory), "%s/doccache", directory);
            ok = ok && *directory && n > 0 && (size_t)n < sizeof(c->directory);
        }
        else ok = ok && document_cache_directory(file, app_id, c->directory, sizeof(c->directory));
        ok = ok && name_paths(c) && valid(c);
    }
    if (ok)
    {
        for (document_cache_t *p = active; p; p = p->next)
            if (!strcmp(p->base, c->base) && p->invalid && valid(p)) c->invalid = true;
        c->next = active; active = c;
    }
    storage_unlock();
    if (!ok) { epd_app_free(c); return NULL; }
    return c;
}
/* Collision recovery scans only one leaf, never the legacy tree. A complete
 * bounded census chooses a vacant 8.3 name even if clocks repeat after reboot.
 * O_EXCL remains mandatory after this advisory lookup. Caller holds storage_lock. */
bool document_cache_temporary_number(const char *directory, const char *extension,
    uint32_t *number, bool (*stopped)(void *), void *user)
{
    if (stopped(user) || !storage_path_available(directory)) return false;
    uint8_t used[128] = {0};
    uint32_t prefix = *number & ~1023u, generation = storage_card_session();
    DIR *dir = opendir(directory);
    bool complete = false;
    for (unsigned count = 0; dir && count < 512 && !stopped(user) &&
         storage_path_available(directory) && generation == storage_card_session(); ++count)
    {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { complete = errno == 0; break; }
        const char *name = entry->d_name;
        if (strlen(name) != 12 || name[8] != '.' || (name[9] | 32) != extension[1] ||
            (name[10] | 32) != extension[2] || (name[11] | 32) != extension[3]) continue;
        uint32_t value = 0;
        unsigned i;
        for (i = 0; i < 8; ++i) {
            unsigned char ch = (unsigned char)name[i];
            if (ch >= 'A' && ch <= 'F') ch += 'a' - 'A';
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) break;
            value = value * 16u + (ch <= '9' ? ch - '0' : ch - 'a' + 10u);
        }
        if (i == 8 && (value & ~1023u) == prefix) used[(value & 1023u) >> 3] |= 1u << (value & 7u);
    }
    if (dir) closedir(dir);
    if (!complete || stopped(user) || !storage_path_available(directory) || generation != storage_card_session()) return false;
    for (unsigned i = 1; i <= 1024; ++i) {
        unsigned slot = (*number + i) & 1023u;
        if (!(used[slot >> 3] & (1u << (slot & 7u)))) { *number = prefix | slot; return true; }
    }
    return false;
}
static bool temporary_stopped(void *user)
{
    document_cache_t *c = user;
    return !valid(c) || cache_stopped(c);
}
const char *document_cache_bucket(const document_cache_t *c) { return c ? c->directory : NULL; }
const char *document_cache_base(const document_cache_t *c) { return c ? c->base : NULL; }
const char *document_cache_binary(const document_cache_t *c) { return c ? c->binary : NULL; }
const char *document_cache_meta(const document_cache_t *c) { return c ? c->meta : NULL; }
const char *document_cache_index(const document_cache_t *c) { return c ? c->index : NULL; }
uint32_t document_cache_identity(const document_cache_t *c)
{
    return c && !c->invalid ? c->identity : 0;
}
static bool read_exact(int fd, void *data, size_t size)
{
    uint8_t *p = data;
    while (size)
    {
        int n = read(fd, p, size > INT_MAX ? INT_MAX : size);
        if (n <= 0) return false;
        p += n; size -= (size_t)n;
    }
    return true;
}
static bool dep_next(const uint8_t **at, size_t *remaining, cache_dep_t *dep, const char **path)
{
    if (*remaining < sizeof(*dep)) return false;
    memcpy(dep, *at, sizeof(*dep));
    *at += sizeof(*dep); *remaining -= sizeof(*dep);
    if (dep->volume >= STORAGE_COUNT || dep->present > 1 || !dep->length ||
        dep->length > BOOKSHELF_PATH_MAX || dep->length > *remaining || (*at)[dep->length - 1] || memchr(*at, 0, dep->length - 1)) return false;
    *path = (const char *)*at;
    *at += dep->length; *remaining -= dep->length;
    return true;
}
/* Storage I/O is serialized in bounded chunks; decoding runs without its lock. */
static cache_meta_t *load_meta(document_cache_t *c, const char *path, size_t *bytes,
                               book_file_version_t *version)
{
    cache_meta_t *remembered = recalled_metadata(c, path, bytes, version);
    if (remembered) return remembered;
    cache_frame_t frame;
    struct stat before, after;
    storage_lock();
    bool ok = valid(c) && !cache_stopped(c) && storage_file_recover(path);
    *version = book_file_version(path);
    int fd = ok && book_file_version_valid(path, version) ? open(path, O_RDONLY, 0) : -1;
    ok = fd >= 0 && fstat(fd, &before) == 0 && S_ISREG(before.st_mode) &&
        read_exact(fd, &frame, sizeof(frame)) && frame.magic == CACHE_MAGIC && frame.version == CACHE_VERSION &&
        frame.size >= sizeof(cache_meta_t) && frame.size <= DOCUMENT_CACHE_METADATA_MAX &&
        before.st_size == (int64_t)sizeof(frame) + frame.size;
    storage_unlock();
    cache_meta_t *m = ok ? allocate(frame.size) : NULL;
    if (!m) ok = false;
    size_t offset = 0;
    while (ok && offset < frame.size) {
        size_t n = frame.size - offset < CACHE_IO_CHUNK ? frame.size - offset : CACHE_IO_CHUNK;
        storage_lock();
        ok = valid(c) && !cache_stopped(c) && book_file_version_valid(path, version) &&
            read_exact(fd, (uint8_t *)m + offset, n);
        storage_unlock();
        if (ok) { offset += n; if (offset < frame.size) rt_thread_yield(); }
    }
    storage_lock();
    ok = ok && valid(c) && book_file_version_valid(path, version) && fstat(fd, &after) == 0 &&
        after.st_size == before.st_size && after.st_mtime == before.st_mtime;
    if (fd >= 0 && close(fd)) ok = false;
    storage_unlock();
    uint32_t checksum;
    if (ok) ok = metadata_check(c, m, frame.size, &checksum) && checksum == frame.checksum;
    storage_lock();
    ok = ok && valid(c) && !cache_stopped(c) && book_file_version_valid(path, version);
    storage_unlock();
    if (!ok) { epd_app_free(m); return NULL; }
    *bytes = frame.size;
    return m;
}
static cache_epoch_t epoch_snapshot(void)
{
    cache_epoch_t epoch;
    storage_lock();
    epoch.session = storage_card_session();
    for (unsigned i = 0; i < STORAGE_COUNT; ++i) epoch.revision[i] = storage_volume_revision((storage_volume_t)i);
    storage_unlock();
    return epoch;
}
/* Constant-time final guard, called with storage_lock held. */
static bool epoch_valid(const cache_epoch_t *epoch)
{
    if (epoch->session != storage_card_session() || storage_changing()) return false;
    for (unsigned i = 0; i < STORAGE_COUNT; ++i)
        if (epoch->revision[i] != storage_volume_revision((storage_volume_t)i)) return false;
    return true;
}
static char *full_path(uint32_t volume, const char *relative)
{
    const char *root = volume == STORAGE_SD ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT;
    size_t a = strlen(root), b = strlen(relative);
    if (b > SIZE_MAX - a - 2) return NULL;
    char *path = allocate(a + b + 2);
    if (path) { memcpy(path, root, a); path[a] = '/'; memcpy(path + a + 1, relative, b + 1); }
    return path;
}
static bool snapshot(document_cache_t *c, const char *path, cache_dep_t *dep,
                     book_file_version_t *version)
{
    const char *relative;
    struct stat st;
    memset(dep, 0, sizeof(*dep));
    if (!valid(c) || !path_valid(c, path) || !split_path(path, &dep->volume, &relative)) return false;
    size_t length = strlen(relative) + 1;
    if (length > BOOKSHELF_PATH_MAX) return false;
    dep->length = (uint32_t)length;
    storage_lock();
    *version = book_file_version(path);
    bool ok = book_file_version_valid(path, version);
    errno = 0;
    if (ok && stat(path, &st))
        ok = errno == ENOENT || errno == -ENOENT || errno == ENOTDIR || errno == -ENOTDIR;
    else if (ok)
    {
        ok = S_ISREG(st.st_mode) && st.st_size >= 0 && (uint64_t)st.st_size <= UINT32_MAX;
        if (ok) { dep->present = 1; dep->size = (uint32_t)st.st_size; dep->modified = (uint32_t)st.st_mtime; }
    }
    storage_unlock();
    if (ok && dep->present)
        ok = storage_file_signature_with(path, &dep->modified, cache_hash_update, c);
    return ok && valid(c) && path_valid(c, path) && book_file_version_valid(path, version);
}
static bool check_dependencies(document_cache_t *c, const void *data, size_t bytes, uint32_t count,
                               book_file_version_t *versions, bool remembered)
{
    const uint8_t *at = data;
    size_t remaining = bytes;
    for (uint32_t i = 0; i < count; ++i)
    {
        cache_dep_t stored, current; const char *relative;
        if (!dep_next(&at, &remaining, &stored, &relative)) return false;
        char *path = full_path(stored.volume, relative);
        if (!path) return false;
        bool ok = remembered && path_valid(c, path) && book_file_version_valid(path, &versions[i]);
        if (!ok) ok = snapshot(c, path, &current, &versions[i]) && !memcmp(&stored, &current, sizeof(stored));
        epd_app_free(path);
        if (!ok) return false;
    }
    return !remaining && valid(c);
}
static bool versions_valid(const document_cache_t *c, const void *data, size_t bytes, uint32_t count,
                           const book_file_version_t *versions)
{
    const uint8_t *at = data;
    size_t remaining = bytes;
    for (uint32_t i = 0; i < count; ++i)
    {
        cache_dep_t dep; const char *relative;
        if (!dep_next(&at, &remaining, &dep, &relative)) return false;
        char *path = full_path(dep.volume, relative);
        if (!path) return false;
        storage_lock();
        bool ok = !cache_stopped(c) && book_file_version_valid(path, &versions[i]) && path_valid(c, path);
        storage_unlock();
        epd_app_free(path);
        if (!ok) return false;
    }
    return !remaining && valid(c);
}
/* Worker-owned LRU, also released by the worker at shutdown. Bulk comparisons
 * and allocation never hold the storage lock. */
static bool recalled_versions(const char *path, const book_file_version_t *version,
                               const void *data, size_t bytes, uint32_t count, book_file_version_t *versions)
{
    for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i)
        if (dependency_cache[i].memory && dependency_cache[i].bytes == bytes && dependency_cache[i].count == count)
        {
            book_file_version_t *saved = dependency_cache[i].memory;
            const cache_meta_t *metadata = (const void *)(saved + count);
            bool same = !memcmp(version, &dependency_cache[i].version, sizeof(*version)) &&
                !strcmp((const char *)(metadata + 1) + bytes, path);
            if (!same && memcmp(metadata + 1, data, bytes)) continue;
            memcpy(versions, saved, (size_t)count * sizeof(*versions));
            dependency_cache[i].used = ++dependency_use;
            return true;
        }
    return false;
}
static cache_meta_t *recalled_metadata(document_cache_t *c, const char *path, size_t *bytes,
                                       book_file_version_t *version)
{
    for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i) {
        if (!dependency_cache[i].memory) continue;
        cache_meta_t *saved = (void *)((book_file_version_t *)dependency_cache[i].memory + dependency_cache[i].count);
        size_t length = sizeof(*saved) + dependency_cache[i].bytes;
        if (strcmp((const char *)saved + length, path)) continue;
        storage_lock();
        bool ok = valid(c) && !cache_stopped(c) && book_file_version_valid(path, &dependency_cache[i].version);
        storage_unlock();
        if (!ok) return NULL;
        cache_meta_t *copy = allocate(length);
        if (!copy) return NULL;
        memcpy(copy, saved, length);
        *bytes = length; *version = dependency_cache[i].version;
        dependency_cache[i].used = ++dependency_use;
        return copy;
    }
    return NULL;
}
static void remember_metadata(const char *path, const book_file_version_t *version,
                              const cache_meta_t *m, size_t bytes, const book_file_version_t *versions)
{
    uint32_t count = m->dependencies;
    size_t path_bytes = strlen(path) + 1;
    if (bytes > DOCUMENT_CACHE_METADATA_MAX || bytes + path_bytes > DEPENDENCY_MEMORY_BUDGET ||
        count > (DEPENDENCY_MEMORY_BUDGET - bytes - path_bytes) / sizeof(*versions)) return;
    size_t charge = bytes + path_bytes + (size_t)count * sizeof(*versions), total = 0;
    unsigned slot = 0;
    for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i)
    {
        total += dependency_cache[i].charge;
        if (!dependency_cache[i].memory || dependency_cache[i].used < dependency_cache[slot].used) slot = i;
    }
    /* Replace the same metadata path, so touch does not retain older copies. */
    for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i)
        if (dependency_cache[i].memory) {
            cache_meta_t *saved = (void *)((book_file_version_t *)dependency_cache[i].memory + dependency_cache[i].count);
            if (strcmp((const char *)(saved + 1) + dependency_cache[i].bytes, path)) continue;
            slot = i;
            if (dependency_cache[i].charge == charge) {
                if (count) memcpy(dependency_cache[i].memory, versions, (size_t)count * sizeof(*versions));
                if (memcmp(&dependency_cache[i].version, version, sizeof(*version)))
                    memcpy((book_file_version_t *)dependency_cache[i].memory + count, m, bytes);
                dependency_cache[i].bytes = bytes - sizeof(*m); dependency_cache[i].count = count;
                memcpy((uint8_t *)dependency_cache[i].memory + charge - path_bytes, path, path_bytes);
                dependency_cache[i].version = *version;
                dependency_cache[i].used = ++dependency_use;
                return;
            }
            break;
        }
    while (charge > DEPENDENCY_MEMORY_BUDGET - total || dependency_cache[slot].memory)
    {
        total -= dependency_cache[slot].charge;
        epd_app_free(dependency_cache[slot].memory);
        memset(&dependency_cache[slot], 0, sizeof(dependency_cache[slot]));
        if (charge <= DEPENDENCY_MEMORY_BUDGET - total) break;
        for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i)
            if (dependency_cache[i].memory && (!dependency_cache[slot].memory ||
                dependency_cache[i].used < dependency_cache[slot].used)) slot = i;
    }
    book_file_version_t *memory = allocate(charge);
    if (!memory) return;
    if (count) memcpy(memory, versions, (size_t)count * sizeof(*versions));
    memcpy(memory + count, m, bytes);
    memcpy((uint8_t *)memory + charge - path_bytes, path, path_bytes);
    dependency_cache[slot].memory = memory; dependency_cache[slot].bytes = bytes - sizeof(*m);
    dependency_cache[slot].charge = charge; dependency_cache[slot].count = count;
    dependency_cache[slot].version = *version;
    dependency_cache[slot].used = ++dependency_use;
}
static bool binary_length(const document_cache_t *c, uint32_t end)
{
    struct stat st;
    return end <= INT32_MAX && valid(c) && storage_file_recover(c->binary) &&
        !stat(c->binary, &st) && S_ISREG(st.st_mode) && st.st_size == (int64_t)end;
}
static uint32_t lastuse(void)
{
    time_t now = time(NULL);
    return now > 0 && (uint64_t)now <= UINT32_MAX ? (uint32_t)now : 0;
}
bool document_cache_lookup(document_cache_t *c, char *title, size_t capacity, uint32_t *end)
{
    if (!c || !title || !capacity || !end) return false;
    storage_lock();
    bool ok = valid(c) && !reuse_disabled && !c->invalid && !c->prepared && !cache_stopped(c);
    for (document_cache_t *p = active; ok && p; p = p->next)
        if (p != c && !strcmp(p->base, c->base) && valid(p) && (p->invalid || p->prepared)) ok = false;
    storage_unlock();
    size_t bytes = 0;
    book_file_version_t meta_version;
    cache_meta_t *m = ok ? load_meta(c, c->meta, &bytes, &meta_version) : NULL;
    ok = m && m->complete && same_source(m, &c->source) && strlen(m->title) < capacity;
    book_file_version_t *versions = ok && m->dependencies ? allocate((size_t)m->dependencies * sizeof(*versions)) : NULL;
    if (ok && m->dependencies && !versions) ok = false;
    bool remembered = ok && m->dependencies && recalled_versions(c->meta, &meta_version,
        m + 1, bytes - sizeof(*m), m->dependencies, versions);
    /* Frame verification already covers the persisted content identity. */
    if (ok) ok = m->identity != 0;
    if (ok) ok = check_dependencies(c, m + 1, bytes - sizeof(*m), m->dependencies, versions, remembered);
    if (!cache_data_close(c)) ok = false;
    cache_epoch_t epoch = epoch_snapshot();
    if (ok) ok = versions_valid(c, m + 1, bytes - sizeof(*m), m->dependencies, versions);
    storage_lock();
    ok = ok && epoch_valid(&epoch) && !cache_stopped(c) && !reuse_disabled && !c->invalid && !c->prepared &&
        book_file_version_valid(c->meta, &meta_version) && binary_length(c, m->end);
    for (document_cache_t *p = active; ok && p; p = p->next)
        if (p != c && !strcmp(p->base, c->base) && valid(p) && (p->invalid || p->prepared)) ok = false;
    if (ok) { memcpy(title, m->title, strlen(m->title) + 1); *end = m->end; c->identity = m->identity; }
    else c->identity = 0;
    storage_unlock();
    if (ok) remember_metadata(c->meta, &meta_version, m, bytes, versions);
    epd_app_free(m); epd_app_free(versions);
    return ok;
}
enum { SAVE_PREPARE, SAVE_INVALIDATE, SAVE_COMMIT, SAVE_TOUCH };
static bool write_exact(int fd, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size) {
        int n = write(fd, p, size);
        if (n <= 0) return false;
        p += n; size -= (size_t)n;
    }
    return true;
}
static bool save_allowed(const document_cache_t *c, unsigned mode, const cache_meta_t *m)
{
    if (!valid(c) || cache_stopped(c)) return false;
    if (mode == SAVE_INVALIDATE) return true;
    if (mode == SAVE_PREPARE) return !c->observing && c->dependencies_ok && !same_base_active(c);
    if (c->invalid || c->observing) return false;
    if (mode == SAVE_TOUCH) return c->identity && c->identity == m->identity && !c->prepared;
    return c->prepared && c->observed && c->dependencies_ok &&
        (c->source.format != BOOK_FORMAT_MARKDOWN || c->explicit_identity ||
         book_file_version_valid(c->source_path, &c->source_version));
}
static bool save_meta(document_cache_t *c, cache_meta_t *m, size_t bytes, unsigned mode,
                       const book_file_version_t *expected, const book_file_version_t *versions)
{
    if (bytes < sizeof(*m) || bytes > DOCUMENT_CACHE_METADATA_MAX ||
        m->dependencies > DOCUMENT_CACHE_DEPENDENCIES_MAX) return false;
    storage_lock();
    book_file_version_t original = expected ? *expected : book_file_version(c->meta);
    bool ok = save_allowed(c, mode, m) && book_file_version_valid(c->meta, &original);
    storage_unlock();
    cache_frame_t frame = {CACHE_MAGIC, CACHE_VERSION, (uint32_t)bytes, 0};
    if (ok) ok = metadata_check(c, m, bytes, &frame.checksum);
    if (!cache_data_close(c)) ok = false;
    char temporary[STORAGE_PATH_MAX];
    int fd = -1;
    static uint32_t sequence;
    storage_lock();
    ok = ok && save_allowed(c, mode, m) && book_file_version_valid(c->meta, &original);
    for (unsigned attempt = 0; ok && fd < 0 && attempt < 9; ++attempt) {
        if (temporary_stopped(c)) break;
        uint32_t number = ++sequence;
        if (attempt == 8 && !document_cache_temporary_number(c->directory, ".new", &number, temporary_stopped, c)) break;
        int n = snprintf(temporary, sizeof(temporary), "%s/%08lx.new", c->directory, (unsigned long)number);
        if (n < 0 || (size_t)n >= sizeof(temporary)) { ok = false; break; }
        fd = open(temporary, O_CREAT | O_EXCL | O_WRONLY, 0);
        if (fd < 0 && errno != EEXIST && errno != -EEXIST) break;
    }
    ok = ok && fd >= 0 && write_exact(fd, &frame, sizeof(frame));
    storage_unlock();
    size_t offset = 0;
    while (ok && offset < bytes) {
        size_t n = bytes - offset < CACHE_IO_CHUNK ? bytes - offset : CACHE_IO_CHUNK;
        storage_lock();
        ok = save_allowed(c, mode, m) && book_file_version_valid(c->meta, &original) &&
            write_exact(fd, (uint8_t *)m + offset, n);
        storage_unlock();
        if (ok) { offset += n; if (offset < bytes) rt_thread_yield(); }
    }
    storage_lock();
    if (ok) ok = valid(c) && fsync(fd) == 0;
    bool closed = fd < 0 || close(fd) == 0;
    if (!closed) ok = false;
    storage_unlock();
    cache_epoch_t epoch = epoch_snapshot();
    if (ok && m->dependencies) ok = versions && versions_valid(c, m + 1, bytes - sizeof(*m), m->dependencies, versions);
    storage_lock();
    ok = ok && epoch_valid(&epoch) && save_allowed(c, mode, m) &&
        book_file_version_valid(c->meta, &original) && (mode < SAVE_COMMIT || binary_length(c, m->end));
    if (ok) ok = storage_file_commit(temporary, c->meta);
    if (ok && mode == SAVE_PREPARE) { c->prepared = true; c->invalid = false; }
    if (ok && mode == SAVE_COMMIT) { c->identity = m->identity; c->prepared = false; }
    if (ok) c->meta_version = book_file_version(c->meta);
    if (!ok && fd >= 0 && closed && valid(c)) (void)unlink(temporary);
    storage_unlock();
    return ok;
}
bool document_cache_invalidate(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    for (document_cache_t *p = active; p; p = p->next)
        if (!strcmp(p->base, c->base) && p->generation == c->generation)
        { p->invalid = true; p->identity = 0; }
    storage_unlock();
    cache_meta_t marker = c->source; marker.lastuse = lastuse();
    bool ok = save_meta(c, &marker, sizeof(marker), SAVE_INVALIDATE, NULL, NULL);
    if (!ok) { storage_lock(); reuse_disabled = true; storage_unlock(); }
    return ok;
}
bool document_cache_prepare(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    bool ok = !c->observing && c->dependencies_ok && valid(c) && !same_base_active(c) &&
        c->dependency_bytes <= DOCUMENT_CACHE_METADATA_MAX - sizeof(cache_meta_t) &&
        c->dependency_count <= DOCUMENT_CACHE_DEPENDENCIES_MAX;
    c->prepared = false; c->identity = 0;
    storage_unlock();
    cache_meta_t marker = c->source; marker.lastuse = lastuse();
    return ok && save_meta(c, &marker, sizeof(marker), SAVE_PREPARE, NULL, NULL);
}
static void observe(void *user, const char *path, book_file_event_t event)
{
    document_cache_t *c = user;
    if (!c->dependencies_ok || cache_stopped(c) || c->source.format != BOOK_FORMAT_MARKDOWN) goto done;
    uint32_t volume; const char *relative;
    if (event == BOOK_FILE_UNAVAILABLE || !split_path(path, &volume, &relative)) goto failed;
    if (volume == c->source.volume && !strcmp(relative, c->source.source)) goto done;
    const uint8_t *at = c->dependencies;
    size_t remaining = c->dependency_bytes;
    for (uint32_t i = 0; i < c->dependency_count; ++i)
    {
        cache_dep_t previous; const char *old_path;
        if (!dep_next(&at, &remaining, &previous, &old_path)) goto failed;
        if (previous.volume == volume && !strcmp(old_path, relative))
        {
            if (previous.present != (event == BOOK_FILE_OPENED) ||
                !book_file_version_valid(path, &c->dependency_versions[i])) goto failed;
            goto done;
        }
    }
    size_t path_bytes = strlen(relative) + 1;
    if (c->dependency_count >= DOCUMENT_CACHE_DEPENDENCIES_MAX || path_bytes > BOOKSHELF_PATH_MAX ||
        c->dependency_bytes > DOCUMENT_CACHE_METADATA_MAX - sizeof(cache_meta_t) - sizeof(cache_dep_t) - path_bytes)
        goto failed;
    cache_dep_t dep;
    book_file_version_t version;
    bool captured = snapshot(c, path, &dep, &version);
    if (!captured || !c->dependencies_ok || !book_file_version_valid(path, &version) ||
        dep.present != (event == BOOK_FILE_OPENED)) goto failed;
    size_t length = dep.length;
    if (c->dependency_count >= DOCUMENT_CACHE_DEPENDENCIES_MAX || length > SIZE_MAX - sizeof(dep) ||
        c->dependency_bytes > SIZE_MAX - sizeof(dep) - length) goto failed;
    size_t bytes = c->dependency_bytes + sizeof(dep) + length;
    if (bytes > DOCUMENT_CACHE_METADATA_MAX - sizeof(cache_meta_t)) goto failed;
    if ((size_t)c->dependency_count >= SIZE_MAX / sizeof(*c->dependency_versions)) goto failed;
    book_file_version_t *versions = epd_app_realloc(c->dependency_versions,
        ((size_t)c->dependency_count + 1) * sizeof(*versions), EPD_APP_PSRAM);
    if (!versions) goto failed;
    c->dependency_versions = versions;
    uint8_t *data = epd_app_realloc(c->dependencies, bytes, EPD_APP_PSRAM);
    if (!data) goto failed;
    memcpy(data + c->dependency_bytes, &dep, sizeof(dep));
    memcpy(data + c->dependency_bytes + sizeof(dep), relative, dep.length);
    c->dependency_versions[c->dependency_count] = version;
    c->dependencies = data; c->dependency_bytes = bytes; ++c->dependency_count;
    goto done;
failed:
    c->dependencies_ok = false;
done:
    return;
}
void document_cache_observe_resource(document_cache_t *c, const char *path, int event)
{
    if (c && c->observing) observe(c, path, (book_file_event_t)event);
}

bool document_cache_observe_begin(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    bool ok = !c->observing && valid(c) && !cache_stopped(c);
    if (ok)
    {
        epd_app_free(c->dependencies); c->dependencies = NULL;
        epd_app_free(c->dependency_versions); c->dependency_versions = NULL;
        c->dependency_bytes = c->dependency_count = 0;
        c->prepared = false; c->observed = false; c->dependencies_ok = true;
        c->observing = true;
        c->source_version = book_file_version(c->source_path);
    }
    else c->dependencies_ok = false;
    storage_unlock();
    return ok;
}
void document_cache_observe_end(document_cache_t *c)
{
    if (!c) return;
    storage_lock();
    if (c->observing)
    { c->observing = false; c->observed = true; }
    storage_unlock();
    if (!cache_data_close(c)) c->dependencies_ok = false;
}
bool document_cache_commit(document_cache_t *c, const char *title, uint32_t end)
{
    if (!c || !title || strlen(title) >= BOOKSHELF_NAME_MAX ||
        c->dependency_bytes > DOCUMENT_CACHE_METADATA_MAX - sizeof(cache_meta_t) ||
        c->dependency_count > DOCUMENT_CACHE_DEPENDENCIES_MAX) return false;
    storage_lock();
    bool ok = save_allowed(c, SAVE_COMMIT, &c->source) && binary_length(c, end);
    book_file_version_t original = book_file_version(c->meta);
    storage_unlock();
    size_t bytes = sizeof(cache_meta_t) + c->dependency_bytes;
    cache_meta_t *m = ok ? allocate(bytes) : NULL;
    if (!m) ok = false;
    if (ok) {
        *m = c->source;
        m->complete = 1; m->end = end; m->lastuse = lastuse(); m->dependencies = c->dependency_count;
        memcpy(m->title, title, strlen(title) + 1);
        if (c->dependency_bytes) memcpy(m + 1, c->dependencies, c->dependency_bytes);
        ok = content_hash(c, m, m + 1, c->dependency_bytes, &m->identity) &&
            save_meta(c, m, bytes, SAVE_COMMIT, &original, c->dependency_versions);
        if (ok) remember_metadata(c->meta, &c->meta_version, m, bytes, c->dependency_versions);
    }
    if (!cache_data_close(c)) ok = false;
    epd_app_free(m);
    return ok;
}
bool document_cache_touch(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    bool ok = !c->invalid && c->identity && !cache_stopped(c);
    storage_unlock();
    size_t bytes = 0;
    book_file_version_t original;
    cache_meta_t *m = ok ? load_meta(c, c->meta, &bytes, &original) : NULL;
    ok = m && m->complete && same_source(m, &c->source) && m->identity == c->identity;
    book_file_version_t *versions = ok && m->dependencies ? allocate((size_t)m->dependencies * sizeof(*versions)) : NULL;
    if (ok && m->dependencies && !versions) ok = false;
    bool remembered = ok && m->dependencies && recalled_versions(c->meta, &original,
        m + 1, bytes - sizeof(*m), m->dependencies, versions);
    if (ok) ok = check_dependencies(c, m + 1, bytes - sizeof(*m), m->dependencies, versions, remembered);
    if (ok) {
        m->lastuse = lastuse();
        ok = save_meta(c, m, bytes, SAVE_TOUCH, &original, versions);
        if (ok) remember_metadata(c->meta, &c->meta_version, m, bytes, versions);
    }
    if (!cache_data_close(c)) ok = false;
    epd_app_free(m); epd_app_free(versions);
    return ok;
}
static bool cache_name(const char *name)
{
    if (strlen(name) != 25 || strncmp(name, CACHE_PREFIX, 4) || strcmp(name + 20, ".meta")) return false;
    for (unsigned i = 4; i < 20; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return true;
}
static bool base_busy(const char *base)
{
    for (document_cache_t *p = active; p; p = p->next)
        if (!strcmp(base, p->base) && valid(p)) return true;
    return false;
}
/* A spool or metadata temporary in an inactive bucket has no live owner.
 * Allocation-fallback spools live in loose/, outside the eviction traversal. */
static bool temporary_name(const char *name)
{
    if (strlen(name) != 12 || (strcmp(name + 8, ".tmp") && strcmp(name + 8, ".new"))) return false;
    for (unsigned i = 0; i < 8; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return true;
}
static bool bucket_busy(const char *path, size_t length)
{
    for (document_cache_t *p = active; p; p = p->next)
        if (strlen(p->directory) == length && !strncmp(path, p->directory, length) && valid(p)) return true;
    return false;
}
static bool reclaim_app(const document_cache_t *c, const cache_meta_t *m)
{
    if (!c->shared_directory) return true;
    /* Other apps can have a separate active list in another loaded module. */
    const char *end = c->source.source[0] == '@' ? strchr(c->source.source, ':') : NULL;
    return end && !strncmp(c->source.source, m->source, (size_t)(end - c->source.source) + 1);
}
bool document_cache_reclaim(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    char victim[STORAGE_PATH_MAX] = "", path[STORAGE_PATH_MAX];
    book_file_version_t victim_version = {0};
    uint32_t oldest = UINT32_MAX;
    unsigned entries = 0, checked = 0;
    /* Reclaim only the new namespace, rotating across buckets. Bound both
     * directory work and metadata decoding when a volume contains junk. */
    for (unsigned visit = 0; visit < 256 && entries < CACHE_RECLAIM_ENTRIES &&
         checked < CACHE_RECLAIM_METADATA && valid(c) && !cache_stopped(c); ++visit)
    {
        unsigned bucket = c->reclaim_next++;
        int prefix = snprintf(path, sizeof(path), "%.*s/%x/%x/",
            (int)c->root_length, c->directory, bucket >> 4, bucket & 15);
        if (prefix < 0 || (size_t)prefix + 26 >= sizeof(path)) break;
        path[prefix - 1] = 0;
        DIR *dir = opendir(path);
        path[prefix - 1] = '/';
        struct dirent *entry;
        while (dir && entries < CACHE_RECLAIM_ENTRIES && checked < CACHE_RECLAIM_METADATA &&
               valid(c) && !cache_stopped(c) && (entry = readdir(dir)) != NULL)
        {
            ++entries;
            if (!c->shared_directory && temporary_name(entry->d_name) && !bucket_busy(path, (size_t)prefix - 1))
            {
                memcpy(path + prefix, entry->d_name, 13);
                struct stat st;
                if (!storage_file_writers(path) && !stat(path, &st) && S_ISREG(st.st_mode) &&
                    st.st_size > 0 && valid(c) && !cache_stopped(c) && !unlink(path))
                { closedir(dir); storage_unlock(); return true; }
            }
            if (!cache_name(entry->d_name)) continue;
            memcpy(path + prefix, entry->d_name, 26);
            int n = prefix + 25;
            path[n - 5] = 0;
            if (base_busy(path)) continue;
            path[n - 5] = '.';
            size_t bytes = 0;
            char name[32]; snprintf(name, sizeof(name), "%s", entry->d_name);
            ++checked;
            storage_unlock();
            book_file_version_t meta_version;
            cache_meta_t *m = load_meta(c, path, &bytes, &meta_version);
            bool decoded = cache_data_close(c);
            storage_lock();
            if (!m || !decoded || !book_file_version_valid(path, &meta_version)) { epd_app_free(m); continue; }
            uint32_t hash = source_hash(m, 2166136261u);
            char expected[32];
            snprintf(expected, sizeof(expected), CACHE_PREFIX "%08lx%08lx.meta",
                     (unsigned long)hash, (unsigned long)source_hash(m, 0x9e3779b9u));
            if ((hash >> 24) == bucket && !strcmp(expected, name) && reclaim_app(c, m) &&
                (!*victim || m->lastuse < oldest))
            { path[n - 5] = 0; memcpy(victim, path, (size_t)n - 4); oldest = m->lastuse; victim_version = meta_version; }
            epd_app_free(m);
        }
        if (dir) closedir(dir);
        if (*victim) break;
    }
    /* Remove both metadata generations first, so recovery cannot resurrect a
     * completed marker after a partially failed cleanup. */
    static const char *const suffixes[] = {
        ".meta.old", ".meta.tmp", ".meta", ".bin", ".bin.old", ".bin.tmp",
        ".idx", ".idx.old", ".idx.tmp", ".count", ".count.old", ".count.tmp"
    };
    snprintf(path, sizeof(path), "%s.meta", victim);
    bool ok = *victim && valid(c) && !cache_stopped(c) && !base_busy(victim) && book_file_version_valid(path, &victim_version), removed = false;
    for (unsigned i = 0; ok && i < sizeof(suffixes) / sizeof(suffixes[0]); ++i)
    {
        int n = snprintf(path, sizeof(path), "%s%s", victim, suffixes[i]);
        if (n < 0 || (size_t)n >= sizeof(path) || !valid(c) || cache_stopped(c)) { ok = false; break; }
        struct stat st;
        errno = 0;
        if (stat(path, &st)) { ok = errno == ENOENT || errno == -ENOENT; continue; }
        if (!S_ISREG(st.st_mode) || unlink(path)) { ok = false; break; }
        removed = true;
    }
    storage_unlock();
    return ok && removed;
}
void document_cache_destroy(document_cache_t *c)
{
    if (!c) return;
    if (c->observing) return;
    document_cache_cancel(c);
    if (!cache_data_close(c)) {
        storage_lock(); c->retiring = c->invalid = true; c->identity = 0; storage_unlock();
        return;
    }
    storage_lock();
    /* The owner must end the worker observation before releasing the object. */
    if (c->observing) { storage_unlock(); return; }
    document_cache_t **p = &active;
    while (*p && *p != c) p = &(*p)->next;
    if (*p) *p = c->next;
    epd_app_free(c->dependencies);
    epd_app_free(c->dependency_versions);
    epd_app_free(c);
    storage_unlock();
}
