#include <time.h>
typedef struct { void *pointer; size_t charge; } allocation_t;
static allocation_t ledger[4096];
static unsigned cold_calls, fail_at, cancel_at, tick_per_block, live_blocks, adopted_blocks;
static size_t cold_physical, cold_peak, max_block;
static void *epd_app_alloc(size_t bytes, int region)
{
    assert(region == EPD_APP_PSRAM && !locks);
    ++cold_calls;
    now_tick += tick_per_block;
    if (cancel_at && cold_calls == cancel_at) ++mailbox.intent;
    size_t charge = epd_app_allocation_peak(bytes);
    if (cold_calls == fail_at || charge > available || charge > largest) return NULL;
    assert(unused_credit >= charge);
    void *p = malloc(bytes); assert(p);
    unsigned at = 0;
    while (at < 4096 && ledger[at].pointer) ++at;
    assert(at < 4096);
    ledger[at] = (allocation_t){p, charge};
    available -= charge; unused_credit -= charge;
    cold_physical += charge; ++live_blocks; ++adopted_blocks;
    if (cold_physical > cold_peak) cold_peak = cold_physical;
    if (charge > max_block) max_block = charge;
    return p;
}
static void epd_app_free(void *p)
{
    assert(p && !locks);
    for (unsigned i = 0; i < 4096; ++i) if (ledger[i].pointer == p) {
        available += ledger[i].charge; cold_physical -= ledger[i].charge;
        ledger[i].pointer = NULL; --live_blocks; free(p); return;
    }
    ++frames_freed; free(p);
}

/* CASES */
static void setup(void)
{
    assert(!frame_bytes && !context.references && !live_blocks && !cold_physical);
    memset(&context, 0, sizeof(context)); state = primary_state = &context;
    context.wanted = 100; context.direction = 1; context.layout = 9;
    context.session = 7; context.intent = 1;
    memset(&mailbox, 0, sizeof(mailbox)); mailbox.thread = (void *)1;
    mailbox.session = context.session; mailbox.intent = context.intent;
    view_resource.provider = 1;
    available = 2u * 1024u * 1024u; largest = 16384;
    credit_accounting = true; unused_credit = largest_reservation = 0;
    cold_calls = fail_at = cancel_at = tick_per_block = live_blocks = adopted_blocks = 0;
    cold_peak = max_block = 0; now_tick = 100;
    allocations = buffer_frees = frames_freed = pressure_requests = font_reclaims = 0;
    font_reclaim_bytes = pressure_reclaim_bytes = font_largest_after_reclaim = 0;
    allocation_fails = false;
    fixture_pressure_pending = false;
    allocation_activity = NULL;
}
static void pattern(uint8_t *out, size_t bytes, unsigned seed)
{
    uint32_t random = seed + 1;
    for (size_t i = 0; i < bytes; ++i) {
        random = random * 1664525u + 1013904223u;
        /* Gray antialiasing, repeated white rows, and a literal image strip. */
        out[i] = i % 784 < 120 && i / 784 % 24 < 16 ? (uint8_t)(random >> 24) :
                 i % 97 < 3 ? (uint8_t)seed : 255;
    }
}
static view_frame_t *make_frame(unsigned number, bool hot)
{
    view_frame_t *f = calloc(1, sizeof(*f)); assert(f);
    f->owner = &context; f->references = 1; f->number = number;
    f->layout = context.layout; f->intent = context.intent; f->resources_ready = true;
    /* Codec transfer cases model explicit foreground requests. Background
     * scheduling cases override this and exercise the optional admission. */
    f->required = !hot;
    f->page.start.record = number; f->page.cursor.record = number + 1;
    f->image = (image_t){{784,881,784,1}, PIXELS, NULL};
    ++context.references;
    if (hot) {
        attach_pixels(&f->pixels, CAPACITY); frame_bytes += CAPACITY;
        pattern(f->pixels.data, PIXELS, number);
        f->complete = true; f->completed_at = 1; f->image.data = f->pixels.data;
    }
    f->next = context.frames; context.frames = f;
    return f;
}
static view_frame_t *find_frame(unsigned number)
{
    for (view_frame_t *f = context.frames; f; f = f->next) if (f->number == number) return f;
    return NULL;
}
static void cleanup(void)
{
    mailbox.stop = true; frame_spare_clear();
    while (context.frames) {
        view_frame_t *f = context.frames; context.frames = f->next;
        assert(f->references == 1 && !f->renderer && !f->backing);
        frame_release(f);
    }
    assert(!context.references && !frame_bytes && !cold_physical && !live_blocks && !locks);
}
static int storage(view_frame_t *f)
{
    int result = 0;
    for (unsigned calls = 0; !result; ++calls) {
        assert(calls < 1000);
        unsigned before = cold_calls;
        result = frame_storage(f, f->required);
        assert(cold_calls - before <= 16);
        ++now_tick;
    }
    return result;
}
static void identity(view_frame_t *f)
{
    uint8_t *expected = malloc(PIXELS); assert(expected);
    pattern(expected, PIXELS, f->number);
    assert(f->pixels.data && !memcmp(expected, f->pixels.data, PIXELS));
    free(expected);
}
static void materialize(view_frame_t *f)
{
    int result = 0; unsigned calls = 0;
    while (!result) {
        assert(++calls < 1000);
        size_t before = f->thaw_offset;
        result = frame_materialize(f, true);
        if (!result && f->thaw_offset >= before) assert(f->thaw_offset - before <= 16u * READER_FRAME_BLOCK);
        ++now_tick;
    }
    assert(result == 1 && f->image.data == f->pixels.data && !f->cold.first);
    identity(f);
}
static void codec_identity(void)
{
    setup();
    uint8_t input[READER_FRAME_BLOCK], output[READER_FRAME_BLOCK];
    for (unsigned kind = 0; kind < 5; ++kind) {
        for (size_t size = 1; size <= READER_FRAME_BLOCK; size = size < 128 ? size + 1 : size + 127) {
            for (size_t i = 0; i < size; ++i)
                input[i] = kind == 0 ? 0 : kind == 1 ? 255 : kind == 2 ? (uint8_t)i :
                           kind == 3 ? (uint8_t)((i / 128) & 1) : (uint8_t)((i * 1237u) ^ (i >> 3));
            reader_frame_store_t s = {0};
            assert(reader_frame_store_append(&s, input, size, SIZE_MAX, &frame_cold_allocator));
            memset(output, 0xcc, sizeof(output));
            assert(reader_frame_block_read(s.first, output, size));
            assert(!memcmp(input, output, size));
            assert(!reader_frame_block_read(s.first, output, size - 1));
            reader_frame_store_clear(&s, &frame_cold_allocator);
        }
    }
    cleanup(); puts("PASS codec: literals, repeats, gray values, token boundaries and exact destination bounds");
}
static void fragmented_transfer_spare(void)
{
    setup(); view_frame_t *shown = make_frame(100, true); shown->references = 2;
    view_frame_t *old = make_frame(97, true); void *raw = old->pixels.data;
    view_frame_t *incoming = make_frame(101, false);
    assert(storage(incoming) == 1 && incoming->pixels.data == raw);
    assert(old->complete && old->cold.first && !old->pixels.data && !old->image.data);
    assert(shown->references == 2 && !buffer_frees && !frames_freed);
    assert(frame_bytes == 2u * CAPACITY + cold_physical && cold_physical < CAPACITY);
    assert(max_block <= epd_app_allocation_peak(sizeof(reader_frame_block_t) + READER_FRAME_BLOCK));
    assert(adopted_blocks == live_blocks && largest_reservation >= VIEW_RENDER_WORKSPACE);
    /* Retiring the incoming page makes the exact buffer available as spare. */
    context.frames = incoming->next; frame_release(incoming);
    assert(frame_spare.pixels.data == raw);
    unsigned before_allocations = allocations;
    rt_tick_t request = now_tick; ++now_tick;
    materialize(old);
    assert(allocations == before_allocations && old->pixels.data == raw);
    assert(old->completed_at >= request); /* Never reports prepared for this cold hit. */
    identity(shown); shown->references = 1; cleanup();
    puts("PASS fragmented heap: real source allocation/adoption, lossless raw-buffer transfer, spare decode and prepared timestamp");
}
static void partial_oom_and_retry(void)
{
    for (unsigned at = 1; at <= 8; ++at) {
        setup(); view_frame_t *old = make_frame(97, true);
        view_frame_t *incoming = make_frame(101, false); void *raw = old->pixels.data;
        fail_at = at;
        assert(frame_cold_reuse(incoming) == -1);
        assert(old->pixels.data == raw && !old->cold.first && frame_bytes == CAPACITY && !live_blocks);
        identity(old);
        unsigned calls = cold_calls;
        for (unsigned i = 0; i < 100; ++i) assert(frame_cold_reuse(incoming) == -1);
        assert(cold_calls == calls);
        ++incoming->intent; ++context.intent; ++mailbox.intent; fail_at = 0;
        int result; do { result = frame_cold_reuse(incoming); } while (!result);
        assert(result == 1 && old->cold.first); cleanup();
    }
    puts("PASS partial OOM at eight allocation boundaries: original intact, partial charges returned, no same-intent retry, new intent can recover");
}
static void refs_cancel_geometry(void)
{
    for (unsigned kind = 0; kind < 4; ++kind) {
        setup(); view_frame_t *old = make_frame(kind == 3 ? 100 : 97, true);
        view_frame_t *incoming = make_frame(101, false);
        if (kind == 0) old->references = 2;
        if (kind == 1) old->renderer = (void *)1;
        if (kind == 2) old->backing = (void *)1;
        assert(frame_cold_reuse(incoming) == -1 && !cold_calls); identity(old);
        old->references = 1; old->renderer = old->backing = NULL; cleanup();
    }
    setup(); view_frame_t *old = make_frame(97, true), *incoming = make_frame(101, false);
    cancel_at = 3;
    assert(frame_cold_reuse(incoming) == -1 && !old->cold.first && !live_blocks);
    identity(old); cleanup();
    setup(); old = make_frame(97, true); incoming = make_frame(101, false);
    tick_per_block = 1;
    assert(frame_cold_reuse(incoming) == 0 && cold_calls == VIEW_RENDER_SLICE_MS);
    ++context.layout; frame_cold_maintain();
    assert(!old->cold.first && !live_blocks); identity(old); cleanup();
    puts("PASS current/UI references, renderer and backing protected; cancellation and geometry invalidate only partial cold work; 4ms slice enforced");
}
static void seven_window(void)
{
    setup(); view_frame_t *shown = make_frame(100, true); shown->references = 2;
    make_frame(99, true);
    const unsigned initial[] = {101,102,103,98,97};
    for (unsigned i = 0; i < sizeof(initial)/sizeof(initial[0]); ++i) {
        view_frame_t *f = make_frame(initial[i], false); assert(storage(f) == 1);
        pattern(f->pixels.data, PIXELS, f->number); f->complete = true;
    }
    unsigned warm = 0, cold = 0;
    for (view_frame_t *f = context.frames; f; f = f->next) { warm += !!f->pixels.data; cold += !!f->cold.first; }
    assert(warm == 2 && cold == 5 && frame_bytes < 5u * CAPACITY);
    clock_t started = clock();
    for (unsigned turn = 0; turn < 33; ++turn) {
        int direction = turn < 30 ? 1 : -1;
        context.direction = direction; context.wanted = (unsigned)((int)context.wanted + direction);
        ++context.intent; mailbox.intent = context.intent;
        frame_cold_maintain();
        view_frame_t *target = find_frame(context.wanted); assert(target);
        materialize(target);
        --shown->references; shown = target; ++shown->references;
        frame_trim();
        for (int d = -3; d <= 3; ++d) {
            unsigned number = (unsigned)((int)context.wanted + d);
            view_frame_t *f = find_frame(number);
            if (!f) {
                f = make_frame(number, false); assert(storage(f) == 1);
                pattern(f->pixels.data, PIXELS, number); f->complete = true;
            }
            assert(f->complete && (f->pixels.data || f->cold.raw == PIXELS));
        }
        assert(frame_bytes < 5u * CAPACITY && context.references == 7);
    }
    double ms = 1000.0 * (double)(clock() - started) / CLOCKS_PER_SEC;
    printf("PASS explicit foreground transfers preserve seven logical frames under 16KiB largest hole: 30 forward + 3 reverse pixel identity; host total=%.3fms mean=%.3fms/turn peak_cold=%zu bytes\n", ms, ms / 33, cold_peak);
    --shown->references; cleanup();
}
static void restore_cancel_and_bad_block(void)
{
    for (unsigned geometry = 0; geometry < 2; ++geometry) {
        setup(); view_frame_t *shown = make_frame(100, true); shown->references = 2;
        view_frame_t *old = make_frame(97, true), *scratch = make_frame(101, false);
        assert(storage(scratch) == 1 && old->cold.first);
        context.frames = scratch->next; frame_release(scratch);
        size_t charge = old->cold.charge;
        assert(frame_materialize(old, true) == 0 && old->thaw_offset && old->pixels.data);
        void *partial = old->pixels.data;
        ++context.intent; ++mailbox.intent;
        if (geometry) ++context.layout;
        frame_cold_maintain();
        assert(!old->pixels.data && !old->thaw_next && !old->thaw_offset && old->cold.charge == charge);
        assert(frame_spare.pixels.data == partial); identity(shown);
        if (geometry) {
            assert(frame_materialize(old, true) == -2 && old->cold.charge == charge);
            --context.layout;
        }
        /* Invalid packed length is detected before publication. The cold
         * source and currently displayed pixels remain available for retry. */
        uint16_t stored = old->cold.first->stored;
        old->cold.first->stored = 0;
        assert(frame_materialize(old, true) == -1 && old->cold.charge == charge);
        identity(shown); assert(shown->references == 2);
        old->cold.first->stored = stored;
        ++context.intent; ++mailbox.intent; frame_cold_maintain();
        materialize(old);
        shown->references = 1; cleanup();
    }
    puts("PASS partial decompression cancellation/reflow preserves cold bytes; invalid block cannot publish; new-intent restore recovers exact pixels while current display stays intact");
}
static void incompressible_and_peak(void)
{
    setup(); view_frame_t *old = make_frame(97, true), *incoming = make_frame(101, false);
    for (size_t i = 0; i < PIXELS; ++i) ((uint8_t *)old->pixels.data)[i] = (uint8_t)i;
    int result; do { result = frame_cold_reuse(incoming); } while (!result);
    assert(result == -1 && !old->cold.first && !live_blocks && old->pixels.data);
    for (size_t i = 0; i < PIXELS; ++i) assert(((uint8_t *)old->pixels.data)[i] == (uint8_t)i);
    unsigned calls = cold_calls;
    assert(frame_cold_reuse(incoming) == -1 && cold_calls == calls); cleanup();
    setup(); old = make_frame(97, true); incoming = make_frame(101, false);
    size_t occupied = VIEW_FRAME_BUDGET - frame_bytes;
    frame_bytes += occupied; /* Existing frames consume the shared frame budget. */
    assert(frame_cold_reuse(incoming) == -1 && !cold_calls && !old->cold.first);
    identity(old); frame_bytes -= occupied; cleanup();
    puts("PASS incompressible pages stop at strict physical-benefit bound with original intact; conversion peak cannot exceed the frame budget");
}
static void before_glyphs(void)
{
    setup(); view_frame_t *shown = make_frame(100, true); shown->references = 2;
    view_frame_t *near = make_frame(101, true), *incoming = make_frame(97, false);
    incoming->resources_ready = false;
    available = VIEW_RENDER_WORKSPACE + 196608u;
    allocation_activity = epd_activity_work_queue(EPD_RESOURCE_INTERACTIVE, 1, 65536, 4096, NULL, NULL);
    void *pixels = near->pixels.data;
    assert(storage(incoming) == 1 && incoming->pixels.data == pixels && cold_calls && !allocations);
    assert(!incoming->resources_ready && !incoming->complete && !incoming->pins);
    assert(!near->pixels.data && near->cold.raw == near->image.data_size);
    identity(shown); assert(shown->references == 2);
    incoming->resources_ready = true;
    assert(storage(incoming) == 1 && near->cold.first && !near->pixels.data);
    epd_activity_work_close(allocation_activity); allocation_activity = NULL;
    shown->references = 1; cleanup();
    puts("PASS foreground lossless donor transfer acquires pixels before glyph preparation while preserving the current frame and complete cold content");
}
int main(void)
{
    codec_identity(); fragmented_transfer_spare(); partial_oom_and_retry(); refs_cancel_geometry();
    restore_cancel_and_bad_block(); incompressible_and_peak(); before_glyphs(); seven_window();
    return 0;
}
