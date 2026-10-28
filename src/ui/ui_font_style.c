#include "ui_font_style.h"
#include "src/draw/lv_draw_buf_private.h"
#include <string.h>

static unsigned bold_width(unsigned flags, int32_t line_height, unsigned width, unsigned height)
{
    if (!(flags & UI_FONT_STYLE_BOLD) || width <= 1 || height <= 1) return 0;
    unsigned pixels = line_height > 0 ? (unsigned)line_height / 32 : 0;
    return pixels ? pixels : 1;
}

static unsigned italic_width(unsigned flags, unsigned width, unsigned height)
{
    return (flags & UI_FONT_STYLE_ITALIC) && width > 1 && height > 1 ? (height - 1) / 4 : 0;
}

void ui_font_style_metrics(lv_font_glyph_dsc_t *glyph, int32_t line_height, unsigned flags)
{
    if (!glyph || !glyph->adv_w || (flags & ~(UI_FONT_STYLE_BOLD | UI_FONT_STYLE_ITALIC))) return;
    unsigned extra = bold_width(flags, line_height, glyph->box_w, glyph->box_h) +
                     italic_width(flags, glyph->box_w, glyph->box_h);
    if (extra > UINT16_MAX - (unsigned)glyph->box_w) return;
    glyph->box_w = (uint16_t)((unsigned)glyph->box_w + extra);
    unsigned advance = (unsigned)glyph->adv_w + extra;
    glyph->adv_w = (uint16_t)(advance > UINT16_MAX ? UINT16_MAX : advance);
    glyph->stride = 0;
}

lv_draw_buf_t *ui_font_style_bitmap(const lv_draw_buf_t *source, int32_t line_height, unsigned flags)
{
    if (!source || source->header.cf != LV_COLOR_FORMAT_A8 ||
        (flags & ~(UI_FONT_STYLE_BOLD | UI_FONT_STYLE_ITALIC))) return NULL;
    unsigned width = source->header.w, height = source->header.h;
    unsigned bold = bold_width(flags, line_height, width, height);
    unsigned shear = italic_width(flags, width, height);
    if (bold + shear > UINT16_MAX - width) return NULL;
    const lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_font_handlers();
    if (!handlers->buf_malloc_cb || !handlers->buf_free_cb) return NULL;
    uint32_t stride = lv_draw_buf_width_to_stride_ex(handlers, width + bold + shear, LV_COLOR_FORMAT_A8);
    if (!width || !height || stride > UINT16_MAX) return NULL;
    /* Optional style allocation must return normally even with LVGL malloc
     * assertions enabled. Preserve the registered font allocator/alignment. */
    lv_draw_buf_t *result = lv_malloc_zeroed(sizeof(*result));
    if (!result) return NULL;
    void *pixels = handlers->buf_malloc_cb(stride * height, LV_COLOR_FORMAT_A8);
    if (!pixels) { lv_free(result); return NULL; }
    result->header.magic = LV_IMAGE_HEADER_MAGIC;
    result->header.w = width + bold + shear;
    result->header.h = height;
    result->header.cf = LV_COLOR_FORMAT_A8;
    result->header.stride = stride;
    result->header.flags = LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_ALLOCATED;
    result->handlers = handlers;
    result->unaligned_data = pixels;
    result->data = lv_draw_buf_align_ex(handlers, pixels, LV_COLOR_FORMAT_A8);
    result->data_size = stride * height;
    memset(result->data, 0, result->data_size);
    for (unsigned y = 0; y < height; ++y)
    {
        const uint8_t *from = source->data + y * source->header.stride;
        uint8_t *to = result->data + y * stride;
        unsigned shift = shear ? (height - 1 - y) / 4 : 0;
        for (unsigned x = 0; x < width; ++x)
            for (unsigned b = 0; b <= bold; ++b)
                if (to[x + shift + b] < from[x]) to[x + shift + b] = from[x];
    }
    lv_draw_buf_flush_cache(result, NULL);
    return result;
}
