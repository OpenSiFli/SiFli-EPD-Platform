#include "epd_memory.h"
#include "platform/epd_app_font.h"
#include "ui_reader.h"
#include "../common/reader/reader.h"
#include "../common/reader/reader_i18n.h"
#include "../common/reader/reader_geometry.h"
#include "storage.h"
#include "storage_file.h"
#include "ui_bookshelf_data.h"
#include "ui_settings.h"
#include "platform/epd_app.h"
#include "reader.h"
#include "document/document_view.h"
#include "src/misc/lv_text_private.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define TEXT_CAPACITY 16384u
#define WINDOW_CAPACITY 4096u
#define STREAM_CAPACITY (2 * WINDOW_CAPACITY)
#define METRIC_CACHE_SIZE 1024u
#define PAGE_CACHE_BUDGET (512u * 1024u)
#define PAGE_CACHE_RADIUS 3u
#define PAGE_CACHE_RESERVE (1024u * 1024u)
#define NEAR_GLYPH_BURST 8u
#define NEAR_GLYPH_BUDGET_MS 4u
#define CHARACTER_SET_BYTES (0x110000u / 8u)
#define MAX_PAGES UI_READER_PAGE_MAX
#define INDEX_MAGIC 0x45504932u
#define INDEX_PARTIAL_MAGIC 0x45505032u
#define INDEX_CHECKPOINT_PAGES 32u
#define INDEX_CHECKPOINT_MS 30000u

typedef struct
{
    uint32_t magic, size, modified, layout, count, checksum;
    char path[BOOKSHELF_PATH_MAX];
} index_header_t;

typedef struct
{
    char *text;
    uint32_t *mapping;
    unsigned head, tail;
    uint32_t source;
    reader_cursor_t *cursor;
} text_stream_t;

typedef struct
{
    uint32_t letter;
    uint16_t width;
    bool valid;
} metric_t;

typedef struct
{
    char *text;
    uint32_t end;
    unsigned page, bytes, lines, warmed;
    bool valid;
} prepared_page_t;

typedef struct cached_page
{
    struct cached_page *next;
    prepared_page_t page;
    /* The NUL-terminated text follows this node in the same allocation. */
} cached_page_t;

typedef struct
{
    rt_tick_t total, bookshelf, source, recent;
} position_timing_t;

static struct
{
    ui_reader_view_t view;
    reader_info_t file;
    lv_font_t *font, *fallback;
    char *text, *scratch, *index_text;
    text_stream_t index_stream, display_stream, prepare_stream;
    cached_page_t *prepared;
    unsigned cache_bytes, cache_pages;
    prepared_page_t pending, building;
    char *prepare_text;
    bool building_active, prefetch_full;
    int64_t prefetch_page;
    unsigned tracked_target, glyph_failures;
    rt_tick_t target_ticks, target_started, open_started;
    rt_tick_t target_ready_at, target_setup_ticks;
    bool target_ready_recorded;
    rt_tick_t read_ticks, line_ticks, metric_ticks;
    rt_tick_t target_read_ticks, target_line_ticks, target_metric_ticks;
    unsigned metric_calls, target_metric_calls;
    bool first_published, index_save_pending;
    unsigned index_saved_count;
    rt_tick_t index_saved_at;
    bool index_save_failed;
    bool target_hit, measure_target;
    unsigned ahead_head, ahead_tail;
    uint32_t ahead_source;
    char *ahead_text;
    uint8_t *characters, *discovered;
    reader_cursor_t *ahead_cursor;
    uint32_t pending_letter;
    unsigned pending_characters;
    unsigned unique_characters, prepared_characters;
    bool scan_complete;
    bool prepare_blocked;
    bool index_collect, discover_index, glyph_waited;
    unsigned near_glyph_steps;
    rt_tick_t near_glyph_ticks;
    int direction;
    reader_error_t error;
    lv_font_t measure_font;
    metric_t *metrics;
    uint32_t *offsets;
    unsigned index_line, index_bytes;
    uint32_t index_end;
    unsigned capacity, count, book, wanted;
    uint32_t layout, anchor, font_id, font_revision;
    /* Last attempted configuration also advances on allocation failure. */
    uint32_t attempted_font_id, attempted_font_revision;
    reader_geometry_t attempted_area;
    unsigned settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    bool opened, complete, restoring, background_paused;
} reader;

static unsigned ticks_ms(rt_tick_t ticks)
{
    return (unsigned)((uint64_t)ticks * 1000 / RT_TICK_PER_SECOND);
}

static uint32_t offsets_hash(const uint32_t *data, unsigned count)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < count; ++i) h = (h ^ data[i]) * 16777619u;
    return h;
}

static bool reserve(unsigned count)
{
    if (count > MAX_PAGES + 1 || count > SIZE_MAX / sizeof(*reader.offsets)) return false;
    if (count <= reader.capacity) return true;
    unsigned capacity = reader.capacity ? reader.capacity * 2 : 256;
    if (capacity < count) capacity = count;
    if (capacity > SIZE_MAX / sizeof(*reader.offsets)) capacity = count;
    uint32_t *data = epd_app_realloc(reader.offsets, capacity * sizeof(*data), EPD_APP_PSRAM);
    if (!data) return false;
    reader.offsets = data;
    reader.capacity = capacity;
    return true;
}

static void fail(reader_error_t error)
{
    reader.error = error;
}

/* This font is used only by the line breaker, never to draw glyphs. */
static bool metric_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                               uint32_t letter, uint32_t next)
{
    const lv_font_t *source = font->user_data;
    metric_t *entry = &reader.metrics[(letter * 2654435761u) >> 22];
    if (source->kerning == LV_FONT_KERNING_NONE && entry->valid && entry->letter == letter)
    {
        glyph->adv_w = entry->width;
        return true;
    }
    lv_font_glyph_dsc_t measured;
    rt_tick_t started = reader.measure_target ? rt_tick_get() : 0;
    bool found = lv_font_get_glyph_dsc(source, &measured, letter, next);
    if (reader.measure_target)
    {
        reader.metric_ticks += (rt_tick_t)(rt_tick_get() - started);
        ++reader.metric_calls;
    }
    glyph->adv_w = measured.adv_w;
    /* Do not retain a failed lookup or temporary placeholder as a real width. */
    if (found && !measured.is_placeholder && source->kerning == LV_FONT_KERNING_NONE)
    {
        entry->letter = letter;
        entry->width = measured.adv_w;
        entry->valid = true;
    }
    return true;
}

static bool stream_create(text_stream_t *stream)
{
    stream->text = epd_app_alloc(STREAM_CAPACITY, EPD_APP_PSRAM);
    stream->mapping = epd_app_alloc(STREAM_CAPACITY * sizeof(uint32_t), EPD_APP_PSRAM);
    return stream->text && stream->mapping;
}

static void stream_reset(text_stream_t *stream, uint32_t start)
{
    stream->head = stream->tail = 0;
    stream->source = start;
    stream->text[0] = '\0';
    stream->mapping[0] = start;
}

static void stream_destroy(text_stream_t *stream)
{
    epd_app_free(stream->text);
    epd_app_free(stream->mapping);
    reader_cursor_destroy(stream->cursor);
}

static uint32_t next_letter(const char *text, unsigned *offset);

static void discover(const char *text, unsigned bytes)
{
    if (!reader.discovered || !reader.characters) return;
    unsigned at = 0;
    while (at < bytes)
    {
        uint32_t letter = next_letter(text, &at);
        if (letter < 0x20 || letter == 0x7f || letter >= 0x110000u) continue;
        uint8_t bit = (uint8_t)(1u << (letter & 7u));
        if (reader.discovered[letter >> 3] & bit) continue;
        reader.discovered[letter >> 3] |= bit;
        ++reader.unique_characters;
        if (!(reader.characters[letter >> 3] & bit))
        {
            if (!reader.pending_characters || letter < reader.pending_letter) reader.pending_letter = letter;
            ++reader.pending_characters;
        }
    }
}

static void prepare_letter(uint32_t letter, bool adjacent)
{
    if (letter < 0x20 || letter == 0x7f || letter >= 0x110000u) return;
    if (!reader.glyph_waited) { lv_draw_wait_for_finish(); reader.glyph_waited = true; }
    if (epd_app_font_prepare(reader.font, letter, adjacent)) ++reader.prepared_characters;
    else ++reader.glyph_failures;
    uint8_t bit = (uint8_t)(1u << (letter & 7u));
    if (reader.characters && !(reader.characters[letter >> 3] & bit))
    {
        reader.characters[letter >> 3] |= bit;
        if (reader.discovered && (reader.discovered[letter >> 3] & bit)) --reader.pending_characters;
    }
}

static unsigned page_lines(void)
{
    unsigned step = reader.font->line_height + reader.view.line_space;
    return (reader.view.bottom - reader.view.top + reader.view.line_space) / step;
}

/* Retain decoded look-ahead and original byte offsets across pages. Compact
 * only when a refill needs space; display seeks use a separate stream. */
static int measure_line(text_stream_t *stream, char *output, unsigned *copied, uint32_t *end)
{
    unsigned available = stream->tail - stream->head;
    while (available < WINDOW_CAPACITY - 8 && stream->source < reader.file.file_size)
    {
        unsigned room = WINDOW_CAPACITY - available;
        if (STREAM_CAPACITY - stream->tail < room)
        {
            memmove(stream->text, stream->text + stream->head, available + 1);
            memmove(stream->mapping, stream->mapping + stream->head,
                    (available + 1) * sizeof(uint32_t));
            stream->head = 0;
            stream->tail = available;
        }
        uint32_t next = stream->source;
        rt_tick_t started = reader.measure_target ? rt_tick_get() : 0;
        int n = reader_cursor_read(stream->cursor, stream->source, stream->text + stream->tail, room,
                                   stream->mapping + stream->tail, &next);
        if (reader.measure_target) reader.read_ticks += (rt_tick_t)(rt_tick_get() - started);
        if (n < 0 || next <= stream->source) return -RT_EIO;
        if (stream == &reader.index_stream && reader.discover_index)
        {
            discover(stream->text + stream->tail, n);
            reader.ahead_source = next;
        }
        stream->source = next;
        stream->tail += n;
        available += n;
    }
    if (!available) return 0;
    lv_text_attributes_t attr;
    lv_text_attributes_init(&attr);
    attr.max_width = reader.view.content_width;
    attr.line_space = reader.view.line_space;
    const char *text = stream->text + stream->head;
    rt_tick_t started = reader.measure_target ? rt_tick_get() : 0;
    unsigned n = lv_text_get_next_line(text, available, &reader.measure_font, NULL, &attr);
    if (reader.measure_target) reader.line_ticks += (rt_tick_t)(rt_tick_get() - started);
    if (!n || n > available || *copied + n + 1 >= TEXT_CAPACITY) return -RT_ERROR;
    if (output) memcpy(output + *copied, text, n);
    *copied += n;
    /* Freeze soft breaks to retain the last line's English word look-ahead. */
    if (text[n - 1] != '\n')
    {
        if (output) output[*copied] = '\n';
        ++*copied;
    }
    stream->head += n;
    *end = stream->mapping[stream->head];
    return 1;
}

static prepared_page_t *prepared_find(unsigned page)
{
    for (cached_page_t *p = reader.prepared; p; p = p->next)
        if (p->page.page == page) return &p->page;
    return NULL;
}

static void prepared_clear(void)
{
    while (reader.prepared)
    {
        cached_page_t *p = reader.prepared;
        reader.prepared = p->next;
        epd_app_free(p);
    }
    reader.cache_bytes = reader.cache_pages = 0;
    reader.building_active = reader.prefetch_full = false;
}

/* Smaller ranks win. Keep both sides of the local window before extending in
 * the reading direction. The displayed text and pending target also have
 * independent buffers, so even an optional allocation failure cannot erase them. */
static uint64_t prepared_rank(unsigned page)
{
    unsigned center = reader.restoring && reader.count ? reader.count - 1 : reader.wanted;
    if (page == center || (reader.view.ready && page + 1 == reader.view.page)) return 0;
    unsigned distance = page > center ? page - center : center - page;
    bool along = reader.direction < 0 ? page < center : page > center;
    uint64_t rank = distance <= PAGE_CACHE_RADIUS ?
        (page < center ? distance : PAGE_CACHE_RADIUS + distance) :
        2 * PAGE_CACHE_RADIUS + (uint64_t)distance + (along ? 0 : (uint64_t)MAX_PAGES + 1);
    if (reader.view.ready)
    {
        unsigned shown = reader.view.page - 1;
        unsigned back = page > shown ? page - shown : shown - page;
        if (back <= PAGE_CACHE_RADIUS)
        {
            unsigned visible_rank = page < shown ? back : PAGE_CACHE_RADIUS + back;
            if (visible_rank < rank) rank = visible_rank;
        }
    }
    return rank;
}

static bool prepared_store(const prepared_page_t *page)
{
    if (!page->valid || !page->text || page->bytes >= TEXT_CAPACITY) return false;
    if (prepared_find(page->page)) return true;
    unsigned bytes = (unsigned)sizeof(cached_page_t) + page->bytes + 1;
    uint64_t rank = prepared_rank(page->page);
    unsigned available = PAGE_CACHE_BUDGET - reader.cache_bytes;
    /* Check admission before evicting anything: a rejected distant page must
     * not churn the cache or discard a partially warmed local page. */
    for (cached_page_t *p = reader.prepared; available < bytes && p; p = p->next)
        if (prepared_rank(p->page.page) > rank)
            available += (unsigned)sizeof(*p) + p->page.bytes + 1;
    if (available < bytes) return false;
    while (reader.cache_bytes + bytes > PAGE_CACHE_BUDGET)
    {
        cached_page_t **victim = NULL;
        uint64_t worst = rank;
        for (cached_page_t **p = &reader.prepared; *p; p = &(*p)->next)
        {
            uint64_t candidate = prepared_rank((*p)->page.page);
            if (candidate > worst) { worst = candidate; victim = p; }
        }
        if (!victim) return false;
        cached_page_t *p = *victim;
        *victim = p->next;
        reader.cache_bytes -= (unsigned)sizeof(*p) + p->page.bytes + 1;
        --reader.cache_pages;
        epd_app_free(p);
    }
    if (epd_app_psram_available() < (size_t)bytes + PAGE_CACHE_RESERVE) return false;
    cached_page_t *p = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (!p) return false;
    p->page = *page;
    p->page.text = (char *)(p + 1);
    memcpy(p->page.text, page->text, page->bytes + 1);
    p->next = reader.prepared;
    reader.prepared = p;
    reader.cache_bytes += bytes;
    ++reader.cache_pages;
    return true;
}

static void track_target(void)
{
    if (reader.tracked_target == reader.wanted + 1) return;
    reader.tracked_target = reader.wanted + 1;
    reader.near_glyph_steps = 0;
    reader.near_glyph_ticks = 0;
    reader.target_hit = prepared_find(reader.wanted) != NULL ||
        (reader.pending.valid && reader.pending.page == reader.wanted);
    reader.prefetch_page = (int64_t)reader.wanted + reader.direction * (int)(PAGE_CACHE_RADIUS + 1);
    reader.prefetch_full = false;
}

static uint32_t next_letter(const char *text, unsigned *offset)
{
    const unsigned char *p = (const unsigned char *)text + *offset;
    uint32_t letter = *p++;
    unsigned rest = letter < 0x80 ? 0 : letter < 0xe0 ? 1 : letter < 0xf0 ? 2 : 3;
    if (rest) letter &= (1u << (6 - rest)) - 1;
    *offset += rest + 1;
    while (rest--) letter = (letter << 6) | (*p++ & 0x3f);
    return letter;
}

static bool prepare_neighbour(unsigned page)
{
    if (page >= reader.count) return false;
    if (prepared_find(page)) return false;
    prepared_page_t *p = &reader.building;
    if (!reader.building_active || p->page != page)
    {
        if (!reader.prepare_text || !reader.prepare_stream.text || !reader.prepare_stream.mapping) return false;
        *p = (prepared_page_t){reader.prepare_text, reader.offsets[page], page, 0, 0, 0, false};
        reader.building_active = true;
        stream_reset(&reader.prepare_stream, p->end);
    }
    int result = measure_line(&reader.prepare_stream, p->text, &p->bytes, &p->end);
    if (result < 0) { reader.prepare_blocked = true; return true; }
    if (!result || ++p->lines >= page_lines() || p->end >= reader.file.file_size)
    {
        p->text[p->bytes] = 0;
        p->valid = p->end == reader.offsets[page + 1];
        reader.building_active = false;
        if (!p->valid) reader.prepare_blocked = true;
        else if (!prepared_store(p))
        {
            reader.prefetch_full = true;
            if (prepared_rank(page) <= 2 * PAGE_CACHE_RADIUS) reader.prepare_blocked = true;
        }
    }
    return true;
}

static bool prepare_far(void)
{
    if (reader.pending_characters)
    {
        /* Bound a sparse-set search as well as glyph work. */
        for (unsigned i = 0; i < 256 && reader.pending_letter < 0x110000u; ++i)
        {
            uint32_t letter = reader.pending_letter++;
            uint8_t bit = (uint8_t)(1u << (letter & 7u));
            if ((reader.discovered[letter >> 3] & bit) && !(reader.characters[letter >> 3] & bit))
            { prepare_letter(letter, false); break; }
        }
        return true;
    }
    if (reader.scan_complete || !reader.ahead_text) return false;
    if (reader.ahead_head == reader.ahead_tail)
    {
        if (reader.ahead_source >= reader.file.file_size)
        {
            reader.scan_complete = true;
            return false;
        }
        uint32_t next = reader.ahead_source;
        int n = reader_cursor_read(reader.ahead_cursor, next, reader.ahead_text, STREAM_CAPACITY, NULL, &next);
        if (n <= 0) { reader.prepare_blocked = true; return false; }
        reader.ahead_head = 0;
        reader.ahead_tail = n;
        reader.ahead_source = next;
    }
    uint32_t letter = next_letter(reader.ahead_text, &reader.ahead_head);
    if (letter >= 0x20 && letter != 0x7f && letter < 0x110000u)
    {
        uint8_t bit = (uint8_t)(1u << (letter & 7u));
        if (!reader.characters || !(reader.characters[letter >> 3] & bit))
        {
            ++reader.unique_characters;
            prepare_letter(letter, false);
        }
    }
    return true;
}

static bool index_header_valid(const index_header_t *header)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book(reader.book);
    return (header->magic == INDEX_MAGIC || header->magic == INDEX_PARTIAL_MAGIC) &&
        header->size == reader.file.file_size && header->modified == book->file.modified &&
        header->layout == reader.layout && memchr(header->path, 0, sizeof(header->path)) &&
        !strcmp(storage_relative_path(header->path), storage_relative_path(book->file.path)) &&
        header->count > 0 && header->count <= MAX_PAGES &&
        header->count <= (INT_MAX - sizeof(*header)) / sizeof(uint32_t) - 1 &&
        (reader.file.file_size == reader.file.data_start ? header->count == 1 :
         header->count <= reader.file.file_size - reader.file.data_start);
}

static bool index_offsets_valid(const index_header_t *header)
{
    if (offsets_hash(reader.offsets, header->count + 1) != header->checksum ||
        reader.offsets[0] != reader.file.data_start) return false;
    uint32_t end = reader.offsets[header->count];
    if (header->magic == INDEX_MAGIC ? end != reader.file.file_size :
        end <= reader.file.data_start || end >= reader.file.file_size) return false;
    for (unsigned i = 1; i <= header->count; ++i)
        if (reader.offsets[i] <= reader.offsets[i - 1] && reader.file.file_size != reader.file.data_start)
            return false;
    return true;
}

static bool index_load(void)
{
    rt_tick_t started = rt_tick_get();
    char path[64];
    index_header_t header = {0};
    ui_bookshelf_cache_path(reader.book, path, sizeof(path), "idx");
    void *snapshot = NULL;
    size_t snapshot_size = 0;
    bool ok = false, queued = false;
    unsigned bytes = 0;
    if (document_save_snapshot(path, &snapshot, &snapshot_size))
    {
        if (snapshot_size >= sizeof(header))
        {
            memcpy(&header, snapshot, sizeof(header));
            if (index_header_valid(&header) &&
                snapshot_size == sizeof(header) + ((size_t)header.count + 1) * sizeof(uint32_t) &&
                reserve(header.count + 1))
            {
                memcpy(reader.offsets, (const unsigned char *)snapshot + sizeof(header), snapshot_size - sizeof(header));
                ok = index_offsets_valid(&header);
                if (ok) { bytes = (unsigned)snapshot_size; queued = true; }
            }
        }
        epd_app_free(snapshot);
    }
    if (!ok)
    {
        storage_lock();
        int fd = storage_file_recover(path) ? open(path, O_RDONLY) : -1;
        if (fd >= 0)
        {
            int loaded = read(fd, &header, sizeof(header));
            bytes = loaded > 0 ? (unsigned)loaded : 0;
            ok = loaded == sizeof(header) && index_header_valid(&header) && reserve(header.count + 1);
            if (ok)
            {
                unsigned length = (header.count + 1) * sizeof(uint32_t);
                loaded = read(fd, reader.offsets, length);
                if (loaded > 0) bytes += (unsigned)loaded;
                ok = loaded == length && index_offsets_valid(&header);
                unsigned char extra;
                if (ok && read(fd, &extra, 1) != 0) ok = false;
            }
            close(fd);
        }
        storage_unlock();
    }
    if (ok)
    {
        reader.count = reader.index_saved_count = header.count;
        reader.complete = header.magic == INDEX_MAGIC;
        reader.index_saved_at = rt_tick_get();
        reader.index_end = reader.offsets[reader.count];
        stream_reset(&reader.index_stream, reader.index_end);
    }
    rt_kprintf("books: txt index_load %s pages=%u bytes=%u ms=%u complete=%u queued=%u\n",
               ok ? "hit" : "miss", ok ? header.count : 0, bytes,
               ticks_ms(rt_tick_get() - started), ok && reader.complete ? 1u : 0u, queued ? 1u : 0u);
    return ok;
}

static bool index_save_due(bool force)
{
    if (!reader.opened || !reader.count || !reader.view.ready || reader.error ||
        reader.index_save_failed || reader.count == reader.index_saved_count) return false;
    if (force) return true;
    if (reader.restoring || reader.view.page != reader.wanted + 1) return false;
    return reader.index_save_pending || !reader.index_saved_count ||
        (reader.count - reader.index_saved_count >= INDEX_CHECKPOINT_PAGES &&
         (rt_tick_t)(rt_tick_get() - reader.index_saved_at) >= rt_tick_from_millisecond(INDEX_CHECKPOINT_MS));
}

static void index_save(void)
{
    rt_tick_t started = rt_tick_get();
    char path[64];
    bool ok = false;
    reader.index_save_pending = false;
    const ui_bookshelf_book_t *book = ui_bookshelf_book(reader.book);
    index_header_t header = {0};
    size_t bytes = 0;
    if (reader.count <= (INT_MAX - sizeof(header)) / sizeof(uint32_t) - 1 &&
        storage_app_available("books") && storage_path_available(book->file.path))
    {
        bytes = ((size_t)reader.count + 1) * sizeof(uint32_t);
        header.magic = reader.complete ? INDEX_MAGIC : INDEX_PARTIAL_MAGIC;
        header.size = reader.file.file_size;
        header.modified = book->file.modified;
        header.layout = reader.layout;
        header.count = reader.count;
        header.checksum = offsets_hash(reader.offsets, reader.count + 1);
        memcpy(header.path, book->file.path, sizeof(header.path));
        /* index_line/index_end may already belong to an unfinished next page.
         * Persist only the complete boundaries in offsets[0..count]. */
        ui_bookshelf_cache_path(reader.book, path, sizeof(path), "idx");
        ok = document_save_async(path, &header, sizeof(header), reader.offsets, bytes);
    }
    /* Remember accepted snapshots, not disk completion. Reopening must consult
     * the save worker's pending snapshot before reading an older disk index. */
    if (ok)
    {
        reader.index_saved_count = reader.count;
        reader.index_saved_at = rt_tick_get();
    }
    else reader.index_save_failed = true;
    rt_kprintf("books: txt index_save pages=%u complete=%u queued=%u bytes=%u ms=%u\n",
               reader.count, reader.complete ? 1u : 0u, ok ? 1u : 0u,
               (unsigned)(sizeof(header) + bytes), ticks_ms(rt_tick_get() - started));
}

static position_timing_t position_save(void)
{
    position_timing_t timing = {0};
    rt_tick_t started = rt_tick_get();
    if (!reader.view.ready) return timing;
    ui_reading_position_t position = {0};
    position.offset = reader.offsets[reader.view.page - 1];
    position.current_page = reader.view.page;
    position.total_pages = reader.complete ? reader.count : 0;
    position.layout = reader.layout;
    position.progress = reader.view.percent;
    const ui_reading_position_t *saved = &ui_bookshelf_book(reader.book)->position;
    if (reader.view.saved && position.offset == saved->offset &&
        position.current_page == saved->current_page && position.total_pages == saved->total_pages &&
        position.layout == saved->layout && position.progress == saved->progress)
    { timing.total = (rt_tick_t)(rt_tick_get() - started); return timing; }
    time_t now = time(NULL);
    if (now >= 1704067200) position.last_read = (uint32_t)now;
    rt_tick_t before = rt_tick_get();
    reader.view.saved = ui_bookshelf_save(reader.book, &position);
    rt_tick_t after = rt_tick_get();
    timing.bookshelf = (rt_tick_t)(after - before);
    before = after;
    reader_set_position(position.offset);
    after = rt_tick_get();
    timing.source = (rt_tick_t)(after - before);
    before = after;
    epd_app_set_recent_reading(reader.view.title, position.progress);
    after = rt_tick_get();
    timing.recent = (rt_tick_t)(after - before);
    timing.total = (rt_tick_t)(after - started);
    return timing;
}

static void show_page(void)
{
    rt_tick_t started = rt_tick_get();
    uint32_t start = reader.offsets[reader.wanted], end = reader.pending.end;
    memcpy(reader.text, reader.scratch, reader.pending.bytes + 1);
    reader.view.page = reader.wanted + 1;
    reader.view.ready = true;
    /* Committing a distant seek releases the previous visible window's priority. */
    reader.prefetch_full = false;
    uint32_t size = reader.file.file_size - reader.file.data_start;
    reader.view.percent = !size ? 0 : end == reader.file.file_size ? 100 :
        (unsigned)((uint64_t)(start - reader.file.data_start) * 100 / size);
    position_timing_t position = position_save();
    ++reader.view.revision;
    rt_tick_t published = rt_tick_get();
    /* These intervals partition request-to-publication wall time. Dispatch also
     * includes service overhead before the target is ready; metric time remains
     * nested inside layout/prepare time, not an additional wall-time phase. */
    rt_tick_t ready_at = reader.target_ready_recorded ? reader.target_ready_at : started;
    rt_tick_t before_ready = (rt_tick_t)(ready_at - reader.target_started);
    rt_tick_t accounted = reader.target_setup_ticks + reader.target_ticks;
    rt_kprintf("books: txt publish_timing page=%u setup_ms=%u dispatch_ms=%u ready_wait_ms=%u show_ms=%u position_ms=%u bookshelf_ms=%u source_ms=%u recent_ms=%u\n",
               reader.view.page, ticks_ms(reader.target_setup_ticks),
               ticks_ms(before_ready > accounted ? before_ready - accounted : 0),
               ticks_ms(started - ready_at), ticks_ms(published - started), ticks_ms(position.total),
               ticks_ms(position.bookshelf), ticks_ms(position.source), ticks_ms(position.recent));
    rt_kprintf("books: txt target_detail page=%u read_ms=%u layout_ms=%u metric_ms=%u metric_calls=%u warm_bytes=%u/%u\n",
               reader.view.page, ticks_ms(reader.read_ticks - reader.target_read_ticks),
               ticks_ms(reader.line_ticks - reader.target_line_ticks),
               ticks_ms(reader.metric_ticks - reader.target_metric_ticks),
               reader.metric_calls - reader.target_metric_calls,
               reader.pending.warmed, reader.pending.bytes);
    if (!reader.first_published)
    {
        reader.first_published = true;
        rt_kprintf("books: txt first_publish page=%u open_ms=%u indexed=%u complete=%u\n",
                   reader.view.page, ticks_ms(published - reader.open_started),
                   reader.count, reader.complete ? 1u : 0u);
    }
    rt_kprintf("books: txt page=%u %s prepare_ms=%u wall_ms=%u glyph_fail=%u cache=%u/%u\n",
               reader.view.page, reader.target_hit ? "hit" : "miss",
               ticks_ms(reader.target_ticks), ticks_ms(published - reader.target_started),
               reader.glyph_failures, reader.cache_pages, reader.cache_bytes);
}

static bool layout_prepare(uint32_t anchor)
{
    rt_tick_t started = rt_tick_get();
    static const unsigned sizes[] = {28, 30, 32, 34, 36, 40, 44, 48};
    static const unsigned margins[] = {4, 8, 12, 16, 20};
    uint32_t font_identity;
    uint32_t font_id = epd_app_reading_font_id();
    uint32_t font_revision = epd_app_reading_font_revision();
    reader.attempted_font_id = font_id;
    reader.attempted_font_revision = font_revision;
    reader.attempted_area = reader_geometry();
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        reader.settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
    lv_font_t *fallback;
    lv_font_t *font = epd_app_font_create(font_id,
                                          sizes[ui_settings_index(UI_SETTING_FONT_SIZE)],
                                          ui_settings_index(UI_SETTING_FONT_WEIGHT), &font_identity, &fallback);
    if (!font) return false;
    unsigned encoding = ui_settings_index(UI_SETTING_ENCODING);
    if (!reserve(2) || reader_set_encoding(encoding) != RT_EOK)
    {
        epd_app_font_destroy(font);
        epd_app_font_destroy(fallback);
        return false;
    }
    prepared_clear();
    memset(&reader.pending, 0, sizeof(reader.pending));
    reader.pending.text = reader.scratch;
    reader.direction = 1;
    reader.near_glyph_steps = 0;
    reader.near_glyph_ticks = 0;
    reader.tracked_target = 0;
    reader.target_ticks = 0;
    reader.target_started = started;
    reader.target_ready_recorded = false;
    reader.target_setup_ticks = 0;
    reader.read_ticks = reader.line_ticks = reader.metric_ticks = 0;
    reader.target_read_ticks = reader.target_line_ticks = reader.target_metric_ticks = 0;
    reader.metric_calls = reader.target_metric_calls = 0;
    reader.index_save_pending = false;
    reader.index_saved_count = 0;
    reader.index_saved_at = 0;
    lv_draw_wait_for_finish();
    epd_app_font_destroy(reader.font);
    epd_app_font_destroy(reader.fallback);
    reader.font = font;
    reader.fallback = fallback;
    reader.view.font = font;
    reader.font_id = font_id;
    reader.font_revision = font_revision;
    reader.ahead_head = reader.ahead_tail = 0;
    reader.ahead_source = reader.file.data_start;
    reader.unique_characters = reader.prepared_characters = 0;
    reader.glyph_failures = 0;
    reader.pending_characters = 0;
    reader.pending_letter = 0;
    reader.scan_complete = reader.prepare_blocked = false;
    if (reader.characters) memset(reader.characters, 0, CHARACTER_SET_BYTES);
    if (reader.discovered) memset(reader.discovered, 0, CHARACTER_SET_BYTES);
    memset(reader.metrics, 0, METRIC_CACHE_SIZE * sizeof(*reader.metrics));
    memset(&reader.measure_font, 0, sizeof(reader.measure_font));
    reader.measure_font.get_glyph_dsc = metric_descriptor;
    reader.measure_font.user_data = font;
    reader.measure_font.line_height = font->line_height;
    reader.measure_font.kerning = font->kerning;
    reader.view.margin = 24 + margins[ui_settings_index(UI_SETTING_MARGIN)];
    reader_geometry_t area = reader_geometry();
    reader.view.content_width = area.width - 2 * reader.view.margin;
    reader.view.top = area.top;
    reader.view.bottom = area.bottom;
    reader.view.line_space = font->line_height * ui_settings_index(UI_SETTING_LINE_SPACING) / 5;
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        reader.settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
    /* Bump the seed when pagination or normalization rules change. */
    uint32_t layout[] = {7, font_identity, font_id, font_revision, reader.view.margin, reader.view.line_space, encoding,
                         reader.view.bottom - reader.view.top, reader.view.content_width, font->line_height};
    reader.layout = offsets_hash(layout, sizeof(layout) / sizeof(layout[0]));
    reader.anchor = anchor < reader.file.data_start ? reader.file.data_start : anchor;
    if (reader.anchor > reader.file.file_size) reader.anchor = reader.file.data_start;
    reader.restoring = true;
    reader.count = 0;
    reader.index_line = reader.index_bytes = 0;
    reader.index_end = reader.file.data_start;
    stream_reset(&reader.index_stream, reader.file.data_start);
    stream_reset(&reader.display_stream, reader.file.data_start);
    reader.complete = false;
    reader.view.ready = false;
    reader.view.error[0] = '\0';
    reader.error = READER_ERROR_NONE;
    reader.view.error_code = READER_ERROR_NONE;
    reader.view.indexing = true;
    ++reader.view.revision;
    rt_tick_t index_started = rt_tick_get();
    if (!index_load())
    {
        reader.offsets[0] = reader.file.data_start;
    }
    /* A restored prefix still needs the eventual whole-book character scan. */
    reader.discover_index = !reader.count && reader.discovered && reader.characters;
    rt_kprintf("books: txt layout setup_ms=%u before_index_ms=%u\n",
               ticks_ms(rt_tick_get() - started), ticks_ms(index_started - started));
    reader.target_setup_ticks = (rt_tick_t)(rt_tick_get() - started);
    return true;
}

void ui_reader_close(void)
{
    if (document_view_active()) document_view_close();
    if (reader.opened) ui_reader_save();
    reader_close();
    epd_app_font_destroy(reader.font);
    epd_app_font_destroy(reader.fallback);
    epd_app_free(reader.text); epd_app_free(reader.scratch);
    epd_app_free(reader.index_text);
    stream_destroy(&reader.index_stream);
    stream_destroy(&reader.display_stream);
    stream_destroy(&reader.prepare_stream);
    prepared_clear();
    epd_app_free(reader.prepare_text);
    epd_app_free(reader.ahead_text);
    epd_app_free(reader.characters);
    epd_app_free(reader.discovered);
    reader_cursor_destroy(reader.ahead_cursor);
    epd_app_free(reader.metrics); epd_app_free(reader.offsets);
    memset(&reader, 0, sizeof(reader));
}

static unsigned opened_book_index = UINT_MAX;
const char *ui_reader_identity(void)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book(opened_book_index);
    return book ? book->file.path : "";
}

bool ui_reader_open(unsigned book_index)
{
    rt_tick_t started = rt_tick_get();
    ui_reader_close();
    rt_tick_t closed = rt_tick_get();
    opened_book_index = book_index;
    const ui_bookshelf_book_t *book = ui_bookshelf_book(book_index);
    if (book && book->file.format != DOCUMENT_TXT) return document_view_open(book_index);
    if (!book || !storage_path_available(book->file.path) || reader_open(book->file.path) != RT_EOK) return false;
    rt_tick_t source_opened = rt_tick_get();
    reader.book = book_index;
    reader.opened = true;
    if (!epd_app_reading_font_begin()) { ui_reader_close(); return false; }
    reader_get_info(&reader.file);
    reader.view.title = book->file.name;
    reader.text = epd_app_alloc(TEXT_CAPACITY, EPD_APP_PSRAM);
    reader.scratch = epd_app_alloc(TEXT_CAPACITY, EPD_APP_PSRAM);
    reader.metrics = epd_app_alloc(METRIC_CACHE_SIZE * sizeof(*reader.metrics), EPD_APP_PSRAM);
    if (!reader.text || !reader.scratch || !reader.metrics ||
        !stream_create(&reader.index_stream) || !stream_create(&reader.display_stream))
    { ui_reader_close(); return false; }
    reader.index_stream.cursor = reader_cursor_create();
    reader.display_stream.cursor = reader_cursor_create();
    reader.index_text = epd_app_alloc(TEXT_CAPACITY, EPD_APP_PSRAM);
    reader.ahead_text = epd_app_alloc(STREAM_CAPACITY, EPD_APP_PSRAM);
    reader.characters = epd_app_alloc(CHARACTER_SET_BYTES, EPD_APP_PSRAM);
    reader.discovered = epd_app_alloc(CHARACTER_SET_BYTES, EPD_APP_PSRAM);
    reader.ahead_cursor = reader_cursor_create();
    reader.prepare_text = epd_app_alloc(TEXT_CAPACITY, EPD_APP_PSRAM);
    stream_create(&reader.prepare_stream);
    if (reader.prepare_stream.text && reader.prepare_stream.mapping)
        reader.prepare_stream.cursor = reader_cursor_create();
    reader.text[0] = '\0';
    reader.view.text = reader.text;
    rt_tick_t layout_started = rt_tick_get();
    if (!layout_prepare(book->position.offset)) { ui_reader_close(); return false; }
    reader.open_started = reader.target_started = started;
    rt_tick_t ready = rt_tick_get();
    rt_kprintf("books: txt open setup_ms=%u close_ms=%u source_ms=%u resources_ms=%u layout_ms=%u\n",
               ticks_ms(ready - started), ticks_ms(closed - started), ticks_ms(source_opened - closed),
               ticks_ms(layout_started - source_opened), ticks_ms(ready - layout_started));
    reader.target_setup_ticks = (rt_tick_t)(rt_tick_get() - started);
    /* Computation and activation both begin at the shared UI service step. */
    return true;
}

bool ui_reader_reflow(void)
{
    if (document_view_active()) return document_view_reflow();
    if (!reader.opened) return false;
    uint32_t anchor = reader.view.ready ? reader.offsets[reader.view.page - 1] : reader.anchor;
    position_save();
    return layout_prepare(anchor);
}

static void resolve_target(void)
{
    if (reader.restoring && reader.count &&
        (reader.complete || reader.anchor < reader.offsets[reader.count]))
    {
        unsigned lo = 0, hi = reader.count;
        while (lo + 1 < hi)
        {
            unsigned mid = lo + (hi - lo) / 2;
            if (reader.offsets[mid] <= reader.anchor) lo = mid;
            else hi = mid;
        }
        reader.wanted = lo;
        reader.restoring = false;
    }
    if (reader.complete && reader.count && reader.wanted >= reader.count) reader.wanted = reader.count - 1;
}

static void index_step(void)
{
    uint32_t start = reader.offsets[reader.count];
    if (!reader.index_line) reader.index_collect = reader.index_text != NULL;
    int n = measure_line(&reader.index_stream, reader.index_collect ? reader.index_text : NULL,
                         &reader.index_bytes, &reader.index_end);
    if (n < 0) { fail(READER_ERROR_TEXT_READ); return; }
    ++reader.index_line;
    if (!n || reader.index_line >= page_lines() || reader.index_end >= reader.file.file_size)
    {
        uint32_t end = reader.index_end;
        if (end <= start && start < reader.file.file_size)
        { fail(READER_ERROR_TEXT_READ); return; }
        if (!reserve(reader.count + 2)) { fail(READER_ERROR_PAGE_MEMORY); return; }
        unsigned page = reader.count;
        reader.offsets[++reader.count] = end;
        reader.complete = end >= reader.file.file_size;
        resolve_target();
        if (!reader.restoring) track_target();
        if (reader.index_collect)
        {
            reader.index_text[reader.index_bytes] = 0;
            if (!reader.restoring && page == reader.wanted)
            {
                reader.pending = (prepared_page_t){reader.scratch, end, page, reader.index_bytes,
                                                  reader.index_line, 0, true};
                memcpy(reader.scratch, reader.index_text, reader.index_bytes + 1);
            }
            prepared_page_t page_data = {reader.index_text, end, page, reader.index_bytes,
                                         reader.index_line, 0, true};
            if (!prepared_store(&page_data) && !reader.restoring && reader.prefetch_page == page)
                reader.prefetch_full = true;
        }
        reader.index_line = reader.index_bytes = 0;
        if (reader.complete) reader.index_save_pending = true;
    }
}

static bool target_ready(void)
{
    return !reader.restoring && reader.wanted < reader.count &&
           reader.pending.valid && reader.pending.page == reader.wanted;
}

static void target_step(void)
{
    prepared_page_t *p = &reader.pending;
    prepared_page_t *cached = prepared_find(reader.wanted);
    if (cached)
    {
        *p = *cached;
        p->text = reader.scratch;
        memcpy(p->text, cached->text, cached->bytes + 1);
        return;
    }
    if (p->valid || !p->lines || p->page != reader.wanted)
    {
        *p = (prepared_page_t){reader.scratch, reader.offsets[reader.wanted], reader.wanted, 0, 0, 0, false};
        stream_reset(&reader.display_stream, p->end);
    }
    int result = measure_line(&reader.display_stream, p->text, &p->bytes, &p->end);
    if (result < 0) { fail(READER_ERROR_TEXT_READ); return; }
    if (!result || ++p->lines >= page_lines() || p->end >= reader.file.file_size)
    {
        p->text[p->bytes] = 0;
        p->valid = p->end == reader.offsets[reader.wanted + 1];
        if (!p->valid) fail(READER_ERROR_TEXT_READ);
        else prepared_store(p);
    }
}

static bool neighbour_work(unsigned page, bool advance)
{
    if (reader.prepare_blocked || !reader.prepare_text || !reader.prepare_stream.text ||
        !reader.prepare_stream.mapping) return false;
    if (page >= reader.count)
    {
        if (reader.complete) return false;
        if (advance) index_step();
        return true;
    }
    if (prepared_find(page)) return false;
    if (advance) prepare_neighbour(page);
    return true;
}

static bool warm_cached(bool advance, bool local)
{
    bool unpublished = !reader.view.ready || reader.view.page != reader.wanted + 1;
    prepared_page_t *next = unpublished && reader.pending.warmed < reader.pending.bytes ? &reader.pending : NULL;
    uint64_t rank = next ? 0 : UINT64_MAX;
    for (cached_page_t *p = reader.prepared; p; p = p->next)
    {
        unsigned page = p->page.page;
        if (reader.view.ready && page + 1 == reader.view.page) continue;
        unsigned distance = page > reader.wanted ? page - reader.wanted : reader.wanted - page;
        if (local && distance > PAGE_CACHE_RADIUS) continue;
        bool along = reader.direction < 0 ? page < reader.wanted : page > reader.wanted;
        uint64_t candidate = !distance ? 0 : (uint64_t)distance * 2 - (along ? 1u : 0u);
        if (p->page.warmed < p->page.bytes && candidate < rank)
        { next = &p->page; rank = candidate; }
    }
    if (!next) return false;
    if (advance)
    {
        uint32_t letter = next_letter(next->text, &next->warmed);
        /* A failed speculative glyph must not stall text preparation. Drawing
         * still retries through the font callback; retain progress on turns. */
        prepare_letter(letter, rank <= 2 * PAGE_CACHE_RADIUS);
        if (next == &reader.pending)
        {
            prepared_page_t *cached = prepared_find(next->page);
            if (cached) cached->warmed = next->warmed;
        }
    }
    return true;
}

static bool compute_step(bool advance, bool glyphs)
{
    resolve_target();
    if (reader.restoring || reader.wanted >= reader.count)
    { if (advance) index_step(); return true; }
    if (advance) track_target();
    if (!target_ready()) { if (advance) target_step(); return true; }
    if (reader.background_paused) return false;
    /* Checkpoints, including EOF, run only after publishing the target. */
    if (index_save_due(false))
    {
        if (advance) index_save();
        return true;
    }
    /* Nearby pages need many glyphs for each line of text. Give them a bounded
     * burst across service calls, then require a text step. Charge elapsed glyph
     * work (including draw waits), not the gaps between service calls. The outer
     * service budget still applies after each character; one slow glyph cannot
     * be interrupted. A ready foreground target bypasses this work entirely. */
    rt_tick_t glyph_budget = rt_tick_from_millisecond(NEAR_GLYPH_BUDGET_MS);
    if (!glyph_budget) glyph_budget = 1;
    if (glyphs && reader.near_glyph_steps < NEAR_GLYPH_BURST && reader.near_glyph_ticks < glyph_budget)
    {
        rt_tick_t started = advance ? rt_tick_get() : 0;
        if (warm_cached(advance, true))
        {
            if (advance)
            {
                ++reader.near_glyph_steps;
                reader.near_glyph_ticks += (rt_tick_t)(rt_tick_get() - started);
            }
            return true;
        }
    }
    for (unsigned distance = 1; distance <= PAGE_CACHE_RADIUS; ++distance)
    {
        int64_t first = (int64_t)reader.wanted + reader.direction * (int)distance;
        int64_t second = (int64_t)reader.wanted - reader.direction * (int)distance;
        if ((first >= 0 && first < MAX_PAGES && neighbour_work((unsigned)first, advance)) ||
            (second >= 0 && second < MAX_PAGES && neighbour_work((unsigned)second, advance)))
        {
            if (advance) { reader.near_glyph_steps = 0; reader.near_glyph_ticks = 0; }
            return true;
        }
    }
    if (!reader.prepare_blocked && reader.prepare_text && reader.prepare_stream.text &&
        reader.prepare_stream.mapping && !reader.prefetch_full && reader.prefetch_page >= 0 &&
        reader.prefetch_page < MAX_PAGES && (!reader.complete || reader.prefetch_page < reader.count))
    {
        unsigned page = (unsigned)reader.prefetch_page;
        if (neighbour_work(page, advance))
        {
            if (advance) { reader.near_glyph_steps = 0; reader.near_glyph_ticks = 0; }
            return true;
        }
        if (advance) reader.prefetch_page += reader.direction;
        return true;
    }
    if (!reader.complete)
    {
        if (advance) { index_step(); reader.near_glyph_steps = 0; reader.near_glyph_ticks = 0; }
        return true;
    }
    if (!glyphs) return false;
    if (warm_cached(advance, false)) return true;
    if (reader.prepare_blocked) return false;
    bool work = reader.pending_characters || (reader.ahead_text && !reader.scan_complete &&
        (reader.ahead_head < reader.ahead_tail || reader.ahead_source < reader.file.file_size));
    return work && (!advance || prepare_far());
}

void ui_reader_background_pause(bool paused)
{
    reader.background_paused = paused;
    epd_reader_background_pause(paused);
}

ui_reader_work_t ui_reader_step(uint32_t budget_ms, bool foreground, bool commit)
{
    if (document_view_active()) return document_view_step(budget_ms, foreground, commit);
    if (!reader.opened) return UI_READER_WORK_IDLE;
    /* The owner must try a new configuration before measuring. A failed
     * attempt keeps the previous font and layout runnable until another change. */
    reader_info_t current;
    if (!reader_get_info(&current) || strcmp(current.path, reader.file.path) ||
        !storage_path_available(reader.file.path)) fail(READER_ERROR_BOOK_CLOSED);
    if (!reader.error && ui_reader_settings_changed()) return UI_READER_WORK_IDLE;
    reader.glyph_waited = false;
    rt_tick_t started = rt_tick_get();
    if (target_ready() && !reader.target_ready_recorded)
    { reader.target_ready_at = started; reader.target_ready_recorded = true; }
    rt_tick_t budget = rt_tick_from_millisecond(budget_ms > INT_MAX ? INT_MAX : (int)budget_ms);
    if (budget_ms && !budget) budget = 1;
    bool present = foreground && commit && ui_reader_waiting();
    while (budget_ms && !reader.error)
    {
        if (present && !reader.restoring && target_ready()) break;
        bool preparing = !target_ready();
        rt_tick_t before = rt_tick_get();
        reader.measure_target = preparing;
        bool work = compute_step(true, true);
        reader.measure_target = false;
        rt_tick_t now = rt_tick_get();
        if (preparing) reader.target_ticks += (rt_tick_t)(now - before);
        if (!reader.target_ready_recorded && target_ready())
        { reader.target_ready_at = now; reader.target_ready_recorded = true; }
        if (!work || (rt_tick_t)(now - started) >= budget) break;
    }
    if (commit)
    {
        if (reader.error)
        {
            if (reader.view.error_code != reader.error)
            {
                reader.view.error_code = reader.error;
                snprintf(reader.view.error, sizeof(reader.view.error), "%s", reader_error_text(reader.error));
                reader.view.indexing = false;
                ++reader.view.revision;
            }
        }
        else
        {
            unsigned pages = reader.complete ? reader.count : 0;
            reader.view.pages = pages;
            reader.view.indexing = !reader.complete;
            if (foreground && target_ready() && (!reader.view.ready || reader.view.page != reader.wanted + 1)) show_page();
            else if (foreground) position_save();
        }
    }
    if (reader.error || !compute_step(false, true)) return UI_READER_WORK_IDLE;
    return foreground && ui_reader_waiting() && (reader.restoring || !target_ready()) ?
        UI_READER_WORK_TARGET : UI_READER_WORK_BACKGROUND;
}

void ui_reader_cancel_pending(void)
{
    if (document_view_active()) { document_view_cancel_pending(); return; }
    if (reader.view.ready)
    {
        if (reader.wanted != reader.view.page - 1)
        {
            reader.target_ticks = 0; reader.target_started = rt_tick_get();
            reader.target_ready_recorded = false; reader.target_setup_ticks = 0;
            reader.target_read_ticks = reader.read_ticks;
            reader.target_line_ticks = reader.line_ticks;
            reader.target_metric_ticks = reader.metric_ticks;
            reader.target_metric_calls = reader.metric_calls;
        }
        reader.wanted = reader.view.page - 1;
        track_target();
    }
}

void ui_reader_seek(unsigned page)
{
    if (document_view_active()) { document_view_seek(page); return; }
    if (!reader.view.ready || reader.view.error[0] || !page) return;
    if (reader.complete && page > reader.count) page = reader.count;
    if (page > MAX_PAGES) page = MAX_PAGES;
    if (reader.wanted != page - 1)
    {
        reader.direction = page - 1 > reader.wanted ? 1 : -1;
        reader.prepare_blocked = false;
        reader.target_ticks = 0;
        reader.target_started = rt_tick_get();
        reader.target_ready_recorded = false;
        reader.target_setup_ticks = 0;
        reader.target_read_ticks = reader.read_ticks;
        reader.target_line_ticks = reader.line_ticks;
        reader.target_metric_ticks = reader.metric_ticks;
        reader.target_metric_calls = reader.metric_calls;
    }
    reader.wanted = page - 1;
    track_target();
}

void ui_reader_turn(int direction)
{
    if (document_view_active())
    {
        int next = (int)document_view_get()->page + direction;
        document_view_seek(next > 0 ? (unsigned)next : 1);
        return;
    }
    if (!reader.view.ready) return;
    int next = (int)reader.view.page + direction;
    if (next < 1) next = 1;
    ui_reader_seek((unsigned)next);
}

const ui_reader_view_t *ui_reader_view(void)
{
    if (document_view_active()) return document_view_get();
    if (reader.view.error_code)
        snprintf(reader.view.error, sizeof(reader.view.error), "%s", reader_error_text(reader.view.error_code));
    return &reader.view;
}
bool ui_reader_save(void)
{
    if (document_view_active()) return document_view_save();
    position_save();
    if (index_save_due(true)) index_save();
    bool flushed = ui_bookshelf_flush();
    if (!flushed || (reader.view.ready && !reader.view.saved))
    {
        reader.view.saved = false;
        rt_kprintf("books: pending reading position could not be flushed\n");
        return false;
    }
    return true;
}
bool ui_reader_waiting(void)
{
    if (document_view_active()) return document_view_waiting();
    return !reader.view.error[0] && (!reader.view.ready || reader.restoring ||
                                    reader.wanted + 1 != reader.view.page);
}

bool ui_reader_settings_changed(void)
{
    if (document_view_active()) return document_view_settings_changed();
    if (reader.attempted_font_id != epd_app_reading_font_id() ||
        reader.attempted_font_revision != epd_app_reading_font_revision()) return true;
    reader_geometry_t area = reader_geometry();
    if (reader.attempted_area.width != area.width || reader.attempted_area.top != area.top ||
        reader.attempted_area.bottom != area.bottom) return true;
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        if (reader.settings[i - UI_SETTING_FONT_SIZE] != ui_settings_index(i)) return true;
    return false;
}
