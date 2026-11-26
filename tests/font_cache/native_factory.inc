/* Actual UI factory, TinyTTF parser and cache; only byte transport is replaced. */
extern const lv_font_t lv_font_montserrat_16, lv_font_montserrat_20, lv_font_montserrat_24,
    lv_font_montserrat_28, lv_font_montserrat_36;
lv_font_t *lv_tiny_ttf_create_file_ex(const char *, int32_t, lv_font_kerning_t, size_t);
#define preparing system_preparing
#include "../../src/ui/ui_font.c"
#undef preparing
const char *ui_global_font_path(void) { return NULL; }
uint32_t ui_global_font_signature(void) { return 0; }
bool ui_global_font_attach(lv_font_t *f,lv_font_t *b) { (void)f;(void)b;return false; }
lv_font_t *ui_global_font_detach(lv_font_t *f) { (void)f;return NULL; }
void ui_global_font_boot(void) {}
void ui_global_font_reset(void) {}
void ui_reading_font_source_revoke(uint32_t id) { (void)id; }
void ui_reading_font_source_revoke_storage(void) {}
const system_font_header_t *ui_font_resource(void)
{
    static const system_font_header_t header = {.signature = 77};
    return &header;
}
const lv_font_t lv_font_montserrat_16 = {0}, lv_font_montserrat_20 = {0},
    lv_font_montserrat_24 = {0}, lv_font_montserrat_28 = {0}, lv_font_montserrat_36 = {0};
static unsigned native_source_opens;
static void *native_font_data;
static size_t native_font_bytes;
bool ui_font_source_init(void) { ++native_source_opens; return true; }
lv_font_t *lv_tiny_ttf_create_file_ex(const char *path, int32_t size, lv_font_kerning_t kern, size_t count)
{
    assert(!strcmp(path, UI_FONT_SOURCE_PATH));
    return lv_tiny_ttf_create_data_ex(native_font_data, native_font_bytes, size, kern, count);
}
bool ui_reader_font_remote_owns(const lv_font_t *font) { (void)font; return false; }
void ui_reader_font_remote_destroy(lv_font_t *font) { (void)font; assert(0); }
static void native_ui_factory_immediate(void)
{
    FILE *input = fopen("font/MiSans-Normal.ttf", "rb"); assert(input);
    assert(!fseek(input, 0, SEEK_END)); native_font_bytes = (size_t)ftell(input); rewind(input);
    native_font_data = malloc(native_font_bytes); assert(native_font_data);
    assert(fread(native_font_data, 1, native_font_bytes, input) == native_font_bytes); fclose(input);
    const unsigned sizes[] = {32, 44, 48, 86};
    for (unsigned w = 0; w < 3; ++w) for (unsigned i = 0; i < 4; ++i) {
        uint32_t identity = 0; lv_font_t *fallback = (void *)1;
        lv_font_t *font = ui_font_reader_create(0, sizes[i], w, &identity, &fallback);
        assert(font && identity && !fallback && font->line_height > 0);
        lv_font_glyph_dsc_t d = {0};
        assert(lv_font_get_glyph_dsc(font, &d, 0x4e2d, 0) && d.adv_w && d.box_w && d.box_h);
        assert(d.resolved_font == font && !d.is_placeholder);
        const lv_draw_buf_t *bitmap = font->get_glyph_bitmap(&d, NULL);
        assert(bitmap && bitmap->data && bitmap->data_size);
        font->release_glyph(font, &d);
        ui_font_reader_destroy(font); ui_font_reader_destroy(fallback);
    }
    assert(native_source_opens == 12);
    free(native_font_data); native_font_data = NULL;
}

unsigned ui_scale_percent_at(unsigned index) { (void)index; assert(0); return 100; }
bool ui_system_font_cache_attach(lv_font_t *font) { (void)font; assert(0); return false; }
void ui_system_font_cache_detach(lv_font_t *font) { (void)font; assert(0); }
lv_font_t *ui_system_font_cache_style_create(const lv_font_t *font, unsigned flags)
{ (void)font; (void)flags; assert(0); return NULL; }
void ui_system_font_cache_style_destroy(lv_font_t *font) { (void)font; assert(0); }
lv_font_t *ui_reader_font_remote_create(uint32_t id, unsigned size, unsigned weight, uint32_t *identity)
{ (void)id; (void)size; (void)weight; (void)identity; assert(0); return NULL; }
void ui_reader_font_remote_revoke(uint32_t id, bool storage) { (void)id; (void)storage; assert(0); }
int ui_reader_font_remote_status(const lv_font_t *font) { (void)font; assert(0); return -1; }
