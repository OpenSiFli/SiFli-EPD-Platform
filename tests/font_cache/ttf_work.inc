static unsigned work_live, retained_live;
static bool retained_denied;
static void *ttf_work_malloc(size_t bytes, lv_color_format_t format)
{ (void)format; void *p = malloc(bytes + LV_DRAW_BUF_ALIGN - 1); if (p) ++work_live; return p; }
static void ttf_work_free(void *p)
{ if (p) { assert(work_live); --work_live; free(p); } }
static void *retained_malloc(size_t bytes, lv_color_format_t format)
{
    (void)format;
    if (retained_denied) return NULL;
    void *p = epd_app_alloc(bytes + LV_DRAW_BUF_ALIGN - 1, EPD_APP_PSRAM);
    if (p) ++retained_live;
    return p;
}
static void retained_free(void *p)
{ if (p) { assert(retained_live); --retained_live; epd_app_free(p); } }
static void ttf_transient_pool(void)
{
    FILE *file = fopen("font/MiSans-Normal.ttf", "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); size_t bytes = (size_t)ftell(file); rewind(file);
    void *data = malloc(bytes); assert(data);
    assert(fread(data, 1, bytes, file) == bytes); fclose(file);
    lv_draw_buf_handlers_t *work = lv_draw_buf_get_font_handlers();
    lv_draw_buf_handlers_t saved = *work, retained = saved;
    retained.buf_malloc_cb = retained_malloc; retained.buf_free_cb = retained_free;
    work->buf_malloc_cb = ttf_work_malloc; work->buf_free_cb = ttf_work_free;
    cache_handlers = &retained;
    lv_font_t *font = lv_tiny_ttf_create_data_ex(data, bytes, 36, LV_FONT_KERNING_NONE, 0);
    assert(font && ui_reader_font_cache_attach(font));
    retained_denied = true;
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(font, &glyph, 0x4e2d, 0));
    assert(lv_font_get_glyph_bitmap(&glyph, NULL));
    assert(work_live == 1 && !retained_live);
    lv_font_glyph_release_draw_data(&glyph); assert(!work_live);
    retained_denied = false;
    assert(lv_font_get_glyph_dsc(font, &glyph, 0x4e2d, 0));
    assert(lv_font_get_glyph_bitmap(&glyph, NULL));
    assert(retained_live == 1 && !work_live);
    lv_font_glyph_release_draw_data(&glyph); assert(retained_live == 1);
    assert(lv_font_get_glyph_dsc(font, &glyph, 0x4e2d, 0));
    assert(lv_font_get_glyph_bitmap(&glyph, NULL));
    assert(retained_live == 1 && !work_live);
    lv_font_glyph_release_draw_data(&glyph);
    ui_reader_font_cache_detach(font); assert(!retained_live);
    lv_tiny_ttf_destroy(font); cache_handlers = NULL; *work = saved; free(data);
}
