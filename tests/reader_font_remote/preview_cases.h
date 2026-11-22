/* SPDX-License-Identifier: Apache-2.0 */
#ifndef REAL_FONT_PIPE
static void preview_isolation(void)
{
    lv_font_t *font = create(0); ready_font(font);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    prepare(font, "A", false);
    LONG calls = commands;
    assert(ui_reader_font_remote_prepare_preview(font, "A", 1) == 1);
    idle(); assert(commands == calls && !lookup(remote(font), 'A')->pixels);
    observe_start(font, true);
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 'Z', 1, true);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    assert(ui_reader_font_remote_prepare_preview(font, "BCBC", 4) == 0);
    assert(ui_reader_font_remote_prepare(font, "CDEF", 4, false) == 0);
    lock();
    assert(remote(font)->purpose == EPD_COMPUTE_BACKGROUND);
    for (unsigned cp = 'B'; cp <= 'F'; ++cp) {
        glyph_t *g = lookup(remote(font), cp);
        assert(g && g->wanted && !g->busy && !g->render && g->purpose == EPD_COMPUTE_BACKGROUND);
        assert(g->eager == ((UI_READER_FONT_EAGER && cp <= 'C') ? EAGER_RENDER : EAGER_NONE));
    }
    unlock(); SetEvent(batch_continue); observe_end();
    calls = commands;
    assert(ui_reader_font_remote_prepare_preview(font, "ABCDEF", 6) == 1);
    idle(); assert(commands == calls);
    for (unsigned cp = 'A'; cp <= 'F'; ++cp) {
        glyph_t *g = lookup(remote(font), cp);
        assert(g->descriptor && g->pixels == (UI_READER_FONT_EAGER && cp >= 'B' && cp <= 'C'));
    }
    observe_start(font, false); prepare(font, "GHI", false); observe_end();
    assert(observed_batches == 1 && batch_order[0].operation == EPD_FONT_MEASURE);
    assert(batch_order[0].purpose == EPD_COMPUTE_BACKGROUND && !lookup(remote(font), 'G')->pixels);
    destroy(font);
}
static void preview_bounds(void)
{
    lv_font_t *font = create(0); ready_font(font);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    LONG calls = commands;
    assert(ui_reader_font_remote_prepare_preview(font, NULL, 1) == -1);
    assert(ui_reader_font_remote_prepare_preview(font, "A\xc0\xaf", 3) == -1);
    assert(ui_reader_font_remote_prepare_preview(font, "A", TEXT_LIMIT + 1u) == -1);
    assert(ui_reader_font_remote_prepare_preview(font, NULL, 0) == 1);
    idle(); assert(commands == calls && !lookup(remote(font), 'A'));
    char text[EAGER_TEXT_MAX + 1]; memset(text, 'B', sizeof(text));
    observe_start(font, false);
    assert(ui_reader_font_remote_prepare_preview(font, text, sizeof(text)) == 0); observe_end();
    assert(observed_batches == 1 && batch_order[0].operation == EPD_FONT_MEASURE);
    assert(!lookup(remote(font), 'B')->pixels);
    memset(text, 'C', sizeof(text)); observe_start(font, false);
    assert(ui_reader_font_remote_prepare_preview(font, text, EAGER_TEXT_MAX) == 0); observe_end();
    assert(observed_batches == 1);
    assert(batch_order[0].operation == (UI_READER_FONT_EAGER ? EPD_FONT_RENDER : EPD_FONT_MEASURE));
    destroy(font);
}
#if UI_READER_FONT_EAGER
static void preview_letters(lv_font_t *font, unsigned first, unsigned count)
{
    char text[128 * 3]; assert(count <= 128);
    for (unsigned i = 0; i < count; ++i) {
        unsigned cp = first + i;
        text[i*3] = (char)(0xe0 | (cp >> 12));
        text[i*3+1] = (char)(0x80 | ((cp >> 6) & 63));
        text[i*3+2] = (char)(0x80 | (cp & 63));
    }
    assert(ui_reader_font_remote_prepare_preview(font, text, count * 3) >= 0);
}
static void preview_priority(void)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    preview_letters(font, 0x4e00, 128);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    assert(batch_order[0].purpose == EPD_COMPUTE_BACKGROUND && batch_order[0].count == BACKGROUND_BATCH_MAX);
    queue_letters(font, EPD_COMPUTE_FOREGROUND, 0x4e64, 1, true);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    preview_letters(font, 0x4e64, 1);
    lock(); assert(lookup(remote(font), 0x4e64)->purpose == EPD_COMPUTE_FOREGROUND); unlock();
    SetEvent(batch_continue); observe_end();
    assert(batch_order[1].purpose == EPD_COMPUTE_FOREGROUND && batch_order[1].count == 1);
    assert(batch_order[1].letters[0] == 0x4e64 && batch_order[1].operation == EPD_FONT_RENDER);
    for (unsigned i = 0; i < observed_batches; ++i)
        if (batch_order[i].purpose == EPD_COMPUTE_BACKGROUND) assert(batch_order[i].count <= BACKGROUND_BATCH_MAX);
    for (unsigned i = 0; i < 128; ++i) assert(lookup(remote(font), 0x4e00+i)->pixels);
    destroy(font);
}
static void preview_fairness(void)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    preview_letters(font, 0x4e00, 32);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    feed_foreground = true;
    queue_letters(font, EPD_COMPUTE_FOREGROUND, 0x6000, 64, false);
    SetEvent(batch_continue); observe_end();
    assert(injected && first_background_after_foreground == EPD_COMPUTE_MAX_BYPASSES);
    assert(observed_batches > first_background_after_foreground + 1);
    for (unsigned i = 0; i < 32; ++i) assert(lookup(remote(font), 0x4e00+i)->pixels);
    for (unsigned i = 0; i < injected*64; ++i) assert(lookup(remote(font), 0x7000+i)->descriptor);
    destroy(font);
}
static void preview_inflight_measure(void)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 'A', 1, false);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    assert(ui_reader_font_remote_prepare_preview(font, "A", 1) == 0);
    SetEvent(batch_continue); observe_end();
    assert(observed_batches == 1 && batch_order[0].operation == EPD_FONT_MEASURE);
    assert(ui_reader_font_remote_prepare_preview(font, "A", 1) == 1);
    idle(); assert(!lookup(remote(font), 'A')->pixels);
    destroy(font);
}
static void preview_host_oom(void)
{
    for (unsigned kind = 0; kind < 3; ++kind) {
        lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
        assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
        assert(ui_reader_font_remote_prepare_preview(font, "A", 1) == 0);
        assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
        if (!kind) available_bytes = 1;
        else if (kind == 1) fail_alloc = 1;
        else { available_bytes = SHARED_RESERVE + 4096u; allocation_race = 4096; }
        SetEvent(batch_continue); observe_end();
        glyph_t *g = lookup(remote(font), 'A');
        assert(g->descriptor && (kind == 2 ? g->pixels : !g->pixels) && !g->wanted && !remote(font)->deferred);
        assert(remote(font)->batches[1].fallbacks == (kind == 2 ? 0u : 1u));
        LONG calls = commands;
        for (unsigned i = 0; i < 10; ++i) {
            assert(ui_reader_font_remote_prepare_preview(font, "A", 1) == 1);
            assert(ui_reader_font_remote_prepare(font, "A", 1, false) == 1);
        }
        idle(); assert(commands == calls);
        available_bytes = 64u * 1024u * 1024u;
        prepare(font, "A", true); assert(ui_reader_font_remote_ready(font, 'A', 0, true) == 1);
        destroy(font);
    }
}
static void preview_remote_oom(void)
{
    for (unsigned kind = 0; kind < 4; ++kind) {
        bool partial = kind == 1, demand = kind == 3;
        lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
        assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
        assert(ui_reader_font_remote_prepare_preview(font, "AB", 2) == 0);
        assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
        output_status = kind == 2 ? CRF_OUTPUT_FULL : CRF_NO_MEMORY; output_status_skip = partial ? 1 : 0;
        if (demand) assert(ui_reader_font_remote_prepare(font, "AB", 2, true) == 0);
        SetEvent(batch_continue); observe_end();
        assert(observed_batches == (demand ? 3u : 2u));
        assert(batch_order[0].operation == EPD_FONT_RENDER && batch_order[1].operation == EPD_FONT_MEASURE);
        assert(batch_order[1].count == (partial ? 1u : 2u));
        assert(!remote(font)->deferred && remote(font)->batches[1].fallbacks == (partial ? 1u : 2u));
        for (unsigned i = 0; i < observed_batches; ++i) assert(batch_order[i].purpose == EPD_COMPUTE_BACKGROUND);
        if (demand) assert(batch_order[2].operation == EPD_FONT_RENDER);
        else assert(!lookup(remote(font), 'B')->pixels);
        if (partial) assert(lookup(remote(font), 'A')->pixels);
        LONG calls = commands;
        for (unsigned i = 0; i < 10; ++i) assert(ui_reader_font_remote_prepare_preview(font, "AB", 2) == 1);
        idle(); assert(commands == calls);
        prepare(font, "AB", true); assert(ui_reader_font_remote_prepare(font, "AB", 2, true) == 1);
        destroy(font);
    }
}
static void preview_errors_cancel(void)
{
    const int errors[] = {CRF_IO_ERROR, CRF_INVALID_FONT, CRF_CANCELLED, 0};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(*errors); ++i) {
        lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
        assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
        assert(ui_reader_font_remote_prepare_preview(font, "AB", 2) == 0);
        assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
        if (errors[i]) output_status = errors[i]; else bad_packet = 1;
        SetEvent(batch_continue); observe_end();
        assert(observed_batches == 1 && ui_reader_font_remote_prepare_preview(font, "AB", 2) == -1);
        destroy(font);
    }
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    preview_letters(font, 0x4e00, 32);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    ui_reader_font_remote_cancel(font); SetEvent(batch_continue); observe_end();
    assert(observed_batches == 1 && ui_reader_font_remote_prepare_preview(font, "A", 1) == -1);
    destroy(font);
}
#endif
#endif
