#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Historical fixture threshold; production no longer reserves this amount. */
#define SHARED_RESERVE (1024u * 1024u)
#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#endif
#include "src/core/lv_global.h"
#include "src/misc/cache/lv_cache_entry.h"
static unsigned eviction_ref_checks;
static int32_t counted_ref(lv_cache_entry_t *entry)
{ ++eviction_ref_checks; return lv_cache_entry_get_ref(entry); }
#define lv_cache_entry_get_ref counted_ref
#include "../../src/ui/ui_reader_font_cache.c"
#undef lv_cache_entry_get_ref


lv_global_t lv_global;
bool ui_reader_font_remote_release_storage(uint32_t release_id) { (void)release_id; return true; }
void ui_reader_font_remote_resume_cleanup(void) {}
static size_t available = 64u * 1024u * 1024u;
static unsigned remote_reclaim_calls;
static size_t remote_reclaim_target;
size_t ui_reader_font_remote_reclaim_pixels(size_t target)
{
#ifdef _WIN32
    DWORD self = GetCurrentThreadId();
    assert(!lock_initialized || (DWORD)(uintptr_t)descriptor_lock.native.OwningThread != self);
    assert(!lock_initialized || (DWORD)(uintptr_t)callback_lock.native.OwningThread != self);
#else
    assert(!descriptor_lock.depth && !callback_lock.depth);
#endif
    ++remote_reclaim_calls; remote_reclaim_target = target;
    return 0;
}
static unsigned source_calls, descriptor_calls;
static bool descriptor_fails;
static const lv_font_t *missing_font;
static unsigned box_width = 16, box_height = 20;
static bool allocation_fails;
static int allocation_countdown = -1;
static unsigned app_allocations;
static bool source_consumes_available;
static unsigned source_stride;
static void (*source_hook)(void);
static void (*descriptor_hook)(void);
static const lv_draw_buf_handlers_t *cache_handlers;
const lv_draw_buf_handlers_t *app_memory_font_cache_handlers(void)
{
    return cache_handlers ? cache_handlers : lv_draw_buf_get_font_handlers();
}

void *epd_app_alloc(size_t size, epd_app_memory_t region)
{
    (void)region;
    if(allocation_fails || allocation_countdown == 0) return NULL;
    if(allocation_countdown > 0) --allocation_countdown;
    void *memory = malloc(size);
    if(memory) ++app_allocations;
    return memory;
}
void epd_app_free(void *memory) { if(memory) { assert(app_allocations); --app_allocations; } free(memory); }
size_t epd_app_psram_available(void) { return available; }
void lv_draw_wait_for_finish(void)
{
    /* Waiting for drawing while owning either lock can deadlock a callback. */
#ifdef _WIN32
    DWORD self = GetCurrentThreadId();
    assert(!lock_initialized || (DWORD)(uintptr_t)descriptor_lock.native.OwningThread != self);
    assert(!lock_initialized || (DWORD)(uintptr_t)callback_lock.native.OwningThread != self);
#else
    assert(!descriptor_lock.depth && !callback_lock.depth);
#endif
}
void epd_app_clean_draw_buffer(const lv_draw_buf_t *buffer) { (void)buffer; }
const lv_draw_buf_t *epd_app_font_derived_acquire(const lv_font_t *f, uint32_t g, uint32_t s, lv_cache_entry_t **e)
{ return ui_reader_font_derived_acquire(f, g, s, e); }
const lv_draw_buf_t *epd_app_font_derived_store(const lv_font_t *f, uint32_t g, uint32_t s, const lv_draw_buf_t *b, lv_cache_entry_t **e)
{ return ui_reader_font_derived_store(f, g, s, b, e); }
void epd_app_font_derived_release(lv_cache_entry_t *e) { ui_reader_font_derived_release(e); }
void epd_app_font_derived_drop(const lv_font_t *f) { ui_reader_font_derived_drop(f); }

static bool source_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *g,
                              uint32_t letter, uint32_t next)
{
    ++descriptor_calls;
    if(descriptor_hook) descriptor_hook();
    if (descriptor_fails || font == missing_font) return false;
    g->box_w = (uint16_t)box_width; g->box_h = (uint16_t)box_height;
    g->adv_w = (uint16_t)(box_width + (font->kerning == LV_FONT_KERNING_NORMAL ? next : 0));
    g->format = LV_FONT_GLYPH_FORMAT_A8;
    g->gid.index = letter + 7;
    return true;
}
static const void *source_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *scratch)
{
    (void)scratch;
    ++source_calls;
    if(source_hook) source_hook();
    lv_draw_buf_t *buf = lv_draw_buf_create_ex(lv_draw_buf_get_font_handlers(),
        g->box_w, g->box_h, LV_COLOR_FORMAT_A8, source_stride);
    assert(buf);
    memset(buf->data, (unsigned char)g->gid.index, buf->data_size);
    if(source_consumes_available) { assert(available >= buf->data_size); available -= buf->data_size; }
    return buf;
}
/* Source buffers use actual LVGL entry ownership, like Tiny TTF. */
static lv_cache_t *source_cache;
typedef struct { uint32_t glyph; lv_draw_buf_t *buffer; } source_item_t;
static lv_cache_compare_res_t source_compare(const void *a, const void *b)
{
    uint32_t x = ((const source_item_t *)a)->glyph, y = ((const source_item_t *)b)->glyph;
    return x == y ? 0 : x > y ? 1 : -1;
}
static bool source_create(void *data, void *user)
{ ((source_item_t *)data)->buffer = (lv_draw_buf_t *)source_bitmap(user, NULL); return true; }
static void source_free(void *data, void *user)
{ (void)user; lv_draw_buf_destroy(((source_item_t *)data)->buffer); }
static const void *source_cached_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *scratch)
{
    (void)scratch;
    source_item_t key = {.glyph = g->gid.index};
    g->entry = lv_cache_acquire_or_create(source_cache, &key, g);
    return g->entry ? ((source_item_t *)lv_cache_entry_get_data(g->entry))->buffer : NULL;
}
static void source_cached_release(const lv_font_t *font, lv_font_glyph_dsc_t *g)
{ (void)font; if(g->entry) lv_cache_release(source_cache, g->entry, NULL); g->entry = NULL; }
static lv_font_t make_font(void)
{
    lv_font_t font = {.get_glyph_dsc = source_descriptor, .get_glyph_bitmap = source_cached_bitmap,
        .release_glyph = source_cached_release, .kerning = LV_FONT_KERNING_NONE, .line_height = 20};
    return font;
}
static void draw(lv_font_t *font, uint32_t letter)
{
    lv_font_glyph_dsc_t g;
    assert(lv_font_get_glyph_dsc(font, &g, letter, 0));
    const lv_draw_buf_t *b = lv_font_get_glyph_bitmap(&g, NULL);
    assert(b && b->data[0] == (unsigned char)g.gid.index);
    lv_font_glyph_release_draw_data(&g);
}
static epd_app_font_cache_stats_t snapshot(void)
{ epd_app_font_cache_stats_t s; ui_reader_font_cache_stats(&s); return s; }
#include "derived_fixture.inc"
static void prepared_key(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_prepare(&font, 0x4e00, true));
    epd_app_font_cache_stats_t a = snapshot();
    draw(&font, 0x4e00);
    epd_app_font_cache_stats_t b = snapshot();
    assert(b.glyph_hits == a.glyph_hits + 1 && b.glyph_misses == a.glyph_misses);
    assert(source_calls == 1);
    ui_reader_font_cache_detach(&font);
}
static void detach_other_font(void)
{
    lv_font_t a = make_font(), b = make_font();
    assert(ui_reader_font_cache_attach(&a)); assert(ui_reader_font_cache_attach(&b));
    assert(ui_reader_font_prepare(&a, 0x4e00, true));
    ui_reader_font_cache_detach(&b);
    epd_app_font_cache_stats_t before = snapshot(); draw(&a, 0x4e00);
    assert(snapshot().glyph_misses == before.glyph_misses);
    ui_reader_font_cache_detach(&a);
}
static void sparse_descriptors(void)
{
    lv_font_t fonts[3] = {make_font(), make_font(), make_font()};
    for(unsigned f = 0; f < 3; ++f) assert(ui_reader_font_cache_attach(&fonts[f]));
    for(unsigned pass = 0; pass < 2; ++pass)
        for(unsigned f = 0; f < 3; ++f)
            for(uint32_t page = 0x4e; page < 0xa0; ++page) {
                lv_font_glyph_dsc_t g;
                assert(lv_font_get_glyph_dsc(&fonts[f], &g, page * 256 + 3, 0));
            }
    assert(descriptor_calls == 3 * (0xa0 - 0x4e));
    for(unsigned f = 0; f < 3; ++f) ui_reader_font_cache_detach(&fonts[f]);
}
static void wrapper_prepare(void)
{
    lv_font_t source = make_font(); assert(ui_reader_font_cache_attach(&source));
    lv_font_t *wrapper = cache_test_derived(&source); assert(wrapper);
    assert(ui_reader_font_prepare(wrapper, 0x4e00, true));
    epd_app_font_cache_stats_t before = snapshot();
    lv_font_glyph_dsc_t g; assert(lv_font_get_glyph_dsc(wrapper, &g, 0x4e00, 0));
    const lv_draw_buf_t *b = lv_font_get_glyph_bitmap(&g, NULL); assert(b);
    assert(b->data[(g.box_h - 1) * b->header.stride] == (unsigned char)(0x4e00 + 7));
    lv_font_glyph_release_draw_data(&g);
    assert(snapshot().glyph_misses == before.glyph_misses);
    assert(snapshot().glyph_hits == before.glyph_hits + 1);
    assert(source_calls == 1);
    cache_test_free(wrapper); ui_reader_font_cache_detach(&source);
}
static void wrapper_failure(void)
{
    lv_font_t source = make_font(); assert(ui_reader_font_cache_attach(&source));
    lv_font_t *wrapper = cache_test_derived(&source); assert(wrapper);
    allocation_fails = true;
    assert(!ui_reader_font_prepare(wrapper, 0x4e00, true));
    allocation_fails = false;
    assert(ui_reader_font_prepare(wrapper, 0x4e00, true));
    cache_test_free(wrapper); ui_reader_font_cache_detach(&source);
}
static void speculative_eviction(void)
{
    box_width = box_height = 1024;
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_prepare(&font, 65, true));
    unsigned next = 66;
    while(ui_reader_font_prepare(&font, next, false)) ++next;
    assert(next > 67 && next < 80);
    assert(ui_reader_font_prepare(&font, next, true));
    epd_app_font_cache_stats_t before = snapshot(); draw(&font, 65);
    assert(snapshot().glyph_misses == before.glyph_misses);
    /* A speculative hit must not turn old nearby text into the newest item. */
    assert(ui_reader_font_prepare(&font, 65, false));
    ui_reader_font_cache_detach(&font);
}
static void speculative_hit_recency(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_prepare(&font, 65, true));
    assert(ui_reader_font_prepare(&font, 66, true));
    assert(ui_reader_font_prepare(&font, 65, false));
    assert(glyph_evict());
    epd_app_font_cache_stats_t before = snapshot(); draw(&font, 66);
    assert(snapshot().glyph_misses == before.glyph_misses);
    ui_reader_font_cache_detach(&font);
}
static void held_reference(void)
{
    lv_font_t a = make_font(), b = make_font();
    assert(ui_reader_font_cache_attach(&a)); assert(ui_reader_font_cache_attach(&b));
    lv_draw_buf_t *buf = lv_draw_buf_create(16, 20, LV_COLOR_FORMAT_A8, 0); assert(buf);
    memset(buf->data, 123, buf->data_size);
    lv_cache_entry_t *held;
    const lv_draw_buf_t *copy = ui_reader_font_derived_store(&a, 7, 1, buf, &held); assert(copy);
    size_t charged = glyph_bytes;
    ui_reader_font_cache_detach(&a);
    assert(copy->data[0] == 123 && glyph_bytes == charged);
    assert(lv_cache_entry_is_invalid(held));
    assert(!glyph_evict());
    ui_reader_font_cache_detach(&b);
    assert(glyph_cache && glyph_bytes == charged);
    ui_reader_font_derived_release(held);
    lv_draw_buf_destroy(buf);
}
static void kerning_and_retry(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    lv_font_glyph_dsc_t g;
    descriptor_fails = true; assert(!lv_font_get_glyph_dsc(&font, &g, 65, 0));
    descriptor_fails = false; assert(lv_font_get_glyph_dsc(&font, &g, 65, 0));
    unsigned calls = descriptor_calls;
    assert(lv_font_get_glyph_dsc(&font, &g, 65, 0) && descriptor_calls == calls);
    font.kerning = LV_FONT_KERNING_NORMAL;
    assert(lv_font_get_glyph_dsc(&font, &g, 65, 1) && g.adv_w == 17);
    assert(lv_font_get_glyph_dsc(&font, &g, 65, 2) && g.adv_w == 18);
    assert(descriptor_calls == calls + 2);
    ui_reader_font_cache_detach(&font);
}
static void descriptor_allocation_retry(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    allocation_fails = true;
    lv_font_glyph_dsc_t g; assert(lv_font_get_glyph_dsc(&font, &g, 65, 0));
    allocation_fails = false;
    assert(lv_font_get_glyph_dsc(&font, &g, 65, 0));
    assert(lv_font_get_glyph_dsc(&font, &g, 65, 0));
    assert(descriptor_calls == 2);
    ui_reader_font_cache_detach(&font);
}
static void fallback_key(void)
{
    lv_font_t a = make_font(), b = make_font(); a.fallback = &b;
    assert(ui_reader_font_cache_attach(&a)); assert(ui_reader_font_cache_attach(&b));
    missing_font = &a;
    assert(ui_reader_font_prepare(&a, 65, true));
    epd_app_font_cache_stats_t before = snapshot(); draw(&a, 65);
    assert(snapshot().glyph_hits == before.glyph_hits + 1);
    ui_reader_font_cache_detach(&a);
    before = snapshot(); draw(&b, 65);
    assert(snapshot().glyph_misses == before.glyph_misses);
    ui_reader_font_cache_detach(&b);
}
static void distinct_font_keys(void)
{
    lv_font_t a = make_font(), b = make_font();
    assert(ui_reader_font_cache_attach(&a)); assert(ui_reader_font_cache_attach(&b));
    assert(ui_reader_font_prepare(&a, 65, true));
    epd_app_font_cache_stats_t before = snapshot(); draw(&b, 65);
    assert(snapshot().glyph_misses == before.glyph_misses + 1);
    ui_reader_font_cache_detach(&a); ui_reader_font_cache_detach(&b);
}
static void pressure_fallback(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_prepare(&font, 65, true));
    available = 0;
    assert(!ui_reader_font_prepare(&font, 66, false));
    epd_app_font_cache_stats_t before = snapshot(); draw(&font, 65);
    assert(snapshot().glyph_hits == before.glyph_hits + 1);
    draw(&font, 66); /* No room: retain/release the source entry normally. */
    assert(glyph_count == 1 && snapshot().source_bitmap_calls == before.source_bitmap_calls + 1);
    available = 64u * 1024u * 1024u;
    assert(ui_reader_font_prepare(&font, 66, true));
    ui_reader_font_cache_detach(&font);
}
static void derived_no_speculative_eviction(void)
{
    box_width = box_height = 1024;
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    lv_font_t *wrapper = cache_test_derived(&font); assert(wrapper);
    assert(ui_reader_font_prepare(&font, 65, true));
    unsigned next = 66;
    while(ui_reader_font_prepare(&font, next, false)) ++next;
    /* Its native pixels already exist, but the transformation cannot fit. */
    assert(!ui_reader_font_prepare(wrapper, 65, false));
    epd_app_font_cache_stats_t before = snapshot(); draw(&font, 65);
    assert(snapshot().glyph_misses == before.glyph_misses);
    cache_test_free(wrapper); ui_reader_font_cache_detach(&font);
}
static void stats_abi_and_preparation(void)
{
    struct { epd_app_font_cache_stats_t stats; uint64_t sentinel; } out;
    out.sentinel = UINT64_C(0x1827364554637281);
    ui_reader_font_cache_stats(&out.stats);
    assert(out.sentinel == UINT64_C(0x1827364554637281));
    assert(sizeof(out.stats) == 48 && out.stats.ticks_per_second == RT_TICK_PER_SECOND);
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_cache_attach(&font)); /* Idempotent: no callback recursion. */
    assert(ui_reader_font_prepare(&font, 65, true));
    assert(ui_reader_font_prepare(&font, 65, true));
    epd_app_font_cache_stats_t s = snapshot();
    assert(!s.glyph_hits && !s.glyph_misses && !s.source_bitmap_calls && !s.prepare_failures);
    draw(&font, 66); s = snapshot();
    assert(s.glyph_misses == 1 && s.source_bitmap_calls == 1 && s.source_bitmap_ticks == 1);
    ui_reader_font_cache_detach(&font);
}

static bool native_resident(const lv_font_t *font, uint32_t letter)
{
    glyph_item_t key = {.font = font, .glyph = letter + 7};
    lv_cache_entry_t *entry = lv_cache_acquire(glyph_cache, &key, NULL);
    if(entry) lv_cache_release(glyph_cache, entry, NULL);
    return entry != NULL;
}
static void tier_order_and_held(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_prepare(&font, 65, true));
    assert(ui_reader_font_prepare(&font, 66, false));
    assert(ui_reader_font_prepare(&font, 67, true));
    assert(ui_reader_font_prepare(&font, 68, false));
    lv_font_glyph_dsc_t held;
    assert(lv_font_get_glyph_dsc(&font, &held, 66, 0));
    /* Hold without promoting the speculative entry. */
    glyph_item_t key = {.font = &font, .glyph = held.gid.index};
    lv_cache_entry_t *held_entry = lv_cache_acquire(glyph_cache, &key, NULL);
    assert(held_entry);
    held.entry = glyph_cache_reference(held_entry);
    assert(glyph_evict());
    assert(!native_resident(&font, 68) && native_resident(&font, 65));
    lv_font_glyph_release_draw_data(&held);
    assert(glyph_evict()); assert(!native_resident(&font, 66));
    draw(&font, 65); /* Promote the oldest nearby glyph. */
    assert(glyph_evict()); assert(!native_resident(&font, 67) && native_resident(&font, 65));
    ui_reader_font_cache_detach(&font);
}
static void held_invalidation_and_reinsert(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    lv_draw_buf_t *buf = lv_draw_buf_create(16, 20, LV_COLOR_FORMAT_A8, 0); assert(buf);
    memset(buf->data, 17, buf->data_size);
    lv_cache_entry_t *old, *replacement;
    const lv_draw_buf_t *old_buf = ui_reader_font_derived_store(&font, 7, 1, buf, &old); assert(old_buf);
    size_t charge = glyph_bytes;
    ui_reader_font_derived_drop(&font);
    memset(buf->data, 29, buf->data_size);
    const lv_draw_buf_t *new_buf = ui_reader_font_derived_store(&font, 7, 1, buf, &replacement); assert(new_buf);
    assert(old != replacement && old_buf->data[0] == 17 && new_buf->data[0] == 29);
    assert(glyph_bytes == 2 * charge && lv_cache_get_size(glyph_cache, NULL) == charge);
    assert(!glyph_evict());
    ui_reader_font_derived_release(replacement);
    assert(glyph_evict()); assert(glyph_bytes == charge && old_buf->data[0] == 17);
    ui_reader_font_cache_detach(&font);
    assert(glyph_cache && lv_cache_entry_is_invalid(old));
    ui_reader_font_derived_release(old); lv_draw_buf_destroy(buf);
}
static void sparse_partial_allocation_and_boundaries(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    allocation_countdown = 1; /* Page succeeds, first block fails. */
    lv_font_glyph_dsc_t g; assert(lv_font_get_glyph_dsc(&font, &g, 0x4e0f, 0));
    assert(descriptor_bytes == UNICODE_PAGES * sizeof(void *) + sizeof(descriptor_page_t));
    allocation_countdown = -1;
    const uint32_t letters[] = {0x4e0f, 0x4e10, 0x4e1f, 0x4e20, 0x4eff, 0x4f00, 0x10ffff};
    for(unsigned i = 0; i < sizeof(letters)/sizeof(*letters); ++i)
        assert(lv_font_get_glyph_dsc(&font, &g, letters[i], 0));
    unsigned calls = descriptor_calls;
    for(unsigned i = 0; i < sizeof(letters)/sizeof(*letters); ++i)
        assert(lv_font_get_glyph_dsc(&font, &g, letters[i], 0));
    assert(descriptor_calls == calls);
    assert(descriptor_bytes == UNICODE_PAGES * sizeof(void *) + 3 * sizeof(descriptor_page_t) + 6 * 16 * sizeof(g));
    assert(lv_font_get_glyph_dsc(&font, &g, 0x110000, 0));
    assert(lv_font_get_glyph_dsc(&font, &g, 0x110000, 0));
    assert(descriptor_calls == calls + 2);
    ui_reader_font_cache_detach(&font);
}
static void source_copy_pressure(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_prepare(&font, 65, true));
    lv_font_glyph_dsc_t g; assert(lv_font_get_glyph_dsc(&font, &g, 66, 0));
    size_t bytes = lv_draw_buf_width_to_stride(g.box_w, LV_COLOR_FORMAT_A8) * g.box_h + LV_DRAW_BUF_ALIGN;
    available = SHARED_RESERVE + DESCRIPTOR_BUDGET - descriptor_bytes + 2 * bytes;
    source_consumes_available = true;
    assert(ui_reader_font_prepare(&font, 66, false));
    assert(native_resident(&font, 65) && native_resident(&font, 66));
    source_consumes_available = false;
    ui_reader_font_cache_detach(&font);
}
static void derived_copy_pressure(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_prepare(&font, 65, true));
    lv_draw_buf_t *buf = lv_draw_buf_create(16, 20, LV_COLOR_FORMAT_A8, 0); assert(buf);
    memset(buf->data, 31, buf->data_size);
    available = SHARED_RESERVE + DESCRIPTOR_BUDGET - descriptor_bytes + buf->data_size + LV_DRAW_BUF_ALIGN;
    lv_cache_entry_t *entry;
    assert(ui_reader_font_derived_store(&font, 65, 1, buf, &entry));
    assert(native_resident(&font, 65));
    ui_reader_font_derived_release(entry);
    lv_draw_buf_destroy(buf); ui_reader_font_cache_detach(&font);
}
static void source_larger_stride_no_implicit_eviction(void)
{
    box_width = box_height = 1024;
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    for(unsigned letter = 65; letter < 72; ++letter) assert(ui_reader_font_prepare(&font, letter, false));
    box_width = 16; box_height = 1024; source_stride = 2048;
    assert(!ui_reader_font_prepare(&font, 80, false));
    for(unsigned letter = 65; letter < 72; ++letter) assert(native_resident(&font, letter));
    assert(ui_reader_font_prepare(&font, 80, true));
    assert(!native_resident(&font, 65) && !native_resident(&font, 66));
    for(unsigned letter = 67; letter < 72; ++letter) assert(native_resident(&font, letter));
    assert(native_resident(&font, 80));
    ui_reader_font_cache_detach(&font);
}

/* An outer wrapper can retain an intermediate derived glyph while its own
 * pixels are immediate. That intermediate hit must not certify the result. */
typedef struct {
    lv_font_t font;
    const lv_font_t *source;
} outer_font_t;
typedef struct {
    lv_font_glyph_dsc_t inner;
    lv_draw_buf_t *immediate;
} outer_reference_t;
static bool outer_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t letter, uint32_t next)
{
    (void)next;
    const outer_font_t *outer = (const outer_font_t *)font;
    bool found = lv_font_get_glyph_dsc(outer->source, g, letter, 0);
    g->gid.index = letter;
    return found;
}
static const void *outer_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *scratch)
{
    const outer_font_t *outer = (const outer_font_t *)g->resolved_font;
    outer_reference_t *ref = calloc(1, sizeof(*ref)); assert(ref);
    assert(lv_font_get_glyph_dsc(outer->source, &ref->inner, g->gid.index, 0));
    const lv_draw_buf_t *inner = lv_font_get_glyph_bitmap(&ref->inner, scratch); assert(inner);
    ref->immediate = lv_draw_buf_dup(inner); assert(ref->immediate);
    ref->immediate->data[0] ^= 0xff;
    g->entry = (lv_cache_entry_t *)ref;
    return ref->immediate;
}
static void outer_release(const lv_font_t *font, lv_font_glyph_dsc_t *g)
{
    (void)font;
    outer_reference_t *ref = (outer_reference_t *)g->entry;
    lv_font_glyph_release_draw_data(&ref->inner);
    lv_draw_buf_destroy(ref->immediate);
    free(ref); g->entry = NULL;
}
static void wrapper_intermediate_is_not_output(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    lv_font_t *inner = cache_test_derived(&font); assert(inner);
    outer_font_t outer = {.font = {.get_glyph_dsc = outer_descriptor, .get_glyph_bitmap = outer_bitmap,
        .release_glyph = outer_release, .kerning = LV_FONT_KERNING_NONE}, .source = inner};
    assert(!ui_reader_font_prepare(&outer.font, 65, true));
    assert(!ui_reader_font_prepare(&outer.font, 65, true));
    cache_test_free(inner); ui_reader_font_cache_detach(&font);
}
#ifdef _WIN32
static HANDLE source_entered, source_continue, draw_entered;
static lv_font_t *thread_wrapper;
static void pause_source(void)
{ SetEvent(source_entered); assert(WaitForSingleObject(source_continue, 5000) == WAIT_OBJECT_0); }
static DWORD WINAPI prepare_wrapper_thread(void *unused)
{ (void)unused; assert(ui_reader_font_prepare(thread_wrapper, 65, true)); return 0; }
static DWORD WINAPI draw_wrapper_thread(void *unused)
{
    (void)unused; SetEvent(draw_entered);
    lv_font_glyph_dsc_t g; assert(lv_font_get_glyph_dsc(thread_wrapper, &g, 65, 0));
    assert(lv_font_get_glyph_bitmap(&g, NULL)); lv_font_glyph_release_draw_data(&g); return 0;
}
static void wrapper_recursive_lock(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    thread_wrapper = cache_test_derived(&font); assert(thread_wrapper);
    source_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    source_continue = CreateEvent(NULL, TRUE, FALSE, NULL);
    draw_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    assert(source_entered && source_continue && draw_entered);
    source_hook = pause_source;
    HANDLE prepare = CreateThread(NULL, 0, prepare_wrapper_thread, NULL, 0, NULL); assert(prepare);
    assert(WaitForSingleObject(source_entered, 5000) == WAIT_OBJECT_0);
    epd_app_font_cache_stats_t before = snapshot();
    HANDLE drawing = CreateThread(NULL, 0, draw_wrapper_thread, NULL, 0, NULL); assert(drawing);
    assert(WaitForSingleObject(draw_entered, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(drawing, 50) == WAIT_TIMEOUT);
    SetEvent(source_continue);
    assert(WaitForSingleObject(prepare, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(drawing, 5000) == WAIT_OBJECT_0);
    epd_app_font_cache_stats_t after = snapshot();
    /* Both wrapper/native lookups observe a cold key before preparation
     * publishes it. The recheck must reuse pixels without a second callback. */
    assert(after.glyph_hits == before.glyph_hits && after.glyph_misses == before.glyph_misses + 2);
    assert(source_calls == 1);
    assert(!after.source_bitmap_calls && callback_lock.max_depth >= 2 && !descriptor_lock.depth);
    assert(!preparing);
    source_hook = NULL;
    CloseHandle(prepare); CloseHandle(drawing); CloseHandle(source_entered);
    CloseHandle(source_continue); CloseHandle(draw_entered);
    cache_test_free(thread_wrapper); ui_reader_font_cache_detach(&font);
}
static DWORD WINAPI draw_warm_thread(void *unused)
{
    (void)unused;
    lv_font_glyph_dsc_t g;
    assert(lv_font_get_glyph_dsc(thread_wrapper, &g, 66, 0));
    const lv_draw_buf_t *buffer = lv_font_get_glyph_bitmap(&g, NULL);
    assert(buffer);
    lv_font_glyph_release_draw_data(&g);
    assert(ui_reader_font_prepare(thread_wrapper, 66, true));
    return 0;
}
static void warm_hits_during_source_io(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    thread_wrapper = &font;
    assert(ui_reader_font_prepare(&font, 66, true));
    source_entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    source_continue = CreateEvent(NULL, TRUE, FALSE, NULL);
    source_hook = pause_source;
    HANDLE prepare = CreateThread(NULL, 0, prepare_wrapper_thread, NULL, 0, NULL); assert(prepare);
    assert(WaitForSingleObject(source_entered, 5000) == WAIT_OBJECT_0);
    epd_app_font_cache_stats_t before = snapshot();
    HANDLE drawing = CreateThread(NULL, 0, draw_warm_thread, NULL, 0, NULL); assert(drawing);
    /* Completion must precede resuming the blocked source callback. */
    assert(WaitForSingleObject(drawing, 1000) == WAIT_OBJECT_0);
    epd_app_font_cache_stats_t after = snapshot();
    assert(after.glyph_hits == before.glyph_hits + 1 && after.glyph_misses == before.glyph_misses);
    SetEvent(source_continue);
    assert(WaitForSingleObject(prepare, 5000) == WAIT_OBJECT_0);
    source_hook = NULL;
    CloseHandle(prepare); CloseHandle(drawing); CloseHandle(source_entered); CloseHandle(source_continue);
    ui_reader_font_cache_detach(&font);
}

#endif
static unsigned pixel_allocations;
static lv_draw_buf_malloc_cb_t real_pixel_alloc;
static void *count_pixel_alloc(size_t size, lv_color_format_t format)
{ ++pixel_allocations; return real_pixel_alloc(size, format); }
static void tiny_ttf_descriptors(void)
{
    FILE *file = fopen("SiFli-SDK/example/lvgl_v8_ttf/disk/test.ttf", "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long bytes = ftell(file); assert(bytes > 0); rewind(file);
    void *data = malloc((size_t)bytes); assert(data);
    assert(fread(data, 1, (size_t)bytes, file) == (size_t)bytes); fclose(file);
    lv_font_t *font = lv_tiny_ttf_create_data_ex(data, (size_t)bytes, 32, LV_FONT_KERNING_NONE, 2);
    assert(font && ui_reader_font_cache_attach(font));
    lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_font_handlers();
    real_pixel_alloc = handlers->buf_malloc_cb; handlers->buf_malloc_cb = count_pixel_alloc;
    lv_font_glyph_dsc_t g;
    for(unsigned pass = 0; pass < 2; ++pass)
        for(unsigned letter = 65; letter < 91; ++letter)
            assert(lv_font_get_glyph_dsc(font, &g, letter, 0));
    assert(pixel_allocations == 0);
    assert(ui_reader_font_prepare(font, 65, true));
    assert(pixel_allocations == 2); /* Tiny TTF raster + retained platform copy. */
    epd_app_font_cache_stats_t before = snapshot();
    assert(lv_font_get_glyph_dsc(font, &g, 65, 0));
    assert(lv_font_get_glyph_bitmap(&g, NULL)); lv_font_glyph_release_draw_data(&g);
    assert(pixel_allocations == 2 && snapshot().glyph_misses == before.glyph_misses);
    ui_reader_font_cache_detach(font); lv_tiny_ttf_destroy(font); free(data);
}
typedef void (*test_fn)(void);
static void bulk_victim_queue(void)
{
    enum { COUNT = 2048, HELD = 256 };
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    lv_cache_entry_t *held[HELD];
    for(unsigned i = 0; i < COUNT; ++i) assert(ui_reader_font_prepare(&font, i + 0x4e00, false));
    for(unsigned i = 0; i < HELD; ++i) {
        glyph_item_t key = {.font = &font, .glyph = i + 0x4e00 + 7};
        held[i] = lv_cache_acquire(glyph_cache, &key, NULL); assert(held[i]);
    }
    eviction_ref_checks = 0;
    for(unsigned i = HELD; i < COUNT; ++i) assert(glyph_evict());
    assert(!glyph_evict() && glyph_count == HELD);
    /* A batch visits each candidate once, even with a held oldest prefix. */
    assert(eviction_ref_checks <= COUNT);
    for(unsigned i = 0; i < HELD; ++i) glyph_release(held[i]);
    eviction_ref_checks = 0;
    for(unsigned i = 0; i < HELD; ++i) assert(glyph_evict());
    assert(!glyph_evict() && eviction_ref_checks == HELD);
    assert(!victim_first[0] && !victim_first[1] && !victim_last[0] && !victim_last[1]);
    ui_reader_font_cache_detach(&font);
}
#include "descriptor_budget.inc"
#include "native_factory.inc"
#include "ttf_work.inc"
static void reserve_remote_without_native(void)
{
    assert(!glyph_cache);
    ui_reader_font_cache_reserve(4096);
    assert(!remote_reclaim_calls);
    ui_reader_font_cache_reserve(SIZE_MAX);
    assert(!remote_reclaim_calls);
}
static void reserve_both_domains(void)
{
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_prepare(&font, 65, true));
    assert(glyph_bytes);
    available = 1; unsigned before = remote_reclaim_calls;
    ui_reader_font_cache_reserve(4096);
    assert(glyph_bytes && remote_reclaim_calls == before);
    available = 64u * 1024u * 1024u;
    ui_reader_font_cache_detach(&font);
}
static const struct { const char *name; test_fn run; } cases[] = {
    {"ttf_transient_pool", ttf_transient_pool},
    {"reserve_remote_without_native", reserve_remote_without_native},
    {"reserve_both_domains", reserve_both_domains},
    {"native_ui_factory_immediate", native_ui_factory_immediate},
    {"descriptor_budget_replacement", descriptor_budget_replacement},
    {"descriptor_root_only_limit", descriptor_root_only_limit},
    {"descriptor_root_shared_reserve", descriptor_root_shared_reserve},
    {"descriptor_root_allocation_failure", descriptor_root_allocation_failure},
#ifdef _WIN32
    {"descriptor_reclaim_waits_callback", descriptor_reclaim_waits_callback},
#endif
    {"bulk_victim_queue", bulk_victim_queue},
    {"prepared_key", prepared_key}, {"detach_other_font", detach_other_font},
    {"sparse_descriptors", sparse_descriptors},
    {"wrapper_prepare", wrapper_prepare}, {"wrapper_failure", wrapper_failure},
    {"speculative_eviction", speculative_eviction}, {"speculative_hit_recency", speculative_hit_recency},
    {"held_reference", held_reference}, {"kerning_and_retry", kerning_and_retry},
    {"descriptor_allocation_retry", descriptor_allocation_retry}, {"tiny_ttf_descriptors", tiny_ttf_descriptors},
    {"fallback_key", fallback_key}, {"distinct_font_keys", distinct_font_keys},
    {"pressure_fallback", pressure_fallback}, {"derived_no_speculative_eviction", derived_no_speculative_eviction},
    {"stats_abi_and_preparation", stats_abi_and_preparation},
    {"tier_order_and_held", tier_order_and_held},
    {"held_invalidation_and_reinsert", held_invalidation_and_reinsert},
    {"sparse_partial_allocation_and_boundaries", sparse_partial_allocation_and_boundaries},
    {"source_copy_pressure", source_copy_pressure},
    {"derived_copy_pressure", derived_copy_pressure},
    {"source_larger_stride_no_implicit_eviction", source_larger_stride_no_implicit_eviction},
    {"wrapper_intermediate_is_not_output", wrapper_intermediate_is_not_output},
#ifdef _WIN32
    {"wrapper_recursive_lock", wrapper_recursive_lock},
    {"warm_hits_during_source_io", warm_hits_during_source_io},
#endif
};
int main(int argc, char **argv)
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    if(argc != 2) return 2;
    if(!strcmp(argv[1], "--list")) {
        for(unsigned i = 0; i < sizeof(cases)/sizeof(*cases); ++i) puts(cases[i].name);
        return 0;
    }
    lv_draw_buf_init_handlers();
    source_cache = lv_cache_create(&lv_cache_class_lru_rb_count, sizeof(source_item_t), 1,
        (lv_cache_ops_t){source_compare, source_create, source_free});
    for(unsigned i = 0; i < sizeof(cases)/sizeof(*cases); ++i) if(!strcmp(argv[1], cases[i].name)) {
        cases[i].run();
        assert(!bindings && !glyph_count && !glyph_bytes && !descriptor_bytes && !glyph_cache);
        assert(!app_allocations && !descriptor_lock.depth && !callback_lock.depth);
        lv_cache_destroy(source_cache, NULL);
        printf("PASS %s\n", cases[i].name); return 0;
    }
    return 2;
}
