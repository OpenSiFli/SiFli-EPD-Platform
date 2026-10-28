#include "app_memory.h"
#include "lvgl.h"
#include "src/draw/lv_draw_buf_private.h"

static void *font_pixels_alloc(size_t size, lv_color_format_t format)
{
    LV_UNUSED(format);
    return epd_app_alloc(size, EPD_APP_PSRAM);
}

void app_memory_init(void)
{
#ifdef TINY_TTF_CACHE_IN_APP_PSRAM
    /* Run after the SDK port initializes handlers and before any fonts exist.
     * Keep alignment and cache writeback handlers unchanged. */
    lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_font_handlers();
    handlers->buf_malloc_cb = font_pixels_alloc;
    handlers->buf_free_cb = epd_app_free;
#endif
}
