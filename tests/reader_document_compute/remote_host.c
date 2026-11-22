/* SPDX-License-Identifier: Apache-2.0 */
#include "book_compute.h"
#include "document_wire.h"
#include "epd_compute_pipe.h"
#include "epd_compute_memory.h"
#include "reader_image.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

extern const epd_compute_ops_t epd_document_remote_ops;
static epd_compute_pipe_message_t messages[2];
static uint8_t payloads[2][EPD_COMPUTE_PIPE_BYTES];
static unsigned selected_slot;
#define message messages[selected_slot]
#define payload payloads[selected_slot]
static const char *note_target;
static unsigned note_ends, note_truncated;
static unsigned wire_version = BOOK_DOCUMENT_WIRE_VERSION;
static size_t live_allocations, allocation_count, fail_allocation = SIZE_MAX;
void *epd_compute_malloc(size_t n)
{
    if (allocation_count++ >= fail_allocation) return NULL;
    void *p = malloc(n); if (p) ++live_allocations; return p;
}
void *epd_compute_calloc(size_t n, size_t s)
{ void *p = epd_compute_malloc(n * s); if (p) memset(p, 0, n * s); return p; }
void epd_compute_free(void *p) { if (p) { assert(live_allocations); --live_allocations; free(p); } }
void *epd_compute_resolve(const epd_compute_wire_t *r, epd_compute_ref_t ref, uint32_t access)
{ (void)access; assert(r->session < 2); return ref.region == 0 ? (void *)&messages[r->session] : payloads[r->session]; }
uint32_t epd_compute_signals(const epd_compute_wire_t *r) { return r->signals; }

struct epd_compute_fiber {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int running, finished, closing, result;
    const epd_compute_wire_t *request;
    epd_compute_fiber_entry_t entry;
    void *user;
};
static int time_budget;
static unsigned checkpoints;
static void *fiber_thread(void *p)
{
    epd_compute_fiber_t *f = p;
    pthread_mutex_lock(&f->mutex);
    while (!f->running) pthread_cond_wait(&f->condition, &f->mutex);
    pthread_mutex_unlock(&f->mutex);
    int result = f->closing ? EPD_COMPUTE_CANCELLED : f->entry(f, f->user);
    pthread_mutex_lock(&f->mutex);
    f->result = result; f->finished = 1; f->running = 0;
    pthread_cond_broadcast(&f->condition); pthread_mutex_unlock(&f->mutex);
    return NULL;
}
epd_compute_fiber_t *epd_compute_fiber_create(const char *name, size_t stack,
                                            epd_compute_fiber_entry_t entry, void *user)
{
    (void)name; (void)stack;
    epd_compute_fiber_t *f = calloc(1, sizeof(*f)); assert(f);
    pthread_mutex_init(&f->mutex, NULL); pthread_cond_init(&f->condition, NULL);
    f->entry = entry; f->user = user;
    assert(!pthread_create(&f->thread, NULL, fiber_thread, f)); return f;
}
bool epd_compute_fiber_cancelled(const epd_compute_fiber_t *f)
{ return f->closing || (f->request && (f->request->signals & EPD_COMPUTE_CANCEL)); }
bool epd_compute_fiber_should_yield(const epd_compute_fiber_t *f)
{ (void)f; return time_budget && ++checkpoints % 8192 == 0; }
bool epd_compute_fiber_yield(epd_compute_fiber_t *f)
{
    if (epd_compute_fiber_cancelled(f)) return false;
    pthread_mutex_lock(&f->mutex);
    f->running = 0; pthread_cond_broadcast(&f->condition);
    while (!f->running) pthread_cond_wait(&f->condition, &f->mutex);
    pthread_mutex_unlock(&f->mutex);
    return !epd_compute_fiber_cancelled(f);
}
int32_t epd_compute_fiber_step(epd_compute_fiber_t *f, const epd_compute_wire_t *request)
{
    pthread_mutex_lock(&f->mutex);
    f->request = request; f->running = 1; pthread_cond_broadcast(&f->condition);
    while (f->running && !f->finished) pthread_cond_wait(&f->condition, &f->mutex);
    int result = f->finished ? f->result : EPD_COMPUTE_MORE;
    pthread_mutex_unlock(&f->mutex); return result;
}
void epd_compute_fiber_close(epd_compute_fiber_t *f)
{
    pthread_mutex_lock(&f->mutex);
    f->closing = 1; f->running = 1; pthread_cond_broadcast(&f->condition);
    pthread_mutex_unlock(&f->mutex);
    pthread_join(f->thread, NULL);
    pthread_mutex_destroy(&f->mutex); pthread_cond_destroy(&f->condition); free(f);
}

#ifndef TEST_REAL_IMAGE
static ri_io image_io;
int ri_decode(const ri_io *io, const char *key, unsigned w, unsigned h, ri_bitmap *out)
{
    size_t size, actual; uint8_t bytes[8];
    (void)w; (void)h;
    if (io->cancel(io->user)) return RI_CANCELLED;
    if (io->read(io->user, key, 0, NULL, 0, &size) || !size ||
        io->read(io->user, key, 0, bytes, sizeof(bytes), &actual)) return RI_IO;
    out->width = 5; out->height = 3;
    out->pixels = io->alloc(io->user, 15); if (!out->pixels) return RI_NOMEM;
    for (size_t i = 0; i < 15; ++i) out->pixels[i] = (uint8_t)((i % 5) * 63);
    image_io = *io; return RI_OK;
}
void ri_bitmap_release(ri_bitmap *b)
{ if (b->pixels) image_io.free(image_io.user, b->pixels); memset(b, 0, sizeof(*b)); }
int ri_svg_init(const ri_io *io) { (void)io; return RI_OK; }
void ri_svg_shutdown(void) {}
int ri_svg_decode(const ri_io *io, const char *key, unsigned w, unsigned h, ri_bitmap *out)
{ return ri_decode(io, key, w, h, out); }
#endif

typedef struct handle { FILE *file; DIR *dir; } handle;
static handle handles[128];
static int opened, closed, batches, events, images, rows, image_height;
static size_t total_text, first_batch, max_batch;
static FILE *transcript;
static unsigned sequence;
static uint64_t event_hash;
static int bad_response;
static uint32_t select_spine, select_anchor, select_skip, select_flags, format_hint;
static unsigned last_event, chapters, targets, beginnings, endings, toc_count, actual_target, end_offset;
static void output_events(void)
{
    size_t at = 0;
    ++batches; if (!first_batch) first_batch = message.length;
    if (max_batch < message.length) max_batch = message.length;
    while (at < message.length)
    {
        book_document_event_t e;
        assert(message.length - at >= sizeof(e)); memcpy(&e, payload + at, sizeof(e));
        assert(e.bytes >= sizeof(e) && !(e.bytes & 3) && e.bytes <= message.length - at);
        assert(e.length1 <= e.bytes - sizeof(e) && e.length2 <= e.bytes - sizeof(e) - e.length1);
        assert(e.type >= BOOK_DOCUMENT_TEXT && e.type <= BOOK_DOCUMENT_NOTE_END);
        if (e.type == BOOK_DOCUMENT_TEXT) {
            assert(e.length1 <= BOOK_DOCUMENT_TEXT_BYTES); total_text += e.length1;
            if ((select_flags || format_hint) && !note_target) assert(last_event == BOOK_DOCUMENT_SOURCE_ANCHOR);
        }
        if (e.type == BOOK_DOCUMENT_NOTE_END) { ++note_ends; note_truncated = e.a; }
        if (e.type == BOOK_DOCUMENT_CHAPTER) { ++chapters; assert(e.a < e.b); }
        if (e.type == BOOK_DOCUMENT_METADATA && e.a == BOOK_DOCUMENT_META_TOC) ++toc_count;
        if (e.type == BOOK_DOCUMENT_SOURCE_ANCHOR) {
            if (e.d & BOOK_DOCUMENT_SOURCE_BEGIN) ++beginnings;
            if (e.d & BOOK_DOCUMENT_SOURCE_TARGET) { ++targets; actual_target = e.b; }
            if (e.d & BOOK_DOCUMENT_SOURCE_END) { ++endings; end_offset = e.b; }
        }
        if (e.type == BOOK_DOCUMENT_IMAGE_BEGIN) { ++images; rows = 0; image_height = (int)e.c; assert(e.d == (e.b + 1) / 2); }
        if (e.type == BOOK_DOCUMENT_IMAGE_DATA) {
            assert(e.b == (unsigned)rows && e.length1 == e.c * e.d); rows += (int)e.c;
#ifndef TEST_REAL_IMAGE
            for (unsigned row = 0; row < e.c; ++row) {
                const uint8_t *p = payload + at + sizeof(e) + row * e.d;
                assert(e.d == 3 && (p[2] & 15u) == 15u);
                assert(p[0] == 0x04 && p[1] == 0x7b && p[2] == 0xff);
            }
#endif
        }
        if (e.type == BOOK_DOCUMENT_IMAGE_END) assert(rows == image_height);
        for (size_t i = sizeof(e) + e.length1 + e.length2; i < e.bytes; ++i) assert(payload[at + i] == 0);
        if (transcript) assert(fwrite(payload + at, 1, e.bytes, transcript) == e.bytes);
        for (size_t i = 0; i < e.bytes; ++i) event_hash = (event_hash ^ payload[at + i]) * UINT64_C(1099511628211);
        ++events; last_event = e.type; at += e.bytes;
    }
    assert(at == message.length);
}
static void reap(void)
{
    for (size_t i = 1; i < 128; ++i)
    {
        if (handles[i].file) { fclose(handles[i].file); handles[i].file = NULL; ++closed; }
        if (handles[i].dir) { closedir(handles[i].dir); handles[i].dir = NULL; ++closed; }
    }
    assert(opened == closed);
}
static void service(int fail_output)
{
    unsigned id = message.handle;
    int rc = BOOK_OK;
    assert(message.sequence != sequence); sequence = message.sequence;
    switch (message.operation)
    {
    case EPD_PIPE_IDLE: break;
    case EPD_PIPE_STAT: {
        struct stat st;
        if (stat(message.path, &st)) rc = BOOK_IO_ERROR;
        else { message.size = (uint64_t)st.st_size; message.flags = S_ISDIR(st.st_mode) ? BOOK_DOCUMENT_DIRECTORY : BOOK_DOCUMENT_REGULAR; }
        break;
    }
    case EPD_PIPE_OPEN:
    case EPD_PIPE_DIR_OPEN:
        for (id = 1; id < 128 && (handles[id].file || handles[id].dir); ++id) {}
        assert(id < 128);
        if (message.operation == EPD_PIPE_OPEN) {
            handles[id].file = fopen(message.path, "rb");
            if (handles[id].file) { fseek(handles[id].file, 0, SEEK_END); message.size = (uint64_t)ftell(handles[id].file); }
        } else handles[id].dir = opendir(message.path);
        if (handles[id].file || handles[id].dir) { ++opened; message.handle = id; }
        else rc = BOOK_IO_ERROR;
        break;
    case EPD_PIPE_READ: {
        assert(id && id < 128 && handles[id].file);
        assert(message.length <= sizeof(payload));
        size_t want = message.length;
        if (want > 113) want = 113;
        if (fseek(handles[id].file, (long)message.offset, SEEK_SET)) rc = BOOK_IO_ERROR;
        else message.length = (uint32_t)fread(payload, 1, want, handles[id].file);
        break;
    }
    case EPD_PIPE_CLOSE:
        assert(id && id < 128 && handles[id].file); fclose(handles[id].file); handles[id].file = NULL; ++closed; break;
    case EPD_PIPE_DIR_NEXT: {
        assert(id && id < 128 && handles[id].dir);
        struct dirent *entry = readdir(handles[id].dir);
        if (!entry) message.length = 0;
        else { message.length = (uint32_t)strlen(entry->d_name) + 1; memcpy(payload, entry->d_name, message.length); }
        break;
    }
    case EPD_PIPE_DIR_CLOSE:
        assert(id && id < 128 && handles[id].dir); closedir(handles[id].dir); handles[id].dir = NULL; ++closed; break;
    case EPD_PIPE_OUTPUT:
        if (fail_output) rc = BOOK_NO_SPACE;
        else output_events();
        break;
    default: assert(0);
    }
    message.status = rc; message.response_sequence = message.sequence;
    if (bad_response) ++message.response_sequence;
}
static unsigned interrupt_note_mode;
static int interrupt_note(void *body, const epd_compute_wire_t *body_wire, const char *path)
{
    epd_compute_pipe_message_t body_message = message;
    uint8_t body_payload[EPD_COMPUTE_PIPE_BYTES]; memcpy(body_payload, payload, sizeof(body_payload));
    unsigned body_sequence = sequence;
    epd_compute_wire_t wire = *body_wire; wire.session = 1;
    void *note = NULL, *extra = NULL;
    selected_slot = 1; sequence = 0;
    memset(&message, 0, sizeof(message)); memset(payload, 0, sizeof(payload));
    message.version = EPD_COMPUTE_PIPE_VERSION; message.values[0] = BOOK_DOCUMENT_WIRE_VERSION;
    message.values[1] = BOOK_DOCUMENT_NOTE_BUDGET; message.values[6] = BOOK_DOCUMENT_NOTE;
    strcpy(message.path, path);
    strcpy((char *)payload, "OPS/notes.xhtml#n"); message.length = (uint32_t)strlen((char *)payload) + 1;
    assert(epd_document_remote_ops.open(&note, &wire) == EPD_COMPUTE_OK);
    assert(epd_document_remote_ops.open(&extra, &wire) == EPD_COMPUTE_BUSY && !extra);
    assert(epd_document_remote_ops.step(body, body_wire) == EPD_COMPUTE_BUSY);
    if (interrupt_note_mode == 3) fail_allocation = allocation_count;
    unsigned calls = 0, ends = 0; size_t bytes = 0;
    int rc;
    while ((rc = epd_document_remote_ops.step(note, &wire)) == EPD_COMPUTE_MORE) {
        assert(++calls < 10000);
        if (message.operation == EPD_PIPE_READ && interrupt_note_mode == 2) {
            epd_document_remote_ops.close(body, body_wire);
            assert(epd_document_remote_ops.step(note, &wire) == EPD_COMPUTE_CANCELLED);
            break;
        }
        if (message.operation == EPD_PIPE_READ && interrupt_note_mode == 1)
            wire.signals |= EPD_COMPUTE_CANCEL;
        if (message.operation != EPD_PIPE_OUTPUT) service(0);
        else {
            for (size_t at = 0; at < message.length;) {
                book_document_event_t e; memcpy(&e, payload + at, sizeof(e));
                assert(e.bytes >= sizeof(e) && e.bytes <= message.length - at);
                if (e.type == BOOK_DOCUMENT_TEXT) bytes += e.length1;
                else { assert(e.type == BOOK_DOCUMENT_NOTE_END); ++ends; }
                at += e.bytes;
            }
            message.status = BOOK_OK; message.response_sequence = message.sequence;
        }
    }
    fail_allocation = SIZE_MAX;
    if (!interrupt_note_mode) assert(rc == EPD_COMPUTE_OK && ends == 1 && bytes && bytes <= 4096);
    if (interrupt_note_mode == 1) assert(message.status == BOOK_CANCELLED);
    if (interrupt_note_mode == 3) assert(message.status == BOOK_NO_MEMORY);
    epd_document_remote_ops.close(note, &wire);
    selected_slot = 0; sequence = body_sequence;
    if (interrupt_note_mode != 2)
        assert(!memcmp(&message, &body_message, sizeof(message)) && !memcmp(payload, body_payload, sizeof(body_payload)));
    return interrupt_note_mode == 2;
}
static bool inject_note;
static unsigned interrupt_operation = EPD_PIPE_OUTPUT;
static int run(const char *path, unsigned cancel_operation, int close_pending, int fail_output, unsigned budget)
{
    epd_compute_wire_t wire = {0}; void *state = NULL, *second = NULL;
    int rc; unsigned calls = 0;
    memset(&message, 0, sizeof(message)); memset(payload, 0, sizeof(payload));
    message.version = EPD_COMPUTE_PIPE_VERSION; message.values[0] = wire_version;
    message.values[1] = budget; strcpy(message.path, path);
    message.values[4] = select_spine; message.values[5] = select_anchor;
    message.values[6] = select_flags; message.size = select_skip; message.flags = format_hint;
    if (select_flags == BOOK_DOCUMENT_METADATA_ONLY) { message.values[2] = 112; message.values[3] = 146; }
    if (note_target) {
        message.length = (uint32_t)strlen(note_target) + 1;
        memcpy(payload, note_target, message.length);
    }
    wire.region_count = 2; wire.params.length = sizeof(message); wire.result = wire.params;
    opened = closed = batches = events = images = rows = image_height = 0;
    sequence = 0; total_text = first_batch = max_batch = 0;
    last_event = chapters = targets = beginnings = endings = toc_count = 0;
    note_ends = note_truncated = 0;
    event_hash = UINT64_C(14695981039346656037); allocation_count = 0;
    rc = epd_document_remote_ops.open(&state, &wire);
    if (rc != EPD_COMPUTE_OK) { assert(!state && !live_allocations); return rc; }
    assert(epd_document_remote_ops.open(&second, &wire) == EPD_COMPUTE_BUSY && !second);
    while ((rc = epd_document_remote_ops.step(state, &wire)) == EPD_COMPUTE_MORE)
    {
        assert(++calls < 100000);
        if (message.operation == cancel_operation)
        {
            if (close_pending) break;
            wire.signals |= EPD_COMPUTE_CANCEL;
        }
        service(fail_output);
        if (inject_note && message.operation == interrupt_operation) {
            inject_note = false;
            if (interrupt_note(state, &wire, path)) { state = NULL; break; }
        }
    }
    int result = message.status;
    if (!close_pending && state) { assert(message.operation == EPD_PIPE_DONE); assert(message.values[1] <= (budget ? budget : BOOK_DOCUMENT_DEFAULT_BUDGET)); }
    if (state) epd_document_remote_ops.close(state, &wire);
    reap(); assert(!live_allocations);
    return close_pending || !state ? BOOK_CANCELLED : result;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 5 && !strcmp(argv[1], "--metadata")) {
        select_flags = BOOK_DOCUMENT_METADATA_ONLY;
        transcript = fopen(argv[3], "wb"); assert(transcript);
        int rc = run(argv[2], 0, 0, 0, 0);
        fclose(transcript); transcript = NULL;
        assert(atoi(argv[4]) ? rc < 0 : rc == BOOK_OK);
        assert(!total_text && !chapters && !targets && !beginnings && !endings && !toc_count);
        assert(run(argv[2], EPD_PIPE_READ, 0, 0, 0) == BOOK_CANCELLED);
        puts("METADATA: package/cover only, cancellation and resource cleanup passed"); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--interrupt-note")) {
        assert(run(argv[2], 0, 0, 0, 0) == BOOK_OK);
        uint64_t reference = event_hash;
        for (unsigned i = 0; i < 16; ++i) {
            interrupt_operation = i < 12 ? EPD_PIPE_OUTPUT : EPD_PIPE_STAT;
            interrupt_note_mode = i % 4; inject_note = true;
            int result = run(argv[2], 0, 0, 0, 0);
            assert(!inject_note);
            if (interrupt_note_mode == 2) assert(result == BOOK_CANCELLED);
            else assert(result == BOOK_OK && event_hash == reference);
        }
        puts("NOTE interrupt: paused XML/inflate, same body output, cancel, parent-close, OOM isolation and bounded pair passed");
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--note")) {
        note_target = argv[3]; select_flags = BOOK_DOCUMENT_NOTE;
        assert(run(argv[2], 0, 0, 0, 0) == BOOK_OK);
        assert(note_ends == 1 && note_truncated && total_text <= 4096 && total_text >= 4090);
        assert(!images && !chapters && !targets && !beginnings && !endings && !toc_count);
        assert(run(argv[2], EPD_PIPE_READ, 0, 0, 0) == BOOK_CANCELLED);
        assert(run(argv[2], EPD_PIPE_OUTPUT, 1, 0, 0) == BOOK_CANCELLED);
        assert(run(argv[2], 0, 0, 1, 0) == BOOK_NO_SPACE);
        wire_version = 2;
        assert(run(argv[2], 0, 0, 0, 0) == EPD_COMPUTE_INVALID);
        wire_version = BOOK_DOCUMENT_WIRE_VERSION;
        note_target = "OPS/notes.xhtml#missing";
        assert(run(argv[2], 0, 0, 0, 0) == BOOK_ERROR);
        puts("NOTE wire: bounded UTF8/end/cancel/close/error and old-version rejection passed"); return 0;
    }
    if (argc == 9 && !strcmp(argv[1], "--capture")) {
        select_spine = (uint32_t)strtoul(argv[4], NULL, 10);
        select_anchor = (uint32_t)strtoul(argv[5], NULL, 10);
        select_skip = (uint32_t)strtoul(argv[6], NULL, 10);
        select_flags = (uint32_t)strtoul(argv[7], NULL, 10);
        format_hint = (uint32_t)strtoul(argv[8], NULL, 10);
        transcript = fopen(argv[3], "wb"); assert(transcript);
        assert(run(argv[2], 0, 0, 0, 0) == BOOK_OK);
        fclose(transcript); transcript = NULL;
        puts("Captured real converter v2 events"); return 0;
    }
    assert(argc == 3);
    if (!strcmp(argv[1], "--target")) {
        select_flags = BOOK_DOCUMENT_SELECT_CHAPTER | BOOK_DOCUMENT_CONTEXT;
        if (strstr(argv[2], ".txt")) { select_anchor = 15000; select_skip = 39; }
        else if (strstr(argv[2], ".xhtml") || strstr(argv[2], ".data")) { format_hint = BOOK_DOCUMENT_FORMAT_XHTML; select_anchor = 30; select_skip = 13; }
        else select_spine = 1;
        for (int i = 0; i < 2; ++i) {
            assert(run(argv[2], 0, 0, 0, 0) == BOOK_OK);
            assert(total_text && beginnings == 1 && targets == 1 && endings == 1);
            assert(message.values[5] == actual_target && message.values[7] == end_offset);
            if (select_spine) assert(chapters == 2 && toc_count == 2);
            assert(run(argv[2], EPD_PIPE_OUTPUT, 0, 0, 0) == BOOK_CANCELLED);
        }
        puts("v2 target/source anchors, all-chapter TOC, independent reopen and cancel passed"); return 0;
    }
    transcript = fopen(argv[2], "wb"); assert(transcript);
    assert(run(argv[1], 0, 0, 0, 0) == BOOK_OK);
    uint64_t reference_hash = event_hash;
    size_t total_allocations = allocation_count;
    fclose(transcript); transcript = NULL;
    assert(total_text > 40000 && batches < events / 2 && max_batch <= 16384 && first_batch >= 2048 && first_batch < 8192);
    printf("pipe/fiber %s: text=%zu events=%d batches=%d first=%zu max=%zu images=%d\n", argv[1], total_text, events, batches, first_batch, max_batch, images);
    assert(run(argv[1], EPD_PIPE_READ, 0, 0, 0) == BOOK_CANCELLED);
    assert(run(argv[1], EPD_PIPE_READ, 1, 0, 0) == BOOK_CANCELLED);
    assert(run(argv[1], EPD_PIPE_OUTPUT, 0, 0, 0) == BOOK_CANCELLED);
    assert(run(argv[1], EPD_PIPE_OUTPUT, 1, 0, 0) == BOOK_CANCELLED);
    assert(run(argv[1], 0, 0, 1, 0) == BOOK_NO_SPACE);
    assert(run(argv[1], 0, 0, 0, 75000) == BOOK_NO_MEMORY);
    time_budget = 1; assert(run(argv[1], 0, 0, 0, 0) == BOOK_OK); time_budget = 0;
    assert(reference_hash == event_hash);
    bad_response = 1; assert(run(argv[1], 0, 0, 0, 0) == BOOK_IO_ERROR); bad_response = 0;
    for (size_t i = 0; i < total_allocations; i += i < 64 ? 1 : 23) {
        fail_allocation = i;
        int rc = run(argv[1], 0, 0, 0, 0);
        assert(rc == BOOK_NO_MEMORY || (i == 0 && rc == EPD_COMPUTE_NO_MEMORY));
    }
    fail_allocation = SIZE_MAX;
    assert(run(argv[1], 0, 0, 0, 0) == BOOK_OK && event_hash == reference_hash);
    printf("pipe cancel/close/budget/sink failure/time slicing passed\n");
    return 0;
}
