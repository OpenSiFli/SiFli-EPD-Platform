#include "render_wire.h"
#include "epd_compute_memory.h"
#include <string.h>

typedef struct { rr_kernel kernel; const void *control; } render_remote;
typedef char render_remote_size_limit[sizeof(render_remote) <= RR_REMOTE_STATE_MAX ? 1 : -1];

static int refs_valid(const epd_compute_wire_t *w, uint32_t params_size)
{
    return w && w->region_count >= 1 && w->region_count <= 3 && w->params.region == 0 && !w->params.offset &&
        w->params.length == params_size && w->result.region == 0 &&
        w->result.offset == RR_RESULT_OFFSET && w->result.length == sizeof(rr_result);
}
static int status_wire(int status)
{
    return status == RR_OK ? EPD_COMPUTE_OK : status == RR_MORE ? EPD_COMPUTE_MORE :
        status == RR_CANCELLED ? EPD_COMPUTE_CANCELLED : EPD_COMPUTE_INVALID;
}
static int cancelled(void *user)
{ return (epd_compute_signals((const epd_compute_wire_t *)user) & EPD_COMPUTE_CANCEL) != 0; }

static int32_t open_remote(void **state, const epd_compute_wire_t *w)
{
    rr_open_wire open;
    render_remote *s;
    const void *params;
    void *frame;
    rr_kernel checked;
    *state = NULL;
    if (!refs_valid(w, sizeof(open))) return EPD_COMPUTE_INVALID;
    params = epd_compute_resolve(w, w->params, EPD_COMPUTE_READ);
    if (!params || !epd_compute_resolve(w, w->result, EPD_COMPUTE_WRITE)) return EPD_COMPUTE_INVALID;
    memcpy(&open, params, sizeof(open));
    if (open.version != RR_VERSION || open.bytes != sizeof(open) || open.frame.region != 1 ||
        open.frame.offset || (uint64_t)open.format.stride * open.format.height != open.frame.length)
        return EPD_COMPUTE_INVALID;
    frame = epd_compute_resolve(w, open.frame, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    if (rr_kernel_bind(&checked, open.format, frame, open.frame.length) != RR_OK) return EPD_COMPUTE_INVALID;
    if (cancelled((void *)w)) return EPD_COMPUTE_CANCELLED;
    s = (render_remote *)epd_compute_malloc(sizeof(*s));
    if (!s) return EPD_COMPUTE_NO_MEMORY;
    s->kernel = checked; s->control = params;
    *state = s;
    return EPD_COMPUTE_OK;
}

static int32_t step_remote(void *state, const epd_compute_wire_t *w)
{
    render_remote *s = (render_remote *)state;
    rr_command_wire command;
    rr_result result;
    const void *params, *source = NULL;
    void *out;
    uint32_t count = 1;
    rr_window pixels, sampled;
    size_t needed;
    int status;
    if (!s || !refs_valid(w, sizeof(command))) return EPD_COMPUTE_INVALID;
    out = epd_compute_resolve(w, w->result, EPD_COMPUTE_WRITE);
    params = epd_compute_resolve(w, w->params, EPD_COMPUTE_READ);
    if (!out || !params || params != s->control) return EPD_COMPUTE_INVALID;
    memcpy(&command, params, sizeof(command));
    memset(&result, 0, sizeof(result)); result.version = RR_VERSION; result.bytes = sizeof(result);
    result.id = command.command.id; result.status = RR_INVALID;
    status = EPD_COMPUTE_INVALID;
    int batch = command.command.operation == RR_BATCH_A8;
    if (batch) {
        if (command.command.version != RR_VERSION || command.command.bytes != sizeof(rr_command) ||
            command.source.offset || command.source.length != command.command.src_width) goto done;
        source = epd_compute_resolve(w, command.source, EPD_COMPUTE_READ);
        if (rr_a8_batch_window(s->kernel.frame, source, command.source.length, command.command.width, &pixels) != RR_OK ||
            ((const rr_command *)source)[0].id != command.command.id) goto done;
        needed = command.source.length; sampled = (rr_window){0, (uint32_t)needed};
    } else {
        if (rr_validate_command(&command.command, &needed) != RR_OK) goto done;
        rr_sync_windows(s->kernel.frame, &command.command, s->kernel.active ? s->kernel.row : 0, &pixels, &sampled);
    }
    if (pixels.bytes) {
        if (command.frame.region != count++ || command.frame.offset || command.frame.length != pixels.bytes ||
            epd_compute_resolve(w, command.frame, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE) != s->kernel.pixels + pixels.offset)
            goto done;
    } else if (command.frame.region != EPD_COMPUTE_REGION_NONE || command.frame.offset || command.frame.length) goto done;
    if (needed) {
        if (command.source.region != count++ || command.source.offset || command.source.length != sampled.bytes) goto done;
        source = epd_compute_resolve(w, command.source, EPD_COMPUTE_READ);
        if (!source || (uintptr_t)source < sampled.offset) goto done;
        source = (const void *)((uintptr_t)source - sampled.offset);
    } else if (command.source.region != EPD_COMPUTE_REGION_NONE || command.source.offset || command.source.length) goto done;
    if (w->region_count != count) goto done;
    if (batch) {
        const rr_command *glyph = (const rr_command *)source;
        const uint8_t *bitmap = (const uint8_t *)source + RR_A8_BATCH_HEADER;
        uint32_t done = 0;
        result.rows_total = command.command.width;
        for (; done < command.command.width; ++done) {
            rr_result part;
            size_t bytes = (size_t)glyph[done].src_stride * glyph[done].src_height;
            do { status = rr_kernel_step(&s->kernel, glyph+done, bitmap, bytes, cancelled, (void *)w, &part); }
            while (status == RR_MORE);
            if (status != RR_OK) break;
            bitmap += bytes;
        }
        result.rows_done = result.rows_this_step = done;
        result.status = status; status = status_wire(status);
    } else status = status_wire(rr_kernel_step(&s->kernel, &command.command, source, needed, cancelled, (void *)w, &result));
done:
    memcpy(out, &result, sizeof(result));
    return status;
}
static int32_t reset_remote(void *state, const epd_compute_wire_t *w)
{ (void)state; (void)w; return EPD_COMPUTE_UNSUPPORTED; }
static void close_remote(void *state, const epd_compute_wire_t *w)
{ (void)w; epd_compute_free(state); }
const epd_compute_ops_t epd_reader_render_remote_ops = {open_remote, step_remote, reset_remote, close_remote};
