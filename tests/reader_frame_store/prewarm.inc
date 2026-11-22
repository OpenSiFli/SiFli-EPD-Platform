typedef struct { view_state_t *owner; unsigned number,distance; } window_page_ref_t;
typedef struct { window_page_ref_t pages[3]; unsigned count; int missing_hop; view_state_t *predecessor; } window_route_t;
static bool data_retiring;
static bool new_frame_fixture;
static unsigned rerenders, publications, prepared_publications;
static rt_tick_t requested_at;
enum { READER_ERROR_IMAGE_MEMORY = 1, READER_ERROR_IMAGE_READ = 2 };
static bool cursor_equal(document_cursor_t a, document_cursor_t b) { return a.record == b.record && a.byte == b.byte; }
static unsigned window_number(const view_state_t *s) { return s->preview_cache_active ? s->preview_page : s->wanted; }
static reader_cached_page_t *reader_page_cache_find(reader_page_cache_t *cache, unsigned n, unsigned layout)
{
    for (reader_cached_page_t *p = cache->first; p; p = p->next)
        if (p->number == n && p->layout == layout) return p;
    return NULL;
}
static bool window_page(view_state_t *s, unsigned n, document_cursor_t *start, bool *eof)
{
    reader_cached_page_t *p = reader_page_cache_find(&s->cache, n, s->layout);
    if (!p) return false;
    *start = p->start; *eof = p->eof; return true;
}
static bool window_start(view_state_t *s, document_cursor_t start) { (void)s; return start.record == 0; }
static view_state_t *window_owner(int hop)
{ return hop == -1 ? primary_state->previous : hop == 1 ? primary_state->following : NULL; }
static bool window_entry(view_state_t *s, int direction, unsigned *n)
{
    if (direction > 0) { *n = 0; return true; }
    for (reader_cached_page_t *p = s->cache.first; p; p = p->next)
        if (p->eof) { *n = p->number; return true; }
    return false;
}
static bool window_work_allowed(void) { return !view.background_paused && !view.failure; }
static bool page_work_allowed(const view_page_t *p) { (void)p; return true; }
static bool request_memory_failed(const view_state_t *s) { (void)s; return false; }
static bool core_waiting(void) { return false; }
static bool window_extra(const view_state_t *s) { return s != primary_state; }
static bool frame_window_contains(const view_frame_t *f, const epd_reader_page_t *p, unsigned n)
{ (void)f; (void)p; (void)n; return false; }
static bool frame_adopt(view_frame_t *f, const view_page_t *p) { (void)f; (void)p; assert(0); return false; }
static int frame_storage(view_frame_t *f, bool required);
static void pattern(uint8_t *out, size_t bytes, unsigned seed);
static view_frame_t *frame_create(const epd_reader_page_t *p, unsigned n, unsigned generation)
{
    assert(new_frame_fixture); ++rerenders;
    view_frame_t *f = calloc(1, sizeof(*f)); assert(f);
    f->owner = state; f->references = 1; ++view.references;
    f->number = n; f->generation = generation; f->intent = view.intent; f->layout = view.layout;
    f->page = *p; f->image = (image_t){{784,881,784,1}, PIXELS, NULL};
    return f;
}
static int frame_render(view_frame_t *f, bool required)
{
    assert(new_frame_fixture); f->required = required; f->resources_ready = true;
    int result = frame_storage(f, required);
    if (result > 0) {
        pattern(f->pixels.data, PIXELS, f->number);
        f->complete = true; f->completed_at = now_tick;
    }
    return result;
}
static void page_reset(view_page_t *p, document_cursor_t start) { p->start = start; }
static void reader_page_cache_load(const reader_cached_page_t *p, epd_reader_page_t *out)
{ out->start = p->start; out->cursor = p->end; out->eof = p->eof; }
static bool frame_pixels_ready(const view_frame_t *f);
static void frame_publish(view_frame_t *f)
{
    assert(frame_pixels_ready(f)); ++publications;
    prepared_publications += f->complete && (int32_t)(requested_at - f->completed_at) > 0;
}
static void frame_accept(view_frame_t *f) { assert(frame_pixels_ready(f)); }
static void page_memory_block(view_page_t *p, const char *stage, unsigned error)
{ (void)p; (void)stage; view.failure = error; }
static void fail(unsigned error) { view.failure = error; }

/* CASES */
static reader_cached_page_t warm_layouts[3][7];
static view_page_t warm_building[3];
static void warm_layouts_set(view_state_t *owner, unsigned slot, unsigned first, unsigned count, bool end)
{
    owner->cache.first = NULL;
    for (unsigned i = 0; i < count; ++i) {
        reader_cached_page_t *p = &warm_layouts[slot][i]; memset(p, 0, sizeof(*p));
        p->number = first + i; p->layout = owner->layout;
        p->start.record = p->number; p->end.record = p->number + 1;
        p->eof = end && i == count - 1;
        p->next = owner->cache.first; owner->cache.first = p;
    }
    owner->initialized = owner->status.ready = true;
    owner->building = owner->display = &warm_building[slot];
    owner->cache_blocked = UINT_MAX;
}
static void warm_begin(void)
{
    setup(); largest = available = 4u * 1024u * 1024u;
    new_frame_fixture = false;
    warm_layouts_set(&context, 0, 97, 7, false);
    context.selected = true; rerenders = publications = prepared_publications = 0;
    allocation_activity = epd_activity_work_queue(EPD_RESOURCE_NEARBY, 1, 65536, 4096, NULL, NULL);
    assert(allocation_activity);
}
static void warm_end(void)
{
    epd_activity_work_close(allocation_activity); allocation_activity = NULL;
    context.cache.first = NULL; cleanup();
}
static void make_cold(view_frame_t *f)
{
    while (f->cold.raw != PIXELS)
        assert(reader_frame_store_append(&f->cold, f->pixels.data, PIXELS, f->pixels.capacity, &frame_cold_allocator));
    frame_bytes -= f->pixels.capacity; epd_compute_buffer_free(&f->pixels); f->image.data = NULL;
    f->cold_intent = f->intent;
}
static void warm_drain(void)
{
    unsigned rounds = 0;
    while (near_frames_pending()) {
        assert(++rounds < 300);
        frame_prepare(); ++now_tick;
    }
    assert(!rerenders && !context.failure);
}
static void warm_seven_and_bursts(void)
{
    warm_begin();
    for (unsigned n = 97; n <= 103; ++n) make_frame(n, true);
    view_frame_t *shown = find_frame(100); shown->references = 2;
    make_cold(find_frame(97)); make_cold(find_frame(98)); make_cold(find_frame(99));
    assert(near_frames_pending()); /* Only the opposite direction is cold. */
    clock_t started = clock(); warm_drain();
    double ms = 1000.0 * (double)(clock() - started) / CLOCKS_PER_SEC;
    for (unsigned n = 97; n <= 103; ++n) { assert(frame_pixels_ready(find_frame(n))); identity(find_frame(n)); }
    assert(frame_bytes == 7u * CAPACITY && !live_blocks && !rerenders);
    const unsigned burst[] = {101,102,103,102,101,100,99,98,97};
    for (unsigned i = 0; i < sizeof(burst)/sizeof(*burst); ++i) {
        view_frame_t *target = find_frame(burst[i]); assert(target);
        context.wanted = target->number; ++context.intent; mailbox.intent = context.intent;
        context.display_pending = true; *context.building = target->page;
        requested_at = ++now_tick; frame_prepare();
        assert(publications == i + 1 && prepared_publications == publications);
    }
    printf("PASS P04 reverse-only cold window warms to seven raw before input; forward/reverse burst publications all prepared; host reheat=%.3fms\n", ms);
    shown->references = 1; warm_end();
}
static void warm_no_swap_and_recovery(void)
{
    warm_begin();
    for (unsigned n = 97; n <= 103; ++n) make_frame(n, true);
    make_cold(find_frame(97)); make_cold(find_frame(98));
    largest = 16384;
    unsigned stores = cold_calls;
    warm_drain();
    unsigned allocs = allocations, recoveries = font_reclaims;
    assert(context.pixels_blocked && !frame_pixels_ready(find_frame(98)) && !frame_pixels_ready(find_frame(97)));
    for (unsigned i = 0; i < 2500; ++i) {
        frame_prepare(); assert(!near_frames_pending()); now_tick += 4;
    }
    assert(allocations == allocs && cold_calls == stores && font_reclaims == recoveries);
    for (unsigned n = 99; n <= 103; ++n) { assert(frame_pixels_ready(find_frame(n))); identity(find_frame(n)); }
    /* A real extra contiguous allocation, not a credit epoch, rearms once. */
    available += 2u * CAPACITY; largest = 2u * CAPACITY;
    assert(near_frames_pending()); warm_drain();
    for (unsigned n = 97; n <= 103; ++n) assert(frame_pixels_ready(find_frame(n)));
    assert(cold_calls == stores && !live_blocks);
    warm_end();
    puts("PASS five hot/two cold fragmented window idles for 10s without swaps, allocations or recovery loops; real capacity growth restores seven hot pages");
}
static void warm_spare_and_cold_input(void)
{
    warm_begin();
    for (unsigned n = 97; n <= 103; ++n) make_frame(n, true);
    view_frame_t *f = find_frame(97); make_cold(f); largest = 16384; warm_drain();
    assert(context.pixels_blocked && !frame_pixels_ready(f));
    view_frame_t *outside = make_frame(104, true); context.frames = outside->next;
    frame_release(outside); unsigned allocs = allocations;
    assert(frame_spare_available(f) && near_frames_pending()); warm_drain();
    assert(frame_pixels_ready(f) && allocations == allocs);
    make_cold(f); largest = 16384;
    context.wanted = 97; context.display_pending = true; *context.building = f->page;
    ++context.intent; mailbox.intent = context.intent; requested_at = ++now_tick;
    unsigned calls = 0;
    while (!publications) { frame_prepare(); ++now_tick; assert(++calls < 100); }
    assert(!prepared_publications && frame_pixels_ready(f)); identity(f);
    warm_end();
    puts("PASS an actual spare rearms background thaw without allocation; on-demand cold target stays prepared=0 and preserves pixels");
}
static void warm_short_chapters(void)
{
    warm_begin();
    view_state_t before = {0}, after = {0};
    before.layout = after.layout = context.layout;
    before.session = after.session = context.session; before.intent = after.intent = context.intent;
    before.wanted = after.wanted = 0; before.direction = -1; after.direction = 1;
    context.wanted = 0; context.previous = &before; context.following = &after;
    warm_layouts_set(&context, 0, 0, 1, true);
    warm_layouts_set(&before, 1, 0, 3, true); warm_layouts_set(&after, 2, 0, 3, true);
    make_frame(0, true);
    view_state_t *owners[] = {&before,&after};
    for (unsigned side = 0; side < 2; ++side) for (unsigned n = 0; n < 3; ++n) {
        view_frame_t *f = make_frame(n, true); context.frames = f->next; --context.references;
        f->owner = owners[side]; f->next = owners[side]->frames; owners[side]->frames = f;
        ++owners[side]->references; make_cold(f);
    }
    window_route_t back = window_route(-1), next = window_route(1);
    assert(back.count == 3 && next.count == 3 && back.pages[0].number == 2 && next.pages[0].number == 0);
    for (unsigned side = 0; side < 2; ++side) {
        state = owners[side]; unsigned rounds = 0;
        assert(window_pixels_pending(state));
        while (window_pixels_pending(state)) {
            /* The neighboring chapter's entry is its displayed layout. */
            if (!frame_pixels_ready(frame_find((document_cursor_t){0,0}))) {
                view.display_pending = true; view.building->start = (document_cursor_t){0,0};
                view.building->cursor = (document_cursor_t){1,0}; view.building->eof = false;
            } else view.display_pending = false;
            frame_prepare(); ++now_tick; assert(++rounds < 100);
        }
        for (view_frame_t *f = view.frames; f; f = f->next) { assert(frame_pixels_ready(f)); identity(f); }
    }
    assert(!rerenders && !live_blocks && frame_bytes == 7u * CAPACITY);
    mailbox.stop = true;
    for (unsigned side = 0; side < 2; ++side) {
        while (owners[side]->frames) { view_frame_t *f = owners[side]->frames; owners[side]->frames = f->next; frame_release(f); }
        assert(!owners[side]->references);
    }
    context.previous = context.following = NULL; state = &context; warm_end();
    puts("PASS production global routes warm three pages on each side across one-page current chapter, including neighbor entry pages");
}
static void warm_growth_and_donor_order(void)
{
    warm_begin(); make_frame(100, true); view_frame_t *back = make_frame(99, true), *far = make_frame(103, true);
    view_frame_t *incoming = make_frame(98, false);
    assert(frame_cold_candidate(incoming) == far);
    assert(storage(incoming) == 1 && !back->cold.first && !far->cold.first && !cold_calls);
    assert(allocations == 1);
    warm_end();
    warm_begin(); make_frame(100, true); back = make_frame(99, true); incoming = make_frame(101, false);
    incoming->required = false;
    largest = 16384; font_reclaim_bytes = CAPACITY; font_largest_after_reclaim = CAPACITY + 32;
    assert(storage(incoming) == 1 && font_reclaims == 1 && !cold_calls && !back->cold.first);
    warm_end();
    puts("PASS raw growth and one successful generic recovery precede compression; compression donor uses distance before directional rank");
}
static void warm_new_frame_preserves_prepared(void)
{
    warm_begin();
    for (unsigned n = 100; n <= 103; ++n) make_frame(n, true);
    view_frame_t *shown = find_frame(101); shown->references = 2;
    context.wanted = 101;
    view_frame_t *incoming = make_frame(104, false); incoming->required = false;
    available = 1569144; largest = 537888;
    unsigned stores = cold_calls;
    assert(storage(incoming) == -3);
    optional_pixels_block(-3, incoming->number);
    assert(!incoming->pixels.data && cold_calls == stores && pressure_requests == 1);
    unsigned allocs = allocations, recoveries = font_reclaims;
    for (unsigned i = 0; i < 2500; ++i) {
        assert(!optional_pixels_ready_for(104));
        now_tick += 4;
    }
    assert(allocations == allocs && cold_calls == stores && font_reclaims == recoveries);
    for (unsigned n = 100; n <= 103; ++n) { assert(frame_pixels_ready(find_frame(n))); identity(find_frame(n)); }
    /* The next two inputs reach the original third prepared page, even
     * though preparing the newly exposed fourth page could not grow raw. */
    for (unsigned n = 102; n <= 103; ++n) {
        view_frame_t *target = find_frame(n);
        context.wanted = n; ++context.intent; mailbox.intent = context.intent;
        context.display_pending = true; *context.building = target->page;
        requested_at = ++now_tick; frame_prepare();
    }
    assert(publications == 2 && prepared_publications == 2 && !rerenders);
    shown->references = 1; warm_end();
    puts("PASS device188 free=1569144/largest=537888: new optional fourth page preserves existing third prepared page; 10s blocked period has no allocation/recovery/store loop");
}
static void warm_pending_foreground_transfer(void)
{
    warm_begin();
    view_frame_t *shown = make_frame(100, true); shown->references = 2;
    view_frame_t *donor = make_frame(97, true), *target = make_frame(101, true);
    make_cold(target); void *raw = donor->pixels.data;
    available = 1569144; largest = 537888; fixture_pressure_pending = true;
    tick_per_block = 1;
    rt_tick_t started = now_tick; clock_t cpu = clock();
    materialize(target);
    double ms = 1000.0 * (double)(clock() - cpu) / CLOCKS_PER_SEC;
    assert(target->pixels.data == raw && donor->cold.raw == PIXELS && !donor->pixels.data);
    assert(target->admission_recovery.state == EPD_MEMORY_RECOVERY_PENDING && pressure_requests == 1);
    assert(elapsed_ms(started) < 1000 && target->completed_at > started);
    identity(target); identity(shown); assert(shown->references == 2);
    printf("PASS foreground cold restore transfers an existing raw buffer while generic recovery remains pending; modeled slices=%ums host=%.3fms, no 5s timeout and no prepared claim\n", elapsed_ms(started), ms);
    shown->references = 1; warm_end();
}
static void warm_pending_transfer_failure(void)
{
    for (unsigned recover = 0; recover < 2; ++recover) {
        warm_begin(); view_frame_t *donor = make_frame(97, true);
        view_frame_t *incoming = make_frame(101, false);
        largest = 537888; fixture_pressure_pending = true; fail_at = 20;
        rt_tick_t started = now_tick;
        assert(frame_storage(incoming, true) == 0 && incoming->cold_reusing && donor->cold.first);
        assert(frame_storage(incoming, true) == 0 && !incoming->cold_reusing && !donor->cold.first);
        unsigned attempts = cold_calls, allocs = allocations;
        assert(attempts == 20 && !live_blocks && !incoming->pixels.data);
        for (unsigned i = 0; i < 100; ++i) {
            ++now_tick; assert(frame_storage(incoming, true) == 0);
        }
        assert(cold_calls == attempts && allocations == allocs && pressure_requests == 1);
        identity(donor);
        if (recover) {
            largest = 2u * CAPACITY; fixture_pressure_pending = false;
            assert(frame_storage(incoming, true) == 1 && incoming->pixels.data);
            assert(!donor->cold.first && elapsed_ms(started) < VIEW_PRESSURE_WAIT_MS);
        } else {
            now_tick = started + VIEW_PRESSURE_WAIT_MS;
            assert(frame_storage(incoming, true) == -3 && !incoming->pixels.data);
        }
        assert(cold_calls == attempts && pressure_requests == 1);
        warm_end();
    }
    puts("PASS partial cold-copy OOM during pending recovery preserves raw donor and refunds blocks; no repeated copy/allocation, one generation ends by capacity recovery or bounded timeout");
}
static void supply_drain(void)
{
    unsigned steps = 0;
    while (near_frames_pending()) {
        assert(++steps < 300); frame_prepare(); ++now_tick;
    }
    assert(!context.failure && !context.rendering);
}
static void supply_idle(void)
{
    unsigned before = allocations, attempts = rerenders, recovery = font_reclaims;
    size_t high = available;
    for (unsigned i = 0; i < 2500; ++i) {
        /* Owner/credit changes and a temporary allocation/free do not raise
         * the physical high-water mark. No heap walk or retry is warranted. */
        unused_credit = i & 1u ? 65536 : 0;
        available = high - (i & 1u ? 128 : 0);
        frame_prepare(); assert(!near_frames_pending()); now_tick += 4;
    }
    available = high;
    assert(allocations == before && rerenders == attempts && font_reclaims == recovery);
}
static void warm_new_supply_after_release(void)
{
    warm_begin(); new_frame_fixture = true;
    for (unsigned n = 100; n <= 102; ++n) make_frame(n, true);
    view_frame_t *shown = find_frame(100); shown->references = 2;
    available = 1569144; largest = 537888;
    supply_drain(); assert(!find_frame(103) && view.pixels_blocked); supply_idle();
    /* A small external free coalesces the existing hole. Requiring another
     * whole frame of free bytes would miss this now-sufficient contiguous gap. */
    available += 4096; largest = CAPACITY;
    assert(near_frames_pending()); supply_drain();
    assert(frame_pixels_ready(find_frame(103)) && frame_pixels_ready(find_frame(99)));
    assert(!find_frame(98)); supply_idle();
    available += 2u * CAPACITY; largest = 2u * CAPACITY;
    supply_drain(); assert(frame_bytes == 7u * CAPACITY && !cold_calls);
    for (unsigned n = 97; n <= 103; ++n) { assert(frame_pixels_ready(find_frame(n))); identity(find_frame(n)); }
    unsigned rendered = rerenders;
    const unsigned burst[] = {101,102,103,102,101,100,99,98,97};
    for (unsigned i = 0; i < sizeof(burst)/sizeof(*burst); ++i) {
        view_frame_t *f = find_frame(burst[i]); context.wanted = f->number;
        ++context.intent; mailbox.intent = context.intent;
        *context.building = f->page; context.display_pending = true;
        requested_at = ++now_tick; frame_prepare();
    }
    assert(publications == 9 && prepared_publications == 9 && rerenders == rendered);
    assert(shown->references == 2); shown->references = 1; warm_end();
    puts("PASS new-page failure resumes after 4KiB external coalescing release; later capacity supplies all seven raw pages; 10s quiescent waits and nine forward/reverse publications stay prepared without rerender or near-page swaps");
}
static void warm_supply_repeated_fragmentation(void)
{
    warm_begin(); new_frame_fixture = true;
    for (unsigned n = 100; n <= 102; ++n) make_frame(n, true);
    warm_layouts_set(&context, 0, 100, 4, false);
    available = 1569144; largest = 537888; supply_drain();
    unsigned attempts = rerenders;
    available += 4096; assert(near_frames_pending());
    size_t watermark = available;
    /* A work control allocation happens after the scheduler latched progress. */
    available -= 256; supply_drain(); assert(rerenders == attempts + 1);
    available += 256; assert(available == watermark); supply_idle();
    view_frame_t *outside = make_frame(104, true); context.frames = outside->next;
    frame_release(outside); unsigned allocs = allocations;
    assert(near_frames_pending()); frame_prepare();
    assert(frame_pixels_ready(find_frame(103)) && allocations == allocs && !cold_calls);
    warm_end();
    puts("PASS one retry per new physical high-water mark; failed retry/control close and 2500 credit churn polls do not loop; matching outside-window spare supplies a new page without fresh pixels");
}
static void prewarm_cases(void)
{
    warm_new_supply_after_release(); warm_supply_repeated_fragmentation();
    warm_begin();
    for (unsigned n = 100; n <= 103; ++n) make_frame(n, true);
    warm_layouts_set(&context, 0, 100, 4, false);
    assert(near_frames_pending() && !near_direction_pending());
    context.direction = -1; assert(near_direction_pending());
    warm_end();
    puts("PASS missing reverse layouts retain background work without blocking the forward-pixels gate that starts their scan");
    warm_begin();
    for (unsigned n = 97; n <= 103; ++n) make_frame(n, true);
    view_frame_t *cold = find_frame(99), *shown = find_frame(100); shown->references = 2;
    make_cold(cold); frame_prepare();
    assert(cold->thaw_offset && !frame_pixels_ready(cold));
    size_t retained = cold->cold.charge; unsigned calls = allocations;
    context.background_paused = mailbox.paused = true;
    for (unsigned i = 0; i < 100; ++i) frame_prepare();
    assert(allocations == calls && cold->cold.charge == retained && !frame_pixels_ready(cold));
    ++context.intent; mailbox.intent = context.intent; frame_cold_maintain();
    assert(!cold->pixels.data && !cold->thaw_offset && cold->cold.charge == retained && frame_spare_available(cold));
    context.background_paused = mailbox.paused = false; warm_drain();
    identity(cold); identity(shown); shown->references = 1; warm_end();
    puts("PASS background pause stops partial thaw; replacement returns its raw buffer as spare and preserves cold/current pixels for resume");
    warm_growth_and_donor_order(); warm_seven_and_bursts(); warm_no_swap_and_recovery();
    warm_spare_and_cold_input(); warm_short_chapters();
    warm_new_frame_preserves_prepared(); warm_pending_foreground_transfer(); warm_pending_transfer_failure();
}
