#include "ui_font.h"
#include "ui_scale.h"
#include "ui_reader_font_cache.h"
#include "ui_font_source.h"
#include "ui_reading_font_source.h"
#include "ui_system_font_cache.h"
#include "epd_memory.h"
#include "reading_font.h"
#include <limits.h>
#include <string.h>

#define DBG_TAG "ui_font"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

extern const uint32_t ui_builtin_font_signature;

static bool builtin_font_prepare(void)
{
    if (ui_font_source_init()) return true;
    LOG_E("builtin font source unavailable, PSRAM free=%u",
          (unsigned)epd_app_psram_available());
    return false;
}

/* A paused page may retain the previous scale's fonts. Each prepared set is
 * immutable and shares the same builtin source for the system lifetime. */
static lv_font_t *font_sets[5][7];
static lv_font_t *style_sets[5][7][3];
static lv_font_t **fonts = font_sets[2];

const void *ui_font_builtin_data(size_t *size)
{
    if (size) *size = 0;
    return NULL;
}

typedef struct
{
    ui_reading_font_source_t *source;
    unsigned weight;
    bool (*descriptor)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void *(*bitmap)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
    void (*release)(const lv_font_t *, lv_font_glyph_dsc_t *);
    bool (*cached_descriptor)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void *(*cached_bitmap)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
} reader_weight_t;

static bool reader_valid(const reader_weight_t *style)
{
    return !style->source || ui_reading_font_source_valid(style->source);
}

/* Called inside the reader cache's serialized TinyTTF callback. The public
 * error API identifies TinyTTF by its original descriptor callback; use a
 * local view so shared font callbacks never need to be changed during draw. */
static void reader_check_error(const lv_font_t *font, const reader_weight_t *style)
{
    if (!style->source) return;
    lv_font_t original = *font;
    original.get_glyph_dsc = style->descriptor;
    lv_tiny_ttf_error_t error = lv_tiny_ttf_get_error(&original);
    /* Memory pressure does not invalidate the file or the user's selection.
     * TinyTTF retries that failure at the next glyph operation boundary. */
    if (error == LV_TINY_TTF_ERROR_IO || error == LV_TINY_TTF_ERROR_INVALID_DATA)
        ui_reading_font_source_fault(style->source);
}

/* These guards sit outside both caches. Cached descriptors cannot resurrect a
 * revoked stream; returning false lets LVGL resolve the builtin fallback. */
static bool reader_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                              uint32_t letter, uint32_t next)
{
    const reader_weight_t *style = font->user_data;
    if (!reader_valid(style)) return false;
    return style->cached_descriptor(font, glyph, letter, next) && reader_valid(style);
}

static const void *reader_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    const lv_font_t *font = glyph->resolved_font;
    const reader_weight_t *style = font->user_data;
    /* A descriptor retained before revocation has no Unicode value with which
     * to resolve a different face. Skip its bitmap until layout runs again. */
    if (!reader_valid(style)) return NULL;
    const void *bitmap = style->cached_bitmap(glyph, scratch);
    if (reader_valid(style)) return bitmap;
    if (glyph->entry && font->release_glyph) font->release_glyph(font, glyph);
    return NULL;
}

static bool weighted_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                                uint32_t letter, uint32_t next)
{
    const reader_weight_t *style = font->user_data;
    bool found = style->descriptor(font, glyph, letter, next);
    reader_check_error(font, style);
    if (!found || !reader_valid(style)) return false;
    /* Reserve the extra ink width in both line measurement and drawing. */
    if (style->weight == 1 && glyph->box_w > 1 && glyph->box_h > 1) ++glyph->adv_w;
    return true;
}

static const void *weighted_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    const reader_weight_t *style = glyph->resolved_font->user_data;
    lv_draw_buf_t *buffer = (lv_draw_buf_t *)style->bitmap(glyph, scratch);
    reader_check_error(glyph->resolved_font, style);
    if (!reader_valid(style))
    {
        if (glyph->entry && style->release) style->release(glyph->resolved_font, glyph);
        return NULL;
    }
    if (!style->weight || !buffer || (buffer->header.flags & LV_IMAGE_FLAGS_USER1) ||
        buffer->header.cf != LV_COLOR_FORMAT_A8) return buffer;
    /* Each reader font owns its Tiny TTF cache. Process each new bitmap once;
     * never repeatedly embolden an already cached glyph. */
    for (uint32_t y = 0; y < buffer->header.h; ++y)
    {
        uint8_t *row = buffer->data + y * buffer->header.stride;
        uint8_t previous = 0;
        for (uint32_t x = 0; x < buffer->header.w; ++x)
        {
            uint8_t current = row[x];
            if (style->weight == 1)
                row[x] = current > previous ? current : previous;
            else
            {
                uint8_t next = x + 1 < buffer->header.w ? row[x + 1] : 0;
                uint8_t edge = previous < next ? previous : next;
                if (edge > current) edge = current;
                /* Half-pixel horizontal erosion preserves one-pixel strokes. */
                row[x] = ((unsigned)current + edge + 1) / 2;
            }
            previous = current;
        }
    }
    buffer->header.flags |= LV_IMAGE_FLAGS_USER1;
    /* Includes stride padding; the registered handler writes back PSRAM. */
    lv_draw_buf_flush_cache(buffer, NULL);
    return buffer;
}

static bool reader_weight_apply(lv_font_t *font, unsigned weight, ui_reading_font_source_t *source)
{
    reader_weight_t *style = lv_malloc_zeroed(sizeof(*style));
    if (!style) return false;
    style->weight = weight;
    style->source = source;
    style->descriptor = font->get_glyph_dsc;
    style->bitmap = font->get_glyph_bitmap;
    style->release = font->release_glyph;
    font->user_data = style;
    font->get_glyph_dsc = weighted_descriptor;
    font->get_glyph_bitmap = weighted_bitmap;
    if (!ui_reader_font_cache_attach(font))
    {
        font->get_glyph_dsc = style->descriptor;
        font->get_glyph_bitmap = style->bitmap;
        font->user_data = NULL;
        lv_free(style);
        return false;
    }
    style->cached_descriptor = font->get_glyph_dsc;
    style->cached_bitmap = font->get_glyph_bitmap;
    font->get_glyph_dsc = reader_descriptor;
    font->get_glyph_bitmap = reader_bitmap;
    return true;
}

void ui_font_reader_destroy(lv_font_t *font)
{
    if (!font) return;
    lv_draw_wait_for_finish();
    reader_weight_t *style = font->get_glyph_bitmap == reader_bitmap ? font->user_data : NULL;
    ui_reader_font_cache_detach(font);
    if (style)
    {
        font->get_glyph_dsc = style->descriptor;
        font->get_glyph_bitmap = style->bitmap;
        font->release_glyph = style->release;
        font->user_data = NULL;
    }
    lv_tiny_ttf_destroy(font);
    if (style)
    {
        ui_reading_font_source_release(style->source);
        lv_free(style);
    }
}

bool ui_font_reader_faulted(const lv_font_t *font)
{
    return font && font->get_glyph_bitmap == reader_bitmap && !reader_valid(font->user_data);
}

void ui_font_reader_revoke(uint32_t id)
{
    if (!id) return;
    lv_draw_wait_for_finish();
    ui_reading_font_source_revoke(id);
}

void ui_font_reader_revoke_storage(void)
{
    lv_draw_wait_for_finish();
    ui_reading_font_source_revoke_storage();
}

bool ui_font_init(void)
{
    return ui_font_prepare_scale(2);
}

bool ui_font_prepare_scale(unsigned index)
{
    static const uint32_t sizes[] = {24, 28, 36, 20, 64, 48, 32};
    static const lv_font_t *fallbacks[] = {
        &lv_font_montserrat_24, &lv_font_montserrat_28, &lv_font_montserrat_36,
        &lv_font_montserrat_20, &lv_font_montserrat_36,
        &lv_font_montserrat_36, &lv_font_montserrat_28
    };
    if (index >= 5) return false;
    lv_font_t **set = font_sets[index];
    if (set[0]) return true;
    if (!builtin_font_prepare()) return false;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
    {
        uint32_t size = (sizes[i] * ui_scale_percent_at(index) + 50) / 100;
        /* Retained glyphs belong to the shared system cache. */
#ifdef UI_BUILTIN_FONT_PATH
        set[i] = lv_tiny_ttf_create_file_ex(UI_BUILTIN_FONT_PATH, size,
                    LV_FONT_KERNING_NONE, 0);
#else
        set[i] = lv_tiny_ttf_create_file_ex(UI_FONT_SOURCE_PATH, size,
                    LV_FONT_KERNING_NONE, 0);
#endif
        lv_font_t *emoji = lv_tiny_ttf_create_file_ex(UI_EMOJI_FONT_PATH, size,
                    LV_FONT_KERNING_NONE, 0);
        if (!set[i] || !emoji || !ui_system_font_cache_attach(set[i]) ||
            !ui_system_font_cache_attach(emoji))
        {
            if (emoji) { ui_system_font_cache_detach(emoji); lv_tiny_ttf_destroy(emoji); }
            if (set[i]) { ui_system_font_cache_detach(set[i]); lv_tiny_ttf_destroy(set[i]); }
            while (i)
            {
                --i;
                lv_font_t *previous_emoji = (lv_font_t *)set[i]->fallback;
                ui_system_font_cache_detach(previous_emoji);
                lv_tiny_ttf_destroy(previous_emoji);
                ui_system_font_cache_detach(set[i]);
                lv_tiny_ttf_destroy(set[i]);
            }
            memset(set, 0, sizeof(font_sets[index]));
            return false;
        }
        emoji->fallback = fallbacks[i];
        set[i]->fallback = emoji;
    }
    return true;
}

void ui_font_select_scale(unsigned index) { if (index < 5 && font_sets[index][0]) fonts = font_sets[index]; }
const lv_font_t *ui_font_at(unsigned index, unsigned role)
{ return index < 5 && role < 7 ? font_sets[index][role] : NULL; }

const lv_font_t *ui_font_small(void) { return fonts[0]; }
const lv_font_t *ui_font_body(void) { return fonts[1]; }
const lv_font_t *ui_font_title(void) { return fonts[2]; }
const lv_font_t *ui_font_caption(void) { return fonts[3]; }
const lv_font_t *ui_font_temperature(void) { return fonts[4]; }
const lv_font_t *ui_font_settings_title(void) { return fonts[5]; }
const lv_font_t *ui_font_settings_body(void) { return fonts[2]; }
const lv_font_t *ui_font_settings_value(void) { return fonts[6]; }

const lv_font_t *ui_font_style(const lv_font_t *normal, unsigned flags)
{
    if (!normal || !flags || (flags & ~(UI_FONT_STYLE_BOLD | UI_FONT_STYLE_ITALIC)))
        return normal;
    for (unsigned scale = 0; scale < 5; ++scale)
        for (unsigned role = 0; role < 7; ++role)
            if (font_sets[scale][role] == normal)
            {
                lv_font_t **slot = &style_sets[scale][role][flags - 1];
                if (!*slot) *slot = ui_system_font_cache_style_create(normal, flags);
                return *slot ? *slot : normal;
            }
    return normal;
}

void ui_font_deinit(void)
{
    lv_draw_wait_for_finish();
    for (unsigned scale = 0; scale < 5; ++scale)
        for (unsigned role = 0; role < 7; ++role)
        {
            for (unsigned style = 0; style < 3; ++style)
            {
                ui_system_font_cache_style_destroy(style_sets[scale][role][style]);
                style_sets[scale][role][style] = NULL;
            }
            lv_font_t *normal = font_sets[scale][role];
            if (!normal) continue;
            lv_font_t *emoji = (lv_font_t *)normal->fallback;
            ui_system_font_cache_detach(emoji);
            lv_tiny_ttf_destroy(emoji);
            ui_system_font_cache_detach(normal);
            lv_tiny_ttf_destroy(normal);
            font_sets[scale][role] = NULL;
        }
    fonts = font_sets[2];
}

lv_font_t *ui_font_reader_create(uint32_t id, unsigned size, unsigned weight, uint32_t *identity,
                                lv_font_t **fallback)
{
    if (identity) *identity = 0;
    if (fallback) *fallback = NULL;
    if (!identity || !fallback || !size || size > INT32_MAX) return NULL;
    lv_font_t *font = NULL;
    ui_reading_font_source_t *source = NULL;
    if (weight > 2) weight = 0;
    uint32_t signature = ui_builtin_font_signature;
    if (id != READING_FONT_BUILTIN)
    {
        source = ui_reading_font_source_acquire(id);
        if (!source) return NULL;
        font = lv_tiny_ttf_create_file_ex(ui_reading_font_source_path(source), size,
                                          LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
        if (!font || lv_tiny_ttf_get_error(font) != LV_TINY_TTF_ERROR_NONE ||
            !ui_reading_font_source_valid(source))
        {
            if (font) lv_tiny_ttf_destroy(font);
            ui_reading_font_source_release(source);
            return NULL;
        }
        signature = ui_reading_font_source_signature(source);
    }
#ifdef UI_BUILTIN_FONT_PATH
    lv_font_t *builtin = lv_tiny_ttf_create_file_ex(UI_BUILTIN_FONT_PATH, size,
                                                  LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
#else
    lv_font_t *builtin = builtin_font_prepare() ?
        lv_tiny_ttf_create_file_ex(UI_FONT_SOURCE_PATH, size,
                    LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT) : NULL;
#endif
    if (!builtin || !reader_weight_apply(builtin, weight, NULL))
    {
        if (font) lv_tiny_ttf_destroy(font);
        if (builtin) lv_tiny_ttf_destroy(builtin);
        ui_reading_font_source_release(source);
        return NULL;
    }
    builtin->fallback = &lv_font_montserrat_16;
    if (font)
    {
        font->fallback = builtin;
        if (!reader_weight_apply(font, weight, source))
        {
            lv_tiny_ttf_destroy(font);
            ui_reading_font_source_release(source);
            ui_font_reader_destroy(builtin);
            return NULL;
        }
        if (ui_font_reader_faulted(font))
        {
            ui_font_reader_destroy(font);
            ui_font_reader_destroy(builtin);
            return NULL;
        }
        *fallback = builtin;
    }
    else font = builtin;
    /* Include all ID bits and both faces' content in pagination identity. */
    uint32_t hash = 2166136261u;
    const uint32_t values[] = {id, signature, ui_builtin_font_signature, size, weight};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        hash = (hash ^ values[i]) * 16777619u;
    *identity = hash;
    return font;
}
