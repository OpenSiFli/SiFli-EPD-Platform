#include "epd_reader_render.h"
#include "render_wire.h"
#include <rtthread.h>
#include <string.h>
#ifndef BUILD_DLMODULE
#include <rtm.h>
#endif

struct epd_reader_render_client {
    epd_compute_session_t session;
    epd_compute_buffer_t frame, control, input;
    rr_frame format;
    uint32_t id, batch_count, batch_bytes;
    int failed;
};

static int buffer_valid(const epd_compute_buffer_t *b, size_t bytes)
{
    return b && b->data && !((uintptr_t)b->data & (EPD_COMPUTE_CACHE_LINE - 1u)) &&
        b->capacity && !(b->capacity & (EPD_COMPUTE_CACHE_LINE - 1u)) && bytes <= b->capacity;
}

int epd_reader_render_close(epd_reader_render_t c)
{
    int status;
    if (!c) return EPD_COMPUTE_OK;
    c->failed = 1;
    status = c->session ? epd_compute_close(c->session) : EPD_COMPUTE_OK;
    if (status != EPD_COMPUTE_OK) return status;
    epd_compute_buffer_free(&c->control);
    epd_compute_buffer_free(&c->input);
    rt_free(c);
    return EPD_COMPUTE_OK;
}

int epd_reader_render_shutdown(epd_reader_render_t *client)
{
    int status;
    if (!client) return EPD_COMPUTE_INVALID;
    if (!*client) return EPD_COMPUTE_OK;
    (*client)->failed = 1;
    if ((*client)->session) epd_compute_cancel((*client)->session);
    status = epd_reader_render_close(*client);
    if (status == EPD_COMPUTE_OK) *client = NULL;
    return status;
}

static int open_client(const epd_compute_buffer_t *frame, rr_frame format,
                       epd_compute_purpose_t purpose, epd_compute_wait_t wait,
                       epd_reader_render_t *out)
{
    epd_reader_render_t c;
    epd_compute_capability_t capability;
    epd_compute_requirements_t requirements = {purpose, 3};
    epd_compute_region_t regions[3];
    epd_compute_request_t request = {0};
    rr_open_wire open;
    uint64_t bytes = (uint64_t)format.stride * format.height;
    int status;
    if (!out) return EPD_COMPUTE_INVALID;
    *out = NULL;
    if (!format.width || !format.height || format.width > RR_MAX_DIM || format.height > RR_MAX_DIM ||
        format.format > RR_FORMAT_L8 || format.stride < format.width * rr_pixel_bytes(format) || (format.stride & 3u) || bytes > RR_MAX_BUFFER_BYTES ||
        !buffer_valid(frame, (size_t)bytes) ||
        (purpose != EPD_COMPUTE_FOREGROUND && purpose != EPD_COMPUTE_BACKGROUND)) return EPD_COMPUTE_INVALID;
    status = epd_compute_query(EPD_READER_RENDER_COMPONENT, EPD_READER_RENDER_VERSION, &capability);
    if (status != EPD_COMPUTE_OK) return status;
    if (!capability.ready) return EPD_COMPUTE_UNAVAILABLE;
    c = (epd_reader_render_t)rt_calloc(1, sizeof(*c));
    if (!c) return EPD_COMPUTE_NO_MEMORY;
    c->frame = *frame; c->format = format;
    if (!epd_compute_buffer_alloc(&c->control, RR_CONTROL_BYTES) ||
        !epd_compute_buffer_alloc(&c->input, RR_INPUT_BYTES)) {
        epd_reader_render_close(c); return EPD_COMPUTE_NO_MEMORY;
    }
    memset(c->control.data, 0, RR_CONTROL_BYTES);
    memset(&open, 0, sizeof(open)); open.version = RR_VERSION; open.bytes = sizeof(open);
    open.frame = (epd_compute_ref_t){1, 0, (uint32_t)bytes}; open.format = format;
    memcpy(c->control.data, &open, sizeof(open));
    regions[0] = epd_compute_buffer_region(&c->control, RR_CONTROL_BYTES, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    regions[1] = epd_compute_buffer_region(&c->frame, (uint32_t)bytes, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    regions[2] = epd_compute_buffer_region(&c->input, 0, EPD_COMPUTE_READ);
    request.regions = regions; request.region_count = 3;
    request.params = (epd_compute_ref_t){0, 0, sizeof(open)};
    request.result = (epd_compute_ref_t){0, RR_RESULT_OFFSET, sizeof(rr_result)};
    status = wait ? epd_compute_open_wait(&capability, &requirements, &request, wait, &c->session) :
        epd_compute_open(&capability, &requirements, &request, &c->session);
    if (status != EPD_COMPUTE_OK) {
        if (c->session) { c->failed = 1; *out = c; }
        else epd_reader_render_close(c);
        return status;
    }
    *out = c;
    return EPD_COMPUTE_OK;
}

int epd_reader_render_open(const epd_compute_buffer_t *frame, rr_frame format,
                           epd_compute_purpose_t purpose, epd_reader_render_t *out)
{ return open_client(frame, format, purpose, NULL, out); }

int epd_reader_render_open_wait(const epd_compute_buffer_t *frame, rr_frame format,
                                epd_compute_purpose_t purpose, epd_compute_wait_t wait,
                                epd_reader_render_t *out)
{
    if (!wait) { if (out) *out = NULL; return EPD_COMPUTE_INVALID; }
    return open_client(frame, format, purpose, wait, out);
}

int epd_reader_render_command_shared(epd_reader_render_t c, const rr_command *command,
                                     const epd_compute_buffer_t *source, uint32_t bytes,
                                     bool (*cancelled)(void *), void *user, rr_result *out)
{
    rr_command_wire p;
    rr_result result;
    epd_compute_region_t regions[3];
    epd_compute_request_t request = {0};
    size_t expected;
    uint32_t previous = 0, total = UINT32_MAX, step;
    int status;
    if (out) memset(out, 0, sizeof(*out));
    if (!c || !command || c->failed) return EPD_COMPUTE_INVALID;
    if (c->batch_count) return EPD_COMPUTE_BUSY;
    memset(&p, 0, sizeof(p)); p.command = *command;
    p.command.version = RR_VERSION; p.command.bytes = sizeof(p.command);
    p.command.id = c->id + 1u; if (!p.command.id) p.command.id = 1;
    if (rr_validate_command(&p.command, &expected) != RR_OK ||
        bytes != expected || (bytes && !buffer_valid(source, bytes))) return EPD_COMPUTE_INVALID;
    if (cancelled && cancelled(user)) return EPD_COMPUTE_CANCELLED;
    if (bytes) {
        uintptr_t a = (uintptr_t)c->frame.data, b = (uintptr_t)source->data;
        if (source->capacity > UINTPTR_MAX - b || c->frame.capacity > UINTPTR_MAX - a ||
            (a < b + source->capacity && b < a + c->frame.capacity)) return EPD_COMPUTE_INVALID;
    }
    regions[0] = epd_compute_buffer_region(&c->control, RR_CONTROL_BYTES, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    request.regions = regions; request.region_count = 3;
    request.params = (epd_compute_ref_t){0, 0, sizeof(p)};
    request.result = (epd_compute_ref_t){0, RR_RESULT_OFFSET, sizeof(result)};
    request.cancelled = cancelled; request.context = user;
    for (step = 0; step <= (RR_MAX_DIM + RR_ROWS_PER_STEP - 1u) / RR_ROWS_PER_STEP; ++step) {
        rr_window pixels, sampled;
        rr_sync_windows(c->format, &p.command, previous, &pixels, &sampled);
        if (pixels.offset > c->frame.capacity || pixels.bytes > c->frame.capacity - pixels.offset ||
            (bytes && (sampled.offset > source->capacity || sampled.bytes > source->capacity - sampled.offset)))
            return EPD_COMPUTE_INVALID;
        request.region_count = 1;
        p.frame = p.source = (epd_compute_ref_t){EPD_COMPUTE_REGION_NONE, 0, 0};
        if (pixels.bytes) {
            regions[1] = epd_compute_buffer_region(&c->frame, pixels.bytes, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
            regions[1].address += pixels.offset; regions[1].capacity = pixels.bytes;
            p.frame = (epd_compute_ref_t){request.region_count++, 0, pixels.bytes};
        }
        if (bytes) {
            regions[request.region_count] = epd_compute_buffer_region(source, sampled.bytes, EPD_COMPUTE_READ);
            regions[request.region_count].address += sampled.offset;
            regions[request.region_count].capacity = sampled.bytes;
            p.source = (epd_compute_ref_t){request.region_count++, 0, sampled.bytes};
        }
        memcpy(c->control.data, &p, sizeof(p));
        memset((uint8_t *)c->control.data + RR_RESULT_OFFSET, 0, sizeof(result));
        status = epd_compute_step(c->session, &request);
        memcpy(&result, (uint8_t *)c->control.data + RR_RESULT_OFFSET, sizeof(result));
        if (out) *out = result;
        if (status != EPD_COMPUTE_OK && status != EPD_COMPUTE_MORE) {
            c->failed = 1; return status;
        }
        if (result.version != RR_VERSION || result.bytes != sizeof(result) || result.id != p.command.id ||
            result.status != (status == EPD_COMPUTE_OK ? RR_OK : RR_MORE) ||
            result.rows_total > RR_MAX_DIM || result.rows_done > result.rows_total ||
            result.rows_done < previous || result.rows_this_step != result.rows_done - previous ||
            result.rows_this_step > RR_ROWS_PER_STEP || (total != UINT32_MAX && result.rows_total != total) ||
            (status == EPD_COMPUTE_MORE && (!result.rows_this_step || result.rows_done == result.rows_total)) ||
            (status == EPD_COMPUTE_OK && result.rows_done != result.rows_total)) break;
        total = result.rows_total; previous = result.rows_done;
        if (status == EPD_COMPUTE_OK) { c->id = p.command.id; return status; }
    }
    c->failed = 1;
    epd_compute_cancel(c->session);
    return EPD_COMPUTE_INVALID;
}

int epd_reader_render_command(epd_reader_render_t c, const rr_command *command,
                              const void *source, size_t bytes,
                              bool (*cancelled)(void *), void *user, rr_result *out)
{
    if (!c || c->failed || bytes > RR_INPUT_BYTES || (bytes && !source)) return EPD_COMPUTE_INVALID;
    if (c->batch_count) return EPD_COMPUTE_BUSY;
    if (cancelled && cancelled(user)) return EPD_COMPUTE_CANCELLED;
    if (bytes) memcpy(c->input.data, source, bytes);
    return epd_reader_render_command_shared(c, command, &c->input, (uint32_t)bytes, cancelled, user, out);
}

int epd_reader_render_a8_enqueue(epd_reader_render_t c, const rr_command *command,
                                 const void *source, size_t bytes)
{
    rr_command q;
    size_t needed;
    uint32_t used;
    if (!c || c->failed || !command || !source) return EPD_COMPUTE_INVALID;
    q = *command; q.version = RR_VERSION; q.bytes = sizeof(q);
    q.id = c->batch_count ? ((rr_command *)c->input.data)[c->batch_count-1].id + 1u : c->id + 1u;
    if (!q.id) q.id = 1;
    if (q.operation != RR_BLIT_A8 || rr_validate_command(&q, &needed) != RR_OK || bytes != needed)
        return EPD_COMPUTE_INVALID;
    used = c->batch_count ? c->batch_bytes : RR_A8_BATCH_HEADER;
    if (c->batch_count == RR_A8_BATCH_MAX || q.height > RR_A8_BATCH_HEIGHT || bytes > RR_INPUT_BYTES - used)
        return EPD_COMPUTE_MORE;
    /* Reject aliasing before either descriptor or pixel copy overwrites input. */
    uintptr_t p = (uintptr_t)source, a = (uintptr_t)c->input.data, b = (uintptr_t)c->frame.data;
    if (bytes > UINTPTR_MAX - p || (p < a+c->input.capacity && a < p+bytes) ||
        (p < b+c->frame.capacity && b < p+bytes)) return EPD_COMPUTE_INVALID;
    ((rr_command *)c->input.data)[c->batch_count++] = q;
    memcpy((uint8_t *)c->input.data + used, source, bytes);
    c->batch_bytes = used + (uint32_t)bytes;
    return EPD_COMPUTE_OK;
}

int epd_reader_render_a8_flush(epd_reader_render_t c, bool (*cancelled)(void *), void *user)
{
    rr_command_wire p = {0}; rr_result result; rr_window pixels;
    epd_compute_region_t regions[3]; epd_compute_request_t request = {0};
    int status;
    if (!c || c->failed) return EPD_COMPUTE_INVALID;
    if (!c->batch_count) return EPD_COMPUTE_OK;
    if (cancelled && cancelled(user)) return EPD_COMPUTE_CANCELLED;
    if (rr_a8_batch_window(c->format, c->input.data, c->batch_bytes, c->batch_count, &pixels) != RR_OK ||
        pixels.offset > c->frame.capacity || pixels.bytes > c->frame.capacity - pixels.offset)
        return EPD_COMPUTE_INVALID;
    rr_command_init(&p.command, RR_BATCH_A8, c->format);
    p.command.id = ((rr_command *)c->input.data)[0].id;
    p.command.width = c->batch_count; p.command.src_width = c->batch_bytes;
    regions[0] = epd_compute_buffer_region(&c->control, RR_CONTROL_BYTES, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
    request.region_count = 1; p.frame = (epd_compute_ref_t){EPD_COMPUTE_REGION_NONE, 0, 0};
    if (pixels.bytes) {
        regions[1] = epd_compute_buffer_region(&c->frame, pixels.bytes, EPD_COMPUTE_READ | EPD_COMPUTE_WRITE);
        regions[1].address += pixels.offset; regions[1].capacity = pixels.bytes;
        p.frame = (epd_compute_ref_t){request.region_count++, 0, pixels.bytes};
    }
    regions[request.region_count] = epd_compute_buffer_region(&c->input, c->batch_bytes, EPD_COMPUTE_READ);
    regions[request.region_count].capacity = (c->batch_bytes + 31u) & ~31u;
    p.source = (epd_compute_ref_t){request.region_count++, 0, c->batch_bytes};
    request.regions = regions; request.params = (epd_compute_ref_t){0, 0, sizeof(p)};
    request.result = (epd_compute_ref_t){0, RR_RESULT_OFFSET, sizeof(result)};
    request.cancelled = cancelled; request.context = user;
    memcpy(c->control.data, &p, sizeof(p));
    memset((uint8_t *)c->control.data + RR_RESULT_OFFSET, 0, sizeof(result));
    status = epd_compute_step(c->session, &request);
    memcpy(&result, (uint8_t *)c->control.data + RR_RESULT_OFFSET, sizeof(result));
    if (status == EPD_COMPUTE_OK && result.version == RR_VERSION && result.bytes == sizeof(result) &&
        result.id == p.command.id && result.status == RR_OK && result.rows_done == c->batch_count &&
        result.rows_total == c->batch_count && result.rows_this_step == c->batch_count) {
        c->id = ((rr_command *)c->input.data)[c->batch_count-1].id;
        c->batch_count = c->batch_bytes = 0; return status;
    }
    c->failed = 1; epd_compute_cancel(c->session);
    return status == EPD_COMPUTE_OK || status == EPD_COMPUTE_MORE ? EPD_COMPUTE_INVALID : status;
}

void epd_reader_render_cancel(epd_reader_render_t c) { if (c) epd_compute_cancel(c->session); }
int epd_reader_render_set_purpose(epd_reader_render_t c, epd_compute_purpose_t purpose)
{ return c ? epd_compute_set_purpose(c->session, purpose) : EPD_COMPUTE_INVALID; }

#ifndef BUILD_DLMODULE
RTM_EXPORT(rr_command_init);
RTM_EXPORT(epd_reader_render_open);
RTM_EXPORT(epd_reader_render_open_wait);
RTM_EXPORT(epd_reader_render_command);
RTM_EXPORT(epd_reader_render_a8_enqueue);
RTM_EXPORT(epd_reader_render_a8_flush);
RTM_EXPORT(epd_reader_render_command_shared);
RTM_EXPORT(epd_reader_render_cancel);
RTM_EXPORT(epd_reader_render_set_purpose);
RTM_EXPORT(epd_reader_render_close);
RTM_EXPORT(epd_reader_render_shutdown);
#endif
