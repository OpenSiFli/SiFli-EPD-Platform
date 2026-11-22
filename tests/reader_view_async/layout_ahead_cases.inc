/* SPDX-License-Identifier: Apache-2.0 */
/* The layout worker uses real production functions; these are already-complete
 * pixel frame identities. No renderer or extra pixel buffer is created here. */
static view_frame_t ahead_frames[3];
static unsigned ahead_original;

static unsigned ahead_begin(void)
{
    setup(); host_interrupted = host_pixels_blocked = false;
    host_frame_model = true;
    ahead_preview_calls = 0; ahead_eager = true; ahead_mixed_error = false;
    ahead_original = 10;
    open_preview_ready(document_cursor(golden[ahead_original].start));
    unsigned ahead = 0, back = 0;
    for (unsigned i = 0; i < 2000 && ahead < 3; ++i) {
        tick(true);
        reader_page_cache_span(&view.cache, view.preview_page, view.layout, &ahead, &back);
    }
    CHECK(ahead == 3 && !view.preview_back_scanning && !view.cache_chapter_known);
    memset(ahead_frames, 0, sizeof(ahead_frames));
    for (unsigned d = 0; d < 3; ++d) {
        reader_cached_page_t *p = reader_page_cache_find(&view.cache, view.preview_page + d + 1, view.layout);
        CHECK(p);
        reader_page_cache_load(p, &ahead_frames[d].page);
        ahead_frames[d].layout = view.layout;
        ahead_frames[d].next = d < 2 ? &ahead_frames[d + 1] : NULL;
    }
    view.frames = ahead_frames;
    return view.preview_page;
}
static void ahead_end(void)
{
    CHECK(!view.rendering && frame_bytes == 0);
    view.frames = NULL;
    host_interrupted = host_pixels_blocked = false;
    host_frame_model = false;
    ahead_eager = false;
    teardown();
}
static void ahead_pump(unsigned limit)
{
    for (unsigned i = 0; i < limit; ++i) { cache_sync(); tick(true); }
}
static void ahead_check(unsigned number, unsigned golden_number)
{
    static epd_reader_page_t loaded;
    reader_cached_page_t *p = reader_page_cache_find(&view.cache, number, view.layout);
    CHECK(p); reader_page_cache_load(p, &loaded);
    check_payload(&loaded, &golden[golden_number]);
}
static void ahead_unknown_chapter(void)
{
    unsigned base = ahead_begin();
    view.preview_back_ready = true;
    ahead_pump(300);
    CHECK(cache_contains(base + 4));
    ahead_check(base + 4, ahead_original + 4);
    reader_cached_page_t *fourth = reader_page_cache_find(&view.cache, base + 4, view.layout);
    CHECK(fourth->warm_run == fourth->count && !fourth->warm_failed);
    CHECK(ahead_preview_calls && !cache_contains(base + 5));
    CHECK(view.frames == ahead_frames && !view.rendering && !frame_bytes);
    document_flow(); CHECK(host.prefetch_enabled && !near_frames_pending());
    ahead_end();
}
static void ahead_requires_three_frames(void)
{
    unsigned base = ahead_begin();
    view.preview_back_ready = true;
    ahead_frames[1].next = NULL;
    ahead_pump(50); CHECK(!cache_contains(base + 4) && !ahead_preview_calls);
    CHECK(!layout_ahead_ready());
    host_pixels_blocked = true;
    document_flow(); CHECK(host.prefetch_enabled);
    ahead_pump(50); CHECK(!cache_contains(base + 4));
    host_pixels_blocked = false; ahead_frames[1].next = &ahead_frames[2];
    ahead_pump(200); CHECK(cache_contains(base + 4));
    ahead_end();
}
static void ahead_bound_and_window(void)
{
    unsigned base = ahead_begin();
    view.preview_back_ready = true;
    /* Existing descriptors and a preview fallback still need pixel warming. */
    ahead_eager = false;
    for (unsigned i = 0; i < async_host.glyph_count; ++i) async_host.glyphs[i].ready &= 1u;
    ahead_pump(400);
    CHECK(cache_contains(base + 4) && !cache_contains(base + 5));
    reader_cached_page_t *extra = reader_page_cache_find(&view.cache, base + 4, view.layout);
    CHECK(extra->warm_run == extra->count && !extra->warm_failed);
    for (unsigned i = 0; i < extra->count; ++i)
        for (unsigned at = 0; at < extra->runs[i].bytes;) {
            uint32_t letter;
            at += utf8_letter(reader_page_cache_text(extra) + extra->runs[i].text + at,
                              extra->runs[i].bytes - at, &letter);
            CHECK(epd_app_font_glyph_ready(font_for(extra->runs[i].style), letter, 0, true) == 1);
        }
    size_t bytes = view.cache.bytes;
    for (unsigned i = 0; i < 20; ++i) cache_sync();
    CHECK(reader_page_cache_find(&view.cache, base + 4, view.layout) == extra && view.cache.bytes == bytes);
    CHECK(view.cache.bytes <= view.cache.budget && host.free_bytes >= VIEW_CACHE_RESERVE);
    view.direction = -1; CHECK(!layout_ahead_ready());
    view.direction = 1; view.preview_page = UINT_MAX - 2; CHECK(!layout_ahead_ready());
    view.preview_page = base;
    ahead_end();
}
static void ahead_pending_source(void)
{
    unsigned base = ahead_begin();
    view.preview_back_ready = true;
    view.progress.complete = false;
    view.progress.end = (uint32_t)view.display->cursor.record;
    document_cursor_t start;
    CHECK(preview_candidate(&start) == UINT_MAX);
    document_flow(); CHECK(host.prefetch_enabled);
    view.progress.end = HOST_END;
    async_host.hold_records = true;
    for (unsigned i = 0; i < 100 && !view.prefetch->read_request; ++i) tick(true);
    CHECK(view.prefetch_number == base + 4 && view.prefetch->read_request);
    CHECK(!view.prefetch->read_request->foreground);
    for (unsigned i = 0; i < 20; ++i) { document_flow(); CHECK(host.prefetch_enabled); tick(true); }
    async_host.hold_records = false; view.progress.complete = true;
    ahead_pump(200); CHECK(cache_contains(base + 4));
    ahead_end();
}
static void ahead_reverse_three(void)
{
    unsigned base = ahead_begin();
    for (unsigned d = 1; d <= 3; ++d)
        CHECK(reader_page_cache_put(&view.cache, base - d, view.layout, &golden[ahead_original - d]));
    async_host.hold_records = true;
    for (unsigned i = 0; i < 100 && !view.prefetch->read_request; ++i) tick(true);
    CHECK(view.prefetch_number == base + 4 && view.prefetch->read_request);
    unsigned reads = host.read_calls;
    for (unsigned d = 1; d <= 3; ++d) {
        epd_reader_turn(-1); wait_publication(); bind_published();
        check_payload((epd_reader_page_t *)view.display, &golden[ahead_original - d]);
        CHECK(view.trace_hit && !view.preview_back_scanning && host.read_calls == reads);
    }
    CHECK(!layout_ahead_ready());
    async_host.hold_records = false;
    ahead_end();
}
static void ahead_cancel_late_layout(void)
{
    unsigned base = ahead_begin();
    view.preview_back_ready = true;
    for (unsigned i = 0; i < 200 && !view.compute_job; ++i) tick(true);
    CHECK(view.compute_job && view.compute_page == view.prefetch && view.prefetch_number == base + 4);
    reader_compute_job_t *late = view.compute_job;
    epd_reader_turn(1);
    CHECK(late->cancelled && !view.compute_job);
    complete_cancelled();
    wait_publication(); bind_published();
    check_payload((epd_reader_page_t *)view.display, &golden[ahead_original + 1]);
    CHECK(!cache_contains(base + 4));
    ahead_end();
}
static void ahead_budget_and_eof(void)
{
    unsigned base = ahead_begin();
    reader_cached_page_t *back[3];
    for (unsigned d = 1; d <= 3; ++d) {
        back[d - 1] = reader_page_cache_put(&view.cache, base - d, view.layout, &golden[ahead_original - d]);
        CHECK(back[d - 1]);
    }
    view.cache.budget = view.cache.bytes;
    view.preview_back_ready = true;
    ahead_pump(200);
    CHECK(view.cache_blocked == base + 4 && !cache_contains(base + 4));
    for (unsigned d = 1; d <= 3; ++d)
        CHECK(reader_page_cache_find(&view.cache, base - d, view.layout) == back[d - 1]);
    unsigned submitted = host.prefetch_submissions;
    ahead_pump(100); CHECK(host.prefetch_submissions == submitted);
    document_flow(); CHECK(host.prefetch_enabled);
    view.cache_blocked = UINT_MAX; view.cache.budget = VIEW_CACHE_BUDGET;
    reader_cached_page_t *third = reader_page_cache_find(&view.cache, base + 3, view.layout);
    CHECK(third); third->eof = true;
    CHECK(!layout_ahead_ready());
    document_cursor_t start; CHECK(preview_candidate(&start) == UINT_MAX);
    ahead_end();
}
static void ahead_reverse_uncached(void)
{
    unsigned base = ahead_begin();
    async_host.hold_records = true;
    for (unsigned i = 0; i < 100 && !view.prefetch->read_request; ++i) tick(true);
    document_record_request_t *pending = view.prefetch->read_request;
    CHECK(pending && view.prefetch_number == base + 4 && !view.preview_back_scanning);
    epd_reader_turn(-1);
    for (unsigned i = 0; i < 50 && !view.preview_back_scanning; ++i) tick(false);
    CHECK(view.turn_pending < 0 && view.preview_back_scanning && pending->cancelled);
    async_host.hold_records = false;
    wait_publication(); bind_published();
    check_payload((epd_reader_page_t *)view.display, &golden[ahead_original - 1]);
    for (unsigned d = 2; d <= 3; ++d) {
        epd_reader_turn(-1); wait_publication(); bind_published();
        check_payload((epd_reader_page_t *)view.display, &golden[ahead_original - d]);
    }
    ahead_end();
}
static void ahead_midpage_input_limit(void)
{
    unsigned base = ahead_begin();
    document_cursor_t start;
    CHECK(preview_candidate(&start) == base + 4);
    view.prefetch_number = base + 4;
    page_reset(view.prefetch, start);
    view.progress.complete = false;
    view.progress.end = start.record + HOST_STRIDE;
    view.prefetch->cursor = (document_cursor_t){view.progress.end, 0};
    CHECK(preview_candidate(&start) == UINT_MAX);
    document_flow(); CHECK(host.prefetch_enabled);
    tick(false);
    CHECK(view.preview_back_scanning && !view.turn_pending);
    CHECK(!cache_contains(base + 4));
    ahead_end();
}
static void ahead_pause_resume(void)
{
    unsigned base = ahead_begin();
    view.preview_back_ready = true;
    core_background_pause(true);
    ahead_pump(50); CHECK(!cache_contains(base + 4) && !ahead_preview_calls);
    core_background_pause(false); host_interrupted = true;
    ahead_pump(50); CHECK(!cache_contains(base + 4) && !ahead_preview_calls);
    host_interrupted = false;
    view.failure = READER_ERROR_IO; CHECK(!layout_ahead_ready());
    document_cursor_t start; CHECK(preview_candidate(&start) == UINT_MAX);
    view.failure = READER_ERROR_NONE;
    ahead_pump(200); CHECK(cache_contains(base + 4));
    ahead_end();
}
static void ahead_font_error_releases(void)
{
    unsigned base = ahead_begin();
    reader_cached_page_t *p = reader_page_cache_put(&view.cache, base + 4, view.layout,
                                                   &golden[ahead_original + 4]);
    CHECK(p && p->count >= 2 && preview_ahead_pending());
    ahead_mixed_error = true;
    CHECK(cache_warm_candidate() == p);
    CHECK(!cache_warm());
    CHECK(p->warm_failed && !p->warm_run);
    CHECK(!preview_ahead_pending() && cache_warm_candidate() != p);
    document_flow(); CHECK(host.prefetch_enabled);
    ahead_mixed_error = false;
    tick(false); CHECK(view.preview_back_scanning);
    ahead_end();
}
static void ahead_pending_timeout(void)
{
    unsigned base = ahead_begin();
    async_host.hold_records = true;
    for (unsigned i = 0; i < 100 && !view.prefetch->read_request; ++i) tick(true);
    document_record_request_t *pending = view.prefetch->read_request;
    CHECK(pending && view.prefetch_number == base + 4 && preview_ahead_pending());
    host.ticks += VIEW_AHEAD_WAIT_MS;
    CHECK(!preview_ahead_pending());
    document_flow(); CHECK(host.prefetch_enabled);
    tick(false);
    CHECK(pending->cancelled && view.preview_back_scanning);
    async_host.hold_records = false;
    for (unsigned i = 0; i < 2000 && view.preview_back_scanning; ++i) tick(true);
    CHECK(!view.preview_back_scanning && !cache_contains(base + 4));
    unsigned submitted = host.prefetch_submissions;
    ahead_pump(50); CHECK(host.prefetch_submissions == submitted);
    ++view.intent; view.preview_back_ready = true;
    ahead_pump(200); CHECK(cache_contains(base + 4));
    ahead_end();
}
static void ahead_compute_timeout(void)
{
    unsigned base = ahead_begin();
    for (unsigned i = 0; i < 200 && !view.compute_job; ++i) tick(true);
    CHECK(view.compute_page == view.prefetch && view.prefetch_number == base + 4);
    reader_compute_job_t *late = view.compute_job;
    CHECK(late && !late->ready);
    host.ticks += VIEW_AHEAD_WAIT_MS;
    compute_one(true);
    CHECK(late->cancelled && view.preview_back_scanning && !cache_contains(base + 4));
    complete_cancelled();
    CHECK(!cache_contains(base + 4));
    ahead_end();
}
static void ahead_glyph_timeout(void)
{
    unsigned base = ahead_begin();
    reader_cached_page_t *p = reader_page_cache_put(&view.cache, base + 4, view.layout,
                                                   &golden[ahead_original + 4]);
    CHECK(p && preview_ahead_pending());
    async_host.hold_fonts = true;
    for (unsigned i = 0; i < async_host.glyph_count; ++i) async_host.glyphs[i].ready &= 1u;
    CHECK(!cache_warm() && !p->warm_run && !p->warm_failed);
    host.ticks += VIEW_AHEAD_WAIT_MS;
    CHECK(!preview_ahead_pending() && cache_warm_candidate() != p);
    document_flow(); CHECK(host.prefetch_enabled);
    tick(false); CHECK(view.preview_back_scanning);
    async_host.hold_fonts = false;
    ahead_end();
}

static void ahead_third_frame_transition(void)
{
    unsigned base = ahead_begin();
    CHECK(!view.preview_back_ready && !view.preview_back_scanning);
    ahead_frames[1].next = NULL;
    CHECK(near_frames_pending() && !preview_ahead_pending());
    compute_cancel();
    tick(false);
    CHECK(!view.preview_back_scanning);
    CHECK(!cache_contains(base + 4) && !ahead_preview_calls);
    ahead_frames[1].next = &ahead_frames[2];
    CHECK(!near_frames_pending() && preview_ahead_pending());
    for (unsigned i = 0; i < 400 && !cache_contains(base + 4); ++i) tick(true);
    ahead_check(base + 4, ahead_original + 4);
    CHECK(ahead_preview_calls && !view.preview_back_scanning);
    reader_cached_page_t *fourth = reader_page_cache_find(&view.cache, base + 4, view.layout);
    for (unsigned i = 0; i < 400 && !view.preview_back_scanning; ++i) tick(true);
    CHECK(fourth->warm_run == fourth->count && !fourth->warm_failed);
    CHECK(view.preview_back_scanning);
    for (unsigned i = 0; i < 2000 && view.preview_back_scanning; ++i) tick(true);
    unsigned ahead, back;
    reader_page_cache_span(&view.cache, base, view.layout, &ahead, &back);
    CHECK(!view.preview_back_scanning && ahead >= 4 && back >= 3);
    CHECK(view.cache.bytes <= view.cache.budget && !frame_bytes);
    for (unsigned d = 1; d <= 3; ++d) {
        epd_reader_turn(-1); wait_publication(); bind_published();
        CHECK(view.trace_hit);
        check_payload((epd_reader_page_t *)view.display, &golden[ahead_original - d]);
    }
    ahead_end();
}

static void ahead_third_frame_reverse_preempts(void)
{
    (void)ahead_begin();
    ahead_frames[1].next = NULL;
    compute_cancel(); tick(false);
    CHECK(!view.preview_back_scanning);
    epd_reader_turn(-1);
    for (unsigned i = 0; i < 50 && !view.preview_back_scanning; ++i) tick(false);
    CHECK(view.turn_pending < 0 && view.preview_back_scanning && !ahead_preview_calls);
    wait_publication(); bind_published();
    check_payload((epd_reader_page_t *)view.display, &golden[ahead_original - 1]);
    ahead_end();
}

static void ahead_third_frame_budget_release(void)
{
    unsigned base = ahead_begin();
    ahead_frames[1].next = NULL;
    compute_cancel(); tick(false);
    CHECK(!view.preview_back_scanning);
    host_pixels_blocked = true;
    CHECK(!near_frames_pending());
    document_flow(); CHECK(host.prefetch_enabled);
    tick(false);
    CHECK(view.preview_back_scanning && !cache_contains(base + 4));
    CHECK(!ahead_preview_calls && !frame_bytes);
    ahead_end();
}

static void ahead_third_frame_timeout(void)
{
    unsigned base = ahead_begin();
    ahead_frames[1].next = NULL;
    compute_cancel(); tick(false);
    CHECK(!view.preview_back_scanning);
    ahead_frames[1].next = &ahead_frames[2];
    async_host.hold_records = true;
    for (unsigned i = 0; i < 100 && !view.prefetch->read_request; ++i) tick(true);
    document_record_request_t *pending = view.prefetch->read_request;
    CHECK(pending && view.prefetch_number == base + 4 && preview_ahead_pending());
    host.ticks += VIEW_AHEAD_WAIT_MS;
    CHECK(!preview_ahead_pending());
    tick(false);
    CHECK(pending->cancelled && view.preview_back_scanning);
    CHECK(!cache_contains(base + 4));
    async_host.hold_records = false;
    ahead_end();
}

static void ahead_third_frame_reverse_direction(void)
{
    (void)ahead_begin();
    ahead_frames[1].next = NULL;
    view.direction = -1;
    CHECK(near_frames_pending() && !preview_ahead_pending());
    compute_cancel(); tick(false);
    CHECK(view.preview_back_scanning && !view.turn_pending && !ahead_preview_calls);
    ahead_end();
}
