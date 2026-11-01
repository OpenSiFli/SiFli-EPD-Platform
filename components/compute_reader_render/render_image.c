#include "epd_reader_render_image.h"
#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"
#ifndef BUILD_DLMODULE
#include <rtm.h>
#endif

static uint32_t gray_palette[256];

static lv_result_t gray_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                             lv_image_header_t *header)
{
    (void)decoder;
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE) return LV_RESULT_INVALID;
    const lv_image_dsc_t *image = dsc->src;
    if (image->header.cf != LV_COLOR_FORMAT_L8 ||
        (image->header.flags & (LV_IMAGE_FLAGS_COMPRESSED | LV_IMAGE_FLAGS_USER_MASK)) ||
        !image->data || !image->header.w || !image->header.h ||
        image->header.stride < image->header.w ||
        image->data_size < (uint32_t)image->header.stride * image->header.h) return LV_RESULT_INVALID;
    *header = image->header;
    return LV_RESULT_OK;
}

static lv_result_t gray_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    (void)decoder;
    lv_draw_buf_t *buf = lv_malloc(sizeof(*buf));
    if (!buf) return LV_RESULT_INVALID;
    if (lv_draw_buf_from_image(buf, dsc->src) != LV_RESULT_OK) {
        lv_free(buf); return LV_RESULT_INVALID;
    }
    dsc->decoded = buf;
    dsc->palette = (const lv_color32_t *)gray_palette;
    dsc->palette_size = 256;
    return LV_RESULT_OK;
}

static void gray_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    (void)decoder;
    lv_free((void *)dsc->decoded);
    dsc->decoded = NULL;
}

bool epd_reader_render_image_init(void)
{
    lv_image_decoder_t *decoder = NULL;
    while ((decoder = lv_image_decoder_get_next(decoder)))
        if (decoder->open_cb == gray_open) return true;
    decoder = lv_image_decoder_create();
    if (!decoder) return false;
    for (uint32_t i = 0; i < 256; ++i) gray_palette[i] = 0xff000000u | i * 0x010101u;
    lv_image_decoder_set_info_cb(decoder, gray_info);
    lv_image_decoder_set_open_cb(decoder, gray_open);
    lv_image_decoder_set_close_cb(decoder, gray_close);
    return true;
}
#ifndef BUILD_DLMODULE
RTM_EXPORT(epd_reader_render_image_init);
#endif
