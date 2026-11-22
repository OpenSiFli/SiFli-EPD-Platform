/* SPDX-License-Identifier: Apache-2.0 */
#ifndef REAL_FONT_PIPE
static HANDLE batch_entered, batch_continue;
static unsigned observed_batches, first_background_after_foreground, injected;
static unsigned selected_pause_count;
static bool block_first_batch, feed_foreground, timed_batches;
static lv_font_t *priority_font;
static struct {
    unsigned purpose, count, operation;
    uint32_t letters[EPD_FONT_BATCH_MAX];
} batch_order[128];

static void queue_letters(lv_font_t *font, unsigned purpose, uint32_t first, unsigned count, bool pixels)
{
    lock();
    assert(ui_reader_font_remote_purpose(font, purpose) == 1);
    for (unsigned i = 0; i < count; ++i)
        assert(ui_reader_font_remote_ready(font, first + i, 0, pixels) >= 0);
    unlock();
}
static void observe_batch(epd_compute_pipe_host_t pipe)
{
    if (!pipe->command.count) return;
    unsigned index = observed_batches++;
    assert(index < 128);
    batch_order[index].purpose = pipe->purpose;
    batch_order[index].count = pipe->command.count;
    batch_order[index].operation = pipe->command.operation;
    for (unsigned i = 0; i < pipe->command.count; ++i)
        batch_order[index].letters[i] = pipe->input[i].codepoint;
    if (timed_batches)
        InterlockedExchangeAdd(&fake_ticks, pipe->command.operation == EPD_FONT_RENDER ? 200 : 100);
    if (index && pipe->purpose == EPD_COMPUTE_BACKGROUND && !first_background_after_foreground)
        first_background_after_foreground = index;
    if (feed_foreground && pipe->purpose == EPD_COMPUTE_FOREGROUND && !first_background_after_foreground) {
        assert(injected < 32);
        queue_letters(priority_font, EPD_COMPUTE_FOREGROUND, 0x7000 + injected++ * 64, 64, false);
    }
    if (!index && block_first_batch) {
        SetEvent(batch_entered);
        assert(WaitForSingleObject(batch_continue, 5000) == WAIT_OBJECT_0);
    }
}
static void observe_start(lv_font_t *font, bool block)
{
    priority_font = font; observed_batches = first_background_after_foreground = injected = 0;
    block_first_batch = block; feed_foreground = timed_batches = false;
    batch_entered = CreateEventA(NULL, FALSE, FALSE, NULL);
    batch_continue = CreateEventA(NULL, FALSE, FALSE, NULL);
    assert(batch_entered && batch_continue); command_probe = observe_batch;
}
static void observe_end(void)
{
    idle(); command_probe = NULL;
    CloseHandle(batch_entered); CloseHandle(batch_continue);
}
static void glyph_foreground_priority(void)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 0x4e00, 128, false);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    assert(batch_order[0].purpose == EPD_COMPUTE_BACKGROUND && batch_order[0].count == BACKGROUND_BATCH_MAX);
    queue_letters(font, EPD_COMPUTE_FOREGROUND, 0x4e64, 1, false);
    queue_letters(font, EPD_COMPUTE_FOREGROUND, 0x6000, 1, true);
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 0x4e64, 1, false);
    lock(); assert(lookup(remote(font), 0x4e64)->purpose == EPD_COMPUTE_FOREGROUND); unlock();
    SetEvent(batch_continue); observe_end();
    assert(batch_order[1].purpose == EPD_COMPUTE_FOREGROUND && batch_order[1].count == 1);
    assert(batch_order[2].purpose == EPD_COMPUTE_FOREGROUND && batch_order[2].count == 1);
    unsigned measure = batch_order[1].operation == EPD_FONT_MEASURE ? 1 : 2;
    unsigned render = measure == 1 ? 2 : 1;
    assert(batch_order[measure].letters[0] == 0x4e64 && batch_order[render].letters[0] == 0x6000);
    assert(batch_order[render].operation == EPD_FONT_RENDER);
    for (unsigned i = 0; i < observed_batches; ++i)
        if (batch_order[i].purpose == EPD_COMPUTE_BACKGROUND) assert(batch_order[i].count <= BACKGROUND_BATCH_MAX);
    for (unsigned i = 0; i < 128; ++i) assert(ui_reader_font_remote_ready(font, 0x4e00 + i, 0, false) == 1);
    assert(ui_reader_font_remote_ready(font, 0x6000, 0, true) == 1);
    destroy(font);
}
static void glyph_background_fairness(void)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 0x4e00, 32, false);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    feed_foreground = true;
    queue_letters(font, EPD_COMPUTE_FOREGROUND, 0x6000, 64, false);
    SetEvent(batch_continue); observe_end();
    assert(injected && first_background_after_foreground > 1);
    assert(first_background_after_foreground <= EPD_COMPUTE_MAX_BYPASSES);
    assert(observed_batches > first_background_after_foreground + 1);
    for (unsigned i = 0; i < 32; ++i) assert(ui_reader_font_remote_ready(font, 0x4e00 + i, 0, false) == 1);
    for (unsigned i = 0; i < injected * 64; ++i) assert(ui_reader_font_remote_ready(font, 0x7000 + i, 0, false) == 1);
    printf("Background served at batch %u while foreground continued; injected=%u batches\n",
        first_background_after_foreground, injected);
    destroy(font);
}
static void glyph_inflight_promotion(void)
{
    lv_font_t *font = create(0); ready_font(font); observe_start(font, true);
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 'A', 1, false);
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    queue_letters(font, EPD_COMPUTE_FOREGROUND, 'A', 1, true);
    assert(ui_reader_font_remote_purpose(font, EPD_COMPUTE_BACKGROUND) == 1);
    lock();
    assert(lookup(remote(font), 'A')->purpose == EPD_COMPUTE_FOREGROUND);
    assert(remote(font)->primary->pipe->purpose == EPD_COMPUTE_FOREGROUND);
    unlock();
    SetEvent(batch_continue); observe_end();
    assert(observed_batches == 2 && batch_order[1].operation == EPD_FONT_RENDER);
    assert(batch_order[1].purpose == EPD_COMPUTE_FOREGROUND);
    assert(ui_reader_font_remote_ready(font, 'A', 0, true) == 1);
    lock(); drop_pixels(lookup(remote(font), 'A')); unlock();
    observe_start(font, false);
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 'A', 1, true); observe_end();
    assert(observed_batches == 1 && batch_order[0].purpose == EPD_COMPUTE_BACKGROUND);
    destroy(font);
}
/* Observe the unlock after glyph selection, before open_face takes ownership.
 * The UI must be able to promote the selected glyph while the worker waits. */
static void pause_selected_batch(void)
{
    lock();
    remote_font_t *font = remote(priority_font);
    glyph_t *a = lookup(font, 'A'), *b = lookup(font, 'B');
    bool selected = a && b && a->busy && b->busy && !font->primary->owner;
    if (selected) {
        assert(!a->wanted && !b->wanted);
        assert(a->purpose == EPD_COMPUTE_BACKGROUND && b->purpose == EPD_COMPUTE_BACKGROUND);
        assert(!observed_batches);
        cache_unlock_probe = NULL;
        ++selected_pause_count;
    }
    unlock();
    if (selected) {
        SetEvent(batch_entered);
        assert(WaitForSingleObject(batch_continue, 5000) == WAIT_OBJECT_0);
    }
}
static void precommand_promotion(bool reopen)
{
    lv_font_t *font = create(0); ready_font(font);
    if (reopen) {
        lv_font_t *a = create(1); ready_font(a);
        lv_font_t *b = create(2); ready_font(b);
        assert(!remote(font)->primary->pipe && !remote(font)->primary->opened);
        destroy(a); destroy(b);
    }
    LONG calls = commands, opened = remote_opens;
    observe_start(font, false);
    lock(); selected_pause_count = 0; cache_unlock_probe = pause_selected_batch;
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 'A', 2, false); unlock();
    assert(WaitForSingleObject(batch_entered, 5000) == WAIT_OBJECT_0);
    assert(selected_pause_count == 1 && !cache_unlock_probe);
    assert(!observed_batches && commands == calls);
    /* Promote the second selected glyph, then return the font to background.
     * The pending glyph, not the font's latest purpose, owns this priority. */
    queue_letters(font, EPD_COMPUTE_FOREGROUND, 'B', 1, false);
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 'B', 1, false);
    lock();
    assert(lookup(remote(font), 'A')->busy && lookup(remote(font), 'B')->busy);
    assert(lookup(remote(font), 'B')->purpose == EPD_COMPUTE_FOREGROUND);
    assert(!remote(font)->primary->owner);
    unlock();
    SetEvent(batch_continue); observe_end();
    assert(selected_pause_count == 1);
    assert(observed_batches == 1 && batch_order[0].count == 2);
    assert(batch_order[0].purpose == EPD_COMPUTE_FOREGROUND);
    if (reopen) assert(remote_opens == opened + 1 && open_purposes[opened] == EPD_COMPUTE_FOREGROUND);
    else assert(remote_opens == opened);
    assert(ui_reader_font_remote_ready(font, 'A', 0, false) == 1);
    assert(ui_reader_font_remote_ready(font, 'B', 0, false) == 1);
    observe_start(font, false);
    queue_letters(font, EPD_COMPUTE_BACKGROUND, 'C', 1, false); observe_end();
    assert(observed_batches == 1 && batch_order[0].purpose == EPD_COMPUTE_BACKGROUND);
    printf("Precommand promotion: reopen=%u selected_pause=1 foreground_batch=1 background_restored=1\n",
        reopen ? 1u : 0u);
    destroy(font);
}
static void glyph_precommand_promotion(void) { precommand_promotion(false); }
static void glyph_reopen_promotion(void) { precommand_promotion(true); }
static void glyph_initial_priority(void)
{
    hold_io = 1; lv_font_t *blocked = create(1);
    assert(WaitForSingleObject(io_entered, 5000) == WAIT_OBJECT_0);
    lv_font_t *background = create(2);
    assert(ui_reader_font_remote_purpose(background, EPD_COMPUTE_BACKGROUND) == 1);
    lv_font_t *foreground = create(0);
    queue_letters(foreground, EPD_COMPUTE_FOREGROUND, 'A', 1, false);
    assert(ui_reader_font_remote_purpose(foreground, EPD_COMPUTE_BACKGROUND) == 1);
    uint32_t foreground_source = remote(foreground)->primary->identity.generation;
    SetEvent(io_continue); idle();
    assert(open_order[1] == foreground_source);
    assert(ui_reader_font_remote_ready(foreground, 'A', 0, false) == 1);
    destroy(blocked); destroy(background); destroy(foreground);
}
static void batch_summaries(void)
{
    lv_font_t *font = create(1); ready_font(font);
    font_batch_stats_t initial = remote(font)->batches[0];
    assert(initial.count == 1 && !initial.glyphs && initial.source_calls == 1 && initial.source_bytes == 16);
    assert(!batch_logs);
    observe_start(font, false); timed_batches = batch_source_reads = true;
    measure_only(font, "AB"); prepare(font, "AB", true); observe_end(); batch_source_reads = false;
    font_batch_stats_t measure = remote(font)->batches[0], render = remote(font)->batches[1];
    assert(measure.count == initial.count + 1 && measure.glyphs == 2 && measure.completed == 2);
    assert(measure.source_calls == 2 && measure.source_bytes == 32);
    assert(measure.exec_ticks == initial.exec_ticks + 100 && measure.max_batch_ticks == 100);
    assert(render.count == 1 && render.glyphs == 2 && render.completed == 2);
    assert(render.source_calls == 1 && render.source_bytes == 16 && render.exec_ticks == 200 && render.max_batch_ticks == 200);
    assert(raw_reads == 1 && source_cache_totals.hits == 2);
    assert(!batch_logs); /* No per-batch log, even for the two slow batches. */
    epd_app_font_remote_stats_t stats; ui_reader_font_remote_stats(&stats);
    assert(batch_logs == 2 && strstr(last_batch_log, "kind=render") && strstr(last_batch_log, "exec_ticks=200"));
    step_transient = EPD_COMPUTE_BUSY; measure_only(font, "C");
    assert(remote(font)->batches[0].failures == 1 && remote(font)->batches[0].retries == 1);
    InterlockedExchangeAdd(&fake_ticks, RT_TICK_PER_SECOND); measure_only(font, "C");
    assert(ui_reader_font_remote_ready(font, 'C', 0, false) == 1);
    destroy(font); assert(batch_logs == 4);
}
#endif
