#include <stdint.h>
#define main legacy_font_main
#include "../font_cache/host.c"
#undef main

#include "cache_bridge.h"
#include "font_memory_bridge.h"
static unsigned work_allocs, work_frees, work_flushes, cache_allocs;
static void *work_malloc(size_t bytes, lv_color_format_t format)
{ (void)format; ++work_allocs; return malloc(bytes + LV_DRAW_BUF_ALIGN - 1); }
static void work_free(void *memory)
{ ++work_frees; free(memory); }
static void *cache_exhausted(size_t bytes, lv_color_format_t format)
{ (void)bytes; (void)format; ++cache_allocs; return NULL; }
static void work_flush(const lv_draw_buf_t *buffer, const lv_area_t *area)
{ (void)area; assert(buffer && buffer->data); ++work_flushes; }
static void transient_font_work_case(void)
{
    lv_draw_buf_init_handlers();
    lv_draw_buf_handlers_t *work = lv_draw_buf_get_font_handlers();
    lv_draw_buf_handlers_t cached = *work;
    cached.buf_malloc_cb = cache_exhausted;
    work->buf_malloc_cb = work_malloc;
    work->buf_free_cb = work_free;
    work->flush_cache_cb = work_flush;
    cache_handlers = &cached;
    lv_draw_buf_t *buffer = lv_draw_buf_create_ex(work, 23, 31, LV_COLOR_FORMAT_A8, 0);
    assert(buffer);
    assert(!lv_draw_buf_dup_ex(app_memory_font_cache_handlers(), buffer));
    assert(buffer && cache_allocs == 1 && work_allocs == 1);
    memset(buffer->data, 0x5a, buffer->header.stride * buffer->header.h);
    lv_draw_buf_flush_cache(buffer, NULL);
    assert(work_flushes == 1);
    lv_draw_buf_destroy(buffer);
    cache_handlers = NULL;
    assert(work_frees == 1);
    puts("PASS transient_font_work");
}
static void native_provider_case(void)
{
    lv_draw_buf_init_handlers();
    source_cache = lv_cache_create(&lv_cache_class_lru_rb_count, sizeof(source_item_t), 1,
        (lv_cache_ops_t){source_compare, source_create, source_free});
    lv_font_t font = make_font(); assert(ui_reader_font_cache_attach(&font));
    assert(ui_reader_font_prepare(&font, 65, true));
    assert(ui_reader_font_prepare(&font, 66, false));
    lv_font_glyph_dsc_t glyph; assert(lv_font_get_glyph_dsc(&font, &glyph, 65, 0));
    const lv_draw_buf_t *held = lv_font_get_glyph_bitmap(&glyph, NULL); assert(held);
    size_t before = glyph_bytes; unsigned char pixel = held->data[0];
    assert(epd_resource_reclaim(1, SIZE_MAX, 2) > 0);
    assert(epd_resource_process(EPD_RESOURCE_UI, 8) > 0);
    assert(glyph_bytes < before && held->data[0] == pixel && glyph_count == 1);
    assert(!descriptor_first && descriptor_bytes == descriptor_roots);
    lv_font_glyph_release_draw_data(&glyph);
    assert(epd_resource_reclaim(1, 1, 1) == 1);
    assert(epd_resource_process(EPD_RESOURCE_UI, 8) > 0 && !glyph_bytes);
    ui_reader_font_cache_detach(&font);
    assert(!glyph_count && !descriptor_bytes && !descriptor_roots && !victim_bytes && !app_allocations);
    lv_cache_destroy(source_cache, NULL);
    puts("PASS native_font_resource_provider");
}
int main(int argc, char **argv)
{
    manager_setup();
    resource_owner = 0x100; /* A client application calls the resident cache. */
    if (argc == 2 && !strcmp(argv[1], "transient_font_work")) { transient_font_work_case(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "native_font_resource_provider")) { native_provider_case(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--list")) puts("native_font_resource_provider\ntransient_font_work");
    return legacy_font_main(argc, argv);
}
