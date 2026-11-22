/* SPDX-License-Identifier: Apache-2.0 */
#ifdef REAL_FONT_PIPE
typedef struct {
    uint32_t advance, width, height, stride, hash;
    int32_t x, y;
} preview_glyph_summary_t;
typedef struct {
    preview_glyph_summary_t glyphs[100];
    crf_ftz_stats ftz;
    unsigned measured, rendered;
    uint64_t elapsed;
} preview_result_t;
/* Consecutive body text, including repeated glyphs and punctuation. */
static const char preview_body[] =
    "北平的天又高起来！“八一三”！上海的炮声把久压在北平人的头上的黑云给掀开了！"
    "祁瑞宣的眉头解开，胖脸上拥起一浪一浪的笑纹，不知不觉的低声哼着岳武穆的《满江红》。"
    "瑞全扯着小顺儿，在院中跳了一个圈，而后把小妞子举起来，扔出去，再接住，";
static void preview_real_run(unsigned px, const char *style, unsigned count, bool preview, preview_result_t *out)
{
    uint32_t letters[100], identity;
    size_t length = 0;
    assert(count <= 100);
    for (unsigned i = 0; i < count; ++i) assert(utf8_next(preview_body, sizeof(preview_body)-1, &length, &letters[i]));
    memset(out, 0, sizeof(*out));
    lv_font_t *base = ui_reader_font_remote_create(0, px,
        !strcmp(style, "bold") ? CRF_WEIGHT_BOLD : CRF_WEIGHT_NORMAL, &identity);
    assert(base); ready_font(base);
    lv_font_t *font = !strcmp(style, "italic") ? ui_reader_font_remote_oblique(base) : base;
    assert(font); ready_font(font);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    measured_records = rendered_records = engine_errors = 0; body_trace = true;
    ULONGLONG begin = GetTickCount64();
    int first = preview ? ui_reader_font_remote_prepare_preview(font, preview_body, length) :
        ui_reader_font_remote_prepare(font, preview_body, length, false);
    assert(first == 0); idle();
    assert(ui_reader_font_remote_prepare(font, preview_body, length, false) == 1);
    unsigned unique = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned j = 0; for (; j < i && letters[j] != letters[i]; ++j) {}
        unique += j == i;
        glyph_t *g = lookup(remote(font), letters[i]);
        assert(g && g->descriptor && !g->wanted && !g->busy);
        assert(g->pixels == (preview && UI_READER_FONT_EAGER));
        preview_glyph_summary_t *s = &out->glyphs[i];
        s->advance = g->dsc.adv_w; s->width = g->dsc.box_w; s->height = g->dsc.box_h;
        s->stride = g->dsc.stride; s->x = g->dsc.ofs_x; s->y = g->dsc.ofs_y;
    }
    assert(remote(font)->purpose == EPD_COMPUTE_BACKGROUND);
    first = ui_reader_font_remote_prepare(font, preview_body, length, true);
    assert(first == (preview && UI_READER_FONT_EAGER ? 1 : 0)); idle();
    assert(ui_reader_font_remote_prepare(font, preview_body, length, true) == 1);
    for (unsigned i = 0; i < count; ++i) {
        epd_app_font_glyph_t g;
        assert(ui_reader_font_remote_acquire(font, letters[i], 0, true, &g) == 1);
        preview_glyph_summary_t *s = &out->glyphs[i];
        assert(s->advance == g.adv_w && s->width == g.box_w && s->height == g.box_h &&
               s->stride == g.stride && s->x == g.ofs_x && s->y == g.ofs_y);
        s->hash = 2166136261u;
        for (size_t p = 0; p < (size_t)g.stride * g.box_h; ++p)
            s->hash = (s->hash ^ g.pixels[p]) * 16777619u;
        ui_reader_font_remote_release(&g);
    }
    out->elapsed = GetTickCount64() - begin;
    out->measured = measured_records; out->rendered = rendered_records;
    assert(!engine_errors && rendered_records == unique);
    assert(measured_records == (preview && UI_READER_FONT_EAGER ? 0 : unique));
    body_trace = false;
    if (font != base) destroy(font);
    destroy(base); real_no_buffers();
    out->ftz = epd_reader_font_last_session.ftz;
    assert(!out->ftz.errors && out->ftz.inflate_calls);
    assert(!fonts && !sources && !resident_sources && !allocations && !pixel_bytes && !descriptor_bytes);
}
#if UI_READER_FONT_EAGER
static void preview_real_faults(void)
{
    const char text[] = "\xe4\xb8\xad\xe6\x96\x87";
    lv_font_t *font = create(0); ready_font(font);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    real_allocation_fault(true);
    assert(ui_reader_font_remote_prepare_preview(font, text, sizeof(text)-1) == 0); idle();
    assert(ui_reader_font_remote_prepare_preview(font, text, sizeof(text)-1) == 1);
    assert(remote(font)->batches[1].count == 1 && remote(font)->batches[1].fallbacks == 2);
    assert(remote(font)->batches[0].glyphs == 2 && !remote(font)->deferred);
    assert(lookup(remote(font), 0x4e2d)->descriptor && !lookup(remote(font), 0x4e2d)->pixels);
    for (unsigned i = 0; i < 10; ++i) {
        assert(ui_reader_font_remote_prepare_preview(font, text, sizeof(text)-1) == 1); idle();
    }
    assert(remote(font)->batches[1].count == 1);
    real_allocation_fault(false); prepare(font, text, true);
    assert(ui_reader_font_remote_ready(font, 0x4e2d, 0, true) == 1);
    destroy(font); real_no_buffers();
    const unsigned operations[] = {EPD_PIPE_READ, EPD_PIPE_OUTPUT};
    for (unsigned i = 0; i < sizeof(operations)/sizeof(*operations); ++i) {
        font = create(0); ready_font(font);
        assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
        ResetEvent(io_entered); ResetEvent(io_continue); hold_eager_operation = operations[i];
        assert(ui_reader_font_remote_prepare_preview(font, text, sizeof(text)-1) == 0);
        assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
        ui_reader_font_remote_cancel(font); SetEvent(io_continue); idle();
        assert(ui_reader_font_remote_prepare_preview(font, text, sizeof(text)-1) == -1);
        destroy(font); real_no_buffers();
        assert(!fonts && !sources && !allocations && !descriptor_bytes && !pixel_bytes);
    }
    puts("PASS real preview: persistent ACPU OOM falls back once; READ/OUTPUT cancellation releases all resources");
}
#endif
static void real_preview(unsigned px, const char *style)
{
    const unsigned counts[] = {16, 100};
    for (unsigned n = 0; n < sizeof(counts)/sizeof(*counts); ++n) {
        preview_result_t separate, preview;
        preview_real_run(px, style, counts[n], false, &separate);
        preview_real_run(px, style, counts[n], true, &preview);
        assert(!memcmp(separate.glyphs, preview.glyphs, sizeof(separate.glyphs)));
#if UI_READER_FONT_EAGER
        /* A complete short record may fit in the FTZ cache across both phases.
         * Preview still eliminates the separate MEASURE command in that case. */
        assert(preview.ftz.inflate_calls <= separate.ftz.inflate_calls);
#else
        assert(preview.ftz.inflate_calls == separate.ftz.inflate_calls);
#endif
        uint32_t hash = 2166136261u;
        const uint8_t *p = (const uint8_t *)preview.glyphs;
        for (size_t i = 0; i < counts[n] * sizeof(*preview.glyphs); ++i) hash = (hash ^ p[i]) * 16777619u;
        printf("PREVIEW px=%u style=%s chars=%u glyph_digest=%08x identical=1 measure=%u->%u render=%u->%u "
               "inflates=%llu->%llu reads=%llu->%llu bytes=%llu->%llu host_ms=%llu->%llu\n",
            px, style, counts[n], hash, separate.measured, preview.measured, separate.rendered, preview.rendered,
            (unsigned long long)separate.ftz.inflate_calls, (unsigned long long)preview.ftz.inflate_calls,
            (unsigned long long)separate.ftz.input_calls, (unsigned long long)preview.ftz.input_calls,
            (unsigned long long)separate.ftz.input_bytes, (unsigned long long)preview.ftz.input_bytes,
            (unsigned long long)separate.elapsed, (unsigned long long)preview.elapsed);
    }
#if UI_READER_FONT_EAGER
    if (px == 28 && !strcmp(style, "normal")) preview_real_faults();
#endif
    printf("PASS real preview px=%u style=%s\n", px, style);
}
#endif
