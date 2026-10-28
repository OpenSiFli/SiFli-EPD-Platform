#include "music_store.h"
#include "storage_file.h"
#include <dfs_posix.h>
#include <limits.h>
#include <errno.h>
#include <string.h>

#define MUSIC_STORE_MAGIC 0x4d555349u
#define MUSIC_PROGRESS_MAGIC 0x4d505247u
#define MUSIC_PROGRESS_VERSION 1u
#define MUSIC_PROGRESS_SIZE 24u

typedef struct { unsigned char *data; size_t size, capacity, cursor; } record_t;

static music_model_result_t put(record_t *r, const void *data, size_t size)
{
    /* storage_record_load uses one int-returning read for its payload. */
    if (size > INT_MAX || r->size > (size_t)INT_MAX - size) return MUSIC_MODEL_RANGE;
    size_t total = r->size + size;
    if (total > r->capacity)
    {
        size_t capacity = r->capacity ? r->capacity : 256;
        while (capacity < total)
        {
            if (capacity > INT_MAX / 2) { capacity = total; break; }
            capacity *= 2;
        }
        void *copy = music_model_realloc(r->data, capacity);
        if (!copy) return MUSIC_MODEL_MEMORY;
        r->data = copy; r->capacity = capacity;
    }
    if (size) memcpy(r->data + r->size, data, size);
    r->size = total;
    return MUSIC_MODEL_OK;
}

static music_model_result_t put_u32(record_t *r, uint32_t value)
{
    unsigned char bytes[4];
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
    return put(r, bytes, sizeof(bytes));
}

static music_model_result_t put_u64(record_t *r, uint64_t value)
{
    unsigned char bytes[8];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
    return put(r, bytes, sizeof(bytes));
}

static music_model_result_t put_string(record_t *r, const char *text)
{
    size_t length = text ? strlen(text) : 0;
    if (length >= STORAGE_PATH_MAX) return MUSIC_MODEL_RANGE;
    music_model_result_t rc = put_u32(r, (uint32_t)length);
    return rc == MUSIC_MODEL_OK ? put(r, text, length) : rc;
}

static bool get_u32(record_t *r, uint32_t *value)
{
    if (r->size - r->cursor < 4) return false;
    const unsigned char *p = r->data + r->cursor;
    *value = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    r->cursor += 4; return true;
}

static bool get_u64(record_t *r, uint64_t *value)
{
    if (r->size - r->cursor < 8) return false;
    *value = 0;
    for (unsigned i = 0; i < 8; ++i) *value |= (uint64_t)r->data[r->cursor++] << (8 * i);
    return true;
}

static music_model_result_t get_string(record_t *r, char **out, bool allow_empty)
{
    uint32_t length;
    if (!get_u32(r, &length) || length >= STORAGE_PATH_MAX || length > r->size - r->cursor ||
        (!allow_empty && !length) || memchr(r->data + r->cursor, 0, length)) return MUSIC_MODEL_FORMAT;
    char *text = music_model_alloc((size_t)length + 1);
    if (!text) return MUSIC_MODEL_MEMORY;
    memcpy(text, r->data + r->cursor, length); text[length] = 0;
    r->cursor += length;
    if (length)
    {
        char *normal = NULL;
        music_model_result_t rc = music_library_resolve(NULL, text, &normal);
        if (rc != MUSIC_MODEL_OK) { music_model_free(text); return rc; }
        bool canonical = !strcmp(normal, text);
        music_model_free(normal);
        if (!canonical) { music_model_free(text); return MUSIC_MODEL_FORMAT; }
    }
    *out = text; return MUSIC_MODEL_OK;
}

void music_store_init(music_store_state_t *state)
{
    memset(state, 0, sizeof(*state)); music_queue_init(&state->queue, 0);
    state->snapshot_dirty = true;
}

void music_store_mark_changed(music_store_state_t *state)
{
    state->snapshot_dirty = true;
}

void music_store_free(music_store_state_t *state)
{
    music_queue_free(&state->queue); music_favorites_free(&state->favorites);
    music_model_free(state->directory); music_store_init(state);
}

#define PUT(expression) do { rc = (expression); if (rc != MUSIC_MODEL_OK) goto done; } while (0)

static music_model_result_t save_progress(const music_store_state_t *state)
{
    if (!state->snapshot_version || state->queue.current_id != state->snapshot_current_id)
        return MUSIC_MODEL_CHANGED;
    char path[STORAGE_PATH_MAX];
    if (!storage_app_path(path, sizeof(path), "music", STORAGE_APP_DATA, "progress.bin")) return MUSIC_MODEL_IO;
    unsigned char bytes[MUSIC_PROGRESS_SIZE];
    record_t r = {bytes, 0, sizeof(bytes), 0};
    /* Fixed-size encoding needs neither a queue walk nor a model allocation. */
    (void)put_u64(&r, state->snapshot_version);
    (void)put_u32(&r, state->snapshot_crc);
    (void)put_u32(&r, state->queue.current_id);
    (void)put_u64(&r, state->queue.position_ms);
    return storage_record_save(path, MUSIC_PROGRESS_MAGIC, MUSIC_PROGRESS_VERSION, bytes, sizeof(bytes)) ?
           MUSIC_MODEL_OK : MUSIC_MODEL_IO;
}

music_model_result_t music_store_save(music_store_state_t *state)
{
    if (!state->snapshot_dirty) return save_progress(state);
    const music_queue_t *q = &state->queue;
    if (!music_queue_valid(q)) return MUSIC_MODEL_INVALID;
    if (q->history_count > UINT32_MAX || state->favorites.count > UINT32_MAX) return MUSIC_MODEL_RANGE;
    char path[STORAGE_PATH_MAX];
    if (!storage_app_path(path, sizeof(path), "music", STORAGE_APP_DATA, "state.bin")) return MUSIC_MODEL_IO;
    if (state->snapshot_version == UINT64_MAX) return MUSIC_MODEL_RANGE;
    uint64_t version = state->snapshot_version + 1;
    record_t r = {0};
    music_model_result_t rc;
    PUT(put_u64(&r, version));
    PUT(put_u32(&r, (uint32_t)q->count));
    PUT(put_u32(&r, q->next_id)); PUT(put_u32(&r, q->current_id));
    PUT(put_u32(&r, (uint32_t)q->mode)); PUT(put_u64(&r, q->position_ms));
    PUT(put_u32(&r, q->random_state));
    PUT(put_u32(&r, (uint32_t)q->shuffle_count)); PUT(put_u32(&r, (uint32_t)q->shuffle_cursor));
    PUT(put_u32(&r, (uint32_t)q->history_count)); PUT(put_u32(&r, (uint32_t)q->history_cursor));
    PUT(put_u32(&r, (uint32_t)state->favorites.count));
    PUT(put_string(&r, state->directory));
    for (size_t i = 0; i < q->count; ++i)
    {
        const music_queue_entry_t *e = q->entries + i;
        music_file_stamp_t stamp = e->stamp;
        /* Capture the initial stat without overwriting a known older signature. */
        if (!stamp.valid) (void)music_library_stat(e->path, false, &stamp);
        PUT(put_u32(&r, e->id)); PUT(put_string(&r, e->path));
        PUT(put_u64(&r, stamp.size)); PUT(put_u64(&r, (uint64_t)stamp.mtime));
        PUT(put_u32(&r, stamp.signature));
        PUT(put_u32(&r, (stamp.valid ? 1u : 0u) | (stamp.signature_valid ? 2u : 0u) |
                         (e->unavailable ? 4u : 0u)));
    }
    for (size_t i = 0; i < q->shuffle_count; ++i) PUT(put_u32(&r, q->shuffle[i]));
    for (size_t i = 0; i < q->history_count; ++i) PUT(put_u32(&r, q->history[i]));
    for (size_t i = 0; i < state->favorites.count; ++i) PUT(put_string(&r, state->favorites.paths[i]));
    if (!storage_record_save(path, MUSIC_STORE_MAGIC, MUSIC_STORE_VERSION, r.data, r.size)) rc = MUSIC_MODEL_IO;
    else {
        state->snapshot_version = version;
        state->snapshot_crc = storage_crc32(r.data, r.size);
        state->snapshot_current_id = q->current_id;
        state->snapshot_dirty = false;
    }
done:
    music_model_free(r.data); return rc;
}

#undef PUT

static music_model_result_t decode(record_t *r, music_store_state_t *state)
{
    music_queue_t *q = &state->queue;
    uint32_t count, mode, shuffle_count, shuffle_cursor, history_count, history_cursor, favorites_count;
    if (!get_u32(r, &count) || !get_u32(r, &q->next_id) || !get_u32(r, &q->current_id) ||
        !get_u32(r, &mode) || !get_u64(r, &q->position_ms) || !get_u32(r, &q->random_state) ||
        !get_u32(r, &shuffle_count) || !get_u32(r, &shuffle_cursor) ||
        !get_u32(r, &history_count) || !get_u32(r, &history_cursor) || !get_u32(r, &favorites_count))
        return MUSIC_MODEL_FORMAT;
    if (mode > MUSIC_MODE_SHUFFLE || (shuffle_count && shuffle_count != count) ||
        shuffle_cursor > shuffle_count || (history_count && history_cursor >= history_count) ||
        (!history_count && history_cursor)) return MUSIC_MODEL_FORMAT;
    q->mode = (music_mode_t)mode;
    music_model_result_t rc = get_string(r, &state->directory, true);
    if (rc != MUSIC_MODEL_OK) return rc;
    /* Each entry needs an ID, string length, nonempty path and 24 stamp bytes. */
    if (count > (r->size - r->cursor) / 33 || count > SIZE_MAX / sizeof(*q->entries)) return MUSIC_MODEL_FORMAT;
    if (count)
    {
        q->entries = music_model_alloc((size_t)count * sizeof(*q->entries));
        if (!q->entries) return MUSIC_MODEL_MEMORY;
        memset(q->entries, 0, (size_t)count * sizeof(*q->entries)); q->capacity = count;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        music_queue_entry_t *e = q->entries + i;
        uint64_t mtime; uint32_t flags;
        ++q->count;
        if (!get_u32(r, &e->id)) return MUSIC_MODEL_FORMAT;
        rc = get_string(r, &e->path, false);
        if (rc != MUSIC_MODEL_OK) return rc;
        if (!get_u64(r, &e->stamp.size) || !get_u64(r, &mtime) ||
            !get_u32(r, &e->stamp.signature) || !get_u32(r, &flags) || flags > 7 ||
            ((flags & 2) && !(flags & 1))) return MUSIC_MODEL_FORMAT;
        e->stamp.mtime = (int64_t)mtime; e->stamp.valid = (flags & 1) != 0;
        e->stamp.signature_valid = (flags & 2) != 0; e->unavailable = (flags & 4) != 0;
    }
    if (shuffle_count > (r->size - r->cursor) / 4) return MUSIC_MODEL_FORMAT;
    if (shuffle_count)
    {
        q->shuffle = music_model_alloc((size_t)shuffle_count * sizeof(*q->shuffle));
        if (!q->shuffle) return MUSIC_MODEL_MEMORY;
        for (uint32_t i = 0; i < shuffle_count; ++i)
            if (!get_u32(r, q->shuffle + i)) return MUSIC_MODEL_FORMAT;
        q->shuffle_count = shuffle_count; q->shuffle_cursor = shuffle_cursor;
    }
    if (history_count > (r->size - r->cursor) / 4) return MUSIC_MODEL_FORMAT;
    if (history_count)
    {
        q->history = music_model_alloc((size_t)history_count * sizeof(*q->history));
        if (!q->history) return MUSIC_MODEL_MEMORY;
        for (uint32_t i = 0; i < history_count; ++i)
            if (!get_u32(r, q->history + i)) return MUSIC_MODEL_FORMAT;
        q->history_count = q->history_capacity = history_count; q->history_cursor = history_cursor;
    }
    if (favorites_count > (r->size - r->cursor) / 5) return MUSIC_MODEL_FORMAT;
    for (uint32_t i = 0; i < favorites_count; ++i)
    {
        char *path = NULL;
        rc = get_string(r, &path, false);
        if (rc != MUSIC_MODEL_OK) return rc;
        rc = music_favorites_set(&state->favorites, path, true);
        music_model_free(path);
        if (rc != MUSIC_MODEL_OK) return rc;
        if (state->favorites.count != (size_t)i + 1) return MUSIC_MODEL_FORMAT;
    }
    if (r->cursor != r->size || !music_queue_valid(q)) return MUSIC_MODEL_FORMAT;
    return MUSIC_MODEL_OK;
}

static void load_progress(music_store_state_t *state)
{
    char path[STORAGE_PATH_MAX];
    unsigned char bytes[MUSIC_PROGRESS_SIZE];
    if (!storage_app_path(path, sizeof(path), "music", STORAGE_APP_DATA, "progress.bin") ||
        !storage_record_load(path, MUSIC_PROGRESS_MAGIC, MUSIC_PROGRESS_VERSION, bytes, sizeof(bytes))) return;
    record_t r = {bytes, sizeof(bytes), sizeof(bytes), 0};
    uint64_t version, position;
    uint32_t crc, current_id;
    if (get_u64(&r, &version) && get_u32(&r, &crc) && get_u32(&r, &current_id) &&
        get_u64(&r, &position) && version == state->snapshot_version &&
        crc == state->snapshot_crc && current_id == state->queue.current_id)
        state->queue.position_ms = position;
}

music_model_result_t music_store_load(music_store_state_t *state)
{
    char path[STORAGE_PATH_MAX]; struct stat st;
    if (!storage_app_path(path, sizeof(path), "music", STORAGE_APP_DATA, "state.bin") ||
        !storage_file_recover(path)) return MUSIC_MODEL_IO;
    if (stat(path, &st) != 0)
        return errno == ENOENT || errno == -ENOENT ? MUSIC_MODEL_NOT_FOUND : MUSIC_MODEL_IO;
    /* Existing storage_file records have four uint32_t envelope fields. */
    const size_t envelope_size = 4 * sizeof(uint32_t);
    if (st.st_size < (int64_t)envelope_size || (uint64_t)st.st_size - envelope_size > INT_MAX)
        return MUSIC_MODEL_FORMAT;
    record_t r = {0}; r.size = (size_t)st.st_size - envelope_size;
    if (!r.size) return MUSIC_MODEL_FORMAT;
    r.data = music_model_alloc(r.size);
    if (!r.data) return MUSIC_MODEL_MEMORY;
    music_model_result_t rc = MUSIC_MODEL_FORMAT;
    music_store_state_t next; music_store_init(&next);
    if (storage_record_load(path, MUSIC_STORE_MAGIC, MUSIC_STORE_VERSION, r.data, r.size)) {
        if (get_u64(&r, &next.snapshot_version) && next.snapshot_version) {
            rc = decode(&r, &next);
            if (rc == MUSIC_MODEL_OK) {
                next.snapshot_crc = storage_crc32(r.data, r.size);
                next.snapshot_current_id = next.queue.current_id;
                next.snapshot_dirty = false;
                load_progress(&next);
            }
        }
    } else if (storage_record_load(path, MUSIC_STORE_MAGIC, 1u, r.data, r.size)) {
        rc = decode(&r, &next);
    }
    music_model_free(r.data);
    if (rc == MUSIC_MODEL_OK)
    {
        (void)music_store_revalidate(&next);
        music_store_free(state); *state = next;
    }
    else music_store_free(&next);
    return rc;
}

music_model_result_t music_store_revalidate(music_store_state_t *state)
{
    bool changed = false, unavailable = false;
    for (size_t i = 0; i < state->queue.count; ++i)
    {
        music_queue_entry_t *e = state->queue.entries + i;
        music_file_stamp_t stamp;
        music_model_result_t rc = music_library_stat(e->path, e->stamp.signature_valid, &stamp);
        if (e->unavailable != (rc != MUSIC_MODEL_OK)) music_store_mark_changed(state);
        e->unavailable = rc != MUSIC_MODEL_OK;
        if (e->unavailable) { unavailable = true; continue; }
        if (!music_library_stamp_equal(&e->stamp, &stamp))
        {
            if (e->id == state->queue.current_id) state->queue.position_ms = 0;
            e->stamp = stamp; changed = true;
            music_store_mark_changed(state);
        }
    }
    return unavailable ? MUSIC_MODEL_IO : changed ? MUSIC_MODEL_CHANGED : MUSIC_MODEL_OK;
}
