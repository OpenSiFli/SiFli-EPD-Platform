#include "app_memory.h"
#include "gui_app_context.h"
#include "epd_memory_manager.h"
#include "epd_json.h"
#include "lvgl.h"
#include "src/draw/lv_draw_buf_private.h"
#include <mem_section.h>
#include <rthw.h>
#include <string.h>

static app_memory_load_watch_t *load_watch;

void app_memory_load_begin(app_memory_load_watch_t *watch)
{
    memset(watch, 0, sizeof(*watch));
    watch->thread = rt_thread_self();
    rt_base_t level = rt_hw_interrupt_disable();
    watch->previous = load_watch;
    load_watch = watch;
    rt_hw_interrupt_enable(level);
}

void app_memory_load_end(app_memory_load_watch_t *watch)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (load_watch == watch) load_watch = watch->previous;
    rt_hw_interrupt_enable(level);
}

app_memory_load_watch_t *app_memory_load_current(void)
{
    if (rt_interrupt_get_nest()) return NULL;
    rt_base_t level = rt_hw_interrupt_disable();
    app_memory_load_watch_t *watch = load_watch;
    if (watch && watch->thread != rt_thread_self()) watch = NULL;
    rt_hw_interrupt_enable(level);
    return watch;
}

void app_memory_load_failed(size_t physical)
{
    app_memory_load_watch_t *watch = app_memory_load_current();
    if (watch && physical > watch->contiguous) watch->contiguous = physical;
}

void gui_app_context_start_failed(const char *app_id, int error)
{
    app_memory_load_watch_t *watch = app_memory_load_current();
    if (watch && watch->target && !strcmp(watch->target, app_id) && !watch->start_error)
        watch->start_error = error;
}

L2_RET_BSS_SECT_BEGIN(font_work_handlers)
static lv_draw_buf_handlers_t font_work_handlers L2_RET_BSS_SECT(font_work_handlers);
L2_RET_BSS_SECT_END

static void *font_pixels_alloc(size_t size, lv_color_format_t format)
{
    LV_UNUSED(format);
    return epd_app_alloc(size, EPD_APP_PSRAM);
}

epd_memory_result_t app_memory_init(void)
{
#ifdef RT_USING_MEMHEAP_OWNER
    epd_memory_result_t result = epd_memory_sdk_init();
    if (result != EPD_MEMORY_OK) return result;
#endif
    epd_json_init();
#ifdef TINY_TTF_CACHE_IN_APP_PSRAM
    /* Run after the SDK port initializes handlers and before any fonts exist.
     * Keep alignment and cache writeback handlers unchanged. */
    lv_draw_buf_handlers_t *handlers = lv_draw_buf_get_font_handlers();
    handlers->buf_malloc_cb = font_pixels_alloc;
    handlers->buf_free_cb = epd_app_free;
    /* Short-lived draw work uses the UI heap; cached glyphs use the app heap. */
    font_work_handlers = *handlers;
    font_work_handlers.buf_malloc_cb = lv_draw_buf_get_handlers()->buf_malloc_cb;
    font_work_handlers.buf_free_cb = lv_draw_buf_get_handlers()->buf_free_cb;
    lv_draw_buf_set_font_work_handlers(&font_work_handlers);
#endif
    return EPD_MEMORY_OK;
}
