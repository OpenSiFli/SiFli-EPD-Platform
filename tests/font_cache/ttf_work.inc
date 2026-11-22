static unsigned work_live, retained_live;
static bool retained_denied;
static void *ttf_work_malloc(size_t bytes, lv_color_format_t format)
{ (void)format; void *p = malloc(bytes + LV_DRAW_BUF_ALIGN - 1); if (p) ++work_live; return p; }
static void ttf_work_free(void *p)
{ if (p) { assert(work_live); --work_live; free(p); } }
static void *retained_malloc(size_t bytes, lv_color_format_t format)
{ (void)format; if (retained_denied) return NULL; void *p = malloc(bytes + LV_DRAW_BUF_ALIGN - 1); if (p) ++retained_live; return p; }
static void retained_free(void *p)
{ if (p) { assert(retained_live); --retained_live; free(p); } }
static void ttf_transient_pool(void)
{
    FILE *file = fopen("font/MiSans-Normal.ttf", "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); size_t bytes = (size_t)ftell(file); rewind(file);
    void *data = malloc(bytes); assert(data);
    assert(fread(data, 1, bytes, file) == bytes); fclose(file);
    lv_draw_buf_handlers_t *retained = lv_draw_buf_get_font_handlers();
    lv_draw_buf_handlers_t saved = *retained, work = saved;
    retained->buf_malloc_cb = retained_malloc; retained->buf_free_cb = retained_free;
    work.buf_malloc_cb = ttf_work_malloc; work.buf_free_cb = ttf_work_free;
    lv_draw_buf_set_font_work_handlers(&work);
    lv_font_t *uncached = lv_tiny_ttf_create_data_ex(data, bytes, 36, LV_FONT_KERNING_NONE, 0);
    lv_font_t *cached = lv_tiny_ttf_create_data_ex(data, bytes, 36, LV_FONT_KERNING_NONE, 8);
    assert(uncached && cached);
    retained_denied = true;
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(uncached, &glyph, 0x4e2d, 0));
    assert(lv_font_get_glyph_bitmap(&glyph, NULL));
    assert(work_live == 1 && !retained_live);
    lv_font_glyph_release_draw_data(&glyph); assert(!work_live);
    retained_denied = false;
    assert(lv_font_get_glyph_dsc(cached, &glyph, 0x4e2d, 0));
    assert(lv_font_get_glyph_bitmap(&glyph, NULL));
    assert(retained_live == 1 && !work_live);
    lv_font_glyph_release_draw_data(&glyph); assert(retained_live == 1);
    lv_tiny_ttf_destroy(cached); assert(!retained_live);
    lv_draw_buf_set_font_work_handlers(NULL);
    assert(lv_font_get_glyph_dsc(uncached, &glyph, 0x56fd, 0));
    assert(lv_font_get_glyph_bitmap(&glyph, NULL));
    assert(retained_live == 1 && !work_live);
    lv_font_glyph_release_draw_data(&glyph); assert(!retained_live);
    lv_tiny_ttf_destroy(uncached); *retained = saved; free(data);
}
