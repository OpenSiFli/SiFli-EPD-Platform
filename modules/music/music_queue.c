#include "music_queue.h"
#include <stdlib.h>
#include <string.h>
#include "storage_file.h"
#ifndef MUSIC_MODEL_HOST
#include "epd_memory.h"
#endif

static music_model_allocator_t model_allocator;

void music_model_set_allocator(const music_model_allocator_t *allocator)
{
    if (allocator) model_allocator = *allocator;
    else memset(&model_allocator, 0, sizeof(model_allocator));
}

void *music_model_alloc(size_t size)
{
    if (model_allocator.allocate) return model_allocator.allocate(size);
#ifdef MUSIC_MODEL_HOST
    return malloc(size);
#else
    return epd_app_alloc(size, EPD_APP_PSRAM);
#endif
}

void *music_model_realloc(void *pointer, size_t size)
{
    if (model_allocator.resize) return model_allocator.resize(pointer, size);
#ifdef MUSIC_MODEL_HOST
    return realloc(pointer, size);
#else
    return epd_app_realloc(pointer, size, EPD_APP_PSRAM);
#endif
}

void music_model_free(void *pointer)
{
    if (!pointer) return;
    if (model_allocator.release) { model_allocator.release(pointer); return; }
#ifdef MUSIC_MODEL_HOST
    free(pointer);
#else
    epd_app_free(pointer);
#endif
}

char *music_model_strdup(const char *text)
{
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = music_model_alloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static size_t index_of(const music_queue_t *q, uint32_t id)
{
    for (size_t i = 0; i < q->count; ++i) if (q->entries[i].id == id) return i;
    return q->count;
}

static music_model_result_t reserve(void **data, size_t *capacity, size_t count, size_t unit)
{
    if (count <= *capacity) return MUSIC_MODEL_OK;
    if (count > SIZE_MAX / unit) return MUSIC_MODEL_RANGE;
    size_t next = *capacity ? *capacity : 8;
    while (next < count)
    {
        if (next > SIZE_MAX / 2 / unit) { next = count; break; }
        next *= 2;
    }
    void *copy = music_model_realloc(*data, next * unit);
    if (!copy) return MUSIC_MODEL_MEMORY;
    *data = copy;
    *capacity = next;
    return MUSIC_MODEL_OK;
}

void music_queue_init(music_queue_t *q, uint32_t seed)
{
    memset(q, 0, sizeof(*q));
    q->next_id = 1;
    q->random_state = seed ? seed : 0x9e3779b9u;
}

void music_queue_clear(music_queue_t *q)
{
    uint32_t next_id = q->next_id, seed = q->random_state;
    music_mode_t mode = q->mode;
    for (size_t i = 0; i < q->count; ++i) music_model_free(q->entries[i].path);
    music_model_free(q->entries);
    music_model_free(q->shuffle);
    music_model_free(q->history);
    music_queue_init(q, seed);
    q->next_id = next_id;
    q->mode = mode;
}

void music_queue_free(music_queue_t *q)
{
    music_queue_clear(q);
    music_queue_init(q, 0);
}

const music_queue_entry_t *music_queue_find(const music_queue_t *q, uint32_t id)
{
    size_t index = index_of(q, id);
    return index < q->count ? &q->entries[index] : NULL;
}

const music_queue_entry_t *music_queue_current(const music_queue_t *q)
{
    return music_queue_find(q, q->current_id);
}

static void invalidate_shuffle(music_queue_t *q)
{
    music_model_free(q->shuffle);
    q->shuffle = NULL;
    q->shuffle_count = q->shuffle_cursor = 0;
}

music_model_result_t music_queue_insert(music_queue_t *q, size_t index, const char *path, uint32_t *id)
{
    if (!path || !*path || index > q->count) return MUSIC_MODEL_INVALID;
    if (strlen(path) >= STORAGE_PATH_MAX) return MUSIC_MODEL_RANGE;
    if (!q->next_id || q->count == SIZE_MAX) return MUSIC_MODEL_RANGE;
    char *copy = music_model_strdup(path);
    if (!copy) return MUSIC_MODEL_MEMORY;
    music_model_result_t rc = reserve((void **)&q->entries, &q->capacity,
                                      q->count + 1, sizeof(*q->entries));
    if (rc != MUSIC_MODEL_OK) { music_model_free(copy); return rc; }
    memmove(q->entries + index + 1, q->entries + index, (q->count - index) * sizeof(*q->entries));
    music_queue_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.path = copy;
    entry.id = q->next_id++;
    entry.attempt_epoch = q->attempt_active ? q->attempt_epoch : 0;
    q->entries[index] = entry;
    ++q->count;
    invalidate_shuffle(q);
    if (id) *id = entry.id;
    return MUSIC_MODEL_OK;
}

music_model_result_t music_queue_append(music_queue_t *q, const char *path, uint32_t *id)
{
    return music_queue_insert(q, q->count, path, id);
}

music_model_result_t music_queue_insert_next(music_queue_t *q, const char *path, uint32_t *id)
{
    size_t index = index_of(q, q->current_id);
    return music_queue_insert(q, index < q->count ? index + 1 : q->count, path, id);
}

static music_model_result_t prepare_history(music_queue_t *q)
{
    size_t count = q->history_count ? q->history_cursor + 1 : 0;
    size_t needed = count < MUSIC_HISTORY_LIMIT ? count + 1 : MUSIC_HISTORY_LIMIT;
    if (needed <= q->history_capacity) return MUSIC_MODEL_OK;
    size_t capacity = q->history_capacity ? q->history_capacity * 2 : 8;
    if (capacity < needed) capacity = needed;
    if (capacity > MUSIC_HISTORY_LIMIT) capacity = MUSIC_HISTORY_LIMIT;
    void *copy = music_model_realloc(q->history, capacity * sizeof(*q->history));
    if (!copy) return MUSIC_MODEL_MEMORY;
    q->history = copy;
    q->history_capacity = capacity;
    return MUSIC_MODEL_OK;
}

static void record_selection(music_queue_t *q, uint32_t id)
{
    size_t count = q->history_count ? q->history_cursor + 1 : 0;
    if (!count || q->history[count - 1] != id) {
        if (count == MUSIC_HISTORY_LIMIT) {
            --count;
            memmove(q->history, q->history + 1, count * sizeof(*q->history));
        }
        q->history[count++] = id;
    }
    q->history_count = count;
    q->history_cursor = count ? count - 1 : 0;
    q->current_id = id;
    q->position_ms = 0;
}

music_model_result_t music_queue_select(music_queue_t *q, uint32_t id)
{
    if (!music_queue_find(q, id)) return MUSIC_MODEL_NOT_FOUND;
    music_model_result_t rc = prepare_history(q);
    if (rc != MUSIC_MODEL_OK) return rc;
    record_selection(q, id);
    invalidate_shuffle(q);
    music_queue_attempt_end(q);
    return MUSIC_MODEL_OK;
}

music_model_result_t music_queue_set_mode(music_queue_t *q, music_mode_t mode)
{
    if ((unsigned)mode > MUSIC_MODE_SHUFFLE) return MUSIC_MODEL_INVALID;
    if (q->mode != mode) { invalidate_shuffle(q); q->mode = mode; }
    music_queue_attempt_end(q);
    return MUSIC_MODEL_OK;
}

static void erase_id(uint32_t *ids, size_t *count, size_t *cursor, uint32_t id, bool next_cursor)
{
    size_t write = 0, before = 0;
    for (size_t i = 0; i < *count; ++i)
    {
        if (ids[i] == id) { if (i < *cursor || (!next_cursor && i == *cursor)) ++before; }
        else ids[write++] = ids[i];
    }
    *cursor = before <= *cursor ? *cursor - before : 0;
    *count = write;
    if (!next_cursor && write && *cursor >= write) *cursor = write - 1;
    if (!write) *cursor = 0;
}

music_model_result_t music_queue_remove(music_queue_t *q, uint32_t id)
{
    size_t index = index_of(q, id);
    if (index == q->count) return MUSIC_MODEL_NOT_FOUND;
    bool current = q->current_id == id;
    uint32_t next = current && index + 1 < q->count ? q->entries[index + 1].id : 0;
    if (next)
    {
        music_model_result_t rc = prepare_history(q);
        if (rc != MUSIC_MODEL_OK) return rc;
    }
    music_model_free(q->entries[index].path);
    memmove(q->entries + index, q->entries + index + 1, (q->count - index - 1) * sizeof(*q->entries));
    --q->count;
    erase_id(q->shuffle, &q->shuffle_count, &q->shuffle_cursor, id, true);
    erase_id(q->history, &q->history_count, &q->history_cursor, id, false);
    if (current)
    {
        q->current_id = next;
        q->position_ms = 0;
        if (next) record_selection(q, next);
    }
    return MUSIC_MODEL_OK;
}

music_model_result_t music_queue_move(music_queue_t *q, uint32_t id, size_t index)
{
    size_t old = index_of(q, id);
    if (old == q->count) return MUSIC_MODEL_NOT_FOUND;
    if (index >= q->count) return MUSIC_MODEL_INVALID;
    music_queue_entry_t entry = q->entries[old];
    if (old < index) memmove(q->entries + old, q->entries + old + 1, (index - old) * sizeof(entry));
    else if (old > index) memmove(q->entries + index + 1, q->entries + index, (old - index) * sizeof(entry));
    q->entries[index] = entry;
    return MUSIC_MODEL_OK;
}

static uint32_t random_word(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    *state = value;
    return value;
}

static size_t random_below(uint32_t *state, size_t count)
{
    uint32_t bound = (uint32_t)count, value, threshold = (0u - bound) % bound;
    do { value = random_word(state); } while (value < threshold);
    return value % bound;
}

static music_model_result_t advance(music_queue_t *q, bool natural, bool failure)
{
    if (!q->count) return MUSIC_MODEL_END;
    if (!failure && natural && q->mode == MUSIC_MODE_REPEAT_ONE && q->current_id)
    { q->position_ms = 0; return MUSIC_MODEL_OK; }
    if (!failure && q->mode == MUSIC_MODE_SHUFFLE && q->history_count &&
        q->history_cursor + 1 < q->history_count)
    {
        q->current_id = q->history[++q->history_cursor];
        q->position_ms = 0;
        return MUSIC_MODEL_OK;
    }
    uint32_t id = 0, seed = q->random_state;
    uint32_t *order = q->shuffle, *fresh = NULL;
    size_t cursor = q->shuffle_cursor, count = q->shuffle_count;
    if (q->mode == MUSIC_MODE_SHUFFLE)
    {
        if (count != q->count || cursor == count)
        {
            if (q->count > SIZE_MAX / sizeof(*fresh)) return MUSIC_MODEL_RANGE;
            fresh = music_model_alloc(q->count * sizeof(*fresh));
            if (!fresh) return MUSIC_MODEL_MEMORY;
            count = q->count;
            for (size_t i = 0; i < count; ++i) fresh[i] = q->entries[i].id;
            for (size_t i = count; i > 1; --i)
            {
                size_t j = random_below(&seed, i);
                uint32_t tmp = fresh[i - 1]; fresh[i - 1] = fresh[j]; fresh[j] = tmp;
            }
            cursor = 0;
            if (!q->shuffle_count && q->current_id)
            {
                for (size_t i = 0; i < count; ++i) if (fresh[i] == q->current_id)
                { uint32_t tmp = fresh[0]; fresh[0] = fresh[i]; fresh[i] = tmp; break; }
                cursor = count > 1 ? 1 : 0;
            }
            else if (count > 1 && fresh[0] == q->current_id)
            { uint32_t tmp = fresh[0]; fresh[0] = fresh[1]; fresh[1] = tmp; }
            order = fresh;
        }
        while (cursor < count)
        {
            uint32_t candidate = order[cursor++];
            const music_queue_entry_t *entry = music_queue_find(q, candidate);
            if (!failure || entry->attempt_epoch != q->attempt_epoch) { id = candidate; break; }
        }
        /* A failed chain can cross the shuffle-round boundary once. */
        if (!id && failure)
            for (size_t i = 0; i < q->count; ++i)
                if (q->entries[i].attempt_epoch != q->attempt_epoch) { id = q->entries[i].id; break; }
    }
    else
    {
        size_t index = index_of(q, q->current_id);
        size_t next = index < q->count ? index + 1 : 0;
        for (size_t seen = 0; seen < q->count; ++seen, ++next)
        {
            if (next >= q->count)
            {
                if (q->mode == MUSIC_MODE_SEQUENCE) break;
                next = 0;
            }
            if (!failure || q->entries[next].attempt_epoch != q->attempt_epoch)
            { id = q->entries[next].id; break; }
        }
    }
    if (!id) { music_model_free(fresh); return MUSIC_MODEL_END; }
    music_model_result_t rc = prepare_history(q);
    if (rc != MUSIC_MODEL_OK) { music_model_free(fresh); return rc; }
    if (fresh) { music_model_free(q->shuffle); q->shuffle = fresh; }
    if (q->mode == MUSIC_MODE_SHUFFLE)
    { q->shuffle_count = count; q->shuffle_cursor = cursor; q->random_state = seed; }
    record_selection(q, id);
    if (failure) q->entries[index_of(q, id)].attempt_epoch = q->attempt_epoch;
    return MUSIC_MODEL_OK;
}

music_model_result_t music_queue_next(music_queue_t *q, bool natural_end)
{
    music_model_result_t rc = advance(q, natural_end, false);
    if (rc == MUSIC_MODEL_OK) music_queue_attempt_end(q);
    return rc;
}

music_model_result_t music_queue_previous(music_queue_t *q)
{
    if (q->mode == MUSIC_MODE_SHUFFLE)
    {
        if (!q->history_count) return MUSIC_MODEL_END;
        if (q->history[q->history_cursor] == q->current_id)
        {
            if (!q->history_cursor) return MUSIC_MODEL_END;
            --q->history_cursor;
        }
        q->current_id = q->history[q->history_cursor];
        q->position_ms = 0;
        music_queue_attempt_end(q);
        return MUSIC_MODEL_OK;
    }
    size_t index = index_of(q, q->current_id);
    if (!q->count || index == q->count) return MUSIC_MODEL_END;
    if (!index)
    {
        if (q->mode == MUSIC_MODE_SEQUENCE) return MUSIC_MODEL_END;
        index = q->count;
    }
    return music_queue_select(q, q->entries[index - 1].id);
}

void music_queue_attempt_begin(music_queue_t *q)
{
    if (++q->attempt_epoch == 0)
    {
        for (size_t i = 0; i < q->count; ++i) q->entries[i].attempt_epoch = 0;
        q->attempt_epoch = 1;
    }
    q->attempt_active = true;
    size_t index = index_of(q, q->current_id);
    if (index < q->count) q->entries[index].attempt_epoch = q->attempt_epoch;
}

void music_queue_attempt_end(music_queue_t *q) { q->attempt_active = false; }

music_model_result_t music_queue_failed_next(music_queue_t *q)
{
    if (!q->attempt_active) music_queue_attempt_begin(q);
    size_t failed_cursor = q->history_cursor;
    bool has_failed_history = q->history_count && q->history[failed_cursor] == q->current_id;
    music_model_result_t rc = advance(q, false, true);
    if (has_failed_history && (rc == MUSIC_MODEL_OK || rc == MUSIC_MODEL_END))
    {
        /* A successful advance at the retained edge evicted the oldest ID. */
        if (rc == MUSIC_MODEL_OK && failed_cursor == MUSIC_HISTORY_LIMIT - 1) --failed_cursor;
        memmove(q->history + failed_cursor, q->history + failed_cursor + 1,
                (q->history_count - failed_cursor - 1) * sizeof(*q->history));
        --q->history_count;
        if (q->history_cursor > failed_cursor) --q->history_cursor;
        if (!q->history_count) q->history_cursor = 0;
        else if (q->history_cursor >= q->history_count) q->history_cursor = q->history_count - 1;
    }
    return rc;
}

bool music_queue_valid(const music_queue_t *q)
{
    if (!q || q->count > q->capacity || q->count > UINT32_MAX ||
        (q->count && !q->entries) || (unsigned)q->mode > MUSIC_MODE_SHUFFLE ||
        !q->random_state || q->shuffle_cursor > q->shuffle_count ||
        (q->shuffle_count && (q->shuffle_count != q->count || !q->shuffle)) ||
        q->history_capacity > MUSIC_HISTORY_LIMIT ||
        q->history_count > q->history_capacity || (q->history_count &&
        (!q->history || q->history_cursor >= q->history_count)) ||
        (!q->history_count && q->history_cursor) ||
        (q->current_id && !music_queue_find(q, q->current_id))) return false;
    for (size_t i = 0; i < q->count; ++i)
    {
        const music_queue_entry_t *e = q->entries + i;
        if (!e->id || !e->path || !*e->path || strlen(e->path) >= STORAGE_PATH_MAX ||
            (q->next_id && e->id >= q->next_id)) return false;
        for (size_t j = 0; j < i; ++j) if (q->entries[j].id == e->id) return false;
    }
    for (size_t i = 0; i < q->shuffle_count; ++i)
    {
        if (!music_queue_find(q, q->shuffle[i])) return false;
        for (size_t j = 0; j < i; ++j) if (q->shuffle[j] == q->shuffle[i]) return false;
    }
    for (size_t i = 0; i < q->history_count; ++i)
        if (!music_queue_find(q, q->history[i])) return false;
    return true;
}

music_model_result_t music_queue_clone(music_queue_t *out, const music_queue_t *source)
{
    if (!music_queue_valid(source)) return MUSIC_MODEL_INVALID;
    music_queue_t copy = *source;
    copy.entries = NULL; copy.shuffle = NULL; copy.history = NULL;
    copy.count = copy.capacity = 0;
    copy.history_capacity = source->history_count;
    if (source->count)
    {
        if (source->count > SIZE_MAX / sizeof(*copy.entries)) return MUSIC_MODEL_RANGE;
        copy.entries = music_model_alloc(source->count * sizeof(*copy.entries));
        if (!copy.entries) goto memory;
        copy.capacity = source->count;
        for (; copy.count < source->count; ++copy.count)
        {
            copy.entries[copy.count] = source->entries[copy.count];
            copy.entries[copy.count].path = music_model_strdup(source->entries[copy.count].path);
            if (!copy.entries[copy.count].path) goto memory;
        }
    }
    if (source->shuffle_count)
    {
        copy.shuffle = music_model_alloc(source->shuffle_count * sizeof(*copy.shuffle));
        if (!copy.shuffle) goto memory;
        memcpy(copy.shuffle, source->shuffle, source->shuffle_count * sizeof(*copy.shuffle));
    }
    if (source->history_count)
    {
        if (source->history_count > SIZE_MAX / sizeof(*copy.history)) goto memory;
        copy.history = music_model_alloc(source->history_count * sizeof(*copy.history));
        if (!copy.history) goto memory;
        memcpy(copy.history, source->history, source->history_count * sizeof(*copy.history));
    }
    music_queue_free(out);
    *out = copy;
    return MUSIC_MODEL_OK;
memory:
    music_queue_free(&copy);
    return MUSIC_MODEL_MEMORY;
}
