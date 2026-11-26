/* Books menu -> reader geometry -> layout, with the device font engine measuring MiSans. */
#include "ui_settings_internal.h"
#include "epd_reader_layout.h"
#include "compute_reader_font.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define epd_get_str(key) #key
#define EPD_FMT_platform_refresh_count(s, n, v) snprintf(s, n, "%u", v)
#define EPD_FMT_platform_minutes(s, n, v) snprintf(s, n, "%u", v)
typedef struct { char name[32]; } reading_font_info_t;
#define reading_font_get(id, out) false
#define reading_font_selected() 0u
unsigned ui_settings_refresh_count(void) { return 1; }
bool ui_settings_enabled(ui_setting_id_t id) { (void)id; return false; }

static uint8_t disk[128];
static size_t disk_size, cursor;
static unsigned reads, active_writers, path_revision, volume_revision;
static bool available = true, change_during_read;
#define InterlockedIncrement(p) (++*(p))
#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_FLASH 0
#include "settings.inc"

static const unsigned percent[] = {100, 110, 130, 150, 170}, margin[] = {2, 6, 10, 14, 18};
static void check_settings(void)
{
    static const unsigned line_wire[] = {100, 120, 140, 160, 180}, margin_wire[] = {4, 8, 12, 16, 20};
    static const char *const lines[] = {"1.0x", "1.1x", "1.3x", "1.5x", "1.7x"};
    static const char *const margins[] = {"2 px", "6 px", "10 px", "14 px", "18 px"};
    uint8_t record[HEADER_SIZE + PAYLOAD_SIZE], values[UI_SETTING_COUNT];
    default_values(values);
    CHECK(values[UI_SETTING_LINE_SPACING] == 1 && values[UI_SETTING_MARGIN] == 0);
    CHECK(ui_settings_index(UI_SETTING_LINE_SPACING) == 1 && ui_settings_index(UI_SETTING_MARGIN) == 0);
    CHECK(!ui_settings_option(UI_SETTING_LINE_SPACING, 5) && !ui_settings_option(UI_SETTING_MARGIN, 5));
    for (unsigned i = 0; i < 5; ++i) {
        unsigned j = 4 - i;
        values[UI_SETTING_LINE_SPACING] = (uint8_t)i; values[UI_SETTING_MARGIN] = (uint8_t)j;
        CHECK(encode_record(values, record));
        unsigned seen = 0;
        for (unsigned p = HEADER_SIZE; p < sizeof(record); p += 4) {
            if (record[p] == 11 || record[p] == 12) {
                CHECK(record[p + 1] == 0 && record[p + 3] == 0);
                CHECK(record[p + 2] == (record[p] == 11 ? line_wire[i] : margin_wire[j]));
                seen |= record[p] == 11 ? 1u : 2u;
            }
        }
        CHECK(seen == 3 && get_u32(record) == 0x45505332u && get_u32(record + 4) == 4);
        CHECK(sizeof(record) <= sizeof(disk));
        disk_size = sizeof(record); memcpy(disk, record, disk_size);
        memset(selected, 0xff, sizeof(selected));
        CHECK(read_record(selected) == UI_SETTINGS_LOAD_OK);
        CHECK(ui_settings_index(UI_SETTING_LINE_SPACING) == i && ui_settings_index(UI_SETTING_MARGIN) == j);
        CHECK(!strcmp(ui_settings_value(UI_SETTING_LINE_SPACING), lines[i]));
        CHECK(!strcmp(ui_settings_value(UI_SETTING_MARGIN), margins[j]));
        CHECK(ui_reading_line_percent(i) == percent[i] && ui_reading_margin_px(j) == margin[j]);
    }
    puts("PASS settings: factory indices 1/0; five display values and legacy wire tags 11/12 round-trip");
}

static crf_context *font;
static crf_font_identity face;
static crf_status read_at(void *file, const crf_font_identity *id, uint32_t offset, void *dst, size_t n, size_t *got)
{
    (void)id;
    if (fseek(file, (long)offset, SEEK_SET)) return CRF_IO_ERROR;
    *got = fread(dst, 1, n, file); return *got == n ? CRF_OK : CRF_SHORT_READ;
}
static void *allocate(void *user, size_t n) { (void)user; return malloc(n); }
static void deallocate(void *user, void *p) { (void)user; free(p); }
static crf_result measure(unsigned px, uint32_t cp, crf_glyph *g)
{
    crf_result result;
    CHECK(crf_measure_pair(font, &face, (crf_style){.size_px=(uint16_t)px}, cp, 0, g, &result) == CRF_OK);
    CHECK(!g->flags && g->advance_px && result.metrics.line_height > 0);
    return result;
}

/* Font shells initially report the requested size. fonts_ready obtains the actual face metrics. */
typedef struct { unsigned line_height, kerning; } lv_font_t;
typedef struct { int width, top, bottom, height; } reader_geometry_t;
typedef struct { unsigned line_height, base_line; } metrics_t;
typedef struct { const lv_font_t *font; unsigned margin; int content_width, top, bottom, line_space; } reader_view_t;
typedef struct {
    lv_font_t *fonts[3], *fallbacks[3], *oblique[3], *retired[6], *retired_oblique[3];
    unsigned font_id, font_revision, font_identity, failed_font_id, failed_font_revision, layout, intent, font_blocked_intent;
    unsigned request_failure, request_failure_intent, cache, cache_blocked, prefetch_number;
    unsigned settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    unsigned failed_font_settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    bool font_failed, font_metrics_ready, font_memory_blocked, selected;
    reader_geometry_t font_area, failed_font_area;
    metrics_t metrics[3], italic_metrics[3];
    reader_view_t status;
} view_state_t;
static view_state_t owner, *state = &owner;
#define view (*state)
#define VIEW_HEIGHT (view.status.bottom - view.status.top)
#define LV_FONT_KERNING_NONE 0
#define EPD_APP_FONT_NO_MEMORY (-2)
#define READER_ERROR_FONT_MEMORY 1
#define epd_app_reading_font_id() 1u
#define epd_app_reading_font_revision() 1u
#define rt_kprintf(...) ((void)0)
#define reader_page_cache_clear(c) CHECK(0)
#define document_font_free(f) CHECK(0)
#define epd_app_font_destroy(f) CHECK(0)
#define fail(e) CHECK(0)
static const reader_geometry_t portrait = {684, 54, 1168, 1216}, landscape = {1216, 54, 636, 684};
static reader_geometry_t reader_geometry(void) { return portrait; }
static lv_font_t shells[6];
static unsigned shell_count;
static lv_font_t *epd_app_reading_font_create(unsigned id, unsigned px, unsigned weight, uint32_t *identity, lv_font_t **fallback)
{
    (void)id; (void)weight; *identity = px; *fallback = NULL;
    CHECK(shell_count < 6); shells[shell_count].line_height = px; return &shells[shell_count++];
}
static lv_font_t *document_font_oblique(lv_font_t *f)
{ CHECK(shell_count < 6); shells[shell_count] = *f; return &shells[shell_count++]; }
static int epd_app_font_metrics(lv_font_t *f, metrics_t *out)
{
    crf_glyph g;
    crf_metrics m = measure(f->line_height, 'i', &g).metrics;
    *out = (metrics_t){(unsigned)m.line_height, (unsigned)m.base_line}; return 1;
}

static unsigned choices[2][3], groups;
static bool wide_menu;
static bool menu_landscape(void) { return wide_menu; }
static void options(ui_setting_id_t setting, const char *const *labels, const unsigned *indices,
                    unsigned n, int x, int y, int span)
{
    (void)labels; (void)x; (void)y; (void)span;
    CHECK((setting == UI_SETTING_LINE_SPACING || setting == UI_SETTING_MARGIN) && indices && n == 3);
    unsigned which = setting == UI_SETTING_MARGIN;
    CHECK(!(groups & (1u << which))); groups |= 1u << which;
    memcpy(choices[which], indices, sizeof(choices[which]));
}
#define BR(key) #key
#define label(...) ((void)0)
#define disable(...) ((void)0)
#define centered_label(...) ((void)0)
#define row(...) ((void)0)
#include "reader.inc"

/* One unbroken text passage spans multiple 1 KiB layout input records. */
enum { RECORDS = 4 };
static char body[RECORDS * EPD_READER_RECORD_TEXT + 1];
static epd_reader_glyph_t glyphs[sizeof(body)];
static unsigned offsets[RECORDS + 1];
static void prepare_body(unsigned px)
{
    unsigned at = 0;
    /* Narrow letters at the first wrap distinguish even the 16 px width changes. */
    for (unsigned i = 0; i < 100; ++i) body[at++] = i % 2 ? 'i' : 'l';
    for (unsigned r = 0; r < RECORDS; ++r) {
        for (unsigned i = 0; i < 28; ++i) {
            int n = snprintf(body + at, sizeof(body) - at, "第%03u段：春水初生。", r * 28 + i);
            CHECK(n > 0 && (size_t)n < sizeof(body) - at); at += (unsigned)n;
        }
        CHECK(at - offsets[r] <= EPD_READER_RECORD_TEXT); offsets[r + 1] = at;
    }
    for (unsigned i = 0; i < at; ) {
        uint32_t cp; crf_glyph g;
        unsigned n = utf8_letter(body + i, at - i, &cp);
        measure(px, cp, &g);
        glyphs[i] = (epd_reader_glyph_t){cp, g.advance_px, n}; i += n;
    }
}

static unsigned byte_offset(epd_reader_cursor_t c)
{
    CHECK(c.record <= RECORDS);
    if (c.record == RECORDS) CHECK(!c.byte);
    else CHECK(c.byte < offsets[c.record + 1] - offsets[c.record]);
    return offsets[c.record] + c.byte;
}

static void check_area(reader_geometry_t area, unsigned spacing, unsigned edge)
{
    unsigned h = view.metrics[0].line_height;
    CHECK(view.font_metrics_ready && view.status.line_space == (int)(h * percent[spacing] / 100 - h));
    CHECK(view.font_area.width == area.width && view.font_area.height == area.height);
    CHECK(view.status.top == area.top && view.status.bottom == area.bottom);
    CHECK(view.status.margin == margin[edge] && view.status.content_width == area.width - 2 * (int)margin[edge]);
}

static void prepare_layout(unsigned spacing, unsigned edge)
{
    static const unsigned sizes[] = {UI_READING_FONT_SIZES};
    unsigned px = sizes[ui_settings_index(UI_SETTING_FONT_SIZE)];
    selected[UI_SETTING_LINE_SPACING] = (uint8_t)spacing; selected[UI_SETTING_MARGIN] = (uint8_t)edge;
    memset(&owner, 0, sizeof(owner)); shell_count = 0;
    CHECK(prepare_fonts(state));
    CHECK(view.status.line_space == (int)(px * (percent[spacing] - 100) / 100));
    CHECK(fonts_ready() && view.metrics[0].line_height != px);
    check_area(portrait, spacing, edge);
}

static void setup_batch(epd_reader_batch_t *b)
{
    memset(b, 0, sizeof(*b)); b->width = view.status.content_width; b->height = VIEW_HEIGHT;
    b->base_height = b->font_height = view.metrics[0].line_height;
    b->font_size = view.fonts[0]->line_height; b->line_space = view.status.line_space; b->limit = EPD_READER_BATCH;
}

static void layout_page(epd_reader_batch_t *b, epd_reader_cursor_t start)
{
    unsigned consumed = byte_offset(start);
    memset(&b->page, 0, sizeof(b->page)); b->page.start = b->page.cursor = start; b->page.collect = 1;
    while (!b->page.done) {
        unsigned r = b->page.cursor.record;
        CHECK(r <= RECORDS);
        b->kind = r < RECORDS ? EPD_READER_TEXT_RECORD : EPD_READER_EOF;
        b->length = r < RECORDS ? offsets[r + 1] - offsets[r] : 0;
        b->next_record = r + 1;
        memcpy(b->data, body + offsets[r], b->length); b->data[b->length] = '\0';
        memcpy(b->glyphs, glyphs + offsets[r], b->length * sizeof(*glyphs));
        unsigned before = byte_offset(b->page.cursor);
        CHECK(epd_reader_layout(b) == 0 && b->events);
        CHECK(b->page.done || byte_offset(b->page.cursor) > before);
    }
    unsigned h = b->font_height, step = h + b->line_space, rows = 1 + (b->height - h) / step;
    CHECK(byte_offset(b->page.start) == consumed && b->page.count && b->page.count <= rows);
    CHECK(b->page.eof || b->page.count == rows);
    for (unsigned i = 0; i < b->page.count; ++i) {
        const epd_reader_run_t *run = &b->page.runs[i];
        CHECK(run->x == 0 && run->y == i * step && run->height == h);
        CHECK(view.status.top + run->y + run->height <= view.status.bottom);
        CHECK(run->bytes && run->text + run->bytes <= b->page.bytes);
        unsigned end = consumed + run->bytes, width = 0;
        CHECK(end <= offsets[RECORDS] && !memcmp(b->page.text + run->text, body + consumed, run->bytes));
        while (consumed < end) {
            epd_reader_glyph_t g = glyphs[consumed];
            CHECK(g.bytes && g.bytes <= end - consumed);
            width += g.width; consumed += g.bytes;
        }
        CHECK(run->width == width && view.status.margin + run->x + width <= (unsigned)view.font_area.width - view.status.margin);
        CHECK(end == offsets[RECORDS] || width + glyphs[end].width > b->width);
    }
    CHECK(byte_offset(b->page.cursor) == consumed);
}

static unsigned paginate(unsigned spacing, unsigned edge)
{
    static epd_reader_batch_t b;
    prepare_layout(spacing, edge);
    setup_batch(&b);
    CHECK(b.height == 1114);
    unsigned pages = 0, first_wrap = 0;
    do {
        layout_page(&b, b.page.cursor);
        if (!pages) first_wrap = b.page.runs[0].bytes;
        ++pages;
    } while (!b.page.eof);
    unsigned consumed = byte_offset(b.page.cursor), step = b.font_height + b.line_space;
    CHECK(pages >= 2 && consumed == offsets[RECORDS]);
    printf("PASS spacing=%u margin=%u: line_height=%u step=%u width=%u first_wrap=%u bytes rows/full_page=%u pages=%u text=%u bytes\n",
           spacing, edge, b.font_height, step, b.width, first_wrap, 1 + (b.height - b.font_height) / step, pages, consumed);
    return first_wrap;
}

static void check_rotation(unsigned spacing, unsigned edge)
{
    static epd_reader_batch_t b;
    prepare_layout(spacing, edge);
    lv_font_t *normal[3], *italic[3];
    memcpy(normal, view.fonts, sizeof(normal)); memcpy(italic, view.oblique, sizeof(italic));
    unsigned size = normal[0]->line_height, identity = view.font_identity, height = view.metrics[0].line_height;
    unsigned tall_key = view.layout;
    setup_batch(&b);
    CHECK(b.height == 1114);
    layout_page(&b, (epd_reader_cursor_t){0, 0});
    epd_reader_cursor_t anchor = b.page.cursor;
    CHECK(!b.page.eof && byte_offset(anchor) > 0);
    layout_page(&b, anchor);
    unsigned tall_wrap = b.page.runs[0].bytes, tall_rows = b.page.count, tall_end = byte_offset(b.page.cursor);

    static const reader_geometry_t *const areas[] = {&landscape, &portrait};
    static const unsigned heights[] = {582, 1114};
    unsigned wide_key = 0;
    for (unsigned i = 0; i < 2; ++i) {
        reader_geometry_t area = *areas[i];
        layout_for_area(state, area);
        CHECK(fonts_ready());
        check_area(area, spacing, edge);
        CHECK(shell_count == 6 && view.font_identity == identity && view.status.font == normal[0]);
        for (unsigned j = 0; j < 3; ++j) CHECK(view.fonts[j] == normal[j] && view.oblique[j] == italic[j]);
        CHECK(view.fonts[0]->line_height == size && view.metrics[0].line_height == height);
        setup_batch(&b);
        CHECK(b.height == heights[i]);
        layout_page(&b, anchor);
        if (!i) {
            wide_key = view.layout;
            CHECK(wide_key != tall_key && b.page.runs[0].bytes > tall_wrap && b.page.count < tall_rows);
        } else {
            CHECK(view.layout == tall_key && b.page.runs[0].bytes == tall_wrap && b.page.count == tall_rows);
            CHECK(byte_offset(b.page.cursor) == tall_end);
        }
    }
    while (!b.page.eof) layout_page(&b, b.page.cursor);
    CHECK(byte_offset(b.page.cursor) == offsets[RECORDS]);
    printf("PASS rotate 684x1114 -> 1216x582 -> 684x1114: font=%u px anchor=%u:%u keys=%08x/%08x/%08x\n",
           size, anchor.record, anchor.byte, tall_key, wide_key, view.layout);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 && VIEW_LAYOUT_VERSION == EPD_READER_LAYOUT_CACHE_VERSION && VIEW_LAYOUT_VERSION >= 8);
    check_settings();
    static const unsigned menu_indices[2][3] = {{1, 3, 4}, {0, 2, 4}};
    for (unsigned i = 0; i < 2; ++i) {
        wide_menu = i != 0; groups = 0;
        build_type();
        CHECK(groups == 3 && !memcmp(choices, menu_indices, sizeof(choices)));
    }
    FILE *file = fopen(argv[1], "rb"); CHECK(file && !fseek(file, 0, SEEK_END));
    long bytes = ftell(file); CHECK(bytes > 0 && bytes <= CRF_MAX_FILE_BYTES); face.file_size = (uint32_t)bytes;
    crf_config config; crf_result opened;
    crf_config_init(&config); config.read_user = file; config.read_at = read_at; config.alloc = allocate; config.free = deallocate;
    CHECK(crf_create(&config, &font) == CRF_OK && crf_open(font, &face, &opened) == CRF_OK);
    static const unsigned sizes[] = {UI_READING_FONT_SIZES};
    unsigned px = sizes[ui_settings_index(UI_SETTING_FONT_SIZE)];
    crf_glyph g;
    unsigned h = (unsigned)measure(px, 'i', &g).metrics.line_height;
    for (unsigned i = 0; i < 5; ++i) CHECK(ui_reading_line_space(h, i) == h * percent[i] / 100 - h);
    prepare_body(px);
    for (unsigned s = 0; s < 3; ++s) {
        unsigned previous = UINT_MAX;
        for (unsigned m = 0; m < 3; ++m) {
            unsigned wrap = paginate(choices[0][s], choices[1][m]);
            CHECK(wrap < previous); previous = wrap;
        }
    }
    check_rotation(choices[0][0], choices[1][2]);
    crf_destroy(font); fclose(file);
    printf("PASS Books typography: 3 x 3 menu combinations, layout cache v%u\n", EPD_READER_LAYOUT_CACHE_VERSION);
    return 0;
}
