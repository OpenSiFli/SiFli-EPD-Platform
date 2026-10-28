/* SPDX-License-Identifier: Apache-2.0 */
#include "storage.h"
#include "epd_memory.h"
#include "document_cache.h"
#include "document.h"
#include "services/storage_file.h"
#include "../file_version.h"
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
#define CACHE_VERSION 1u
#define CACHE_PREFIX "dc1-"

/* Payload of storage_record_save; fixed-width fields, zeroed padding. */
typedef struct {
    uint32_t complete, volume, format, size, modified, conversion;
    uint32_t image_width, image_height, end, identity, dependencies, lastuse;
    char source[BOOKSHELF_PATH_MAX], title[BOOKSHELF_NAME_MAX];
} cache_meta_t;
typedef struct { uint32_t volume, present, size, modified, length; } cache_dep_t;
/* Inspect storage_record_save framing only to discover the payload length.
 * storage_record_load subsequently verifies the entire framing and CRC. */
typedef struct { uint32_t magic, version, size, crc; } cache_frame_t;

struct document_cache {
    struct document_cache *next;
    cache_meta_t source;
    char directory[STORAGE_PATH_MAX], base[STORAGE_PATH_MAX];
    char binary[STORAGE_PATH_MAX], meta[STORAGE_PATH_MAX], index[STORAGE_PATH_MAX];
    uint8_t *dependencies;
    book_file_version_t *dependency_versions;
    book_file_version_t source_version;
    char source_path[BOOKSHELF_PATH_MAX];
    size_t dependency_bytes;
    uint32_t generation, identity, dependency_count;
    bool prepared, observed, observing, dependencies_ok, invalid, explicit_identity;
};
static document_cache_t *active;
static bool reuse_disabled;
/* Keep validated dependency sets across document closes, independently of the
 * shared file-signature LRU. Tokens are never written into persistent metadata. */
#define DEPENDENCY_MEMORY_BUDGET (128u * 1024u)
static struct {
    void *memory;
    size_t bytes, charge;
    uint32_t count;
    uint64_t used;
} dependency_cache[4];
static uint64_t dependency_use;

void document_cache_release_memory(void)
{
    storage_lock();
    for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i)
        epd_app_free(dependency_cache[i].memory);
    memset(dependency_cache, 0, sizeof(dependency_cache));
    dependency_use = 0;
    storage_unlock();
}

static void *allocate(size_t size) { return epd_app_alloc(size, EPD_APP_PSRAM); }
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
static uint32_t content_hash(const cache_meta_t *m, const void *deps, size_t bytes)
{
    uint32_t value = source_hash(m, 2166136261u);
    value = hash(value, &m->end, sizeof(m->end));
    value = hash(value, deps, bytes);
    return value ? value : 1;
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
    int n = snprintf(c->base, sizeof(c->base), "%s/" CACHE_PREFIX "%08lx%08lx", c->directory,
                     (unsigned long)source_hash(&c->source, 2166136261u),
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
        !storage_app_path(directory, capacity, app_id, STORAGE_APP_CACHE, "documents")) return false;
    if (!strcmp(storage_path_root(file->path), STORAGE_SD_ROOT))
    {
        int n = snprintf(directory, capacity, STORAGE_SD_ROOT "/.epd/cache/%s/documents", app_id);
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
         file->format != BOOK_FORMAT_SPLIT_EPUB && file->format != BOOK_FORMAT_MOBI)) return NULL;
    document_cache_t *c = allocate(sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    const char *relative;
    storage_lock();
    c->generation = storage_card_session();
    c->explicit_identity = identity != NULL;
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
            size_t n = strlen(directory);
            ok = ok && n && n < sizeof(c->directory);
            if (ok) memcpy(c->directory, directory, n + 1);
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
const char *document_cache_base(const document_cache_t *c) { return c ? c->base : NULL; }
const char *document_cache_binary(const document_cache_t *c) { return c ? c->binary : NULL; }
const char *document_cache_meta(const document_cache_t *c) { return c ? c->meta : NULL; }
const char *document_cache_index(const document_cache_t *c) { return c ? c->index : NULL; }
uint32_t document_cache_identity(const document_cache_t *c)
{
    storage_lock();
    uint32_t identity = c && !c->invalid ? c->identity : 0;
    storage_unlock();
    return identity;
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
        dep->length > *remaining || (*at)[dep->length - 1] || memchr(*at, 0, dep->length - 1)) return false;
    *path = (const char *)*at;
    if (!relative_valid(*path)) return false;
    *at += dep->length; *remaining -= dep->length;
    return true;
}
static cache_meta_t *load_meta(const document_cache_t *c, const char *path, size_t *bytes)
{
    cache_frame_t frame;
    struct stat st;
    if (!valid(c) || !storage_file_recover(path)) return NULL;
    int fd = open(path, O_RDONLY, 0);
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
        read_exact(fd, &frame, sizeof(frame)) && frame.magic == CACHE_MAGIC && frame.version == CACHE_VERSION &&
        frame.size >= sizeof(cache_meta_t) && frame.size <= INT32_MAX - sizeof(frame) &&
        st.st_size == (int64_t)sizeof(frame) + frame.size;
    if (fd >= 0 && close(fd)) ok = false;
    if (!ok || !valid(c)) return NULL;
    cache_meta_t *m = allocate(frame.size);
    if (!m) return NULL;
    ok = storage_record_load(path, CACHE_MAGIC, CACHE_VERSION, m, frame.size) &&
        m->complete <= 1 && m->volume < STORAGE_COUNT && m->end <= INT32_MAX &&
        memchr(m->source, 0, sizeof(m->source)) && memchr(m->title, 0, sizeof(m->title)) &&
        (relative_valid(m->source) || (c->explicit_identity && m->source[0] == '@'));
    const uint8_t *at = (const uint8_t *)(m + 1);
    size_t remaining = frame.size - sizeof(*m);
    for (uint32_t i = 0; ok && i < m->dependencies; ++i)
    {
        cache_dep_t dep; const char *relative;
        ok = dep_next(&at, &remaining, &dep, &relative);
    }
    if (remaining || !valid(c)) ok = false;
    if (!ok) { epd_app_free(m); return NULL; }
    *bytes = frame.size;
    return m;
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
static bool snapshot(const document_cache_t *c, const char *path, cache_dep_t *dep,
                     book_file_version_t *version)
{
    const char *relative;
    struct stat st;
    memset(dep, 0, sizeof(*dep));
    if (!valid(c) || !path_valid(c, path) || !split_path(path, &dep->volume, &relative)) return false;
    size_t length = strlen(relative) + 1;
    if (length > UINT32_MAX) return false;
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
        ok = storage_file_signature(path, &dep->modified);
    return ok && valid(c) && path_valid(c, path) && book_file_version_valid(path, version);
}
static bool check_dependencies(const document_cache_t *c, const void *data, size_t bytes, uint32_t count,
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
        bool ok = book_file_version_valid(path, &versions[i]) && path_valid(c, path);
        epd_app_free(path);
        if (!ok) return false;
    }
    return !remaining && valid(c);
}
/* Called under storage_lock; each allocation stores aligned tokens then the
 * original persistent dependency payload, so equality includes every path. */
static bool recalled_versions(const void *data, size_t bytes, uint32_t count, book_file_version_t *versions)
{
    for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i)
        if (dependency_cache[i].memory && dependency_cache[i].bytes == bytes && dependency_cache[i].count == count)
        {
            book_file_version_t *saved = dependency_cache[i].memory;
            if (memcmp(saved + count, data, bytes)) continue;
            memcpy(versions, saved, (size_t)count * sizeof(*versions));
            dependency_cache[i].used = ++dependency_use;
            return true;
        }
    return false;
}
static void remember_versions(const void *data, size_t bytes, uint32_t count, const book_file_version_t *versions)
{
    if (!count || bytes > DEPENDENCY_MEMORY_BUDGET ||
        count > (DEPENDENCY_MEMORY_BUDGET - bytes) / sizeof(*versions)) return;
    size_t charge = bytes + (size_t)count * sizeof(*versions), total = 0;
    unsigned slot = 0;
    for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i)
    {
        total += dependency_cache[i].charge;
        if (!dependency_cache[i].memory || dependency_cache[i].used < dependency_cache[slot].used) slot = i;
    }
    /* Reuse an exact entry without allocating or evicting another document. */
    for (unsigned i = 0; i < sizeof(dependency_cache) / sizeof(dependency_cache[0]); ++i)
        if (dependency_cache[i].memory && dependency_cache[i].bytes == bytes && dependency_cache[i].count == count &&
            !memcmp((book_file_version_t *)dependency_cache[i].memory + count, data, bytes))
        {
            memcpy(dependency_cache[i].memory, versions, (size_t)count * sizeof(*versions));
            dependency_cache[i].used = ++dependency_use;
            return;
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
    memcpy(memory, versions, (size_t)count * sizeof(*versions));
    memcpy(memory + count, data, bytes);
    dependency_cache[slot].memory = memory; dependency_cache[slot].bytes = bytes;
    dependency_cache[slot].charge = charge; dependency_cache[slot].count = count;
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
    bool ok = valid(c) && !reuse_disabled && !c->invalid && !c->prepared;
    for (document_cache_t *p = active; ok && p; p = p->next)
        if (p != c && !strcmp(p->base, c->base) && valid(p) && (p->invalid || p->prepared)) ok = false;
    size_t bytes = 0;
    book_file_version_t meta_version = book_file_version(c->meta);
    cache_meta_t *m = ok ? load_meta(c, c->meta, &bytes) : NULL;
    ok = m && m->complete && same_source(m, &c->source) &&
        m->identity == content_hash(m, m + 1, bytes - sizeof(*m)) && strlen(m->title) < capacity;
    book_file_version_t *versions = ok && m->dependencies ?
        allocate((size_t)m->dependencies * sizeof(*versions)) : NULL;
    if (ok && m->dependencies && !versions) ok = false;
    bool remembered = ok && m->dependencies && recalled_versions(m + 1, bytes - sizeof(*m), m->dependencies, versions);
    storage_unlock();
    if (ok) ok = check_dependencies(c, m + 1, bytes - sizeof(*m), m->dependencies, versions, remembered);
    storage_lock();
    ok = ok && !reuse_disabled && !c->invalid && !c->prepared &&
        book_file_version_valid(c->meta, &meta_version) &&
        versions_valid(c, m + 1, bytes - sizeof(*m), m->dependencies, versions) && binary_length(c, m->end);
    for (document_cache_t *p = active; ok && p; p = p->next)
        if (p != c && !strcmp(p->base, c->base) && valid(p) && (p->invalid || p->prepared)) ok = false;
    if (ok)
    {
        memcpy(title, m->title, strlen(m->title) + 1); *end = m->end; c->identity = m->identity;
        remember_versions(m + 1, bytes - sizeof(*m), m->dependencies, versions);
    }
    else c->identity = 0;
    epd_app_free(m);
    epd_app_free(versions);
    storage_unlock();
    return ok;
}
static bool save_incomplete(document_cache_t *c)
{
    cache_meta_t marker = c->source;
    marker.lastuse = lastuse();
    return valid(c) && storage_record_save(c->meta, CACHE_MAGIC, CACHE_VERSION, &marker, sizeof(marker)) && valid(c);
}
bool document_cache_invalidate(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    for (document_cache_t *p = active; p; p = p->next)
        if (!strcmp(p->base, c->base) && p->generation == c->generation)
        { p->invalid = true; p->identity = 0; }
    bool ok = save_incomplete(c);
    if (!ok) reuse_disabled = true;
    storage_unlock();
    return ok;
}
bool document_cache_prepare(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    bool ok = !c->observing && valid(c) && !same_base_active(c);
    c->prepared = false;
    if (ok)
    {
        c->identity = 0;
        ok = save_incomplete(c);
        if (ok)
        {
            c->prepared = true; c->invalid = false;
        }
    }
    storage_unlock();
    return ok;
}
static void observe(void *user, const char *path, book_file_event_t event)
{
    document_cache_t *c = user;
    storage_lock();
    if (!c->dependencies_ok || c->source.format != BOOK_FORMAT_MARKDOWN) goto done;
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
    cache_dep_t dep;
    book_file_version_t version;
    storage_unlock();
    bool captured = snapshot(c, path, &dep, &version);
    storage_lock();
    if (!captured || !c->dependencies_ok || !book_file_version_valid(path, &version) ||
        dep.present != (event == BOOK_FILE_OPENED)) goto failed;
    size_t length = dep.length;
    if (c->dependency_count == UINT32_MAX || length > SIZE_MAX - sizeof(dep) ||
        c->dependency_bytes > SIZE_MAX - sizeof(dep) - length) goto failed;
    size_t bytes = c->dependency_bytes + sizeof(dep) + length;
    if (bytes > INT32_MAX - sizeof(cache_frame_t) - sizeof(cache_meta_t)) goto failed;
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
    storage_unlock();
}
bool document_cache_observe_begin(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    bool ok = !c->observing && valid(c) && book_file_observer_begin(observe, c);
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
    { book_file_observer_end(c); c->observing = false; c->observed = true; }
    storage_unlock();
}
bool document_cache_commit(document_cache_t *c, const char *title, uint32_t end)
{
    if (!c || !title || strlen(title) >= BOOKSHELF_NAME_MAX) return false;
    storage_lock();
    bool ok = c->prepared && c->observed && !c->observing && c->dependencies_ok && !c->invalid &&
        valid(c) &&
        (c->source.format != BOOK_FORMAT_MARKDOWN || c->explicit_identity ||
         book_file_version_valid(c->source_path, &c->source_version)) &&
        versions_valid(c, c->dependencies, c->dependency_bytes, c->dependency_count, c->dependency_versions) &&
        binary_length(c, end);
    size_t bytes = sizeof(cache_meta_t) + c->dependency_bytes;
    cache_meta_t *m = ok ? allocate(bytes) : NULL;
    if (!m) ok = false;
    if (ok)
    {
        *m = c->source;
        m->complete = 1; m->end = end; m->lastuse = lastuse(); m->dependencies = c->dependency_count;
        memcpy(m->title, title, strlen(title) + 1);
        if (c->dependency_bytes) memcpy(m + 1, c->dependencies, c->dependency_bytes);
        m->identity = content_hash(m, m + 1, c->dependency_bytes);
        ok = storage_record_save(c->meta, CACHE_MAGIC, CACHE_VERSION, m, bytes) && valid(c);
        if (ok && (!versions_valid(c, c->dependencies, c->dependency_bytes, c->dependency_count, c->dependency_versions) ||
            (c->source.format == BOOK_FORMAT_MARKDOWN && !c->explicit_identity &&
             !book_file_version_valid(c->source_path, &c->source_version))))
        {
            if (!save_incomplete(c)) reuse_disabled = true;
            ok = false;
        }
        if (ok)
        {
            c->identity = m->identity; c->prepared = false;
            remember_versions(c->dependencies, c->dependency_bytes, c->dependency_count, c->dependency_versions);
        }
    }
    epd_app_free(m);
    storage_unlock();
    return ok;
}
bool document_cache_touch(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    size_t bytes = 0;
    cache_meta_t *m = !c->invalid && c->identity ? load_meta(c, c->meta, &bytes) : NULL;
    bool ok = m && m->complete && same_source(m, &c->source) && m->identity == c->identity;
    if (ok)
    {
        m->lastuse = lastuse();
        ok = valid(c) && storage_record_save(c->meta, CACHE_MAGIC, CACHE_VERSION, m, bytes) && valid(c);
    }
    epd_app_free(m);
    storage_unlock();
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
bool document_cache_reclaim(document_cache_t *c)
{
    if (!c) return false;
    storage_lock();
    char victim[STORAGE_PATH_MAX] = "", path[STORAGE_PATH_MAX];
    uint32_t oldest = UINT32_MAX;
    DIR *dir = valid(c) ? opendir(c->directory) : NULL;
    struct dirent *entry;
    while (dir && valid(c) && (entry = readdir(dir)) != NULL)
    {
        if (!cache_name(entry->d_name)) continue;
        int n = snprintf(path, sizeof(path), "%s/%s", c->directory, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        path[n - 5] = 0;
        if (base_busy(path)) continue;
        path[n - 5] = '.';
        size_t bytes = 0;
        cache_meta_t *m = load_meta(c, path, &bytes);
        if (!m) continue;
        char expected[32];
        snprintf(expected, sizeof(expected), CACHE_PREFIX "%08lx%08lx.meta",
                 (unsigned long)source_hash(m, 2166136261u), (unsigned long)source_hash(m, 0x9e3779b9u));
        if (!strcmp(expected, entry->d_name) && (!*victim || m->lastuse < oldest))
        { path[n - 5] = 0; memcpy(victim, path, (size_t)n - 4); oldest = m->lastuse; }
        epd_app_free(m);
    }
    if (dir) closedir(dir);
    /* Remove both metadata generations first, so recovery cannot resurrect a
     * completed marker after a partially failed cleanup. */
    static const char *const suffixes[] = {
        ".meta.old", ".meta.tmp", ".meta", ".bin", ".bin.old", ".bin.tmp",
        ".idx", ".idx.old", ".idx.tmp"
    };
    bool ok = *victim && valid(c) && !base_busy(victim), removed = false;
    for (unsigned i = 0; ok && i < sizeof(suffixes) / sizeof(suffixes[0]); ++i)
    {
        int n = snprintf(path, sizeof(path), "%s%s", victim, suffixes[i]);
        if (n < 0 || (size_t)n >= sizeof(path) || !valid(c)) { ok = false; break; }
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
