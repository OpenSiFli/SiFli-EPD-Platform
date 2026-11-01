#include "font_wire.h"
#include "epd_compute_pipe.h"
#include "epd_compute_memory.h"
#include "epd_compute_cache.h"
#include "font_ftz.h"
#include "font_diagnostics.h"
#include "font_metadata.h"
#include "font_step_profile.h"
#include <string.h>
#ifdef SF32LB57X
#include <board.h>
#endif

/* Covers the current common fiber's thread/semaphore/control allocation.
 * The configured 16 KiB stack is reserved separately from algorithm memory. */
#define FIBER_CONTROL_RESERVE 4096u
crf_session_diagnostics epd_reader_font_last_session;
typedef struct {
    epd_compute_pipe_remote_t pipe;
    epd_font_open_t open;
    epd_font_command_t command;
    crf_context *engine;
    crf_ftz *ftz;
    crf_stats stats;
    crf_codepoint input[EPD_FONT_BATCH_MAX];
    crf_glyph glyphs[EPD_FONT_BATCH_MAX];
    union { uint32_t alignment; uint8_t bytes[EPD_FONT_PACKET_BYTES]; } output;
    int protocol_fault;
    int read_hints;
    int step_profile_enabled;
    font_step_profile step_profile;
} font_remote;

static int same_font(const crf_font_identity *a, const crf_font_identity *b)
{
    return a->source_id == b->source_id && a->signature == b->signature &&
        a->generation == b->generation && a->file_size == b->file_size && a->face_index == b->face_index;
}

static void prepare(font_remote *s)
{
    memset(s->pipe.message, 0, sizeof(*s->pipe.message));
    s->pipe.message->version = EPD_COMPUTE_PIPE_VERSION;
    s->pipe.message->values[0] = EPD_FONT_VERSION;
}

static int exchange(font_remote *s, uint32_t op)
{
    int profiling = s->step_profile.active;
    int final = profiling && op == EPD_PIPE_OUTPUT &&
        (((const epd_font_output_t *)s->output.bytes)->flags & EPD_FONT_OUTPUT_FINAL);
    if (profiling) {
        font_step_before(&s->step_profile, op, crf_ticks(), final);
        if (final) {
            s->pipe.message->values[1] = EPD_FONT_STEP_PROFILE_MAGIC;
            memcpy(s->pipe.message->path, &s->step_profile.totals, sizeof(s->step_profile.totals));
        }
    }
    int exchanged = epd_compute_pipe_exchange(&s->pipe, op);
    if (profiling && !final) font_step_after(&s->step_profile, op, crf_ticks());
    if (!exchanged ||
        s->pipe.message->values[0] != EPD_FONT_VERSION || s->pipe.message->operation != op ||
        s->pipe.message->sequence != s->pipe.sequence) {
        if (!epd_compute_fiber_cancelled(s->pipe.fiber)) s->protocol_fault = 1;
        return 0;
    }
    return 1;
}

static int interrupted(void *user)
{
    font_remote *s = (font_remote *)user;
    if (s->protocol_fault || epd_compute_fiber_cancelled(s->pipe.fiber)) return 1;
    if (epd_compute_fiber_should_yield(s->pipe.fiber)) {
        prepare(s);
        if (!exchange(s, EPD_PIPE_IDLE)) return 1;
        if (s->pipe.message->status != EPD_COMPUTE_OK) { s->protocol_fault = 1; return 1; }
    }
    return 0;
}

static void *allocate(void *user, size_t bytes) { (void)user; return epd_compute_malloc(bytes); }
static void release(void *user, void *p) { (void)user; epd_compute_free(p); }

static crf_status provider_read(void *user, uint32_t offset, void *dst, size_t length)
{
    font_remote *s = (font_remote *)user;
    const crf_font_identity *font = &s->open.font;
    uint32_t hint = s->read_hints && !s->ftz && crf_read_is_metadata(s->engine, offset, length) ?
                    EPD_FONT_READ_METADATA : 0;
    epd_compute_pipe_message_t *m = s->pipe.message;
    if (length > EPD_FONT_PACKET_BYTES || offset > s->open.source_bytes ||
        length > s->open.source_bytes - offset) return CRF_IO_ERROR;
#ifdef SF32LB57X
    if (s->open.source_address) {
        if (interrupted(s)) return CRF_CANCELLED;
        const void *source = (const void *)(uintptr_t)
            HCPU_MPI_CBUS_ADDR_2_SBUS_ADDR(s->open.source_address + offset);
        memcpy(dst, source, length);
        return CRF_OK;
    }
#endif
    prepare(s);
    m->handle = s->open.handle;
    m->offset = offset; m->size = s->open.source_bytes; m->length = (uint32_t)length;
    m->values[1] = font->source_id; m->values[2] = font->signature;
    m->values[3] = font->generation; m->values[4] = font->file_size; m->values[5] = font->face_index;
    m->values[7] = hint;
    if (!exchange(s, EPD_PIPE_READ)) return s->protocol_fault ? CRF_IO_ERROR : CRF_CANCELLED;
    if (m->handle != s->open.handle || m->offset != offset || m->size != s->open.source_bytes ||
        m->values[1] != font->source_id || m->values[2] != font->signature ||
        m->values[3] != font->generation || m->values[4] != font->file_size || m->values[5] != font->face_index)
        return CRF_STALE_SOURCE;
    if (m->values[6] == CRF_STALE_SOURCE || m->values[6] == CRF_CANCELLED ||
        m->values[6] == CRF_SHORT_READ || m->values[6] == CRF_IO_ERROR) return (crf_status)m->values[6];
    if (m->status == EPD_COMPUTE_CANCELLED) return CRF_CANCELLED;
    if (m->status != EPD_COMPUTE_OK || m->values[6] != CRF_OK || m->values[7] != hint) return CRF_IO_ERROR;
    if (m->length != length) return CRF_SHORT_READ;
    memcpy(dst, s->pipe.data, length);
    return CRF_OK;
}

static crf_status read_at(void *user, const crf_font_identity *font, uint32_t offset,
                          void *dst, size_t length, size_t *got)
{
    font_remote *s = (font_remote *)user;
    *got = 0;
    if (!same_font(font, &s->open.font)) return CRF_STALE_SOURCE;
    crf_status status = s->ftz ? crf_ftz_read(s->ftz, offset, dst, length) :
                               provider_read(s, offset, dst, length);
    if (status == CRF_OK) *got = length;
    return status;
}

static int publish(font_remote *s, const crf_result *r, uint32_t first, uint32_t flags,
                    uint32_t reserved_records)
{
    epd_font_output_t header;
    uint32_t records_bytes = r->completed * (uint32_t)sizeof(crf_glyph);
    uint32_t payload = (uint32_t)sizeof(header) + records_bytes + r->bitmap_bytes;
    memset(&header, 0, sizeof(header));
    header.version = EPD_FONT_VERSION; header.bytes = payload;
    header.request_id = s->command.request_id;
    header.status = r->status; header.flags = flags;
    header.first = first; header.completed = r->completed; header.total = s->command.count;
    header.bitmap_bytes = r->bitmap_bytes; header.required_bytes = r->required_bytes;
    header.font = s->open.font; header.style = s->command.style; header.metrics = r->metrics;
    if (r->bitmap_bytes) memmove(s->output.bytes + sizeof(header) + records_bytes,
        s->output.bytes + sizeof(header) + reserved_records * sizeof(crf_glyph), r->bitmap_bytes);
    memcpy(s->output.bytes, &header, sizeof(header));
    if (records_bytes) memcpy(s->output.bytes + sizeof(header), s->glyphs, records_bytes);
    prepare(s);
    memcpy(s->pipe.data, s->output.bytes, payload);
    s->pipe.message->length = payload;
    if (!exchange(s, EPD_PIPE_OUTPUT)) return 0;
    if (s->pipe.message->length != payload || s->pipe.message->status != EPD_COMPUTE_OK) {
        s->protocol_fault = 1; return 0;
    }
    return 1;
}

static int valid_command(font_remote *s)
{
    epd_compute_pipe_message_t *m = s->pipe.message;
    epd_font_command_t *c = &s->command;
    if (m->status != EPD_COMPUTE_OK || m->length < sizeof(*c)) return 0;
    memcpy(c, s->pipe.data, sizeof(*c));
    if (c->version != EPD_FONT_VERSION || c->bytes != m->length || !c->request_id || c->flags ||
        !same_font(&c->font, &s->open.font) || c->count > EPD_FONT_BATCH_MAX ||
        m->length != sizeof(*c) + c->count * sizeof(crf_codepoint)) return 0;
    if (c->operation == EPD_FONT_END) return c->count == 0;
    if ((c->operation != EPD_FONT_MEASURE && c->operation != EPD_FONT_RENDER) ||
        !c->style.size_px || c->style.size_px > CRF_MAX_SIZE_PX || c->style.weight > CRF_WEIGHT_LIGHT ||
        c->style.kerning > 1 || c->style.oblique > 1 || c->style.reserved[0] ||
        c->style.reserved[1] || c->style.reserved[2]) return 0;
    memcpy(s->input, s->pipe.data + sizeof(*c), c->count * sizeof(crf_codepoint));
    return 1;
}

static int32_t run(epd_compute_fiber_t *fiber, void *user)
{
    font_remote *s = (font_remote *)user;
    crf_config config;
    crf_result result;
    int32_t status = EPD_COMPUTE_OK;
    s->pipe.fiber = fiber;
    if (s->step_profile_enabled)
        font_step_begin(&s->step_profile, crf_ticks(), CRF_TICKS_PER_SECOND);
    crf_config_init(&config);
    config.alloc = allocate; config.free = release;
    config.read_at = read_at; config.read_user = s;
    config.cancelled = interrupted; config.cancel_user = s;
    config.memory_limit = s->open.memory_bytes - sizeof(*s) - EPD_FONT_STACK_BYTES - FIBER_CONTROL_RESERVE -
                          (s->ftz ? crf_ftz_bytes() : 0);
    config.cache_bytes = s->open.cache_bytes;
    config.operation_limit = s->open.operation_limit;
    memset(&result, 0, sizeof(result));
    result.status = s->ftz ? crf_ftz_open(s->ftz, s->open.source_bytes, s->open.font.file_size,
                                         provider_read, interrupted, s) : CRF_OK;
    if (result.status == CRF_OK) result.status = crf_create(&config, &s->engine);
    if (result.status == CRF_OK) crf_open(s->engine, &s->open.font, &result);
    if (s->protocol_fault) goto failed;
    if (epd_compute_fiber_cancelled(fiber)) goto cancelled;
    if (!publish(s, &result, 0, EPD_FONT_OUTPUT_OPEN | EPD_FONT_OUTPUT_FINAL, 0)) goto failed;
    if (result.status != CRF_OK) { status = EPD_COMPUTE_COMPONENT_ERROR; goto done; }
    for (;;) {
        uint32_t first = 0;
        prepare(s);
        if (!exchange(s, EPD_PIPE_COMMAND)) goto failed;
        if (!valid_command(s)) { status = EPD_COMPUTE_INVALID; goto done; }
        if (s->command.operation == EPD_FONT_END) break;
        if (s->step_profile_enabled)
            font_step_begin(&s->step_profile, crf_ticks(), CRF_TICKS_PER_SECOND);
        do {
            uint32_t remaining = s->command.count - first;
            uint32_t batch_count = remaining;
            uint32_t offset = (uint32_t)sizeof(epd_font_output_t) + remaining * (uint32_t)sizeof(crf_glyph);
            uint32_t flags;
            crf_batch(s->engine, &s->open.font, s->command.style, s->input + first, remaining,
                s->command.operation == EPD_FONT_RENDER, s->glyphs, remaining,
                s->output.bytes + offset, EPD_FONT_PACKET_BYTES - offset, &result);
            if (result.status == CRF_OUTPUT_FULL && !result.completed && remaining > 1) {
                batch_count = 1;
                offset = (uint32_t)(sizeof(epd_font_output_t) + sizeof(crf_glyph));
                crf_batch(s->engine, &s->open.font, s->command.style, s->input + first, 1,
                    s->command.operation == EPD_FONT_RENDER, s->glyphs, 1,
                    s->output.bytes + offset, EPD_FONT_PACKET_BYTES - offset, &result);
            }
            if (s->protocol_fault) goto failed;
            if (epd_compute_fiber_cancelled(fiber)) goto cancelled;
            if (result.status == CRF_CANCELLED) goto cancelled;
            /* OUTPUT_FULL with progress is packetization, not a failed command. */
            if (result.status == CRF_OUTPUT_FULL && result.completed) result.status = CRF_OK;
            flags = result.status != CRF_OK || first + result.completed == s->command.count ? EPD_FONT_OUTPUT_FINAL : 0;
            if (!publish(s, &result, first, flags, batch_count)) goto failed;
            if (result.status == CRF_STALE_SOURCE || result.status == CRF_IO_ERROR ||
                result.status == CRF_SHORT_READ || result.status == CRF_INVALID_FONT ||
                result.status == CRF_NOT_OPEN) { status = EPD_COMPUTE_COMPONENT_ERROR; goto done; }
            first += result.completed;
            if (flags) break;
        } while (first < s->command.count);
    }
    goto done;
failed:
    status = s->protocol_fault ? EPD_COMPUTE_INVALID : EPD_COMPUTE_CANCELLED;
    goto done;
cancelled:
    status = EPD_COMPUTE_CANCELLED;
done:
    if (s->engine) crf_get_stats(s->engine, &s->stats);
    crf_destroy(s->engine); s->engine = NULL;
    prepare(s);
    s->pipe.message->operation = EPD_PIPE_DONE;
    s->pipe.message->status = status;
    return status;
}

static int32_t open_remote(void **state, const epd_compute_wire_t *request)
{
    epd_compute_pipe_remote_t pipe = {0};
    epd_font_open_t config;
    font_remote *s;
    crf_config defaults;
    *state = NULL;
    if (!epd_compute_pipe_bind(&pipe, request) || pipe.message->operation != EPD_PIPE_OPEN ||
        pipe.message->length != sizeof(config)) return EPD_COMPUTE_INVALID;
    memcpy(&config, pipe.data, sizeof(config));
    crf_config_init(&defaults);
    if (!config.memory_bytes) config.memory_bytes = EPD_FONT_MEMORY_DEFAULT;
    if (!config.cache_bytes) config.cache_bytes = defaults.cache_bytes;
    if (!config.operation_limit) config.operation_limit = defaults.operation_limit;
    if (!config.source_bytes && !config.flags) config.source_bytes = config.font.file_size;
    if (config.source_address) {
#if defined(SF32LB57X) && defined(BUILTIN_RESOURCE_START_ADDR) && defined(BUILTIN_RESOURCE_SIZE)
        if (config.font.source_id || config.flags != EPD_FONT_SOURCE_FTZ1 ||
            config.source_address < BUILTIN_RESOURCE_START_ADDR ||
            config.source_address - BUILTIN_RESOURCE_START_ADDR > BUILTIN_RESOURCE_SIZE ||
            config.source_bytes > BUILTIN_RESOURCE_SIZE -
                (config.source_address - BUILTIN_RESOURCE_START_ADDR)) return EPD_COMPUTE_INVALID;
#else
        return EPD_COMPUTE_INVALID;
#endif
    }
    if (config.version != EPD_FONT_VERSION || config.bytes != sizeof(config) ||
        (config.flags & ~EPD_FONT_SOURCE_FTZ1) ||
        (config.flags ? config.source_bytes < 24 || config.source_bytes > CRF_MAX_FILE_BYTES :
                        config.source_bytes != config.font.file_size) ||
        !config.handle || config.font.file_size < 12 || config.font.file_size > CRF_MAX_FILE_BYTES ||
        config.font.face_index > 65535u || config.memory_bytes > EPD_FONT_MEMORY_MAX ||
        config.cache_bytes > CRF_MAX_CACHE_BYTES || config.cache_bytes % CRF_CACHE_BLOCK_BYTES ||
        config.operation_limit > 1048576u || config.memory_bytes < sizeof(*s) + EPD_FONT_STACK_BYTES +
            FIBER_CONTROL_RESERVE + crf_context_bytes() + config.cache_bytes +
            (config.flags ? crf_ftz_bytes() : 0)) return EPD_COMPUTE_INVALID;
    s = (font_remote *)epd_compute_calloc(1, sizeof(*s));
    if (!s) return EPD_COMPUTE_NO_MEMORY;
    s->pipe = pipe; s->open = config;
    s->step_profile_enabled = pipe.message->values[6] == EPD_FONT_STEP_PROFILE_MAGIC;
    s->read_hints = config.font.source_id && !config.flags &&
                    pipe.message->values[7] == EPD_FONT_READ_HINTS_MAGIC;
    if (config.flags) {
        s->ftz = (crf_ftz *)epd_compute_calloc(1, crf_ftz_bytes());
        if (!s->ftz) { epd_compute_free(s); return EPD_COMPUTE_NO_MEMORY; }
    }
    s->pipe.fiber = epd_compute_fiber_create("rfont", EPD_FONT_STACK_BYTES, run, s);
    if (!s->pipe.fiber) { epd_compute_free(s->ftz); epd_compute_free(s); return EPD_COMPUTE_NO_MEMORY; }
    *state = s;
    return EPD_COMPUTE_OK;
}

static int32_t step_remote(void *state, const epd_compute_wire_t *request)
{
    font_remote *s = (font_remote *)state;
    if (!s || !epd_compute_pipe_validate(&s->pipe, request)) return EPD_COMPUTE_INVALID;
    return epd_compute_fiber_step(s->pipe.fiber, request);
}
static int32_t reset_remote(void *state, const epd_compute_wire_t *request)
{ (void)state; (void)request; return EPD_COMPUTE_UNSUPPORTED; }
static void close_remote(void *state, const epd_compute_wire_t *request)
{
    font_remote *s = (font_remote *)state;
    (void)request;
    if (!s) return;
    epd_compute_fiber_close(s->pipe.fiber);
    if (s->engine) crf_get_stats(s->engine, &s->stats);
    crf_destroy(s->engine);
    crf_session_diagnostics *snapshot = &epd_reader_font_last_session;
    ++snapshot->sequence; epd_compute_barrier();
    snapshot->font = s->open.font; snapshot->engine = s->stats;
    crf_ftz_get_stats(s->ftz, &snapshot->ftz);
    snapshot->ftz_workspace = s->ftz ? (uint32_t)crf_ftz_bytes() : 0;
    snapshot->ticks_per_second = CRF_TICKS_PER_SECOND;
    epd_compute_barrier(); ++snapshot->sequence;
    const crf_stats *v = &s->stats;
    (void)v;
    crf_log("font A source=%lu generation=%lu engine_current=%lu engine_peak=%lu logical_reads=%lu logical_bytes=%lu cache_hits=%lu u32sat=1\n",
        (unsigned long)s->open.font.source_id, (unsigned long)s->open.font.generation,
        crf_log32(v->memory_current), crf_log32(v->memory_peak), crf_log32(v->read_calls),
        crf_log32(v->read_bytes), crf_log32(v->cache_hits));
    if (s->ftz) {
        crf_ftz_stats z; crf_ftz_get_stats(s->ftz, &z);
        crf_log("font A source=%lu generation=%lu ftz_workspace=%lu hits=%lu misses=%lu blocks=%lu errors=%lu input_calls=%lu input_bytes=%lu inflate_calls=%lu inflate_ticks=%lu inflate_max=%lu hz=%lu\n",
            (unsigned long)s->open.font.source_id, (unsigned long)s->open.font.generation,
            (unsigned long)crf_ftz_bytes(), crf_log32(z.block_hits), crf_log32(z.block_misses),
            crf_log32(z.blocks), crf_log32(z.errors), crf_log32(z.input_calls), crf_log32(z.input_bytes),
            crf_log32(z.inflate_calls), crf_log32(z.inflate_ticks), (unsigned long)z.inflate_max_ticks,
            (unsigned long)CRF_TICKS_PER_SECOND);
    }
    epd_compute_free(s->ftz);
    epd_compute_free(s);
}
const epd_compute_ops_t epd_reader_font_remote_ops = {open_remote, step_remote, reset_remote, close_remote};
