/* SPDX-License-Identifier: Apache-2.0 */
#ifndef REAL_FONT_PIPE
static void prepare_queues_pending(void)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    queue_letters(font, EPD_COMPUTE_FOREGROUND, 'A', 1, true);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    assert(ui_reader_font_remote_prepare(font, "ABCCDE", 6, true) == 0);
    lock();
    for (unsigned cp = 'B'; cp <= 'E'; ++cp) {
        glyph_t *g = lookup(remote(font), cp); assert(g && g->wanted && !g->busy);
    }
    unlock(); SetEvent(batch_continue); observe_end();
    assert(observed_batches == 2 && batch_order[1].count == 4);
    assert(ui_reader_font_remote_prepare(font, "ABCCDE", 6, true) == 1);
    destroy(font);
}
#if UI_READER_FONT_EAGER
static void eager_prepare(lv_font_t *font, const char *text)
{ assert(ui_reader_font_remote_prepare(font, text, strlen(text), false) >= 0); idle(); }
static void eager_foreground(void)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, false);
    eager_prepare(font, "ABBA \xc2\xa0\n"); observe_end();
    assert(observed_batches == 1 && batch_order[0].operation == EPD_FONT_RENDER && batch_order[0].count == 5);
    LONG calls = commands;
    assert(ui_reader_font_remote_prepare(font, "ABBA \xc2\xa0\n", 8, true) == 1);
    assert(commands == calls && !lookup(remote(font), 'A')->render);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    observe_start(font, false); eager_prepare(font, "XYZ"); observe_end();
    assert(observed_batches == 1 && batch_order[0].operation == EPD_FONT_MEASURE);
    assert(!lookup(remote(font), 'X')->pixels);
    destroy(font);
    font = create(1); ready_font(font); eager_prepare(font, "AX");
    assert(ui_reader_font_remote_prepare(font, "AX", 2, true) == 1);
    assert(lookup(remote(font), 'X')->fallback && lookup(remote(font), 'X')->pixels);
    destroy(font);
}
static void queue_eager_letters(unsigned first)
{
    char text[64 * 3];
    for (unsigned i = 0; i < 64; ++i) {
        unsigned cp = first + i;
        text[i*3] = (char)(0xe0 | (cp >> 12));
        text[i*3+1] = (char)(0x80 | ((cp >> 6) & 63));
        text[i*3+2] = (char)(0x80 | (cp & 63));
    }
    assert(ui_reader_font_remote_purpose(priority_font, EPD_COMPUTE_FOREGROUND) == 1);
    assert(ui_reader_font_remote_prepare(priority_font, text, sizeof(text), false) == 0);
}
static void observe_eager_fairness(epd_compute_pipe_host_t pipe)
{
    observe_batch(pipe);
    if (pipe->purpose == EPD_COMPUTE_FOREGROUND && !first_background_after_foreground) {
        assert(injected < 32);
        queue_eager_letters(0x7000 + injected++ * 64);
    }
}
static void eager_background_fairness(void)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    command_probe = observe_eager_fairness;
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 0x4e00, 32, false);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    queue_eager_letters(0x6000); SetEvent(batch_continue); observe_end();
    assert(injected && first_background_after_foreground == EPD_COMPUTE_MAX_BYPASSES);
    assert(observed_batches > first_background_after_foreground + 1);
    assert(batch_order[1].operation == EPD_FONT_RENDER);
    for (unsigned i = 0; i < 32; ++i) assert(ui_reader_font_remote_ready(font, 0x4e00+i, 0, false) == 1);
    for (unsigned i = 0; i < injected*64; ++i) assert(ui_reader_font_remote_ready(font, 0x7000+i, 0, true) == 1);
    destroy(font);
}
static void eager_bounds(void)
{
    lv_font_t *font = create(0); ready_font(font);
    char text[EAGER_TEXT_MAX + 2]; memset(text, 'A', sizeof(text)); text[sizeof(text)-1] = 0;
    observe_start(font, false); eager_prepare(font, text); observe_end();
    assert(observed_batches == 1 && batch_order[0].operation == EPD_FONT_MEASURE);
    observe_start(font, false); eager_prepare(font, "A"); observe_end();
    assert(!observed_batches && !lookup(remote(font), 'A')->pixels);
    destroy(font);
}
static void eager_host_oom(void)
{
    for (unsigned kind = 0; kind < 3; ++kind) {
        lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
        assert(ui_reader_font_remote_prepare(font, "A", 1, false) == 0);
        assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
        if (!kind) available_bytes = 1;
        else if (kind == 1) fail_alloc = 1;
        else { available_bytes = SHARED_RESERVE + 4096u; allocation_race = 4096; }
        SetEvent(batch_continue); observe_end();
        glyph_t *g = lookup(remote(font), 'A');
        assert(g->descriptor && (kind == 2 ? g->pixels : !g->pixels) && !g->wanted && !remote(font)->deferred);
        assert(remote(font)->batches[1].fallbacks == (kind == 2 ? 0u : 1u));
        LONG calls = commands;
        for (unsigned i = 0; i < 10; ++i) assert(ui_reader_font_remote_prepare(font, "A", 1, false) == 1);
        idle(); assert(commands == calls);
        available_bytes = 64u * 1024u * 1024u;
        prepare(font, "A", true); assert(ui_reader_font_remote_ready(font, 'A', 0, true) == 1);
        destroy(font);
    }
}
static void eager_remote_fallback(crf_status status, bool partial, bool demand)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    assert(ui_reader_font_remote_prepare(font, "AB", 2, false) == 0);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    output_status = status; output_status_skip = partial ? 1 : 0;
    if (demand) assert(ui_reader_font_remote_prepare(font, "AB", 2, true) == 0);
    SetEvent(batch_continue); observe_end();
    assert(observed_batches == (demand ? 3u : 2u));
    assert(batch_order[0].operation == EPD_FONT_RENDER && batch_order[1].operation == EPD_FONT_MEASURE);
    assert(batch_order[1].count == (partial ? 1u : 2u));
    assert(!remote(font)->deferred && remote(font)->batches[1].fallbacks == (partial ? 1u : 2u));
    if (demand) assert(batch_order[2].operation == EPD_FONT_RENDER);
    else assert(!lookup(remote(font), 'B')->pixels);
    if (partial) assert(lookup(remote(font), 'A')->pixels);
    LONG calls = commands;
    for (unsigned i = 0; i < 10; ++i) assert(ui_reader_font_remote_prepare(font, "AB", 2, false) == 1);
    idle(); assert(commands == calls);
    prepare(font, "AB", true); assert(ui_reader_font_remote_prepare(font, "AB", 2, true) == 1);
    destroy(font);
}
static void eager_remote_oom(void)
{ eager_remote_fallback(CRF_NO_MEMORY, false, false); eager_remote_fallback(CRF_OUTPUT_FULL, false, false); }
static void eager_partial_oom(void) { eager_remote_fallback(CRF_NO_MEMORY, true, false); }
static void eager_demand_during_oom(void) { eager_remote_fallback(CRF_NO_MEMORY, false, true); }
static void eager_errors_cancel(void)
{
    const int errors[] = {CRF_IO_ERROR, CRF_INVALID_FONT, CRF_CANCELLED, 0};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(*errors); ++i) {
        lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
        assert(ui_reader_font_remote_prepare(font, "AB", 2, false) == 0);
        assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
        if (errors[i]) output_status = errors[i]; else bad_packet = 1;
        SetEvent(batch_continue); observe_end();
        assert(observed_batches == 1 && ui_reader_font_remote_prepare(font, "AB", 2, false) == -1);
        destroy(font);
    }
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    assert(ui_reader_font_remote_prepare(font, "AB", 2, false) == 0);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    ui_reader_font_remote_cancel(font); SetEvent(batch_continue); observe_end();
    assert(ui_reader_font_remote_status(font) == -1); destroy(font);
}
#endif
#endif
