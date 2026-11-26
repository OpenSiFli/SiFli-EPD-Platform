/* Production reader bridge and asynchronous cache; transport is deterministic. */

const lv_font_t lv_font_montserrat_16 = {.line_height = 16};
const lv_font_t lv_font_montserrat_20 = {.line_height = 20};
const lv_font_t lv_font_montserrat_24 = {.line_height = 24};
const lv_font_t lv_font_montserrat_28 = {.line_height = 28};
const lv_font_t lv_font_montserrat_36 = {.line_height = 36};
bool ui_reader_font_cache_attach(lv_font_t *font) { (void)font; assert(0); return false; }
void ui_reader_font_cache_detach(lv_font_t *font) { (void)font; assert(0); }
void *lv_malloc_zeroed(size_t n) { (void)n; assert(0); return NULL; }
void lv_free(void *p) { (void)p; assert(0); }
/* Reading construction must never enter native TinyTTF/system-font loading. */
bool ui_font_source_init(void) { assert(0); return false; }
unsigned ui_scale_percent_at(unsigned index) { (void)index; return 100; }
bool ui_system_font_cache_attach(lv_font_t *font) { (void)font; assert(0); return false; }
void ui_system_font_cache_detach(lv_font_t *font) { (void)font; assert(0); }
lv_font_t *ui_system_font_cache_style_create(const lv_font_t *font, unsigned flags)
{ (void)font; (void)flags; assert(0); return NULL; }
void ui_system_font_cache_style_destroy(lv_font_t *font) { (void)font; assert(0); }
lv_font_t *lv_tiny_ttf_create_file_ex(const char *path, int32_t size, lv_font_kerning_t kerning, size_t count)
{ (void)path; (void)size; (void)kerning; (void)count; assert(0); return NULL; }
void lv_tiny_ttf_destroy(lv_font_t *font) { (void)font; assert(0); }
void lv_draw_wait_for_finish(void) { assert(0); }
lv_font_t *epd_app_font_oblique(const lv_font_t *font) { return ui_reader_font_remote_oblique(font); }
void epd_app_font_destroy(lv_font_t *font) { ui_reader_font_remote_destroy(font); }

#define fonts system_fonts
#include "../../src/ui/ui_font.c"
#undef fonts
void ui_global_font_boot(void) {}
void ui_global_font_reset(void) {}
const char *ui_global_font_path(void) { return NULL; }
uint32_t ui_global_font_signature(void) { return 0; }
bool ui_global_font_attach(lv_font_t *f,lv_font_t *b) { (void)f;(void)b;return false; }
lv_font_t *ui_global_font_detach(lv_font_t *f) { (void)f;return NULL; }
void ui_reading_font_source_revoke(uint32_t id) { (void)id; }
void ui_reading_font_source_revoke_storage(void) {}
#include "../../modules/common/reader/reader_font.c"

static lv_font_t *chain_create(unsigned id, unsigned size)
{
    uint32_t identity = 0; lv_font_t *fallback = (void *)1;
    lv_font_t *font = ui_font_reading_create(id, size, 0, &identity, &fallback);
    assert(font && identity && !fallback && !font->fallback && !font->line_height);
    return font;
}
static void chain_destroy(lv_font_t *font) { ui_font_reader_destroy(font); idle(); }
static void reader_async_sizes(void)
{
    const unsigned sizes[] = {16, 24, 32, 36, 48};
    uint32_t previous = 0;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        lv_font_t *font = chain_create(1, sizes[i]);
        idle(); assert(!font->line_height);
        assert(!ui_font_reader_faulted(font));
        assert(font->line_height == sizes[i] + 2 && remote(font)->generation != previous);
        previous = remote(font)->generation; chain_destroy(font);
    }
}
static void reader_async_error_cleanup(void)
{
    allow_shared = false;
    for (unsigned id = 0; id < 2; ++id) {
        lv_font_t *font = chain_create(id, 32); idle();
        assert(ui_font_reader_faulted(font) && !font->fallback && !font->line_height);
        assert(ui_reader_font_remote_prepare(font, "A", 1, true) == -1);
        chain_destroy(font);
    }
}
static void reader_revoked_cached_source(void)
{
    lv_font_t *font = chain_create(1, 36); ready_font(font); prepare(font, "A", true);
    epd_app_font_glyph_t held;
    assert(ui_reader_font_remote_acquire(font, 'A', 0, true, &held) == 1);
    const uint8_t *old_pixels = held.pixels;
    lv_font_glyph_dsc_t stale = descriptor(font, 'A');
    LONG before = commands;
    ui_font_reader_revoke(1); idle();
    assert(ui_font_reader_faulted(font) && !font->get_glyph_bitmap(&stale, NULL));
    assert(ui_reader_font_remote_ready(font, 'A', 0, true) == -1 && !font->fallback);
    assert(ui_reader_font_remote_prepare(font, "A", 1, true) == -1);
    assert(commands == before && held.pixels == old_pixels && held.pixels[0] == 0xab);
    InterlockedIncrement(&fixture_generation);
    lv_font_t *replacement = chain_create(1, 36); ready_font(replacement); prepare(replacement, "A", true);
    assert(remote(replacement)->generation != remote(font)->generation);
    assert(ui_reader_font_remote_ready(replacement, 'A', 0, true) == 1);
    chain_destroy(font); ui_reader_font_remote_release(&held); idle(); chain_destroy(replacement);
}
static void reader_remote_oblique(void)
{
    lv_font_t *font = chain_create(0, 24), *italic = document_font_oblique(font); assert(italic);
    ready_font(font); ready_font(italic);
    assert(ui_reader_font_remote_owns(italic) && remote(italic)->primary == remote(font)->primary);
    prepare(italic, "A", true); LONG before = commands;
    lv_font_glyph_dsc_t d = descriptor(italic, 'A'); assert(d.box_w > glyph_side);
    assert(italic->get_glyph_bitmap(&d, NULL)); italic->release_glyph(italic, &d);
    assert(commands == before);
    document_font_free(italic); idle(); chain_destroy(font);
}
