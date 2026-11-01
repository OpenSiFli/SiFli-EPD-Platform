#include "ui_reading_font_source.h"
#include "reading_font.h"
#include "storage_file.h"
#include "epd_memory.h"
#include "lvgl.h"
#include <rthw.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE 4096u
#define BLOCK_COUNT 128u
#define BLOCK_WAYS 4u
#define INDEX_BUDGET (384u * 1024u)
#define SOURCE_LIMIT 16u
#define TABLE_COUNT 6u
#define CURSOR_CACHE_SIZE 1024u

typedef struct
{
    uint32_t offset, length;
    uint8_t *bytes;
} reading_table_t;

typedef struct
{
    unsigned references;
    reading_table_t tables[TABLE_COUNT];
} reading_index_t;

struct ui_reading_font_source
{
    struct ui_reading_font_source *next;
    reading_font_lease_t lease;
    uint32_t id, token, position;
    storage_volume_t volume;
    int fd;
    bool leased, opened, fault;
    char path[20];
    reading_index_t *index;
    uint32_t reads, read_bytes, hits, table_bytes, callbacks;
    rt_tick_t read_ticks, lock_ticks;
    bool reported;
};

typedef struct
{
    uint32_t id, signature, session, revision, failure_revision, size, offset, length;
    storage_volume_t volume;
    bool valid;
    unsigned age;
    uint8_t bytes[BLOCK_SIZE];
} reading_block_t;

static struct rt_mutex source_lock;
static lv_fs_drv_t source_driver;
static bool initialized;
static ui_reading_font_source_t *sources;
static reading_block_t *blocks;
static unsigned source_count;
static size_t index_bytes;
static uint32_t next_token;

static bool storage_valid(const ui_reading_font_source_t *source)
{
    return storage_session_valid(source->volume, source->lease.session) &&
        !storage_file_writers(source->lease.path) &&
        storage_path_revision(source->lease.path) == source->lease.revision;
}

static void fault_locked(ui_reading_font_source_t *source)
{
    if (source->fault) return;
    source->fault = true;
    if (source->leased) reading_font_report_failure(source->id);
}

/* Lease validation only inspects memory. Raw I/O takes storage_lock after
 * validation; no service call is made while storage_lock is held. */
static bool valid_locked(ui_reading_font_source_t *source)
{
    if (!source || source->fault || !source->leased || source->fd < 0) return false;
    if (!reading_font_lease_valid(&source->lease)) source->fault = true;
    return !source->fault;
}

static bool block_matches(const reading_block_t *block, const reading_font_lease_t *lease)
{
    return block->valid && block->id == lease->id && block->signature == lease->signature &&
           block->session == lease->session && block->revision == lease->revision &&
           block->failure_revision == lease->failure_revision &&
           block->volume == lease->volume &&
           block->size == lease->size;
}

static void close_locked(ui_reading_font_source_t *source)
{
    if (!source->reported && source->leased) {
        source->reported = true;
        rt_kprintf("font io id=%lu reads=%lu bytes=%lu hits=%lu table_bytes=%lu\n",
            (unsigned long)source->id, (unsigned long)source->reads, (unsigned long)source->read_bytes,
            (unsigned long)source->hits, (unsigned long)source->table_bytes);
        rt_kprintf("font io_time id=%lu read_ms=%lu lock_wait_ms=%lu\n", (unsigned long)source->id,
            (unsigned long)((uint64_t)source->read_ticks * 1000u / RT_TICK_PER_SECOND),
            (unsigned long)((uint64_t)source->lock_ticks * 1000u / RT_TICK_PER_SECOND));
        rt_kprintf("font io_cache id=%lu callbacks=%lu cursor_bytes=%u\n",
            (unsigned long)source->id, (unsigned long)source->callbacks, CURSOR_CACHE_SIZE);
    }
    source->fault = true;
    if (blocks)
        for (unsigned i = 0; i < BLOCK_COUNT; ++i)
        {
            if (!block_matches(&blocks[i], &source->lease)) continue;
            bool shared = false;
            for (ui_reading_font_source_t *p = sources; p; p = p->next)
                if (p != source && !p->fault && p->leased && block_matches(&blocks[i], &p->lease))
                { shared = true; break; }
            if (!shared) blocks[i].valid = false;
        }
    if (source->fd >= 0)
    {
        storage_lock();
        /* Close also retires a descriptor from an expired card session. It
         * never opens the path again or accesses a replacement card's file. */
        if (!storage_valid(source)) source->fault = true;
        close(source->fd);
        source->fd = -1;
        storage_unlock();
    }
    if (source->index)
    {
        reading_index_t *index = source->index;
        source->index = NULL;
        if (!--index->references)
        {
            for (unsigned i = 0; i < TABLE_COUNT; ++i)
                if (index->tables[i].bytes)
                {
                    index_bytes -= index->tables[i].length;
                    epd_app_free(index->tables[i].bytes);
                }
            index_bytes -= sizeof(*index);
            epd_app_free(index);
        }
    }
    if (source->leased)
    {
        source->leased = false;
        reading_font_release(&source->lease);
    }
}

static void *source_open(lv_fs_drv_t *driver, const char *path, lv_fs_mode_t mode)
{
    (void)driver;
    if (mode != LV_FS_MODE_RD) return NULL;
    if (*path == '/') ++path;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    ui_reading_font_source_t *source;
    for (source = sources; source; source = source->next)
        if (!strcmp(path, source->path + 3)) break;
    if (!source || source->opened || !valid_locked(source)) source = NULL;
    else { source->opened = true; source->position = 0; }
    rt_mutex_release(&source_lock);
    return source;
}

static lv_fs_res_t source_close(lv_fs_drv_t *driver, void *file)
{
    (void)driver;
    ui_reading_font_source_t *source = file;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    close_locked(source);
    source->opened = false;
    rt_mutex_release(&source_lock);
    return LV_FS_RES_OK;
}

static reading_block_t *read_block(ui_reading_font_source_t *source, uint32_t offset)
{
    /* Four-way sets bound lookup cost independently of the cache capacity.
     * Saturating recency avoids timestamp wrap and preserves hot metadata. */
    unsigned set = ((offset / BLOCK_SIZE) ^ source->id) % (BLOCK_COUNT / BLOCK_WAYS);
    reading_block_t *first = blocks + set * BLOCK_WAYS, *block = first;
    for (unsigned i = 0; i < BLOCK_WAYS; ++i)
        if (first[i].age < BLOCK_WAYS) ++first[i].age;
    for (unsigned i = 0; i < BLOCK_WAYS; ++i)
    {
        if (block_matches(&first[i], &source->lease) && first[i].offset == offset)
        {
            first[i].age = 0;
            ++source->hits;
            return &first[i];
        }
    }
    for (unsigned i = 0; i < BLOCK_WAYS; ++i)
        if (!first[i].valid) { block = &first[i]; break; }
        else if (first[i].age > block->age) block = &first[i];
    block->age = 0;
    block->valid = false;
    uint32_t length = source->lease.size - offset;
    if (length > BLOCK_SIZE) length = BLOCK_SIZE;
    rt_tick_t started = rt_tick_get();
    storage_lock();
    source->lock_ticks += rt_tick_get() - started;
    started = rt_tick_get();
    bool ok = storage_valid(source);
    if (ok) ok = lseek(source->fd, (off_t)offset, SEEK_SET) == (off_t)offset;
    if (ok) ok = storage_valid(source);
    if (ok) ok = read(source->fd, block->bytes, length) == (int)length;
    source->read_ticks += rt_tick_get() - started;
    ++source->reads;
    if (ok) source->read_bytes += length;
    if (ok) ok = storage_valid(source);
    storage_unlock();
    if (!ok) { fault_locked(source); return NULL; }
    block->id = source->lease.id;
    block->signature = source->lease.signature;
    block->session = source->lease.session;
    block->revision = source->lease.revision;
    block->failure_revision = source->lease.failure_revision;
    block->volume = source->lease.volume;
    block->size = source->lease.size;
    block->offset = offset;
    block->length = length;
    block->valid = true;
    return block;
}

static lv_fs_res_t source_read(lv_fs_drv_t *driver, void *file, void *buffer,
                               uint32_t count, uint32_t *read_count)
{
    (void)driver;
    ui_reading_font_source_t *source = file;
    *read_count = 0;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    ++source->callbacks;
    bool ok = valid_locked(source);
    if (ok && source->position > source->lease.size)
    { fault_locked(source); ok = false; }
    /* LVGL fills its cursor cache past the requested field. EOF is a short
     * read; TinyTTF checks each actual field against the file size itself. */
    if (ok && count > source->lease.size - source->position)
        count = source->lease.size - source->position;
    while (ok && *read_count < count)
    {
        if (!valid_locked(source)) { ok = false; break; }
        bool indexed = false;
        for (unsigned i = 0; source->index && i < TABLE_COUNT; ++i)
        {
            const reading_table_t *table = &source->index->tables[i];
            uint32_t within = source->position - table->offset;
            if (!table->bytes || within >= table->length) continue;
            uint32_t length = table->length - within;
            if (length > count - *read_count) length = count - *read_count;
            memcpy((uint8_t *)buffer + *read_count, table->bytes + within, length);
            source->table_bytes += length;
            *read_count += length;
            source->position += length;
            indexed = true;
            break;
        }
        if (indexed) continue;
        uint32_t offset = source->position - source->position % BLOCK_SIZE;
        reading_block_t *block = read_block(source, offset);
        if (!block) { ok = false; break; }
        uint32_t within = source->position - offset;
        uint32_t length = block->length - within;
        if (length > count - *read_count) length = count - *read_count;
        memcpy((uint8_t *)buffer + *read_count, block->bytes + within, length);
        *read_count += length;
        source->position += length;
    }
    if (ok) ok = valid_locked(source);
    rt_mutex_release(&source_lock);
    return ok ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t source_seek(lv_fs_drv_t *driver, void *file, uint32_t position,
                               lv_fs_whence_t origin)
{
    (void)driver;
    ui_reading_font_source_t *source = file;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    bool ok = valid_locked(source);
    uint64_t next = UINT64_MAX;
    if (ok)
    {
        switch (origin)
        {
        case LV_FS_SEEK_SET: next = position; break;
        case LV_FS_SEEK_CUR: next = (uint64_t)source->position + position; break;
        case LV_FS_SEEK_END:
            if (position <= source->lease.size) next = source->lease.size - position;
            break;
        default: break;
        }
        ok = next <= source->lease.size;
        if (ok) source->position = (uint32_t)next;
        else fault_locked(source);
    }
    rt_mutex_release(&source_lock);
    return ok ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t source_tell(lv_fs_drv_t *driver, void *file, uint32_t *position)
{
    (void)driver;
    ui_reading_font_source_t *source = file;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    bool ok = valid_locked(source);
    *position = source->position;
    rt_mutex_release(&source_lock);
    return ok ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static uint32_t source_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Pin only bounded lookup tables, never the glyf payload. Loading a table uses
 * sequential block reads; all sizes/offsets are checked before allocation.
 * Identical live faces at different sizes share the same immutable index. */
static void index_prepare(ui_reading_font_source_t *source)
{
    for (ui_reading_font_source_t *p = sources; p; p = p->next)
        if (p->index && !p->fault && p->lease.id == source->lease.id &&
            p->lease.signature == source->lease.signature && p->lease.size == source->lease.size &&
            p->lease.volume == source->lease.volume && p->lease.session == source->lease.session &&
            p->lease.revision == source->lease.revision &&
            p->lease.failure_revision == source->lease.failure_revision)
        {
            source->index = p->index;
            ++source->index->references;
            return;
        }
    uint8_t header[12], record[16];
    uint32_t n;
    source->position = 0;
    if (source_read(NULL, source, header, sizeof(header), &n) != LV_FS_RES_OK || n != sizeof(header)) return;
    unsigned tables = (unsigned)header[4] << 8 | header[5];
    if (source_be32(header) != 0x10000u || !tables || tables > 128u ||
        tables * 16u > source->lease.size - sizeof(header)) return;
    if (sizeof(reading_index_t) > INDEX_BUDGET - index_bytes) return;
    reading_index_t *index = epd_app_alloc(sizeof(*index), EPD_APP_PSRAM);
    if (!index) return;
    memset(index, 0, sizeof(*index));
    index->references = 1;
    source->index = index;
    index_bytes += sizeof(*index);
    static const uint32_t tags[TABLE_COUNT] = {
        0x636d6170u, 0x6c6f6361u, 0x686d7478u, /* cmap, loca, hmtx */
        0x68656164u, 0x6d617870u, 0x68686561u  /* head, maxp, hhea */
    };
    for (unsigned t = 0; t < tables; ++t)
    {
        source->position = 12u + t * 16u;
        if (source_read(NULL, source, record, sizeof(record), &n) != LV_FS_RES_OK || n != sizeof(record)) break;
        for (unsigned i = 0; i < TABLE_COUNT; ++i)
        {
            if (source_be32(record) != tags[i] || index->tables[i].bytes) continue;
            uint32_t offset = source_be32(record + 8), length = source_be32(record + 12);
            if (offset > source->lease.size || !length || length > source->lease.size - offset ||
                length > INDEX_BUDGET - index_bytes) break;
            uint8_t *bytes = epd_app_alloc(length, EPD_APP_PSRAM);
            if (!bytes) break;
            source->position = offset;
            if (source_read(NULL, source, bytes, length, &n) != LV_FS_RES_OK || n != length)
            { epd_app_free(bytes); break; }
            index->tables[i] = (reading_table_t){offset, length, bytes};
            index_bytes += length;
            break;
        }
        if (source->fault) break;
    }
    source->position = 0;
}

static bool source_init(void)
{
    if (initialized) return true;
    if (lv_fs_get_drv('R') || rt_mutex_init(&source_lock, "reader_io", RT_IPC_FLAG_PRIO) != RT_EOK)
        return false;
    lv_fs_drv_init(&source_driver);
    source_driver.letter = 'R';
    /* Collapse TinyTTF's byte-sized reads and seeks. The outer font callbacks
     * validate the lease before and after consuming cached data. */
    source_driver.cache_size = CURSOR_CACHE_SIZE;
    source_driver.open_cb = source_open;
    source_driver.close_cb = source_close;
    source_driver.read_cb = source_read;
    source_driver.seek_cb = source_seek;
    source_driver.tell_cb = source_tell;
    lv_fs_drv_register(&source_driver);
    initialized = true;
    return true;
}

ui_reading_font_source_t *ui_reading_font_source_acquire(uint32_t id)
{
    if (!id || !source_init() || source_count >= SOURCE_LIMIT) return NULL;
    ui_reading_font_source_t *source = lv_malloc_zeroed(sizeof(*source));
    if (!source) return NULL;
    source->fd = -1;
    if (!reading_font_acquire(id, &source->lease)) { lv_free(source); return NULL; }
    source->leased = true;
    source->id = id;
    source->volume = source->lease.volume;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    if (!blocks)
    {
        blocks = epd_app_alloc(BLOCK_COUNT * sizeof(*blocks), EPD_APP_PSRAM);
        if (blocks) memset(blocks, 0, BLOCK_COUNT * sizeof(*blocks));
    }
    bool ok = blocks && source->lease.id == id && source->lease.size &&
              source->lease.size <= READING_FONT_FILE_MAX && reading_font_lease_valid(&source->lease);
    bool file_failed = false;
    if (ok)
    {
        struct stat st;
        storage_lock();
        ok = storage_valid(source);
        if (ok)
        {
            source->fd = open(source->lease.path, O_RDONLY);
            ok = source->fd >= 0;
            file_failed = !ok;
        }
        if (ok) ok = storage_valid(source);
        if (ok)
        {
            ok = fstat(source->fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size > 0 && (uint64_t)st.st_size == source->lease.size;
            file_failed = !ok;
        }
        if (ok) ok = storage_valid(source);
        storage_unlock();
    }
    if (ok) { index_prepare(source); source->position = 0; ok = valid_locked(source); }
    if (!ok)
    {
        /* A changed catalog/session or cache allocation failure is not a bad
         * font file. Report actual file failures outside the storage lock. */
        if (file_failed && reading_font_lease_valid(&source->lease)) fault_locked(source);
        close_locked(source);
        if (!sources) { epd_app_free(blocks); blocks = NULL; }
        rt_mutex_release(&source_lock);
        lv_free(source);
        return NULL;
    }
    bool used;
    do
    {
        source->token = ++next_token;
        used = !source->token;
        for (ui_reading_font_source_t *p = sources; p && !used; p = p->next)
            used = p->token == source->token;
    } while (used);
    snprintf(source->path, sizeof(source->path), "R:/%08lx.ttf", (unsigned long)source->token);
    source->next = sources;
    sources = source;
    ++source_count;
    rt_mutex_release(&source_lock);
    return source;
}

void ui_reading_font_source_release(ui_reading_font_source_t *source)
{
    if (!source) return;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    close_locked(source);
    ui_reading_font_source_t **link = &sources;
    while (*link && *link != source) link = &(*link)->next;
    if (*link) { *link = source->next; --source_count; }
    if (!sources) { epd_app_free(blocks); blocks = NULL; }
    rt_mutex_release(&source_lock);
    lv_free(source);
}

const char *ui_reading_font_source_path(const ui_reading_font_source_t *source) { return source->path; }
uint32_t ui_reading_font_source_signature(const ui_reading_font_source_t *source) { return source->lease.signature; }

bool ui_reading_font_source_valid(ui_reading_font_source_t *source)
{
    if (!source) return false;
    /* Source lifetime is UI-owned and revocation drains drawing. Validity is
     * memory-only, so a retained glyph never waits for an unrelated SD read. */
    rt_base_t level = rt_hw_interrupt_disable();
    bool ok = valid_locked(source);
    rt_hw_interrupt_enable(level);
    return ok;
}

void ui_reading_font_source_fault(ui_reading_font_source_t *source)
{
    if (!source) return;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    fault_locked(source);
    rt_mutex_release(&source_lock);
}

void ui_reading_font_source_revoke(uint32_t id)
{
    if (!initialized || !id) return;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    for (ui_reading_font_source_t *p = sources; p; p = p->next)
        if (p->id == id) close_locked(p);
    rt_mutex_release(&source_lock);
}

void ui_reading_font_source_revoke_storage(void)
{
    if (!initialized) return;
    rt_mutex_take(&source_lock, RT_WAITING_FOREVER);
    for (ui_reading_font_source_t *p = sources; p; p = p->next)
        if (p->volume == STORAGE_SD) close_locked(p);
    rt_mutex_release(&source_lock);
}
