#include "ui_reader_font_cache.h"
#include "epd_memory.h"
#include "src/misc/cache/lv_cache.h"
#include "src/misc/cache/class/lv_cache_lru_rb.h"
#include "src/draw/lv_draw_buf_private.h"
#include <rthw.h>
#include <string.h>

#define GLYPH_BUDGET (8u * 1024u * 1024u)
#define GLYPH_NODE_LIMIT 8192u
#define SHARED_RESERVE (1024u * 1024u)
#define DESCRIPTOR_BUDGET (1024u * 1024u)
#define UNICODE_PAGES (0x110000u / 256u)

typedef struct
{
    uint32_t valid[8];
    /* Sparse CJK pages across multiple sizes should not consume 256 complete
     * descriptors for the first letter. Allocate only the touched 16-slot blocks. */
    lv_font_glyph_dsc_t *glyphs[16];
} descriptor_page_t;

typedef struct font_binding
{
    struct font_binding *next;
    lv_font_t *font;
    bool (*descriptor)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void *(*bitmap)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
    void (*release)(const lv_font_t *, lv_font_glyph_dsc_t *);
    descriptor_page_t **descriptors;
} font_binding_t;

typedef struct glyph_item
{
    lv_cache_slot_size_t slot;
    const lv_font_t *font;
    uint32_t glyph, style;
    bool derived;
    bool speculative;
    lv_draw_buf_t *buffer;
    /* Links only existing LRU nodes, including invalidated held references. */
    struct glyph_item *previous, *next;
    struct glyph_item *victim_previous, *victim_next;
    bool queued;
} glyph_item_t;

static lv_cache_t *glyph_cache;
static font_binding_t *bindings;
static struct rt_mutex descriptor_lock;
static bool lock_initialized, preparing, prepare_adjacent;
static unsigned glyph_count;
static glyph_item_t *glyph_items;
/* Oldest first, speculative before nearby. Held entries are removed lazily
 * once, then rejoin only when their last reference is released. */
static glyph_item_t *victim_first[2], *victim_last[2];
static size_t glyph_bytes;
static size_t descriptor_bytes;
static epd_app_font_cache_stats_t cache_stats;

/* UI and EPIC draw threads share this HCPU. Keep statistics independent of the
 * descriptor mutex so a snapshot never waits behind a source bitmap callback.
 * Short interrupt-protected updates also make the 64-bit counters coherent. */
void ui_reader_font_cache_stats(epd_app_font_cache_stats_t *out)
{
    if (!out) return;
    rt_base_t level = rt_hw_interrupt_disable();
    *out = cache_stats;
    rt_hw_interrupt_enable(level);
    out->ticks_per_second = RT_TICK_PER_SECOND;
}

static void stats_lookup(bool hit)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (hit) ++cache_stats.glyph_hits;
    else ++cache_stats.glyph_misses;
    rt_hw_interrupt_enable(level);
}

static bool stats_prepare_failed(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    ++cache_stats.prepare_failures;
    rt_hw_interrupt_enable(level);
    return false;
}

static void stats_source_bitmap(rt_tick_t elapsed)
{
    rt_base_t level = rt_hw_interrupt_disable();
    ++cache_stats.source_bitmap_calls;
    cache_stats.source_bitmap_ticks += elapsed;
    rt_hw_interrupt_enable(level);
}

static font_binding_t *binding_find(const lv_font_t *font)
{
    for (font_binding_t *p = bindings; p; p = p->next)
        if (p->font == font) return p;
    return NULL;
}

static void *descriptor_alloc(size_t bytes)
{
    if (bytes > DESCRIPTOR_BUDGET - descriptor_bytes ||
        epd_app_psram_available() < bytes + SHARED_RESERVE) return NULL;
    void *memory = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (memory)
    {
        memset(memory, 0, bytes);
        descriptor_bytes += bytes;
    }
    return memory;
}

static bool cached_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                              uint32_t letter, uint32_t next)
{
    font_binding_t *binding = binding_find(font);
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    /* Reader fonts disable kerning. A caller that enables it must still get
     * the original pair-dependent advance, never a single-letter cached one. */
    if (font->kerning != LV_FONT_KERNING_NONE || letter >= 0x110000u)
    {
        bool found = binding->descriptor(font, glyph, letter, next);
        rt_mutex_release(&descriptor_lock);
        return found;
    }
    descriptor_page_t *page = binding->descriptors[letter >> 8];
    unsigned slot = letter & 255u;
    uint32_t bit = 1u << (slot & 31u);
    if (page && (page->valid[slot >> 5] & bit))
    {
        *glyph = page->glyphs[slot >> 4][slot & 15u];
        rt_mutex_release(&descriptor_lock);
        return true;
    }
    bool found = binding->descriptor(font, glyph, letter, next);
    /* Never turn a temporary Tiny TTF allocation failure into a cached miss. */
    if (found)
    {
        if (!page)
        {
            page = descriptor_alloc(sizeof(*page));
            if (page) binding->descriptors[letter >> 8] = page;
        }
        if (page)
        {
            if (!page->glyphs[slot >> 4])
                page->glyphs[slot >> 4] = descriptor_alloc(16u * sizeof(*glyph));
            if (page->glyphs[slot >> 4])
            {
                lv_font_glyph_dsc_t *saved = &page->glyphs[slot >> 4][slot & 15u];
                *saved = *glyph;
                saved->entry = NULL;
                page->valid[slot >> 5] |= bit;
            }
        }
    }
    rt_mutex_release(&descriptor_lock);
    return found;
}

static lv_cache_compare_res_t glyph_compare(const glyph_item_t *a, const glyph_item_t *b)
{
    if (a->font != b->font) return (uintptr_t)a->font > (uintptr_t)b->font ? 1 : -1;
    if (a->glyph != b->glyph) return a->glyph > b->glyph ? 1 : -1;
    if (a->derived != b->derived) return a->derived ? 1 : -1;
    if (a->style != b->style) return a->style > b->style ? 1 : -1;
    return 0;
}

static void victim_remove(glyph_item_t *item)
{
    if (!item->queued) return;
    unsigned tier = item->speculative ? 0 : 1;
    if (item->victim_previous) item->victim_previous->victim_next = item->victim_next;
    else victim_first[tier] = item->victim_next;
    if (item->victim_next) item->victim_next->victim_previous = item->victim_previous;
    else victim_last[tier] = item->victim_previous;
    item->victim_previous = item->victim_next = NULL;
    item->queued = false;
}

static void victim_append(glyph_item_t *item)
{
    if (item->queued) return;
    unsigned tier = item->speculative ? 0 : 1;
    item->victim_previous = victim_last[tier];
    item->victim_next = NULL;
    if (victim_last[tier]) victim_last[tier]->victim_next = item;
    else victim_first[tier] = item;
    victim_last[tier] = item;
    item->queued = true;
}

static void glyph_release(lv_cache_entry_t *entry)
{
    /* An invalid entry can be freed by release; do not touch it afterwards. */
    if (!lv_cache_entry_is_invalid(entry) && lv_cache_entry_get_ref(entry) == 1)
        victim_append(lv_cache_entry_get_data(entry));
    lv_cache_release(glyph_cache, entry, NULL);
}

static bool glyph_create(glyph_item_t *item, void *source)
{
    item->buffer = lv_draw_buf_dup_ex(lv_draw_buf_get_font_handlers(), source);
    if (!item->buffer) return false;
    lv_draw_buf_flush_cache(item->buffer, NULL);
    item->speculative = preparing && !prepare_adjacent;
    item->previous = NULL;
    item->next = glyph_items;
    if (glyph_items) glyph_items->previous = item;
    glyph_items = item;
    victim_append(item);
    glyph_bytes += item->slot.size;
    ++glyph_count;
    return true;
}

static void glyph_free(glyph_item_t *item, void *unused)
{
    LV_UNUSED(unused);
    if (item->buffer)
    {
        victim_remove(item);
        if (item->previous) item->previous->next = item->next;
        else glyph_items = item->next;
        if (item->next) item->next->previous = item->previous;
        glyph_bytes -= item->slot.size;
        lv_draw_buf_destroy(item->buffer);
        --glyph_count;
    }
}

static size_t glyph_charge(uint32_t bytes)
{
    /* Pixel bytes plus a conservative allowance for buffer/node/heap headers;
     * no 1 KiB minimum for small glyphs. Node count is bounded independently. */
    return bytes + 256u;
}

static void glyph_touch(lv_cache_entry_t *entry, bool nearby)
{
    /* Whole-book scans must not refresh the recency of already useful text. */
    if (!entry || !nearby) return;
    glyph_item_t *item = lv_cache_entry_get_data(entry);
    victim_remove(item);
    item->speculative = false;
    victim_append(item);
    if (item == glyph_items) return;
    if (item->previous) item->previous->next = item->next;
    if (item->next) item->next->previous = item->previous;
    item->previous = NULL;
    item->next = glyph_items;
    if (glyph_items) glyph_items->previous = item;
    glyph_items = item;
}

static bool glyph_evict(void)
{
    for (unsigned tier = 0; tier < 2; ++tier)
    {
        while (victim_first[tier])
        {
            glyph_item_t *item = victim_first[tier];
            victim_remove(item);
            lv_cache_entry_t *entry = lv_cache_entry_get_entry(item, sizeof(*item));
            if (lv_cache_entry_is_invalid(entry) || lv_cache_entry_get_ref(entry)) continue;
            lv_cache_drop(glyph_cache, item, NULL);
            return true;
        }
    }
    return false;
}

static bool glyph_room(uint32_t bytes, bool evict, unsigned copies)
{
    if (bytes > GLYPH_BUDGET - 256u) return false;
    size_t charge = glyph_charge(bytes);
    /* Before rasterization allow for both source and retained pixels. Once the
     * source exists, available memory already excludes it; only copy space is needed. */
    /* Keep descriptor growth possible after the bitmap budget fills. */
    size_t reserve = SHARED_RESERVE + DESCRIPTOR_BUDGET - descriptor_bytes;
    while (glyph_count >= GLYPH_NODE_LIMIT ||
           epd_app_psram_available() < copies * bytes + reserve ||
           GLYPH_BUDGET - glyph_bytes < charge)
    {
        if (!evict || !glyph_evict()) return false;
    }
    return true;
}

/* descriptor_lock held. Drop preserves held pixels; they remain charged until
 * their last reference is released, even after leaving the LRU search tree. */
static void glyph_drop(const lv_font_t *font, bool derived_only)
{
    glyph_item_t *next;
    for (glyph_item_t *item = glyph_items; item; item = next)
    {
        next = item->next;
        lv_cache_entry_t *entry = lv_cache_entry_get_entry(item, sizeof(*item));
        if ((!font || (item->font == font && (!derived_only || item->derived))) &&
            !lv_cache_entry_is_invalid(entry))
        {
            victim_remove(item);
            lv_cache_drop(glyph_cache, item, NULL);
        }
    }
}

const lv_draw_buf_t *ui_reader_font_derived_acquire(const lv_font_t *font, uint32_t glyph,
                                                  uint32_t style, lv_cache_entry_t **entry)
{
    if (!entry) return NULL;
    *entry = NULL;
    if (!font) return NULL;
    if (!lock_initialized) { stats_lookup(false); return NULL; }
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    glyph_item_t key = {.font = font, .glyph = glyph, .style = style, .derived = true};
    if (glyph_cache) *entry = lv_cache_acquire(glyph_cache, &key, NULL);
    glyph_touch(*entry, !preparing || prepare_adjacent);
    const lv_draw_buf_t *buffer = *entry ? ((glyph_item_t *)lv_cache_entry_get_data(*entry))->buffer : NULL;
    if (!preparing) stats_lookup(buffer != NULL);
    rt_mutex_release(&descriptor_lock);
    return buffer;
}

const lv_draw_buf_t *ui_reader_font_derived_store(const lv_font_t *font, uint32_t glyph,
                                                uint32_t style, const lv_draw_buf_t *bitmap,
                                                lv_cache_entry_t **entry)
{
    if (!entry) return NULL;
    *entry = NULL;
    if (!font || !bitmap || !bitmap->data || !lock_initialized ||
        bitmap->header.cf != LV_COLOR_FORMAT_A8 || !bitmap->header.w || !bitmap->header.h ||
        bitmap->header.stride < bitmap->header.w) return NULL;
    uint64_t bytes = (uint64_t)bitmap->header.stride * bitmap->header.h;
    if (bytes > bitmap->data_size || bytes > GLYPH_BUDGET - LV_DRAW_BUF_ALIGN - 256u) return NULL;
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    glyph_item_t key = {.font = font, .glyph = glyph, .style = style, .derived = true,
                        .slot = {.size = glyph_charge((uint32_t)bytes + LV_DRAW_BUF_ALIGN)}};
    if (glyph_cache && bindings)
    {
        *entry = lv_cache_acquire(glyph_cache, &key, NULL);
        if (!*entry && glyph_room((uint32_t)bytes + LV_DRAW_BUF_ALIGN,
                                  !preparing || prepare_adjacent, 1))
            *entry = lv_cache_acquire_or_create(glyph_cache, &key, (void *)bitmap);
    }
    glyph_touch(*entry, !preparing || prepare_adjacent);
    const lv_draw_buf_t *buffer = *entry ? ((glyph_item_t *)lv_cache_entry_get_data(*entry))->buffer : NULL;
    rt_mutex_release(&descriptor_lock);
    return buffer;
}

void ui_reader_font_derived_release(lv_cache_entry_t *entry)
{
    if (!entry) return;
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    glyph_release(entry);
    if (!bindings && !glyph_count)
    {
        lv_cache_destroy(glyph_cache, NULL);
        glyph_cache = NULL;
    }
    rt_mutex_release(&descriptor_lock);
}

void ui_reader_font_derived_drop(const lv_font_t *font)
{
    if (!font || !lock_initialized) return;
    lv_draw_wait_for_finish();
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    glyph_drop(font, true);
    rt_mutex_release(&descriptor_lock);
}

static const void *cached_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    /* The EPIC draw thread and UI layout may access the same file-backed
     * Tiny TTF font. Serialize the original callbacks as well as our metadata. */
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    font_binding_t *binding = binding_find(glyph->resolved_font);
    glyph_item_t key = {.font = glyph->resolved_font, .glyph = glyph->gid.index};
    lv_cache_entry_t *entry = lv_cache_acquire(glyph_cache, &key, NULL);
    if (!preparing) stats_lookup(entry != NULL);
    if (entry)
    {
        glyph_touch(entry, !preparing || prepare_adjacent);
        glyph->entry = entry;
        const void *buffer = ((glyph_item_t *)lv_cache_entry_get_data(entry))->buffer;
        rt_mutex_release(&descriptor_lock);
        return buffer;
    }
    uint32_t stride = lv_draw_buf_width_to_stride(glyph->box_w, LV_COLOR_FORMAT_A8);
    bool keep = glyph_room(stride * glyph->box_h + LV_DRAW_BUF_ALIGN,
                           !preparing || prepare_adjacent, 2);
    if (!keep && preparing)
    {
        rt_mutex_release(&descriptor_lock);
        return NULL;
    }
    /* The original callback applies weight and writes back the rasterized data. */
    rt_tick_t started = preparing ? 0 : rt_tick_get();
    const lv_draw_buf_t *source = binding->bitmap(glyph, scratch);
    if (!preparing) stats_source_bitmap((rt_tick_t)(rt_tick_get() - started));
    /* The source may use a different stride/size than the descriptor estimate.
     * Recheck before LVGL insertion, whose implicit LRU eviction has no tiers. */
    if (!source || !keep || !glyph_room(source->data_size, !preparing || prepare_adjacent, 1))
    {
        rt_mutex_release(&descriptor_lock);
        return source;
    }
    key.slot.size = glyph_charge(source->data_size);
    entry = lv_cache_acquire_or_create(glyph_cache, &key, (void *)source);
    if (!entry)
    {
        rt_mutex_release(&descriptor_lock);
        return source;
    }
    binding->release(binding->font, glyph);
    glyph->entry = entry;
    const void *buffer = ((glyph_item_t *)lv_cache_entry_get_data(entry))->buffer;
    rt_mutex_release(&descriptor_lock);
    return buffer;
}

static void cached_release(const lv_font_t *font, lv_font_glyph_dsc_t *glyph)
{
    if (!glyph->entry) return;
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    if (lv_cache_entry_get_cache(glyph->entry) == glyph_cache)
    {
        glyph_release(glyph->entry);
        glyph->entry = NULL;
    }
    else binding_find(font)->release(font, glyph);
    rt_mutex_release(&descriptor_lock);
}

void ui_reader_font_cache_reserve(size_t bytes)
{
    if (!glyph_cache) return;
    /* UI-thread allocation boundary (new TTF or application). References held
     * by EPIC must finish before reclaiming speculative glyph bitmaps. */
    lv_draw_wait_for_finish();
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    size_t target = bytes > SIZE_MAX - SHARED_RESERVE ? SIZE_MAX : bytes + SHARED_RESERVE;
    while (epd_app_psram_available() < target)
        if (!glyph_evict()) break;
    rt_mutex_release(&descriptor_lock);
}

bool ui_reader_font_cache_attach(lv_font_t *font)
{
    if (!font || !font->get_glyph_dsc || !font->get_glyph_bitmap || !font->release_glyph) return false;
    if (binding_find(font)) return true;
    font_binding_t *binding = lv_malloc(sizeof(*binding));
    if (!binding) return false;
    size_t bytes = UNICODE_PAGES * sizeof(*binding->descriptors);
    ui_reader_font_cache_reserve(bytes);
    binding->descriptors = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (!binding->descriptors) { lv_free(binding); return false; }
    memset(binding->descriptors, 0, bytes);
    if (!glyph_cache)
    {
        glyph_cache = lv_cache_create(&lv_cache_class_lru_rb_size, sizeof(glyph_item_t),
            GLYPH_BUDGET, (lv_cache_ops_t){
                .compare_cb = (lv_cache_compare_cb_t)glyph_compare,
                .create_cb = (lv_cache_create_cb_t)glyph_create,
                .free_cb = (lv_cache_free_cb_t)glyph_free});
        if (!glyph_cache)
        {
            epd_app_free(binding->descriptors);
            lv_free(binding);
            return false;
        }
        lv_cache_set_name(glyph_cache, "READER_GLYPHS");
    }
    if (!lock_initialized)
    {
        rt_mutex_init(&descriptor_lock, "font_dsc", RT_IPC_FLAG_PRIO);
        lock_initialized = true;
    }
    binding->font = font;
    binding->descriptor = font->get_glyph_dsc;
    binding->bitmap = font->get_glyph_bitmap;
    binding->release = font->release_glyph;
    binding->next = bindings;
    bindings = binding;
    font->get_glyph_dsc = cached_descriptor;
    font->get_glyph_bitmap = cached_bitmap;
    font->release_glyph = cached_release;
    return true;
}

void ui_reader_font_cache_detach(lv_font_t *font)
{
    font_binding_t **link = &bindings;
    while (*link && (*link)->font != font) link = &(*link)->next;
    if (!*link) return;
    lv_draw_wait_for_finish();
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    glyph_drop(font, false);
    font_binding_t *binding = *link;
    font->get_glyph_dsc = binding->descriptor;
    font->get_glyph_bitmap = binding->bitmap;
    font->release_glyph = binding->release;
    *link = binding->next;
    for (unsigned i = 0; i < UNICODE_PAGES; ++i)
    {
        if (!binding->descriptors[i]) continue;
        for (unsigned block = 0; block < 16; ++block)
        {
            if (!binding->descriptors[i]->glyphs[block]) continue;
            epd_app_free(binding->descriptors[i]->glyphs[block]);
            descriptor_bytes -= 16u * sizeof(lv_font_glyph_dsc_t);
        }
        epd_app_free(binding->descriptors[i]);
        descriptor_bytes -= sizeof(descriptor_page_t);
    }
    epd_app_free(binding->descriptors);
    lv_free(binding);
    if (!bindings)
    {
        glyph_drop(NULL, false);
        if (!glyph_count)
        {
            lv_cache_destroy(glyph_cache, NULL);
            glyph_cache = NULL;
        }
    }
    rt_mutex_release(&descriptor_lock);
}

/* descriptor_lock held; wrappers still own their returned draw data. Match the
 * final buffer, not an intermediate derived hit made by a nested wrapper. */
static bool glyph_derived_resident(const void *bitmap)
{
    if (!bitmap) return false;
    for (glyph_item_t *item = glyph_items; item; item = item->next)
    {
        if (item->derived && item->buffer == bitmap)
        {
            lv_cache_entry_t *entry = lv_cache_entry_get_entry(item, sizeof(*item));
            return !lv_cache_entry_is_invalid(entry);
        }
    }
    return false;
}

bool ui_reader_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent)
{
    if (letter < 0x20 || letter == 0x7f) return true;
    lv_font_glyph_dsc_t glyph;
    if (!font || !lv_font_get_glyph_dsc(font, &glyph, letter, 0)) return stats_prepare_failed();
    if (glyph.is_placeholder || !glyph.box_w || !glyph.box_h || !lock_initialized) return true;
    /* RT-Thread mutexes are recursive: cached_bitmap/release take this same
     * mutex. Hold it through preparation so another draw thread cannot observe
     * preparing=true or race the speculative cache lookup/eviction. */
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    if (!binding_find(glyph.resolved_font))
    {
        /* Wrappers own their draw-data handles, which are not LVGL entries.
         * Let the wrapper resolve the source and populate its derived key. */
        preparing = true;
        prepare_adjacent = adjacent;
        const void *bitmap = lv_font_get_glyph_bitmap(&glyph, NULL);
        bool cached = glyph_derived_resident(bitmap);
        lv_font_glyph_release_draw_data(&glyph);
        preparing = false;
        rt_mutex_release(&descriptor_lock);
        return cached ? true : stats_prepare_failed();
    }
    glyph_item_t key = {.font = glyph.resolved_font, .glyph = glyph.gid.index};
    lv_cache_entry_t *entry = lv_cache_acquire(glyph_cache, &key, NULL);
    if (entry)
    {
        glyph_touch(entry, adjacent);
        glyph_release(entry);
        rt_mutex_release(&descriptor_lock);
        return true;
    }
    uint32_t stride = lv_draw_buf_width_to_stride(glyph.box_w, LV_COLOR_FORMAT_A8);
    /* Book-wide preparation may fill unused space but never evict nearby text. */
    if (!glyph_room(stride * glyph.box_h + LV_DRAW_BUF_ALIGN, adjacent, 2))
    { rt_mutex_release(&descriptor_lock); return stats_prepare_failed(); }
    prepare_adjacent = adjacent;
    preparing = true;
    const void *bitmap = lv_font_get_glyph_bitmap(&glyph, NULL);
    preparing = false;
    bool cached = bitmap && glyph.entry && lv_cache_entry_get_cache(glyph.entry) == glyph_cache;
    lv_font_glyph_release_draw_data(&glyph);
    rt_mutex_release(&descriptor_lock);
    return cached ? true : stats_prepare_failed();
}
