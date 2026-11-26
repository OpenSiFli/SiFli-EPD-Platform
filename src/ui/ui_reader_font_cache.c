#include "ui_reader_font_cache.h"
#include "ui_reader_font_remote.h"
#include "epd_memory.h"
#include "epd_resource_runtime.h"
#include "ui_font_memory.h"
#include "src/misc/cache/lv_cache.h"
#include "src/misc/cache/class/lv_cache_lru_rb.h"
#include "src/draw/lv_draw_buf_private.h"
#include "platform/app_memory.h"
#include <rthw.h>
#include <string.h>

#define GLYPH_BUDGET (8u * 1024u * 1024u)
#define GLYPH_NODE_LIMIT 8192u
#define DESCRIPTOR_BUDGET (1024u * 1024u)
#define UNICODE_PAGES (0x110000u / 256u)

typedef struct descriptor_page
{
    struct descriptor_page *previous, *next;
    struct font_binding *binding;
    unsigned index;
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
    bool queued, resource_pinned;
} glyph_item_t;

static lv_cache_t *glyph_cache;
static font_binding_t *bindings;
static struct rt_mutex descriptor_lock, callback_lock;
static rt_thread_t preparing_thread;
static bool lock_initialized, preparing, prepare_adjacent;
static unsigned glyph_count;
static glyph_item_t *glyph_items;
/* Oldest first, speculative before nearby. Held entries are removed lazily
 * once, then rejoin only when their last reference is released. */
static glyph_item_t *victim_first[2], *victim_last[2];
static size_t glyph_bytes, victim_bytes;
static size_t descriptor_bytes;
static epd_app_font_cache_stats_t cache_stats;
static epd_resource_cache_t glyph_resource, descriptor_resource;
static descriptor_page_t *descriptor_first, *descriptor_last;
static size_t descriptor_roots;
static size_t resource_published[4] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
static int font_resource_shrink(void *, uint32_t, size_t, size_t *);
extern struct rt_memheap app_image_psram_memheap;
static void font_resource_publish(void)
{
    if (!glyph_resource.provider || !descriptor_resource.provider) {
        epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
        if (previous == EPD_MEMORY_OWNER_INVALID) return;
        epd_resource_cache_open(&glyph_resource, EPD_RESOURCE_UI, font_resource_shrink, &glyph_resource);
        epd_resource_cache_open(&descriptor_resource, EPD_RESOURCE_UI, font_resource_shrink, &descriptor_resource);
        epd_memory_owner_leave_trusted(previous);
    }
    size_t values[] = {glyph_bytes, victim_bytes, descriptor_bytes, descriptor_bytes - descriptor_roots};
    for (unsigned i = 0; i < 2; ++i) {
        if (resource_published[i * 2] == values[i * 2] && resource_published[i * 2 + 1] == values[i * 2 + 1]) continue;
        if (epd_resource_cache_update(i ? &descriptor_resource : &glyph_resource, &app_image_psram_memheap,
            values[i * 2], values[i * 2 + 1], i ? 1 : 2, 0) == EPD_RESOURCE_OK) {
            resource_published[i * 2] = values[i * 2]; resource_published[i * 2 + 1] = values[i * 2 + 1];
        }
    }
}
static void descriptor_unlock(void)
{
    font_resource_publish();
    rt_mutex_release(&descriptor_lock);
}


/* Only misses enter TinyTTF's mutable stream/cache. Cache hits and retained
 * reference release use descriptor_lock without waiting for source I/O. */
static bool is_preparing(void)
{
    return preparing && preparing_thread == rt_thread_self();
}

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
    if (bytes > DESCRIPTOR_BUDGET - descriptor_bytes) return NULL;
    epd_memory_recovery_t recovery = {0};
    if (!ui_font_memory_begin(&recovery, epd_app_allocation_peak(bytes))) return NULL;
    void *memory = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (memory) {
        font_resource_publish();
        if (ui_font_memory_cache_adopt(&descriptor_resource, memory) != EPD_RESOURCE_OK) {
            epd_app_free(memory); memory = NULL;
        }
    }
    ui_font_memory_result(&recovery, memory != NULL);
    if (memory)
    {
        memset(memory, 0, bytes);
        descriptor_bytes += bytes;
    }
    return memory;
}

/* callback_lock then descriptor_lock held: a descriptor miss keeps a local
 * page pointer while its source callback runs without descriptor_lock. */
static void descriptor_page_free(font_binding_t *binding, unsigned index)
{
    descriptor_page_t *page = binding->descriptors[index];
    if (!page) return;
    binding->descriptors[index] = NULL;
    if (page->previous) page->previous->next = page->next; else descriptor_first = page->next;
    if (page->next) page->next->previous = page->previous; else descriptor_last = page->previous;
    for (unsigned block = 0; block < 16; ++block)
    {
        if (!page->glyphs[block]) continue;
        epd_app_free(page->glyphs[block]);
        descriptor_bytes -= 16u * sizeof(lv_font_glyph_dsc_t);
    }
    epd_app_free(page);
    descriptor_bytes -= sizeof(*page);
}

static void descriptor_reserve_root(size_t bytes)
{
    /* Roots are required to bind a new face; cached descriptor pages can be
     * rebuilt on demand. Copies returned to callers and pixel references do
     * not point into these pages, so they remain usable after reclamation. */
    for (font_binding_t *binding = bindings; binding; binding = binding->next)
        for (unsigned i = 0; i < UNICODE_PAGES; ++i)
        {
            if (bytes <= DESCRIPTOR_BUDGET - descriptor_bytes) return;
            descriptor_page_free(binding, i);
        }
}

static bool cached_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                              uint32_t letter, uint32_t next)
{
    bool serialized = false;
retry:
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    font_binding_t *binding = binding_find(font);
    /* Reader fonts disable kerning. A caller that enables it must still get
     * the original pair-dependent advance, never a single-letter cached one. */
    if (font->kerning != LV_FONT_KERNING_NONE || letter >= 0x110000u)
    {
        descriptor_unlock();
        rt_mutex_take(&callback_lock, RT_WAITING_FOREVER);
        bool found = binding->descriptor(font, glyph, letter, next);
        rt_mutex_release(&callback_lock);
        return found;
    }
    descriptor_page_t *page = binding->descriptors[letter >> 8];
    unsigned slot = letter & 255u;
    uint32_t bit = 1u << (slot & 31u);
    if (page && (page->valid[slot >> 5] & bit))
    {
        *glyph = page->glyphs[slot >> 4][slot & 15u];
        descriptor_unlock();
        if (serialized) rt_mutex_release(&callback_lock);
        return true;
    }
    descriptor_unlock();
    if (!serialized)
    {
        rt_mutex_take(&callback_lock, RT_WAITING_FOREVER);
        serialized = true;
        goto retry;
    }
    bool found = binding->descriptor(font, glyph, letter, next);
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    /* Never turn a temporary Tiny TTF allocation failure into a cached miss. */
    if (found)
    {
        if (!page)
        {
            page = descriptor_alloc(sizeof(*page));
            if (page) {
                binding->descriptors[letter >> 8] = page;
                page->binding = binding; page->index = letter >> 8;
                page->previous = descriptor_last;
                if (descriptor_last) descriptor_last->next = page; else descriptor_first = page;
                descriptor_last = page;
            }
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
    descriptor_unlock();
    rt_mutex_release(&callback_lock);
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
    if (!item->resource_pinned) victim_bytes -= item->slot.size;
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
    if (!item->resource_pinned) victim_bytes += item->slot.size;
}

static void glyph_release(lv_cache_entry_t *entry)
{
    /* An invalid entry can be freed by release; do not touch it afterwards. */
    if (!lv_cache_entry_is_invalid(entry) && lv_cache_entry_get_ref(entry) == 1) {
        glyph_item_t *item = lv_cache_entry_get_data(entry);
        if (item->resource_pinned && item->queued) victim_bytes += item->slot.size;
        item->resource_pinned = false;
        victim_append(item);
    }
    lv_cache_release(glyph_cache, entry, NULL);
}

typedef struct {
    const lv_draw_buf_t *buffer;
    epd_memory_owner_t producer;
} glyph_source_t;

static bool glyph_create(glyph_item_t *item, void *source)
{
    const glyph_source_t *input = source;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(input->producer);
    if (previous == EPD_MEMORY_OWNER_INVALID) return false;
    epd_memory_recovery_t recovery = {0};
    bool admitted = ui_font_memory_begin(&recovery, epd_app_allocation_peak(item->slot.size));
    item->buffer = admitted ? lv_draw_buf_dup_ex(app_memory_font_cache_handlers(), input->buffer) : NULL;
    if (item->buffer &&
        (ui_font_memory_cache_adopt(&glyph_resource, item->buffer->unaligned_data) != EPD_RESOURCE_OK ||
         ui_font_memory_cache_adopt(&glyph_resource, item->buffer) != EPD_RESOURCE_OK)) {
        lv_draw_buf_destroy(item->buffer); item->buffer = NULL;
    }
    ui_font_memory_result(&recovery, item->buffer != NULL);
    epd_memory_owner_leave_trusted(previous);
    if (!item->buffer) return false;
    lv_draw_buf_flush_cache(item->buffer, NULL);
    item->speculative = is_preparing() && !prepare_adjacent;
    item->previous = NULL;
    item->next = glyph_items;
    if (glyph_items) glyph_items->previous = item;
    glyph_items = item;
    victim_append(item);
    glyph_bytes += item->slot.size;
    ++glyph_count;
    return true;
}

/* LVGL owns the cache nodes; glyph_create charges pixel preparation to the
 * requesting work and settles the completed buffers into the stable cache. */
static lv_cache_entry_t *glyph_insert(glyph_item_t *key, const lv_draw_buf_t *buffer)
{
    font_resource_publish();
    if (!glyph_resource.provider) return NULL;
    glyph_source_t source = {buffer, epd_memory_owner_current()};
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(glyph_resource.owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) return NULL;
    lv_cache_entry_t *entry = lv_cache_acquire_or_create(glyph_cache, key, &source);
    epd_memory_owner_leave_trusted(previous);
    return entry;
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
    if (!entry) return;
    glyph_item_t *item = lv_cache_entry_get_data(entry);
    if (!item->resource_pinned && item->queued) victim_bytes -= item->slot.size;
    item->resource_pinned = true;
    if (!nearby) return;
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


static int font_resource_shrink(void *context, uint32_t backend, size_t requested, size_t *released)
{
    (void)backend;
    lv_draw_wait_for_finish();
    rt_mutex_take(&callback_lock, RT_WAITING_FOREVER);
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    size_t before = context == &glyph_resource ? glyph_bytes : descriptor_bytes;
    for (unsigned n = 0; n < 16; ++n) {
        size_t current = context == &glyph_resource ? glyph_bytes : descriptor_bytes;
        if (before - current >= requested) break;
        if (context == &glyph_resource) { if (!glyph_evict()) break; }
        else if (descriptor_first) descriptor_page_free(descriptor_first->binding, descriptor_first->index);
        else break;
    }
    *released = before - (context == &glyph_resource ? glyph_bytes : descriptor_bytes);
    descriptor_unlock();
    rt_mutex_release(&callback_lock);
    return EPD_RESOURCE_OK;
}

static bool glyph_room(uint32_t bytes, bool evict, unsigned copies)
{
    if (bytes > GLYPH_BUDGET - 256u) return false;
    size_t charge = glyph_charge(bytes);
    while (glyph_count >= GLYPH_NODE_LIMIT ||
           GLYPH_BUDGET - glyph_bytes < charge)
    {
        if (!evict || !glyph_evict()) return false;
    }
    /* Source rasterization and the retained copy coexist. Include both physical
     * peaks without reserving another component's hypothetical future growth. */
    size_t peak = epd_app_allocation_peak(charge);
    if (!copies || peak > SIZE_MAX / copies) return false;
    epd_memory_recovery_t recovery = {0};
    return ui_font_memory_begin(&recovery, peak * copies);
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
    glyph_touch(*entry, !is_preparing() || prepare_adjacent);
    const lv_draw_buf_t *buffer = *entry ? ((glyph_item_t *)lv_cache_entry_get_data(*entry))->buffer : NULL;
    if (!is_preparing()) stats_lookup(buffer != NULL);
    descriptor_unlock();
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
                                  !is_preparing() || prepare_adjacent, 1))
            *entry = glyph_insert(&key, bitmap);
    }
    glyph_touch(*entry, !is_preparing() || prepare_adjacent);
    const lv_draw_buf_t *buffer = *entry ? ((glyph_item_t *)lv_cache_entry_get_data(*entry))->buffer : NULL;
    descriptor_unlock();
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
    descriptor_unlock();
}

void ui_reader_font_derived_drop(const lv_font_t *font)
{
    if (!font || !lock_initialized) return;
    lv_draw_wait_for_finish();
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    glyph_drop(font, true);
    descriptor_unlock();
}

/* Providers can put a raw draw buffer in glyph->entry. Tag only our own
 * cache references so release can forward the provider's opaque value. */
static lv_cache_entry_t *glyph_cache_reference(lv_cache_entry_t *entry)
{
    return (lv_cache_entry_t *)((uintptr_t)entry | 1u);
}

static lv_cache_entry_t *glyph_cache_entry(lv_cache_entry_t *reference)
{
    uintptr_t value = (uintptr_t)reference;
    return (value & 1u) ? (lv_cache_entry_t *)(value & ~(uintptr_t)1u) : NULL;
}

static const void *cached_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    bool serialized = false;
retry:
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    font_binding_t *binding = binding_find(glyph->resolved_font);
    glyph_item_t key = {.font = glyph->resolved_font, .glyph = glyph->gid.index};
    lv_cache_entry_t *entry = lv_cache_acquire(glyph_cache, &key, NULL);
    if (!serialized && !is_preparing()) stats_lookup(entry != NULL);
    if (entry)
    {
        glyph_touch(entry, !is_preparing() || prepare_adjacent);
        glyph->entry = glyph_cache_reference(entry);
        const void *buffer = ((glyph_item_t *)lv_cache_entry_get_data(entry))->buffer;
        descriptor_unlock();
        if (serialized) rt_mutex_release(&callback_lock);
        return buffer;
    }
    if (!serialized)
    {
        descriptor_unlock();
        rt_mutex_take(&callback_lock, RT_WAITING_FOREVER);
        serialized = true;
        goto retry;
    }
    uint32_t stride = lv_draw_buf_width_to_stride(glyph->box_w, LV_COLOR_FORMAT_A8);
    bool keep = glyph_room(stride * glyph->box_h + LV_DRAW_BUF_ALIGN,
                           !is_preparing() || prepare_adjacent, 2);
    descriptor_unlock();
    if (!keep && is_preparing())
    {
        rt_mutex_release(&callback_lock);
        return NULL;
    }
    /* The original callback applies weight and writes back the rasterized data. */
    rt_tick_t started = is_preparing() ? 0 : rt_tick_get();
    const lv_draw_buf_t *source = binding->bitmap(glyph, scratch);
    if (!is_preparing()) stats_source_bitmap((rt_tick_t)(rt_tick_get() - started));
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    /* The source may use a different stride/size than the descriptor estimate.
     * Recheck before LVGL insertion, whose implicit LRU eviction has no tiers. */
    if (!source || !keep || !glyph_room(source->data_size, !is_preparing() || prepare_adjacent, 1))
    {
        descriptor_unlock();
        rt_mutex_release(&callback_lock);
        return source;
    }
    key.slot.size = glyph_charge(source->data_size);
    entry = glyph_insert(&key, source);
    if (!entry)
    {
        descriptor_unlock();
        rt_mutex_release(&callback_lock);
        return source;
    }
    descriptor_unlock();
    binding->release(binding->font, glyph);
    glyph->entry = glyph_cache_reference(entry);
    const void *buffer = ((glyph_item_t *)lv_cache_entry_get_data(entry))->buffer;
    rt_mutex_release(&callback_lock);
    return buffer;
}

static void cached_release(const lv_font_t *font, lv_font_glyph_dsc_t *glyph)
{
    if (!glyph->entry) return;
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    lv_cache_entry_t *entry = glyph_cache_entry(glyph->entry);
    if (entry)
    {
        glyph_release(entry);
        glyph->entry = NULL;
    }
    else
    {
        font_binding_t *binding = binding_find(font);
        descriptor_unlock();
        rt_mutex_take(&callback_lock, RT_WAITING_FOREVER);
        binding->release(font, glyph);
        rt_mutex_release(&callback_lock);
        return;
    }
    descriptor_unlock();
}

void ui_reader_font_cache_reserve(size_t bytes)
{
    epd_memory_recovery_t recovery = {0};
    (void)ui_font_memory_begin(&recovery, epd_app_allocation_peak(bytes));
}

bool ui_reader_font_cache_attach(lv_font_t *font)
{
    if (!font || !font->get_glyph_dsc || !font->get_glyph_bitmap || !font->release_glyph) return false;
    if (binding_find(font)) return true;
    font_binding_t *binding = NULL;
    size_t bytes = UNICODE_PAGES * sizeof(*binding->descriptors);
    if (!lock_initialized)
    {
        rt_mutex_init(&descriptor_lock, "font_dsc", RT_IPC_FLAG_PRIO);
        rt_mutex_init(&callback_lock, "font_src", RT_IPC_FLAG_PRIO);
        lock_initialized = true;
    }
    /* Drain drawing before either mutex, then use the same order as misses.
     * Keep callbacks excluded until the root and binding are published. */
    lv_draw_wait_for_finish();
    rt_mutex_take(&callback_lock, RT_WAITING_FOREVER);
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    font_resource_publish();
    if (!glyph_resource.provider || !descriptor_resource.provider) goto failed;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(descriptor_resource.owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) goto failed;
    binding = lv_malloc(sizeof(*binding));
    epd_memory_owner_leave_trusted(previous);
    if (!binding) goto failed;
    descriptor_reserve_root(bytes);
    binding->descriptors = descriptor_alloc(bytes);
    if (!binding->descriptors) goto failed;
    descriptor_roots += bytes;
    if (!glyph_cache)
    {
        previous = epd_memory_owner_enter_trusted(glyph_resource.owner);
        if (previous == EPD_MEMORY_OWNER_INVALID) {
            epd_app_free(binding->descriptors);
            descriptor_bytes -= bytes; descriptor_roots -= bytes;
            goto failed;
        }
        glyph_cache = lv_cache_create(&lv_cache_class_lru_rb_size, sizeof(glyph_item_t),
            GLYPH_BUDGET, (lv_cache_ops_t){
                .compare_cb = (lv_cache_compare_cb_t)glyph_compare,
                .create_cb = (lv_cache_create_cb_t)glyph_create,
                .free_cb = (lv_cache_free_cb_t)glyph_free});
        epd_memory_owner_leave_trusted(previous);
        if (!glyph_cache)
        {
            epd_app_free(binding->descriptors);
            descriptor_bytes -= bytes; descriptor_roots -= bytes;
            goto failed;
        }
        lv_cache_set_name(glyph_cache, "READER_GLYPHS");
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
    descriptor_unlock();
    rt_mutex_release(&callback_lock);
    return true;
failed:
    descriptor_unlock();
    rt_mutex_release(&callback_lock);
    lv_free(binding);
    return false;
}

void ui_reader_font_cache_detach(lv_font_t *font)
{
    font_binding_t **link = &bindings;
    while (*link && (*link)->font != font) link = &(*link)->next;
    if (!*link) return;
    lv_draw_wait_for_finish();
    rt_mutex_take(&callback_lock, RT_WAITING_FOREVER);
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    glyph_drop(font, false);
    font_binding_t *binding = *link;
    font->get_glyph_dsc = binding->descriptor;
    font->get_glyph_bitmap = binding->bitmap;
    font->release_glyph = binding->release;
    *link = binding->next;
    for (unsigned i = 0; i < UNICODE_PAGES; ++i)
        descriptor_page_free(binding, i);
    epd_app_free(binding->descriptors);
    descriptor_bytes -= UNICODE_PAGES * sizeof(*binding->descriptors);
    descriptor_roots -= UNICODE_PAGES * sizeof(*binding->descriptors);
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
    descriptor_unlock();
    rt_mutex_release(&callback_lock);
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
    /* Serialize preparation context and original callbacks, but release cache
     * metadata while rasterizing so drawing can consume already warm glyphs. */
    bool serialized = false;
retry:
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    bool native = binding_find(glyph.resolved_font) != NULL;
    if (native)
    {
        glyph_item_t key = {.font = glyph.resolved_font, .glyph = glyph.gid.index};
        lv_cache_entry_t *entry = lv_cache_acquire(glyph_cache, &key, NULL);
        if (entry)
        {
            glyph_touch(entry, adjacent);
            glyph_release(entry);
            descriptor_unlock();
            if (serialized) rt_mutex_release(&callback_lock);
            return true;
        }
    }
    if (!serialized)
    {
        descriptor_unlock();
        rt_mutex_take(&callback_lock, RT_WAITING_FOREVER);
        serialized = true;
        goto retry;
    }
    if (native)
    {
        uint32_t stride = lv_draw_buf_width_to_stride(glyph.box_w, LV_COLOR_FORMAT_A8);
        if (!glyph_room(stride * glyph.box_h + LV_DRAW_BUF_ALIGN, adjacent, 2))
        {
            descriptor_unlock();
            rt_mutex_release(&callback_lock);
            return stats_prepare_failed();
        }
    }
    preparing_thread = rt_thread_self();
    preparing = true;
    prepare_adjacent = adjacent;
    descriptor_unlock();
    const void *bitmap = lv_font_get_glyph_bitmap(&glyph, NULL);
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    bool cached = native ? bitmap && glyph_cache_entry(glyph.entry) :
                           glyph_derived_resident(bitmap);
    preparing = false;
    descriptor_unlock();
    lv_font_glyph_release_draw_data(&glyph);
    rt_mutex_release(&callback_lock);
    return cached ? true : stats_prepare_failed();
}
