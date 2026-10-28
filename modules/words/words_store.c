#include "storage_file.h"
#include "storage.h"
#include "epd_memory.h"
#include "words_store.h"
#include <dfs_posix.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

static char root[64], config_path[96], content_path[96];
#define CHUNK_RECORDS 32u
#define CHUNK_MAGIC 0x57444331u
#define CONFIG_MAGIC 0x57444346u
typedef struct { uint32_t count; words_record_t records[CHUNK_RECORDS]; } chunk_t;

static words_index_t *index_data;
static chunk_t *cache;
static words_config_t config;
static uint32_t count, capacity, cached_chunk;
static bool dirty_chunk, dirty_config;
static words_message_t error;
/* V1 configuration is read once; record chunks and pending slot identities stay unchanged. */
typedef struct
{
    char book[STORAGE_PATH_MAX], title[WORDS_KEY_SIZE], imported_title[WORDS_KEY_SIZE];
    uint8_t identity[24];
    uint32_t cursor, pending, quota;
} legacy_config_t;

static bool fail(words_message_t code) { error = code; return false; }
static void chunk_path(char *path, uint32_t n) { snprintf(path, 96, "%s/states/%03lu.dat", root, (unsigned long)n); }

static bool reserve(uint32_t needed)
{
    if (needed <= capacity) return true;
    /* Slots are returned as int; multiplication must also fit the allocator. */
    if (needed > INT_MAX || needed > SIZE_MAX / sizeof(*index_data)) return fail(WORDS_MESSAGE_INDEX_LIMIT);
    size_t size = needed;
    size_t extra = 128u - needed % 128u;
    if (extra != 128u && extra <= SIZE_MAX / sizeof(*index_data) - size && extra <= INT_MAX - size)
        size += extra;
    words_index_t *next = epd_app_realloc(index_data, size * sizeof(*next), EPD_APP_PSRAM);
    if (!next && size != needed)
    {
        size = needed;
        next = epd_app_realloc(index_data, size * sizeof(*next), EPD_APP_PSRAM);
    }
    if (!next) return fail(WORDS_MESSAGE_MEMORY);
    index_data = next; capacity = (uint32_t)size;
    return true;
}

static void index_set(uint32_t slot, const words_record_t *record)
{
    index_data[slot] = (words_index_t){.due = record->card.due, .hash = words_word_hash(record->word),
                                     .phase = record->card.phase, .flags = (uint8_t)record->flags};
}

bool words_store_flush(void)
{
    if ((dirty_chunk || dirty_config) && !storage_app_available("words")) return fail(WORDS_MESSAGE_STORAGE_UNAVAILABLE);
    if (dirty_chunk)
    {
        char path[96]; chunk_path(path, cached_chunk);
        if (!storage_record_save(path, CHUNK_MAGIC, 1, cache, sizeof(*cache)))
            return fail(WORDS_MESSAGE_SAVE_FAILED);
        dirty_chunk = false;
    }
    if (dirty_config)
    {
        if (!storage_record_save(config_path, CONFIG_MAGIC, 2, &config, sizeof(config)))
            return fail(WORDS_MESSAGE_SETTINGS_SAVE_FAILED);
        dirty_config = false;
    }
    error = WORDS_MESSAGE_NONE;
    return true;
}

static bool load(uint32_t chunk, bool create)
{
    if (chunk == cached_chunk) return true;
    if (!words_store_flush()) return false;
    char path[96]; chunk_path(path, chunk);
    if (create) memset(cache, 0, sizeof(*cache));
    else if (!storage_record_load(path, CHUNK_MAGIC, 1, cache, sizeof(*cache)))
    { cached_chunk = UINT32_MAX; return fail(WORDS_MESSAGE_RECORDS_READ_FAILED); }
    cached_chunk = chunk;
    return true;
}

bool words_store_open(void)
{
    count = capacity = 0; cached_chunk = UINT32_MAX;
    dirty_chunk = dirty_config = false; error = WORDS_MESSAGE_NONE;
    char states[96];
    if (!storage_app_path(root, sizeof(root), "words", STORAGE_APP_DATA, "") ||
        !storage_app_path(config_path, sizeof(config_path), "words", STORAGE_APP_DATA, "config.dat") ||
        !storage_app_path(content_path, sizeof(content_path), "words", STORAGE_APP_DATA, "content.bin") ||
        !storage_app_path(states, sizeof(states), "words", STORAGE_APP_DATA, "states")) return fail(WORDS_MESSAGE_STORAGE_UNAVAILABLE);
    cache = epd_app_alloc(sizeof(*cache), EPD_APP_PSRAM);
    if (!cache) return fail(WORDS_MESSAGE_MEMORY);
    memset(&config, 0, sizeof(config));
    strcpy(config.scope_name, "请选择学习范围"); config.quota = 20;
    if (!storage_mkdirs(states) || !storage_file_recover(config_path)) return fail(WORDS_MESSAGE_STORAGE_UNAVAILABLE);
    struct stat st;
    storage_lock();
    int config_status = stat(config_path, &st), config_errno = errno;
    storage_unlock();
    if (config_status != 0 && config_errno != -ENOENT) return fail(WORDS_MESSAGE_SETTINGS_READ_FAILED);
    bool exists = config_status == 0;
    if (exists && !storage_record_load(config_path, CONFIG_MAGIC, 2, &config, sizeof(config)))
    {
        legacy_config_t old;
        if (!storage_record_load(config_path, CONFIG_MAGIC, 1, &old, sizeof(old)) ||
            !memchr(old.book, 0, sizeof(old.book)) || !memchr(old.title, 0, sizeof(old.title)) ||
            !memchr(old.imported_title, 0, sizeof(old.imported_title)) || old.quota > 200)
            return fail(WORDS_MESSAGE_SETTINGS_CORRUPT);
        memset(&config, 0, sizeof(config));
        config.quota = old.quota; config.pending = old.pending;
        strcpy(config.scope_name, "请选择学习范围"); dirty_config = true;
    }
    if (!memchr(config.scope_id, 0, sizeof(config.scope_id)) ||
        !memchr(config.scope_name, 0, sizeof(config.scope_name)) || config.quota > 200) return fail(WORDS_MESSAGE_SETTINGS_CORRUPT);
    for (uint32_t n = 0; ; ++n)
    {
        char path[96]; chunk_path(path, n);
        storage_lock();
        bool recovered = storage_app_available("words") && storage_file_recover(path);
        int status = recovered ? stat(path, &st) : -1;
        int reason = errno;
        storage_unlock();
        if (!recovered) return fail(WORDS_MESSAGE_RECORDS_READ_FAILED);
        if (status != 0)
        {
            if (reason == -ENOENT) break;
            return fail(WORDS_MESSAGE_RECORDS_READ_FAILED);
        }
        if (!load(n, false)) return false;
        if (!cache->count || cache->count > CHUNK_RECORDS) return fail(WORDS_MESSAGE_RECORDS_CORRUPT);
        if (cache->count > UINT32_MAX - count) return fail(WORDS_MESSAGE_INDEX_LIMIT);
        if (!reserve(count + cache->count)) return false;
        for (uint32_t j = 0; j < cache->count; ++j)
        {
            words_record_t *r = &cache->records[j];
            if (!r->word[0] || !memchr(r->word, 0, sizeof(r->word)) || r->card.phase > WORDS_RELEARNING ||
                r->content_size < WORDS_FIELD_COUNT || r->content_offset > INT32_MAX ||
                r->content_size > INT32_MAX - r->content_offset ||
                (r->flags & ~(WORDS_COLLECTED | WORDS_PAUSED)))
                return fail(WORDS_MESSAGE_RECORDS_CORRUPT);
            index_set(count++, r);
        }
        if (cache->count < CHUNK_RECORDS) break;
    }
    if (config.pending > count) { config.pending = 0; dirty_config = true; }
    return true;
}

void words_store_close(void)
{
    epd_app_free(cache); cache = NULL;
    epd_app_free(index_data); index_data = NULL;
    count = capacity = 0;
}
bool words_store_dirty(void) { return dirty_chunk || dirty_config; }
words_message_t words_store_error(void) { return error; }
words_config_t *words_store_config(void) { return &config; }
void words_store_config_changed(void) { dirty_config = true; }
uint32_t words_store_count(void) { return count; }
const words_index_t *words_store_index(void) { return index_data; }

bool words_store_get(uint32_t slot, words_record_t *record)
{
    if (slot >= count || !load(slot / CHUNK_RECORDS, false)) return false;
    *record = cache->records[slot % CHUNK_RECORDS]; return true;
}
bool words_store_put(uint32_t slot, const words_record_t *record)
{
    if (slot > count || slot == UINT32_MAX || !reserve(slot + 1) ||
        !load(slot / CHUNK_RECORDS, slot == count && slot % CHUNK_RECORDS == 0)) return false;
    cache->records[slot % CHUNK_RECORDS] = *record;
    if (slot == count) { ++count; ++cache->count; }
    dirty_chunk = true; index_set(slot, record);
    return true;
}

int words_store_find(const char *word)
{
    uint32_t hash = words_word_hash(word);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (index_data[i].hash != hash) continue;
        words_record_t record;
        if (!words_store_get(i, &record)) return -2;
        if (!strcmp(record.word, word)) return (int)i;
    }
    return -1;
}

static bool transfer(int fd, void *data, size_t length, bool writing)
{
    size_t done = 0;
    while (done < length)
    {
        if (!storage_app_available("words")) return false;
        int n = writing ? write(fd, (char *)data + done, length - done) : read(fd, (char *)data + done, length - done);
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

int words_store_ensure(const words_result_t *entry)
{
    if (!entry->fields[WORDS_WORD] || !entry->content || !entry->content_size) return -2;
    int found = words_store_find(entry->fields[WORDS_WORD]);
    if (found == -2) return -2;
    if (found < 0 && (count == UINT32_MAX || !reserve(count + 1))) return -2;
    words_record_t record = {0};
    if (found >= 0 && !words_store_get(found, &record)) return -2;
    uint32_t crc = storage_crc32(entry->content, entry->content_size);
    if (found >= 0 && record.content_size == entry->content_size && record.content_crc == crc) return found;
    strcpy(record.word, entry->fields[WORDS_WORD]);
    record.content_size = entry->content_size; record.content_crc = crc;
    storage_lock();
    int fd = storage_app_available("words") ? open(content_path, O_WRONLY | O_CREAT, 0) : -1;
    off_t offset = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    bool ok = offset >= 0 && (uint64_t)offset + record.content_size < INT32_MAX &&
              transfer(fd, (void *)entry->content, record.content_size, true) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    storage_unlock();
    if (!ok) { fail(WORDS_MESSAGE_ENTRY_SAVE_FAILED); return -2; }
    record.content_offset = (uint32_t)offset;
    uint32_t slot = found >= 0 ? (uint32_t)found : count;
    return words_store_put(slot, &record) ? (int)slot : -2;
}

bool words_store_content(uint32_t slot, words_result_t *result)
{
    words_record_t record;
    if (!words_store_get(slot, &record)) return false;
    storage_lock();
    struct stat st;
    bool valid = storage_app_available("words") && stat(content_path, &st) == 0 &&
        st.st_size >= 0 && (uint64_t)record.content_offset + record.content_size <= (uint64_t)st.st_size;
    storage_unlock();
    if (!valid) return fail(WORDS_MESSAGE_SAVED_ENTRY_READ_FAILED);
    if (!words_result_reserve(result, record.content_size)) return fail(WORDS_MESSAGE_MEMORY);
    storage_lock();
    int fd = storage_app_available("words") ? open(content_path, O_RDONLY) : -1;
    bool ok = fd >= 0 && lseek(fd, record.content_offset, SEEK_SET) == (off_t)record.content_offset &&
              transfer(fd, result->content, record.content_size, false);
    if (fd >= 0) close(fd);
    storage_unlock();
    if (!ok || storage_crc32(result->content, record.content_size) != record.content_crc)
        return fail(WORDS_MESSAGE_SAVED_ENTRY_READ_FAILED);
    if (!words_result_parse(result)) return fail(result->error ? result->error : WORDS_MESSAGE_SAVED_ENTRY_CORRUPT);
    if (strcmp(record.word, result->fields[WORDS_WORD])) return fail(WORDS_MESSAGE_SAVED_ENTRY_CORRUPT);
    result->slot = slot + 1; result->flags = record.flags;
    return true;
}

bool words_store_counts(int32_t day, int64_t now, words_counts_t *counts)
{
    memset(counts, 0, sizeof(*counts));
    for (uint32_t i = 0; i < count; ++i)
    {
        words_record_t record;
        if (!words_store_get(i, &record)) return false;
        words_count_record(counts, &record, day);
        if (record.card.phase != WORDS_NEW && !(record.flags & WORDS_PAUSED) && record.card.due <= now)
            ++counts->due;
    }
    return true;
}
