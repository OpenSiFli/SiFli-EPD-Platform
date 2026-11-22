/* Run real font sessions beside real document/image/SVG work in a bounded heap. */
#define main document_fixture_main
#define epd_compute_malloc fixture_malloc
#define epd_compute_calloc fixture_calloc
#define epd_compute_free fixture_free
#define epd_compute_fiber_create fixture_fiber_create
#define epd_compute_fiber_close fixture_fiber_close
#include "../reader_document_compute/remote_host.c"
#undef main
#undef epd_compute_malloc
#undef epd_compute_calloc
#undef epd_compute_free
#undef epd_compute_fiber_create
#undef epd_compute_fiber_close
#include "font_wire.h"
#ifdef COMBINED_SOURCE_MEMORY_BYTES
_Static_assert(COMBINED_SOURCE_MEMORY_BYTES == EPD_FONT_MEMORY_DEFAULT, "HCPU and wire font budgets differ");
#endif

extern const epd_compute_ops_t epd_reader_font_remote_ops;
typedef union { max_align_t alignment; struct { size_t request, charge; } h; } budget_header;
static size_t requested, charged, peak_requested, peak_charged, blocks, peak_blocks;
static unsigned heap_failures;
#define PRIVATE_BYTES (2u * 1024u * 1024u)
static size_t total_peak;
void *epd_compute_malloc(size_t size)
{
    /* M33 configuration: algorithm header 8+7 bytes, heap item 28, align 4.
     * Host algorithm structures retain their native sizes. Fragmentation and
     * uncharged platform allocations require separate device measurement. */
    size_t cost = ((size + 15u + 3u) & ~(size_t)3u) + 28u;
    assert(charged + 56u <= PRIVATE_BYTES);
    if (cost > PRIVATE_BYTES - 56u - charged) { ++heap_failures; return NULL; }
    budget_header *h = fixture_malloc(sizeof(*h) + size);
    if (!h) { ++heap_failures; return NULL; }
    h->h.request = size; h->h.charge = cost;
    requested += size; charged += cost; ++blocks;
    if (requested > peak_requested) peak_requested = requested;
    if (charged > peak_charged) { peak_charged = charged; peak_blocks = blocks; }
    if (charged + 56u > total_peak) total_peak = charged + 56u;
    return h + 1;
}
void *epd_compute_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) return NULL;
    void *p = epd_compute_malloc(n * size); if (p) memset(p, 0, n * size); return p;
}
void epd_compute_free(void *p)
{
    if (!p) return;
    budget_header *h = (budget_header *)p - 1;
    requested -= h->h.request; charged -= h->h.charge; --blocks; fixture_free(h);
}
static struct { epd_compute_fiber_t *fiber; void *reserve; } stacks[4];
epd_compute_fiber_t *epd_compute_fiber_create(const char *name, size_t stack,
    epd_compute_fiber_entry_t entry, void *user)
{
    unsigned i = 0; while (i < 4 && stacks[i].fiber) ++i; assert(i < 4);
    /* The host uses pthread stacks; charge the complete target stack/control
     * reservation independently so it cannot disappear from the heap total. */
    stacks[i].reserve = epd_compute_malloc(stack + 4096u);
    if (!stacks[i].reserve) return NULL;
    stacks[i].fiber = fixture_fiber_create(name, stack, entry, user);
    if (!stacks[i].fiber) { epd_compute_free(stacks[i].reserve); stacks[i].reserve = NULL; }
    return stacks[i].fiber;
}
void epd_compute_fiber_close(epd_compute_fiber_t *f)
{
    unsigned i = 0; while (i < 4 && stacks[i].fiber != f) ++i; assert(i < 4);
    fixture_fiber_close(f); epd_compute_free(stacks[i].reserve); memset(&stacks[i], 0, sizeof(stacks[i]));
}
const epd_compute_wire_t *epd_compute_fiber_request(const epd_compute_fiber_t *f) { return f->request; }

typedef struct {
    void *state;
    epd_font_open_t open;
    epd_compute_pipe_message_t saved;
    uint8_t bytes[EPD_COMPUTE_PIPE_BYTES];
    unsigned request, outputs;
} held_font;
static uint8_t *font_bytes;
static size_t font_size, pack_size;
static uint8_t *pack_bytes;
static epd_compute_wire_t wire;
static unsigned font_output_status, font_completed;
static unsigned font_count, font_batches;
static unsigned font_cancel_operation;
static bool font_close_pending, font_cancel_triggered;
static held_font fonts[2];

typedef struct { size_t requested, charged, blocks, live; } allocation_snapshot;
static allocation_snapshot snapshot(void)
{ return (allocation_snapshot){requested, charged, blocks, live_allocations}; }
static void unchanged(allocation_snapshot s)
{
    assert(requested == s.requested && charged == s.charged && blocks == s.blocks && live_allocations == s.live);
    assert(total_peak <= PRIVATE_BYTES);
}
static void reset_peak(void)
{ peak_requested = requested; peak_charged = charged; peak_blocks = blocks; }

static int font_drive(held_font *f)
{
    unsigned steps = 0;
    for (;;) {
        int rc = epd_reader_font_remote_ops.step(f->state, &wire);
        assert(++steps < 100000);
        if (rc != EPD_COMPUTE_MORE) {
            assert(message.operation == EPD_PIPE_DONE);
            return rc;
        }
        if (message.operation == EPD_PIPE_COMMAND) break;
        if (!font_cancel_triggered && message.operation == font_cancel_operation) {
            font_cancel_triggered = true;
            if (font_close_pending) {
                f->saved = message; memcpy(f->bytes, payload, sizeof(payload));
                return EPD_COMPUTE_CANCELLED;
            }
            wire.signals = EPD_COMPUTE_CANCEL;
        }
        if (message.operation == EPD_PIPE_READ) {
            size_t size = f->open.flags ? pack_size : font_size;
            const uint8_t *bytes = f->open.flags ? pack_bytes : font_bytes;
            assert(message.handle == f->open.handle && message.size == size);
            assert(message.offset <= size && message.length <= size - message.offset);
            memcpy(payload, bytes + message.offset, message.length); message.values[6] = CRF_OK;
        } else if (message.operation == EPD_PIPE_OUTPUT) {
            epd_font_output_t output; memcpy(&output, payload, sizeof(output));
            font_output_status = output.status; font_completed += output.completed; ++f->outputs;
        } else assert(message.operation == EPD_PIPE_IDLE);
        message.status = EPD_COMPUTE_OK; message.response_sequence = message.sequence;
    }
    f->saved = message; memcpy(f->bytes, payload, sizeof(payload));
    return EPD_COMPUTE_OK;
}
static int font_open(held_font *f, unsigned source, bool packed)
{
    memset(f, 0, sizeof(*f));
    f->open = (epd_font_open_t){0}; f->open.version = EPD_FONT_VERSION; f->open.bytes = sizeof(f->open);
    f->open.font = (crf_font_identity){source, 77, source + 1, (uint32_t)font_size, 0};
    /* Two compressed sessions exercise the maximum simultaneous inflater residency. */
    f->open.flags = packed ? EPD_FONT_SOURCE_FTZ1 : 0;
    f->open.source_bytes = (uint32_t)(packed ? pack_size : font_size);
    /* The runner also verifies HCPU SOURCE_MEMORY_BYTES matches this wire default. */
    f->open.handle = source + 1; f->open.memory_bytes = EPD_FONT_MEMORY_DEFAULT;
    memset(&message, 0, sizeof(message)); message.version = EPD_COMPUTE_PIPE_VERSION;
    message.operation = EPD_PIPE_OPEN; message.values[0] = EPD_FONT_VERSION; message.length = sizeof(f->open);
    memcpy(payload, &f->open, sizeof(f->open));
    font_output_status = CRF_OK; font_completed = 0;
    int rc = epd_reader_font_remote_ops.open(&f->state, &wire);
    return rc == EPD_COMPUTE_OK ? font_drive(f) : rc;
}
static int font_batch_count(held_font *f, unsigned first, unsigned size, unsigned count)
{
    assert(count && count <= EPD_FONT_BATCH_MAX);
    message = f->saved; memcpy(payload, f->bytes, sizeof(payload));
    struct { epd_font_command_t header; crf_codepoint chars[64]; } command = {0};
    command.header.version = EPD_FONT_VERSION;
    command.header.bytes = sizeof(command.header) + count * sizeof(crf_codepoint);
    command.header.request_id = ++f->request; command.header.operation = EPD_FONT_RENDER;
    command.header.font = f->open.font; command.header.style.size_px = size;
    command.header.style.oblique = first & 1; command.header.count = count;
    for (unsigned i = 0; i < count; ++i) command.chars[i].codepoint = 0x4e00 + first + i;
    memcpy(payload, &command, command.header.bytes); message.length = command.header.bytes;
    font_output_status = CRF_OK; font_completed = 0;
    message.status = EPD_COMPUTE_OK; message.response_sequence = message.sequence;
    return font_drive(f);
}
static void font_batch(held_font *f, unsigned first, unsigned size)
{
    assert(font_batch_count(f, first, size, 64) == EPD_COMPUTE_OK);
    assert(font_output_status == CRF_OK && font_completed == 64); ++font_batches;
}
static void font_close(held_font *f)
{
    if (!f->state) return;
    message = f->saved; memcpy(payload, f->bytes, sizeof(payload));
    epd_reader_font_remote_ops.close(f->state, &wire); f->state = NULL;
}
static unsigned output_images;
static unsigned placeholders, svg_attempts;
int __real_ri_svg_render_active(const ri_io *, const char *, unsigned, unsigned, ri_bitmap *);
int __wrap_ri_svg_render_active(const ri_io *io, const char *path, unsigned w, unsigned h, ri_bitmap *out)
{
    ++svg_attempts;
    return __real_ri_svg_render_active(io, path, w, h, out);
}
static void document_output(void)
{
    for (size_t at = 0; at < message.length;) {
        book_document_event_t e; memcpy(&e, payload + at, sizeof(e));
        if (e.type == BOOK_DOCUMENT_IMAGE_BEGIN && e.b == 16 && e.c == 16) ++placeholders;
        assert(e.bytes); at += e.bytes;
    }
    output_events(); output_images = (unsigned)images;
    message.status = BOOK_OK; message.response_sequence = message.sequence;
}
enum document_mode { DOC_OK, DOC_CANCEL_READ, DOC_CLOSE_READ, DOC_CANCEL_IMAGE, DOC_CLOSE_IMAGE,
                     DOC_HEAP_OOM, DOC_PNG_OOM, DOC_SVG_OOM, DOC_OPEN_OOM, DOC_STACK_OOM,
                     DOC_RESIDENT_PRESSURE, DOC_VIEWPORT_OOM, DOC_PLACEHOLDER, DOC_MISSING_RESOURCE };
static int document_run(const char *path, enum document_mode mode)
{
    allocation_snapshot baseline = snapshot();
    unsigned failures_before = heap_failures, batches_before = font_batches, image_batches = 0;
    unsigned svg_before = svg_attempts;
    void *pressure = NULL;
    bool triggered = false, closed_pending = false;
    unsigned image_handle = 0;
    reset_peak();
    memset(&message, 0, sizeof(message)); memset(payload, 0, sizeof(payload));
    wire.signals = 0; event_hash = UINT64_C(14695981039346656037);
    batches = events = images = rows = image_height = 0; total_text = first_batch = max_batch = 0;
    output_images = placeholders = 0; last_event = chapters = targets = beginnings = endings = toc_count = 0;
    message.version = EPD_COMPUTE_PIPE_VERSION; message.values[0] = BOOK_DOCUMENT_WIRE_VERSION;
    message.values[1] = BOOK_DOCUMENT_DEFAULT_BUDGET; strcpy(message.path, path);
    /* Explicit render bounds make successful output independent of OOM retries. */
    if (mode != DOC_VIEWPORT_OOM) { message.values[2] = 216; message.values[3] = 384; }
    if (mode == DOC_OPEN_OOM || mode == DOC_STACK_OOM)
        fail_allocation = allocation_count + (mode == DOC_STACK_OOM);
    void *state = NULL;
    int rc = epd_document_remote_ops.open(&state, &wire);
    if (mode == DOC_OPEN_OOM || mode == DOC_STACK_OOM) {
        fail_allocation = SIZE_MAX;
        assert(rc == EPD_COMPUTE_NO_MEMORY && !state && heap_failures > failures_before);
        unchanged(baseline);
        printf("PASS document_open_oom stage=%u baseline_heap=%zu\n", mode == DOC_STACK_OOM, charged + 56u);
        return BOOK_NO_MEMORY;
    }
    assert(rc == EPD_COMPUTE_OK);
    sequence = 0; unsigned steps = 0;
    while ((rc = epd_document_remote_ops.step(state, &wire)) == EPD_COMPUTE_MORE) {
        assert(++steps < 100000);
        unsigned operation = message.operation;
        if (!triggered && operation == EPD_PIPE_READ && (mode == DOC_CANCEL_READ || mode == DOC_CLOSE_READ)) {
            triggered = true;
            if (mode == DOC_CLOSE_READ) { closed_pending = true; break; }
            wire.signals = EPD_COMPUTE_CANCEL;
        }
        bool resource = operation == EPD_PIPE_OPEN &&
            ((mode == DOC_PNG_OOM && strstr(message.path, "image.png")) ||
             (mode == DOC_SVG_OOM && strstr(message.path, "complex.svg")));
        if (message.operation == EPD_PIPE_OUTPUT) document_output(); else service(0);
        if (resource) image_handle = message.handle;
        if (!triggered && operation == EPD_PIPE_READ && image_handle && message.handle == image_handle) {
            fail_allocation = allocation_count; triggered = true;
        }
        if (!triggered && mode == DOC_HEAP_OOM && operation == EPD_PIPE_READ) {
            /* Fill the real modeled heap after conversion has acquired its input. */
            size_t available = PRIVATE_BYTES - 56u - charged;
            assert(available > 64u); pressure = epd_compute_malloc(available - 64u); assert(pressure);
            triggered = true;
        }
        if (!triggered && operation == EPD_PIPE_OUTPUT && image_height > 3 && rows > 0 &&
            (mode == DOC_CANCEL_IMAGE || mode == DOC_CLOSE_IMAGE)) {
            assert(rows < image_height); triggered = true;
            if (mode == DOC_CLOSE_IMAGE) { closed_pending = true; break; }
            wire.signals = EPD_COMPUTE_CANCEL;
        }
        bool image_pending = operation == EPD_PIPE_OUTPUT && image_height > 3 && rows > 0 && rows < image_height;
        if ((mode == DOC_OK || mode == DOC_RESIDENT_PRESSURE) && font_count &&
            (steps == 1 || steps % 101 == 0 || image_pending)) {
            epd_compute_pipe_message_t saved = message; uint8_t data[EPD_COMPUTE_PIPE_BYTES];
            memcpy(data, payload, sizeof(data));
            font_batch(&fonts[(font_batches - batches_before) % font_count], steps % 512, 24);
            if (image_pending) ++image_batches;
            message = saved; memcpy(payload, data, sizeof(data));
        }
    }
    int status = closed_pending ? BOOK_CANCELLED : message.status;
    fail_allocation = SIZE_MAX;
    if (!closed_pending) {
        assert(message.operation == EPD_PIPE_DONE && message.values[1] <= BOOK_DOCUMENT_DEFAULT_BUDGET);
        assert(rc == (status == BOOK_OK ? EPD_COMPUTE_OK : status == BOOK_CANCELLED ? EPD_COMPUTE_CANCELLED : EPD_COMPUTE_COMPONENT_ERROR));
    }
    epd_document_remote_ops.close(state, &wire); reap();
    wire.signals = 0; epd_compute_free(pressure); unchanged(baseline);
    if (mode == DOC_OK) {
        assert(status == BOOK_OK && heap_failures == failures_before && output_images >= 2 && !placeholders);
        assert(!font_count || font_batches > batches_before);
        assert(!font_count || image_height <= 3 || image_batches);
    } else if (mode == DOC_RESIDENT_PRESSURE) {
        assert(status == BOOK_NO_MEMORY && heap_failures == failures_before + 1);
        assert(svg_attempts == svg_before + 1 && !placeholders && output_images < 2);
        printf("PASS resident_pressure status=%d refusals=1 svg_attempts=1 placeholders=0\n", status);
    } else if (mode == DOC_VIEWPORT_OOM) {
        assert(status == BOOK_NO_MEMORY && svg_attempts == svg_before + 1 && !placeholders);
    } else if (mode == DOC_PLACEHOLDER) {
        assert(status == BOOK_OK && placeholders == 1 && output_images == 1);
    } else if (mode == DOC_MISSING_RESOURCE) {
        /* A resource-open failure already invalidates the container stream. */
        assert(status == BOOK_IO_ERROR && !placeholders && heap_failures == failures_before);
    } else {
        assert(triggered);
        if (mode == DOC_HEAP_OOM || mode == DOC_PNG_OOM || mode == DOC_SVG_OOM)
            assert(status == BOOK_NO_MEMORY && heap_failures > failures_before);
        else assert(status == BOOK_CANCELLED);
    }
    printf("PASS document mode=%u fonts=%u path=%s status=%d combined_requested=%zu modeled_heap=%zu headroom=%zu peak_blocks=%zu interleaved_batches=%u image_batches=%u cleanup=baseline\n",
        mode, font_count, path, status, peak_requested, peak_charged + 56u,
        PRIVATE_BYTES - peak_charged - 56u, peak_blocks, font_batches - batches_before, image_batches);
    return status;
}
static void clean(void)
{
    assert(!requested && !charged && !blocks && !live_allocations && opened == closed);
    for (unsigned i = 0; i < 4; ++i) assert(!stacks[i].fiber && !stacks[i].reserve);
}
static void warm_fonts(unsigned count, bool mixed)
{
    font_count = count;
    for (unsigned i = 0; i < count; ++i) {
        assert(font_open(&fonts[i], i, !mixed || !i) == EPD_COMPUTE_OK && font_output_status == CRF_OK);
        for (unsigned batch = 0; batch < 6; ++batch) font_batch(&fonts[i], batch * 64 + i, 24 + 8 * i);
    }
    printf("fonts=%u mixed=%u source_budget=%u resident_requested=%zu modeled_heap=%zu\n",
        count, mixed, EPD_FONT_MEMORY_DEFAULT, requested, charged + 56u);
}
static void font_faults(void)
{
    allocation_snapshot peer = snapshot();
    held_font candidate;
    size_t start = allocation_count;
    assert(font_open(&candidate, 1, true) == EPD_COMPUTE_OK);
    size_t opening_allocations = allocation_count - start;
    font_close(&candidate); unchanged(peer);
    assert(opening_allocations >= 5 && opening_allocations < 32);
    for (size_t i = 0; i < opening_allocations; ++i) {
        unsigned before = heap_failures;
        fail_allocation = allocation_count + i;
        int rc = font_open(&candidate, 1, true);
        fail_allocation = SIZE_MAX;
        assert(heap_failures > before);
        assert(rc == EPD_COMPUTE_NO_MEMORY || (rc == EPD_COMPUTE_COMPONENT_ERROR && font_output_status == CRF_NO_MEMORY));
        font_close(&candidate); unchanged(peer);
        font_batch(&fonts[0], 0, 24); unchanged(peer);
        printf("PASS font_open_oom allocation=%zu cleanup=peer_baseline peer_usable=1\n", i);
    }
    assert(font_open(&candidate, 1, true) == EPD_COMPUTE_OK);
    allocation_snapshot resident = snapshot();
    start = allocation_count;
    assert(font_batch_count(&candidate, 512, 48, 1) == EPD_COMPUTE_OK && font_output_status == CRF_OK);
    size_t raster_allocations = allocation_count - start;
    unchanged(resident); assert(raster_allocations && raster_allocations < 32);
    for (size_t i = 0; i < raster_allocations; ++i) {
        unsigned before = heap_failures;
        fail_allocation = allocation_count + i;
        assert(font_batch_count(&candidate, 512, 48, 1) == EPD_COMPUTE_OK);
        fail_allocation = SIZE_MAX;
        assert(heap_failures > before && font_output_status == CRF_NO_MEMORY && !font_completed);
        unchanged(resident);
        assert(font_batch_count(&candidate, 512, 48, 1) == EPD_COMPUTE_OK && font_output_status == CRF_OK && font_completed == 1);
        unchanged(resident);
        printf("PASS glyph_oom allocation=%zu cleanup=resident_baseline same_session_recovery=1\n", i);
    }
    font_close(&candidate); unchanged(peer);
    for (unsigned close_pending = 0; close_pending < 2; ++close_pending) {
        const unsigned operations[] = {EPD_PIPE_READ, EPD_PIPE_OUTPUT};
        for (unsigned i = 0; i < 2; ++i) {
            assert(font_open(&candidate, 1, true) == EPD_COMPUTE_OK);
            font_cancel_operation = operations[i]; font_close_pending = close_pending; font_cancel_triggered = false;
            assert(font_batch_count(&candidate, 512, 48, 1) == EPD_COMPUTE_CANCELLED);
            assert(font_cancel_triggered);
            font_close(&candidate); wire.signals = 0; font_cancel_operation = 0;
            unchanged(peer); font_batch(&fonts[0], 0, 24); unchanged(peer);
            printf("PASS font_cancel operation=%u close_pending=%u cleanup=peer_baseline peer_usable=1\n", operations[i], close_pending);
        }
    }
}
int main(int argc, char **argv)
{
    assert(argc >= 4); setbuf(stdout, NULL);
    FILE *file = fopen(argv[1], "rb"); assert(file); fseek(file, 0, SEEK_END); font_size = (size_t)ftell(file); rewind(file);
    font_bytes = malloc(font_size); assert(font_bytes && fread(font_bytes, 1, font_size, file) == font_size); fclose(file);
    file = fopen(argv[2], "rb"); assert(file); fseek(file, 0, SEEK_END); pack_size = (size_t)ftell(file); rewind(file);
    pack_bytes = malloc(pack_size); assert(pack_bytes && fread(pack_bytes, 1, pack_size, file) == pack_size); fclose(file);
    wire.region_count = 2; wire.params.length = sizeof(message); wire.result = wire.params;
    uint64_t expected_hash[16] = {0}; assert(argc - 3 <= 16);
    /* Production shares one builtin FTZ across all faces; other sources are raw TTF. */
    for (unsigned combination = 0; combination < 3; ++combination) {
        warm_fonts(combination, combination == 2);
        for (int i = 3; i < argc; ++i) {
            assert(document_run(argv[i], DOC_OK) == BOOK_OK);
            if (!combination) expected_hash[i - 3] = event_hash;
            else assert(expected_hash[i - 3] == event_hash);
        }
        for (unsigned i = 0; i < font_count; ++i) font_close(&fonts[i]);
        clean();
    }
    assert(!heap_failures);
    size_t normal_peak = total_peak;
    font_count = 0;
    assert(document_run(argv[argc - 1], DOC_VIEWPORT_OOM) == BOOK_NO_MEMORY);
    assert(document_run("fixtures/unsupported.xhtml", DOC_PLACEHOLDER) == BOOK_OK);
    assert(document_run("fixtures/missing-resource.xhtml", DOC_MISSING_RESOURCE) == BOOK_IO_ERROR);
    clean();
    warm_fonts(2, false);
    assert(document_run(argv[argc - 1], DOC_RESIDENT_PRESSURE) == BOOK_NO_MEMORY);
    for (unsigned i = 0; i < font_count; ++i) font_batch(&fonts[i], 0, 24);
    font_close(&fonts[1]); font_count = 1;
    assert(document_run(argv[argc - 1], DOC_OK) == BOOK_OK);
    assert(event_hash == expected_hash[argc - 4]);
    font_close(&fonts[0]); clean();
    warm_fonts(1, false); font_faults(); font_close(&fonts[0]); clean();
    warm_fonts(2, true);
    for (enum document_mode mode = DOC_CANCEL_READ; mode <= DOC_STACK_OOM; ++mode) {
        assert(document_run(argv[argc - 1], mode) != BOOK_OK);
        for (unsigned i = 0; i < font_count; ++i) font_batch(&fonts[i], 0, 24);
        assert(document_run(argv[argc - 1], DOC_OK) == BOOK_OK);
        assert(event_hash == expected_hash[argc - 4]);
    }
    for (unsigned i = 0; i < font_count; ++i) font_close(&fonts[i]);
    clean(); free(font_bytes); free(pack_bytes);
    printf("PASS combined real font/document/PNG/SVG normal_peak=%zu normal_headroom=%zu fault_peak=%zu limit=%u rejected_allocations=%u live=0 handles=0 fibers=0\n",
        normal_peak, PRIVATE_BYTES - normal_peak, total_peak, PRIVATE_BYTES, heap_failures);
    return 0;
}

/* Host execution serializes remote steps and close. */
void epd_compute_barrier(void) {}
