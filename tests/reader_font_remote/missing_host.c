/* The real HCPU cache, pipe endpoints, FTZ provider and ACPU engine run here. */
#include "epd_compute_pipe.h"
static void missing_probe(void *, epd_compute_pipe_message_t *, uint8_t *);
#define READER_FONT_SERVICE_PROBE missing_probe
#define main original_host_main
#include "host.c"
#undef main

/* Host adapters for the actual LVGL font wrapper translation unit. */
void lv_memset(void *dst, uint8_t value, size_t bytes) { memset(dst, value, bytes); }
int lv_strcmp(const char *a, const char *b) { return strcmp(a, b); }
_Static_assert(LV_USE_FONT_PLACEHOLDER == 1, "placeholder wrapper configuration");

static unsigned probe_mode;
static unsigned missing_records, primary_records, external_records;
static void missing_probe(void *user, epd_compute_pipe_message_t *m, uint8_t *data)
{
    batch_t *b = user;
    if (m->operation != EPD_PIPE_OUTPUT || !b->count || m->length < sizeof(epd_font_output_t)) return;
    epd_font_output_t *h = (epd_font_output_t *)data;
    crf_glyph *records = (crf_glyph *)(data + sizeof(*h));
    assert(h->completed <= b->count);
    for (unsigned i = 0; i < h->completed; ++i) {
        missing_records += !!(records[i].flags & CRF_GLYPH_MISSING);
        if (b->face->identity.source_id) ++external_records; else ++primary_records;
    }
    if (probe_mode == 1 && b->render && !b->face->identity.source_id) {
        probe_mode = 0; SetEvent(io_entered);
        assert(WaitForSingleObject(io_continue, 5000) == WAIT_OBJECT_0);
    } else if (probe_mode == 2) {
        probe_mode = 0; ++h->request_id;
    } else if (probe_mode == 3 && h->completed && (records[0].flags & CRF_GLYPH_MISSING)) {
        probe_mode = 0; records[0].flags |= CRF_GLYPH_CONTROL;
    }
}

static void clean(void)
{
    idle(); real_no_buffers();
    assert(!fonts && !sources && !resident_sources && !font_count && !leases && !open_fds);
    assert(!allocations && !descriptor_bytes && !pixel_bytes && !diagnostics.pins);
}
static void request(lv_font_t *f, uint32_t cp, bool pixels)
{
    assert(ui_reader_font_remote_ready(f, cp, 0, pixels) >= 0); idle();
    assert(ui_reader_font_remote_ready(f, cp, 0, pixels) == 1);
}
static void same_metrics(const epd_app_font_glyph_t *a, const epd_app_font_glyph_t *b)
{
    assert(a->adv_w == b->adv_w && a->box_w == b->box_w && a->box_h == b->box_h);
    assert(a->stride == b->stride && a->ofs_x == b->ofs_x && a->ofs_y == b->ofs_y);
}
static void check_square(lv_font_t *f, uint32_t cp)
{
    epd_app_font_glyph_t m = {0}, r = {0};
    assert(ui_reader_font_remote_purpose(f, EPD_COMPUTE_BACKGROUND) == 1);
    request(f, cp, false);
    glyph_t *g = lookup(remote(f), cp);
    assert(g && g->descriptor && !g->pixels && !g->error && g->dsc.is_placeholder);
    assert(ui_reader_font_remote_acquire(f, cp, 0, false, &m) == 1 && !m.pixels);
    request(f, cp, true);
    assert(ui_reader_font_remote_acquire(f, cp, 0, true, &r) == 1 && r.pixels);
    same_metrics(&m, &r);
    assert(r.box_w == r.box_h && r.box_w && r.adv_w > r.box_w);
    int top = remote(f)->metrics.line_height - remote(f)->metrics.base_line - (int)r.box_h - r.ofs_y;
    assert(top >= 0 && top + (int)r.box_h <= remote(f)->metrics.line_height);
    if (r.box_w >= 3u)
        assert(r.pixels[(r.box_h / 2) * r.stride + r.box_w / 2] == 0 && "missing square interior");
    unsigned edge = remote(f)->style.size_px / 16u; if (!edge) edge = 1;
    if (r.box_w >= 3u && edge > (r.box_w - 1u) / 2u) edge = (r.box_w - 1u) / 2u;
    for (unsigned y = 0; y < r.box_h; ++y)
        for (unsigned x = 0; x < r.stride; ++x) {
            bool border = x < r.box_w && (x < edge || y < edge || x + edge >= r.box_w || y + edge >= r.box_h);
            assert(r.pixels[y * r.stride + x] == (border ? 255 : 0));
        }
    lv_font_glyph_dsc_t d = descriptor(f, cp);
    const lv_draw_buf_t *buf = f->get_glyph_bitmap(&d, NULL);
    assert(buf && buf->data == r.pixels && d.is_placeholder);
    f->release_glyph(f, &d);
    ui_reader_font_remote_release(&m); ui_reader_font_remote_release(&r);
}
static void two_levels(void)
{
    lv_font_t *primary = create(0), *style = create(1);
    ready_font(primary); ready_font(style);
    assert(ui_reader_font_remote_purpose(style, EPD_COMPUTE_BACKGROUND) == 1);
    request(style, 'A', false);
    glyph_t *g = lookup(remote(style), 'A');
    assert(g->fallback && g->descriptor && !g->pixels && !g->dsc.is_placeholder);
    epd_app_font_glyph_t m, a, b;
    assert(ui_reader_font_remote_acquire(style, 'A', 0, false, &m) == 1);
    request(style, 'A', true); request(primary, 'A', true);
    assert(ui_reader_font_remote_acquire(style, 'A', 0, true, &a) == 1);
    assert(ui_reader_font_remote_acquire(primary, 'A', 0, true, &b) == 1);
    same_metrics(&m, &a); same_metrics(&a, &b);
    assert(a.pixels && !memcmp(a.pixels, b.pixels, a.stride * a.box_h));
    ui_reader_font_remote_release(&m); ui_reader_font_remote_release(&a); ui_reader_font_remote_release(&b);
    check_square(style, 0x5101);
    assert(lookup(remote(style), 0x5101)->fallback);
    assert(missing_records >= 3 && primary_records && external_records);
    uint64_t batches = batch_totals[0].count + batch_totals[1].count;
    LONG reads = raw_reads, packs = pack_reads, allocs = allocations;
    for (unsigned i = 0; i < 100; ++i) {
        assert(ui_reader_font_remote_prepare(style, "AB\xe5\x84\x81", 5, true) >= 0); idle();
        if (!i) { batches = batch_totals[0].count + batch_totals[1].count; reads = raw_reads; packs = pack_reads; allocs = allocations; }
    }
    assert(batches == batch_totals[0].count + batch_totals[1].count && reads == raw_reads && packs == pack_reads && allocs == allocations);
    destroy(style); destroy(primary); clean();
}
static void primary_square(void)
{
    for (unsigned weight = 0; weight < 3; ++weight) {
        uint32_t key; lv_font_t *f = ui_reader_font_remote_create(0, 36, weight, &key); assert(f); ready_font(f);
        lv_font_t *italic = ui_reader_font_remote_oblique(f); assert(italic); ready_font(italic);
        check_square(f, 0x5101); check_square(italic, 0x5101);
        assert(!lookup(remote(f), 0x5101)->fallback);
        request(f, ' ', true); request(f, '\n', true);
        assert(!lookup(remote(f), ' ')->dsc.is_placeholder && !lookup(remote(f), '\n')->dsc.is_placeholder);
        destroy(italic); destroy(f); clean();
    }
}
static void reclaim_square(void)
{
    lv_font_t *f = create(0); ready_font(f); check_square(f, 0x5101);
    glyph_t *g = lookup(remote(f), 0x5101); size_t bytes = g->bitmap.data_size;
    uint8_t *copy = malloc(bytes); assert(copy); memcpy(copy, g->bitmap.data, bytes);
    lv_font_glyph_dsc_t d = g->dsc;
    size_t charge = g->charge;
    assert(provider_reclaim_target(epd_app_psram_available() + 1) == charge);
    assert(g->descriptor && !g->pixels && !g->busy && !g->wanted);
    request(f, 0x5101, true);
    assert(!memcmp(copy, g->bitmap.data, bytes) && !memcmp(&d, &g->dsc, sizeof(d)));
    free(copy); destroy(f); clean();
}
static void pins_close_reopen(void)
{
    lv_font_t *old = create(1); ready_font(old); check_square(old, 0x5101);
    epd_app_font_page_t p = ui_reader_font_remote_page_create(), q = ui_reader_font_remote_page_create(); assert(p && q);
    assert(ui_reader_font_remote_page_pin(p, old, 0x5101, 0, true) == 1);
    assert(ui_reader_font_remote_page_pin(q, old, 0x5101, 0, true) == 1);
    epd_app_font_glyph_t held; assert(ui_reader_font_remote_acquire(old, 0x5101, 0, true, &held) == 1);
    glyph_t *g = held.token; uint32_t ticket = g->ticket;
    uint8_t first = held.pixels[0];
    assert(g->pins == 2 && g->draws == 1);
    size_t charge = g->charge, total_pixels = pixel_bytes;
    (void)provider_reclaim_target(SIZE_MAX);
    assert(lookup(remote(old), 0x5101) == g && g->ticket == ticket && g->charge == charge);
    assert(pixel_bytes == total_pixels && g->bitmap.data == held.pixels && held.pixels[0] == first);
    assert(g->pins == 2 && g->draws == 1);
    ui_reader_font_remote_cancel(old); ui_reader_font_remote_destroy(old); idle();
    assert(g->pins == 2 && g->draws == 1 && held.pixels[0] == first);
    ++fixture_generation;
    lv_font_t *fresh = create(1); ready_font(fresh); check_square(fresh, 0x5101);
    assert(lookup(remote(fresh), 0x5101)->ticket != ticket && ui_reader_font_remote_ready(old, 0x5101, 0, true) == -1);
    ui_reader_font_remote_page_release(p); idle(); assert(g->pins == 1 && g->draws == 1);
    ui_reader_font_remote_page_release(q); idle(); assert(!g->pins && g->draws == 1 && held.pixels[0] == first);
    ui_reader_font_remote_release(&held); idle(); destroy(fresh); clean();
}
static void hold_square(lv_font_t *f)
{
    ResetEvent(io_entered); ResetEvent(io_continue); probe_mode = 1;
    assert(ui_reader_font_remote_ready(f, 0x5101, 0, true) == 0);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
}
static void square_oom(void)
{
    lv_font_t *f = create(0); ready_font(f);
    assert(ui_reader_font_remote_purpose(f, EPD_COMPUTE_BACKGROUND) == 1);
    request(f, 0x5101, false); hold_square(f);
    LONG64 capacity = InterlockedExchange64(&available_bytes, 0);
    SetEvent(io_continue); idle();
    glyph_t *g = lookup(remote(f), 0x5101);
    assert(g->descriptor && g->dsc.is_placeholder && !g->pixels && !g->error && !g->allocation);
    epd_app_font_glyph_t absent;
    assert(ui_reader_font_remote_acquire(f, 0x5101, 0, true, &absent) != 1 && !absent.token);
    idle(); InterlockedExchangeAdd64(&available_bytes, capacity);
    fake_ticks += RT_TICK_PER_SECOND; ui_reader_font_remote_retry_memory(f); request(f, 0x5101, true);
    destroy(f); clean();
}
static void cancel_square(void)
{
    lv_font_t *f = create(1); ready_font(f); hold_square(f);
    ui_reader_font_remote_cancel(f); ui_reader_font_remote_destroy(f); SetEvent(io_continue); idle(); clean();
    f = create(1); ready_font(f); check_square(f, 0x5101); destroy(f); clean();
}
static void engine_oom(void)
{
    lv_font_t *f = create(0); ready_font(f);
    assert(ui_reader_font_remote_purpose(f, EPD_COMPUTE_BACKGROUND) == 1); request(f, 'A', false);
    real_allocation_fault(true); assert(ui_reader_font_remote_ready(f, 'A', 0, true) == 0); idle();
    glyph_t *g = lookup(remote(f), 'A');
    assert(g->descriptor && !g->pixels && !g->dsc.is_placeholder && !g->fallback && g->remote_memory);
    real_allocation_fault(false); fake_ticks += RT_TICK_PER_SECOND; ui_reader_font_remote_retry_memory(f); request(f, 'A', true);
    assert(!g->dsc.is_placeholder); destroy(f); clean();
}
static void errors(const char *name)
{
    unsigned char saved[4]; memcpy(saved, real_font_bytes, sizeof(saved));
    if (!strcmp(name, "io")) source_fault = 4;
    if (!strcmp(name, "short")) source_fault = 5;
    if (!strcmp(name, "corrupt")) memset(real_font_bytes, 0, 4);
    lv_font_t *f = create(1); idle();
    if (source_fault || !strcmp(name, "corrupt")) assert(ui_reader_font_remote_status(f) < 0);
    else {
        ready_font(f); probe_mode = !strcmp(name, "transport") ? 2 : 3;
        assert(ui_reader_font_remote_ready(f, 0x5101, 0, true) == 0); idle();
        assert(ui_reader_font_remote_ready(f, 0x5101, 0, true) < 0);
        glyph_t *g = lookup(remote(f), 0x5101);
        assert(!g || (!g->descriptor && !g->pixels));
    }
    assert(!primary_records); source_fault = 0; memcpy(real_font_bytes, saved, sizeof(saved));
    destroy(f); clean();
}

static glyph_t *wrapper_pinned;
static uint64_t wrapper_hits;
static unsigned wrapper_reclaims;
static void wrapper_between_queries(void)
{
    cache_unlock_probe = NULL;
    assert(diagnostics.descriptor_hits == wrapper_hits + 1);
    assert(wrapper_pinned->pins == 1 && !wrapper_pinned->draws);
    const void *allocation = wrapper_pinned->allocation;
    uint32_t ticket = wrapper_pinned->ticket;
    size_t charge = wrapper_pinned->charge;
    (void)provider_reclaim_target(SIZE_MAX);
    assert(wrapper_pinned->allocation == allocation && wrapper_pinned->ticket == ticket);
    assert(wrapper_pinned->charge == charge && wrapper_pinned->pixels && wrapper_pinned->pins == 1);
    ++wrapper_reclaims;
}
typedef struct { lv_font_t *font; lv_font_glyph_dsc_t *dsc; } wrapper_lookup_args;
static DWORD WINAPI wrapper_lookup(void *value)
{
    wrapper_lookup_args *args = value;
    assert(lv_font_get_glyph_dsc(args->font, args->dsc, 0x5101, 0)); return 0;
}
static void lvgl_wrapper(void)
{
    for (unsigned id = 0; id < 2; ++id) {
        lv_font_t *f = create(id); ready_font(f); check_square(f, 0x5101);
        assert(!f->fallback);
        epd_app_font_page_t page = ui_reader_font_remote_page_create(); assert(page);
        assert(ui_reader_font_remote_page_pin(page, f, 0x5101, 0, true) == 1);
        wrapper_pinned = lookup(remote(f), 0x5101); wrapper_hits = diagnostics.descriptor_hits;
        cache_unlock_probe = wrapper_between_queries;
        lv_font_glyph_dsc_t d;
        wrapper_lookup_args args = {f, &d};
        HANDLE drawing = CreateThread(NULL, 0, wrapper_lookup, &args, 0, NULL); assert(drawing);
        assert(WaitForSingleObject(drawing, 5000) == WAIT_OBJECT_0); CloseHandle(drawing);
        assert(!cache_unlock_probe && diagnostics.descriptor_hits == wrapper_hits + 2);
        assert(d.resolved_font == f && d.format == LV_FONT_GLYPH_FORMAT_A8 && d.is_placeholder);
        assert(d.gid.index == wrapper_pinned->ticket && d.adv_w == wrapper_pinned->dsc.adv_w);
        assert(d.box_w == wrapper_pinned->dsc.box_w && d.box_h == wrapper_pinned->dsc.box_h);
        assert(d.stride == wrapper_pinned->dsc.stride && d.ofs_x == wrapper_pinned->dsc.ofs_x && d.ofs_y == wrapper_pinned->dsc.ofs_y);
        d.req_raw_bitmap = 1;
        const lv_draw_buf_t *buf = lv_font_get_glyph_bitmap(&d, NULL);
        assert(buf && buf->data == wrapper_pinned->bitmap.data && d.req_raw_bitmap == 1);
        assert(d.entry == (lv_cache_entry_t *)wrapper_pinned && wrapper_pinned->draws == 1);
        request(f, 'A', true);
        lv_font_glyph_dsc_t ordinary;
        uint64_t hits = diagnostics.descriptor_hits;
        assert(lv_font_get_glyph_dsc(f, &ordinary, 'A', 0));
        assert(diagnostics.descriptor_hits == hits + 1 && !ordinary.is_placeholder && ordinary.resolved_font == f);
        assert(lv_font_get_glyph_bitmap(&ordinary, NULL)); lv_font_glyph_release_draw_data(&ordinary);
        ui_reader_font_remote_cancel(f); ui_reader_font_remote_destroy(f); idle();
        assert(wrapper_pinned->pins == 1 && wrapper_pinned->draws == 1 && buf->data);
        lv_font_glyph_release_draw_data(&d);
        assert(!d.entry && !wrapper_pinned->draws && wrapper_pinned->pins == 1);
        ui_reader_font_remote_page_release(page); idle(); wrapper_pinned = NULL; clean();
    }
    assert(wrapper_reclaims == 2);
}
static void narrow_metrics(bool extreme)
{
    const unsigned sizes[] = {64, 32, 16, 24, 256};
    const unsigned heights[] = {6, 3, 1, 2, 24};
    for (unsigned id = 0; id < 2; ++id) {
        for (unsigned i = 0; i < (extreme ? 1u : 5u); ++i) {
            unsigned size = extreme ? 256u : sizes[i], height = extreme ? 3u : heights[i];
            uint32_t key; lv_font_t *f = ui_reader_font_remote_create(id, size, 0, &key); assert(f); ready_font(f);
            assert(remote(f)->metrics.line_height == (int32_t)height && !remote(f)->metrics.base_line);
            printf("Real metrics id=%u size=%u line_height=%d baseline=%d\n", id, size,
                   remote(f)->metrics.line_height, remote(f)->metrics.base_line);
            check_square(f, 0x5101);
            glyph_t *g = lookup(remote(f), 0x5101);
            unsigned ink = 0, clear = 0;
            for (unsigned y = 0; y < g->dsc.box_h; ++y)
                for (unsigned x = 0; x < g->dsc.box_w; ++x)
                    if (g->bitmap.data[y * g->dsc.stride + x]) ++ink; else ++clear;
            assert(ink && (height < 3 ? !clear : clear > 0));
            printf("Square %ux%u ink=%u clear=%u\n", g->dsc.box_w, g->dsc.box_h, ink, clear);
            destroy(f); clean();
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, abort_without_dialog); setvbuf(stdout, NULL, _IONBF, 0);
    ui_thread = GetCurrentThreadId(); InitializeConditionVariable(&transition); InitializeCriticalSection(&interrupt_gate);
    io_entered = CreateEventA(NULL, TRUE, FALSE, NULL); io_continue = CreateEventA(NULL, TRUE, FALSE, NULL);
    FILE *file = fopen(argv[2], "rb"); assert(file); assert(!fseek(file, 0, SEEK_END));
    fixture_size = (uint32_t)ftell(file); rewind(file); real_font_bytes = malloc(fixture_size); assert(real_font_bytes);
    assert(fread(real_font_bytes, 1, fixture_size, file) == fixture_size); fclose(file);
    if (!strcmp(argv[1], "two_levels")) two_levels();
    else if (!strcmp(argv[1], "primary_square")) primary_square();
    else if (!strcmp(argv[1], "reclaim")) reclaim_square();
    else if (!strcmp(argv[1], "pins_close_reopen")) pins_close_reopen();
    else if (!strcmp(argv[1], "square_oom")) square_oom();
    else if (!strcmp(argv[1], "cancel")) cancel_square();
    else if (!strcmp(argv[1], "engine_oom")) engine_oom();
    else if (!strcmp(argv[1], "lvgl_wrapper")) lvgl_wrapper();
    else if (!strcmp(argv[1], "narrow_metrics")) narrow_metrics(false);
    else if (!strcmp(argv[1], "extreme_metrics")) narrow_metrics(true);
    else errors(argv[1]);
    free(real_font_bytes); printf("PASS real missing glyph: %s (primary records=%u external=%u missing=%u)\n",
        argv[1], primary_records, external_records, missing_records); return 0;
}
