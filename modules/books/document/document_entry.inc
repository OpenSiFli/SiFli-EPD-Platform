/* SPDX-License-Identifier: Apache-2.0 */
#define DOCUMENT_ENTRY_SLOTS 4u
#define DOCUMENT_ENTRY_MAGIC 0x31434544u
typedef struct {
    char source[BOOKSHELF_PATH_MAX];
    uint32_t size, modified, bytes;
    uint8_t data[BOOK_ENTRY_MAX];
} document_entry_slot_t;
typedef struct {
    uint32_t magic, version, checksum, next;
    document_entry_slot_t slot[DOCUMENT_ENTRY_SLOTS];
} document_entry_table_t;
static struct {
    document_entry_table_t *table;
    char path[STORAGE_PATH_MAX];
    uint32_t generation;
    book_file_version_t file_version, source_version[DOCUMENT_ENTRY_SLOTS];
    bool source_known[DOCUMENT_ENTRY_SLOTS], dirty;
    const char *origin;
} entry_cache;

static uint32_t entry_table_checksum(const document_entry_table_t *t)
{
    uint32_t h = hash_bytes(2166136261u, t, offsetof(document_entry_table_t, checksum));
    return hash_bytes(h, &t->next, sizeof(*t) - offsetof(document_entry_table_t, next));
}
static bool entry_table_valid(const document_entry_table_t *t)
{
    if (t->magic != DOCUMENT_ENTRY_MAGIC || t->version != 1 ||
        t->next >= DOCUMENT_ENTRY_SLOTS || t->checksum != entry_table_checksum(t)) return false;
    for (unsigned i = 0; i < DOCUMENT_ENTRY_SLOTS; ++i) {
        const document_entry_slot_t *s = &t->slot[i];
        if (!s->bytes) continue;
        if (!s->source[0] || !memchr(s->source, 0, sizeof(s->source)) ||
            !book_entry_valid(s->data, s->bytes) || ((const book_entry_t *)s->data)->source_size != s->size) return false;
    }
    return true;
}
static void entry_cache_queue(void)
{
    if (!entry_cache.table || !entry_cache.dirty) return;
    entry_cache.dirty = false;
    if (!media_valid(entry_cache.path, entry_cache.generation)) {
        rt_kprintf("[document] entry_queue generation=%lu result=stale\n", (unsigned long)entry_cache.generation);
        return;
    }
    entry_cache.table->checksum = entry_table_checksum(entry_cache.table);
    /* The existing worker queue coalesces this one fixed metadata path and
     * writes only when optional persistence is admitted. Failure drops reuse. */
    bool queued = document_save_async(entry_cache.path, NULL, 0, entry_cache.table, sizeof(*entry_cache.table));
    rt_kprintf("[document] entry_queue generation=%lu bytes=%u accepted=%u\n",
        (unsigned long)entry_cache.generation, (unsigned)sizeof(*entry_cache.table), queued);
}
static size_t entry_cache_forget(void)
{
    size_t bytes = entry_cache.table ? sizeof(*entry_cache.table) : 0;
    epd_app_free(entry_cache.table);
    memset(&entry_cache, 0, sizeof(entry_cache));
    return bytes;
}
static bool entry_cache_load(document_t *d)
{
    char path[STORAGE_PATH_MAX], directory[STORAGE_PATH_MAX];
    if (!document_cache_directory(&d->file, "books", directory, sizeof(directory)) ||
        snprintf(path, sizeof(path), "%s/chapter-entry-v1.meta", directory) >= (int)sizeof(path)) return false;
    storage_lock();
    bool ok = !cancelled(d) && media_valid(path, d->generation);
    bool retained = ok && entry_cache.table && !strcmp(path, entry_cache.path) &&
        entry_cache.generation == d->generation &&
        (entry_cache.dirty || book_file_version_valid(path, &entry_cache.file_version));
    storage_unlock();
    if (retained) { entry_cache.origin = "memory"; return true; }
    if (!ok) return false;
    entry_cache_queue();
    entry_cache_forget();
    document_entry_table_t *table = allocate(sizeof(*table));
    if (!table) return false;
    memset(table, 0, sizeof(*table));
    void *queued = NULL; size_t bytes = 0;
    book_file_version_t version = {0};
    const char *origin = "disk";
    if (document_save_snapshot(path, &queued, &bytes)) {
        origin = "pending";
        if (bytes == sizeof(*table)) memcpy(table, queued, bytes);
        epd_app_free(queued);
    } else {
        storage_lock();
        version = book_file_version(path);
        int fd = !cancelled(d) && media_valid(path, d->generation) ? open(path, O_RDONLY, 0) : -1;
        struct stat st;
        bool readable = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_size == (int64_t)sizeof(*table);
        storage_unlock();
        size_t at = 0;
        while (readable && at < sizeof(*table)) {
            size_t n = sizeof(*table) - at;
            if (n > 4096) n = 4096;
            storage_lock();
            readable = !cancelled(d) && media_valid(path, d->generation) && book_file_version_valid(path, &version);
            int got = readable ? read(fd, (uint8_t *)table + at, n) : -1;
            storage_unlock();
            if (got <= 0) { readable = false; break; }
            at += (size_t)got;
        }
        storage_lock();
        if (fd >= 0 && close(fd)) readable = false;
        readable = readable && book_file_version_valid(path, &version);
        storage_unlock();
        if (!readable) memset(table, 0, sizeof(*table));
    }
    if (!entry_table_valid(table)) {
        origin = "empty";
        memset(table, 0, sizeof(*table)); table->magic = DOCUMENT_ENTRY_MAGIC; table->version = 1;
    }
    storage_lock();
    ok = !cancelled(d) && media_valid(path, d->generation);
    if (ok && !version.path) version = book_file_version(path);
    storage_unlock();
    if (!ok) { epd_app_free(table); return false; }
    entry_cache.table = table; entry_cache.generation = d->generation;
    entry_cache.origin = origin;
    memcpy(entry_cache.path, path, strlen(path) + 1); entry_cache.file_version = version;
    return true;
}
/* Called before OPEN, without a compute/storage lock. Only fixed metadata is
 * read; package CRC/size checks happen against the already loaded ZIP directory. */
static const void *entry_cache_find(document_t *d, size_t *bytes, const char **reason)
{
    *reason = "ineligible";
    *bytes = 0; d->entry_source_valid = false;
    if (d->format != DOCUMENT_EPUB || !d->body_first || d->source.external ||
        d->note || d->metadata_only || d->nav_state == DOCUMENT_NAV_ACTIVE) return NULL;
    storage_lock();
    struct stat st;
    d->entry_source_version = book_file_version(d->file.path);
    d->entry_source_valid = !cancelled(d) && media_valid(d->file.path, d->generation) &&
        book_file_version_valid(d->file.path, &d->entry_source_version) &&
        stat(d->file.path, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size == (int64_t)d->file.size && (uint32_t)st.st_mtime == d->file.modified;
    storage_unlock();
    *reason = "source";
    if (!d->entry_source_valid) return NULL;
    *reason = "load";
    if (!entry_cache_load(d)) return NULL;
    *reason = "miss";
    for (unsigned i = 0; i < DOCUMENT_ENTRY_SLOTS; ++i) {
        document_entry_slot_t *s = &entry_cache.table->slot[i];
        if (!s->bytes || strcmp(s->source, d->file.path) || s->size != d->file.size || s->modified != d->file.modified) continue;
        const book_entry_t *e = (const void *)s->data;
        if (d->first_body ? !(e->flags & BOOK_ENTRY_FIRST) : e->chapter != d->requested.chapter) continue;
        storage_lock();
        bool valid_source = !entry_cache.source_known[i] || book_file_version_valid(d->file.path, &entry_cache.source_version[i]);
        if (valid_source) { entry_cache.source_version[i] = d->entry_source_version; entry_cache.source_known[i] = true; }
        storage_unlock();
        if (!valid_source) { *reason = "version"; continue; }
        *reason = "offered";
        *bytes = s->bytes; return s->data;
    }
    return NULL;
}
static const void *entry_cache_request(document_t *d, size_t *bytes)
{
    rt_tick_t started = rt_tick_get();
    const char *reason;
    const void *entry = entry_cache_find(d, bytes, &reason);
    if (!d->entry_use_logged && strcmp(reason, "ineligible")) {
        d->entry_use_logged = true;
        rt_kprintf("[document] entry_use id=%lu chapter=%lu result=%s origin=%s bytes=%u load_ms=%u generation=%lu\n",
            (unsigned long)d->serial, (unsigned long)d->requested.chapter, reason,
            (!strcmp(reason, "source") || !strcmp(reason, "load")) ? "none" : entry_cache.origin ? entry_cache.origin : "none", (unsigned)*bytes,
            elapsed_ms(started), (unsigned long)d->generation);
    }
    return entry;
}
static int entry_cache_accept(document_t *d, const void *data, size_t bytes, const char **reason)
{
    *reason = "cancelled";
    if (cancelled(d)) return BOOK_CANCELLED;
    *reason = "invalid";
    if (!d->entry_source_valid || !entry_cache.table || entry_cache.generation != d->generation ||
        !book_entry_valid(data, bytes)) return BOOK_OK;
    const book_entry_t *e = data;
    document_part_t *part = d->parts;
    while (part && part->number != e->chapter) part = part->next;
    *reason = "chapter";
    if (!part || e->chapters != d->chapter_count || e->source_size != d->file.size ||
        strcmp(part->text, book_entry_field(e, BOOK_ENTRY_HREF))) return BOOK_OK;
    storage_lock();
    bool ok = !cancelled(d) && media_valid(d->file.path, d->generation) &&
        book_file_version_valid(d->file.path, &d->entry_source_version);
    storage_unlock();
    *reason = "source";
    if (!ok) return cancelled(d) ? BOOK_CANCELLED : BOOK_OK;
    unsigned index;
    for (index = 0; index < DOCUMENT_ENTRY_SLOTS; ++index) {
        document_entry_slot_t *s = &entry_cache.table->slot[index];
        if (s->bytes && !strcmp(s->source, d->file.path) &&
            ((const book_entry_t *)s->data)->chapter == e->chapter &&
            (((const book_entry_t *)s->data)->flags & BOOK_ENTRY_FIRST) == (e->flags & BOOK_ENTRY_FIRST)) break;
    }
    if (index == DOCUMENT_ENTRY_SLOTS) {
        index = entry_cache.table->next;
        entry_cache.table->next = (index + 1) % DOCUMENT_ENTRY_SLOTS;
    }
    document_entry_slot_t *s = &entry_cache.table->slot[index];
    bool same = s->bytes == bytes && s->size == d->file.size && s->modified == d->file.modified &&
        !strcmp(s->source, d->file.path) && !memcmp(s->data, data, bytes);
    if (!same) {
        memset(s, 0, sizeof(*s)); memcpy(s->source, d->file.path, strlen(d->file.path) + 1);
        s->size = d->file.size; s->modified = d->file.modified; s->bytes = (uint32_t)bytes;
        memcpy(s->data, data, bytes); entry_cache.dirty = true;
    }
    entry_cache.source_version[index] = d->entry_source_version; entry_cache.source_known[index] = true;
    *reason = same ? "unchanged" : "stored";
    return BOOK_OK;
}
static int entry_cache_receive(document_t *d, const void *data, size_t bytes)
{
    const char *reason;
    int rc = entry_cache_accept(d, data, bytes, &reason);
    if (!d->entry_receive_logged) {
        d->entry_receive_logged = true;
        rt_tick_t started = rt_tick_get();
        rt_kprintf("[document] entry_receive id=%lu result=%s bytes=%u status=%d\n",
            (unsigned long)d->serial, reason, (unsigned)bytes, rc);
        if (d->remote) d->remote->profile.trace_ticks += rt_tick_get() - started;
    }
    return rc;
}
static bool entry_save_job(const document_save_t *job)
{
    static const char suffix[] = "/chapter-entry-v1.meta";
    size_t length = strlen(job->path);
    return job->length == sizeof(document_entry_table_t) && length >= sizeof(suffix) - 1 &&
        !strcmp(job->path + length - (sizeof(suffix) - 1), suffix);
}
