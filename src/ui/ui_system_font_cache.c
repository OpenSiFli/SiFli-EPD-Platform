#include "platform/epd_render_profile.h"
#include "ui_system_font_cache.h"
#include "ui_font.h"
#include "epd_memory.h"
#include "mem_section.h"
#include "src/misc/cache/lv_cache.h"
#include "src/misc/cache/class/lv_cache_lru_rb.h"
#include "src/misc/lv_rb_private.h"
#include <rtthread.h>
#include <string.h>

typedef struct
{
    lv_font_t *font;
    bool (*descriptor)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void *(*bitmap)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
    void (*release)(const lv_font_t *, lv_font_glyph_dsc_t *);
    const lv_font_t *normal;
    unsigned style;
} font_binding_t;

typedef struct
{
    lv_cache_slot_size_t slot;
    const lv_font_t *font;
    uint32_t id;
    bool pixels;
    union
    {
        lv_font_glyph_dsc_t descriptor;
        lv_draw_buf_t *bitmap;
    } value;
} font_item_t;

/* Five scales, seven roles: normal, Emoji and three lazy style wrappers. */
L2_RET_BSS_SECT_BEGIN(font_bindings)
static font_binding_t bindings[5 * 7 * 5] L2_RET_BSS_SECT(font_bindings);
L2_RET_BSS_SECT_END

/* Retained PSRAM is outside the startup zero table. Initialize on boot,
 * before fonts are created; normal sleep/resume keeps the bindings. */
static int font_bindings_init(void)
{
    memset(bindings, 0, sizeof(bindings));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(font_bindings_init);

#define FONT_BINDING_COUNT (sizeof(bindings) / sizeof(bindings[0]))
static lv_cache_t *cache;
static struct rt_mutex cache_lock;
static bool lock_ready;

/* Reserve the full binding table and worst-case wrapper allocations, in
 * addition to the LRU/mutex overhead, within the same total byte budget. */
#define CACHE_FIXED_BYTES (4096u + sizeof(bindings) + \
    5u * 7u * 3u * (sizeof(lv_font_t) + sizeof(struct rt_memheap_item) + RT_ALIGN_SIZE))
#define APP_PSRAM_RESERVE (1024u * 1024u)

static size_t aligned(size_t bytes)
{
    return RT_ALIGN(bytes, RT_ALIGN_SIZE);
}

static size_t item_charge(void)
{
    /* SDK LRU: tree node, payload/entry/LRU pointer, linked-list node. */
    return aligned(sizeof(lv_rb_node_t)) +
           aligned(lv_cache_entry_get_size(sizeof(font_item_t)) + sizeof(void *)) +
           aligned(3u * sizeof(void *)) + 3u * aligned(sizeof(struct rt_memheap_item));
}

static size_t bitmap_charge(uint32_t bytes)
{
    return item_charge() + aligned(sizeof(lv_draw_buf_t)) + aligned(bytes) +
           2u * aligned(sizeof(struct rt_memheap_item));
}

static lv_cache_compare_res_t item_compare(const font_item_t *a, const font_item_t *b)
{
    if (a->font != b->font) return (uintptr_t)a->font > (uintptr_t)b->font ? 1 : -1;
    if (a->pixels != b->pixels) return a->pixels ? 1 : -1;
    if (a->id != b->id) return a->id > b->id ? 1 : -1;
    return 0;
}

static bool item_create(font_item_t *item, void *source)
{
    if (!item->pixels)
    {
        item->value.descriptor = *(const lv_font_glyph_dsc_t *)source;
        item->value.descriptor.entry = NULL;
        return true;
    }
    item->value.bitmap = ui_font_style_bitmap(source, 0, 0);
    if (!item->value.bitmap) return false;
    lv_draw_buf_flush_cache(item->value.bitmap, NULL);
    return true;
}

static void item_free(font_item_t *item, void *unused)
{
    LV_UNUSED(unused);
    if (item->pixels && item->value.bitmap) lv_draw_buf_destroy(item->value.bitmap);
}

static bool styled_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                              uint32_t letter, uint32_t next)
{
    font_binding_t *binding = font->user_data;
    const lv_font_t *normal = binding->normal;
    /* Do not traverse the fallback here: LVGL resolves missing MiSans glyphs
     * against the unchanged Emoji face in the wrapper's fallback chain. */
    if (!normal->get_glyph_dsc(normal, glyph, letter, next)) return false;
    ui_font_style_metrics(glyph, font->line_height, binding->style);
    return true;
}

static const void *styled_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    font_binding_t *binding = glyph->resolved_font->user_data;
    lv_font_glyph_dsc_t original = *glyph;
    original.resolved_font = binding->normal;
    original.entry = NULL;
    const lv_draw_buf_t *source = binding->normal->get_glyph_bitmap(&original, scratch);
    if (!source) return NULL;
    lv_draw_buf_t *result = ui_font_style_bitmap(source, binding->font->line_height,
                                                glyph->adv_w ? binding->style : 0);
    if (!result)
    {
        /* LVGL has already calculated the wider drawing coordinates. A
         * narrower normal bitmap is not a safe fallback for that rectangle. */
        binding->normal->release_glyph(binding->normal, &original);
        glyph->entry = NULL;
        return NULL;
    }
    binding->normal->release_glyph(binding->normal, &original);
    lv_draw_buf_flush_cache(result, NULL);
    glyph->entry = (lv_cache_entry_t *)result;
    return result;
}

static void styled_release(const lv_font_t *font, lv_font_glyph_dsc_t *glyph)
{
    LV_UNUSED(font);
    lv_draw_buf_destroy((lv_draw_buf_t *)glyph->entry);
    glyph->entry = NULL;
}

static bool cached_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                              uint32_t letter, uint32_t next)
{
    font_binding_t *binding = font->user_data;
    rt_mutex_take(&cache_lock, RT_WAITING_FOREVER);
    font_item_t key = {.slot = {.size = item_charge()}, .font = font, .id = letter};
    /* Pair-dependent metrics must remain with the original font callback. */
    bool reusable = font->kerning == LV_FONT_KERNING_NONE;
    lv_cache_entry_t *entry = reusable ? lv_cache_acquire(cache, &key, NULL) : NULL;
    bool found;
    if (entry)
    {
        *glyph = ((font_item_t *)lv_cache_entry_get_data(entry))->value.descriptor;
        lv_cache_release(cache, entry, NULL);
        found = true;
    }
    else
    {
        uint32_t trace = epd_render_profile_begin(EPD_RENDER_DSC);
        found = binding->descriptor(font, glyph, letter, next);
        epd_render_profile_end(EPD_RENDER_DSC, trace);
        /* TinyTTF's uncached descriptor path reports success for missing glyphs. */
        if (found && !glyph->gid.index && letter >= 0x20 &&
            letter != 0xf8ff && letter != 0x200c) found = false;
        if (found && reusable && !glyph->is_placeholder)
        {
            entry = lv_cache_acquire_or_create(cache, &key, glyph);
            if (entry) lv_cache_release(cache, entry, NULL);
        }
    }
    rt_mutex_release(&cache_lock);
    return found;
}

static const void *cached_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    font_binding_t *binding = glyph->resolved_font->user_data;
    rt_mutex_take(&cache_lock, RT_WAITING_FOREVER);
    font_item_t key = {.font = glyph->resolved_font, .id = glyph->gid.index, .pixels = true};
    lv_cache_entry_t *entry = lv_cache_acquire(cache, &key, NULL);
    if (entry)
    {
        glyph->entry = entry;
        const void *bitmap = ((font_item_t *)lv_cache_entry_get_data(entry))->value.bitmap;
        rt_mutex_release(&cache_lock);
        return bitmap;
    }
    uint32_t trace = epd_render_profile_begin(EPD_RENDER_BITMAP);
    const lv_draw_buf_t *source = binding->bitmap(glyph, scratch);
    epd_render_profile_end(EPD_RENDER_BITMAP, trace);
    if (source)
    {
        key.slot.size = bitmap_charge(source->data_size);
        /* Leave shared PSRAM for applications and the font rasterizer. */
        if (key.slot.size <= UI_SYSTEM_FONT_CACHE_BYTES - CACHE_FIXED_BYTES &&
            epd_app_psram_available() >= source->data_size + APP_PSRAM_RESERVE)
            entry = lv_cache_acquire_or_create(cache, &key, (void *)source);
        if (entry)
        {
            binding->release(binding->font, glyph);
            glyph->entry = entry;
            source = ((font_item_t *)lv_cache_entry_get_data(entry))->value.bitmap;
        }
    }
    rt_mutex_release(&cache_lock);
    return source;
}

static void cached_release(const lv_font_t *font, lv_font_glyph_dsc_t *glyph)
{
    if (!glyph->entry) return;
    rt_mutex_take(&cache_lock, RT_WAITING_FOREVER);
    /* Uncached TinyTTF stores a draw buffer, not a cache entry, in entry. */
    font_item_t key = {.font = font, .id = glyph->gid.index, .pixels = true};
    lv_cache_entry_t *entry = lv_cache_acquire(cache, &key, NULL);
    bool shared = entry && entry == glyph->entry;
    if (entry) lv_cache_release(cache, entry, NULL);
    if (shared)
    {
        lv_cache_release(cache, glyph->entry, NULL);
        glyph->entry = NULL;
    }
    else ((font_binding_t *)font->user_data)->release(font, glyph);
    rt_mutex_release(&cache_lock);
}

bool ui_system_font_cache_attach(lv_font_t *font)
{
    if (!font || font->user_data) return false;
    unsigned index;
    for (index = 0; index < FONT_BINDING_COUNT && bindings[index].font; ++index) {}
    if (index == FONT_BINDING_COUNT) return false;
    if (!lock_ready)
    {
        if (rt_mutex_init(&cache_lock, "ui_glyph", RT_IPC_FLAG_PRIO) != RT_EOK) return false;
        lock_ready = true;
    }
    if (!cache)
    {
        /* Account for the shared cache object and fixed binding table. */
        cache = lv_cache_create(&lv_cache_class_lru_rb_size, sizeof(font_item_t),
            UI_SYSTEM_FONT_CACHE_BYTES - CACHE_FIXED_BYTES, (lv_cache_ops_t){
                .compare_cb = (lv_cache_compare_cb_t)item_compare,
                .create_cb = (lv_cache_create_cb_t)item_create,
                .free_cb = (lv_cache_free_cb_t)item_free});
        if (!cache) return false;
        lv_cache_set_name(cache, "SYSTEM_GLYPHS");
    }
    font_binding_t *binding = &bindings[index];
    binding->font = font;
    binding->descriptor = font->get_glyph_dsc;
    binding->bitmap = font->get_glyph_bitmap;
    binding->release = font->release_glyph;
    font->user_data = binding;
    font->get_glyph_dsc = cached_descriptor;
    font->get_glyph_bitmap = cached_bitmap;
    font->release_glyph = cached_release;
    return true;
}

lv_font_t *ui_system_font_cache_style_create(const lv_font_t *normal, unsigned flags)
{
    if (!normal || normal->get_glyph_bitmap != cached_bitmap || !flags ||
        (flags & ~(UI_FONT_STYLE_BOLD | UI_FONT_STYLE_ITALIC))) return NULL;
    font_binding_t *base = normal->user_data;
    if (base->normal) return NULL;
    lv_font_t *font = lv_malloc(sizeof(*font));
    if (!font) return NULL;
    *font = *normal;
    font->user_data = NULL;
    font->get_glyph_dsc = styled_descriptor;
    font->get_glyph_bitmap = styled_bitmap;
    font->release_glyph = styled_release;
    if (!ui_system_font_cache_attach(font))
    {
        lv_free(font);
        return NULL;
    }
    font_binding_t *binding = font->user_data;
    binding->normal = normal;
    binding->style = flags;
    return font;
}

void ui_system_font_cache_style_destroy(lv_font_t *font)
{
    if (!font || font->get_glyph_bitmap != cached_bitmap ||
        !((font_binding_t *)font->user_data)->normal) return;
    ui_system_font_cache_detach(font);
    lv_free(font);
}

void ui_system_font_cache_detach(lv_font_t *font)
{
    if (!font || font->get_glyph_bitmap != cached_bitmap) return;
    lv_draw_wait_for_finish();
    rt_mutex_take(&cache_lock, RT_WAITING_FOREVER);
    /* Font sets only detach on failed construction or system teardown. */
    lv_cache_drop_all(cache, NULL);
    font_binding_t *binding = font->user_data;
    font->get_glyph_dsc = binding->descriptor;
    font->get_glyph_bitmap = binding->bitmap;
    font->release_glyph = binding->release;
    font->user_data = NULL;
    memset(binding, 0, sizeof(*binding));
    bool live = false;
    for (unsigned i = 0; i < FONT_BINDING_COUNT; ++i) live |= bindings[i].font != NULL;
    if (!live)
    {
        lv_cache_destroy(cache, NULL);
        cache = NULL;
    }
    rt_mutex_release(&cache_lock);
}
