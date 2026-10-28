/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>
#include <string.h>
#include "epd_tjpgd.h"
#include "epd_jpeg_compute.h"
#include "epd_compute_cache.h"

typedef struct
{
    JDEC jpeg;
    const uint8_t *input;
    uint32_t length, position;
    epd_compute_region_t pool;
    const epd_compute_wire_t *request;
    epd_jpeg_result_t *result;
    uint32_t result_capacity;
} jpeg_remote_t;

static size_t input_data(JDEC *jpeg, uint8_t *data, size_t size)
{
    jpeg_remote_t *s = jpeg->device;
    if (size > s->length - s->position) size = s->length - s->position;
    if (data) memcpy(data, s->input + s->position, size);
    s->position += size;
    return size;
}

static int output_block(JDEC *jpeg, void *pixels, JRECT *rect)
{
    jpeg_remote_t *s = jpeg->device;
    epd_jpeg_result_t *r = s->result;
    uint32_t bytes = (rect->right - rect->left + 1u) * (rect->bottom - rect->top + 1u);
    uint32_t record_bytes = (sizeof(epd_jpeg_block_t) + bytes + 3u) & ~3u;
    if (record_bytes > s->result_capacity - r->bytes) return 0;
    epd_jpeg_block_t *block = (void *)((uint8_t *)r + r->bytes);
    *block = (epd_jpeg_block_t){rect->left, rect->top, rect->right, rect->bottom, bytes};
    memcpy(block + 1, pixels, bytes);
    r->bytes += record_bytes;
    ++r->blocks;
    return 1;
}

static JDCONTROL control(JDEC *jpeg)
{
    jpeg_remote_t *s = jpeg->device;
    epd_compute_barrier();
    uint32_t signals = epd_compute_signals(s->request);
    return signals & EPD_COMPUTE_CANCEL ? JD_CANCEL : signals & EPD_COMPUTE_YIELD ? JD_YIELD : JD_CONTINUE;
}

static int32_t open_jpeg(void **state, const epd_compute_wire_t *request)
{
    const epd_jpeg_open_t *params = epd_compute_resolve(request, request->params, EPD_COMPUTE_READ);
    epd_jpeg_result_t *result = epd_compute_resolve(request, request->result, EPD_COMPUTE_WRITE);
    if (!params || !result || request->params.length != sizeof(*params) ||
        request->result.length < sizeof(*result) || params->scale > 3) return EPD_COMPUTE_INVALID;
    const uint8_t *input = epd_compute_resolve(request, params->input, EPD_COMPUTE_READ);
    void *pool = epd_compute_resolve(request, params->pool, EPD_COMPUTE_WRITE);
    if (!input || !pool || !params->input.length || params->pool.length < EPD_JPEG_POOL_BYTES)
        return EPD_COMPUTE_INVALID;
    jpeg_remote_t *s = rt_calloc(1, sizeof(*s));
    if (!s) return EPD_COMPUTE_NO_MEMORY;
    s->input = input;
    s->length = params->input.length;
    const epd_compute_region_t *regions = (const void *)(uintptr_t)request->regions;
    s->pool = regions[params->pool.region];
    JRESULT code = jd_prepare(&s->jpeg, input_data, pool, params->pool.length, s);
    if (code == JDR_OK) code = jd_decomp_init(&s->jpeg, params->scale);
    *result = (epd_jpeg_result_t){code, s->jpeg.width, s->jpeg.height, s->jpeg.msy, 0, sizeof(*result), 0};
    if (code != JDR_OK) { rt_free(s); return EPD_COMPUTE_COMPONENT_ERROR; }
    *state = s;
    return EPD_COMPUTE_OK;
}

static int32_t step_jpeg(void *state, const epd_compute_wire_t *request)
{
    jpeg_remote_t *s = state;
    epd_jpeg_result_t *result = epd_compute_resolve(request, request->result, EPD_COMPUTE_WRITE);
    if (!result || request->result.length < sizeof(*result)) return EPD_COMPUTE_INVALID;
    uint32_t room = request->result.length - sizeof(*result);
    uint32_t max_mcus = room / (sizeof(epd_jpeg_block_t) + EPD_JPEG_MCU_PIXELS);
    if (max_mcus > request->step_limit) max_mcus = request->step_limit;
    if (!max_mcus) return EPD_COMPUTE_INVALID;
    *result = (epd_jpeg_result_t){0, s->jpeg.width, s->jpeg.height, s->jpeg.msy, 0, sizeof(*result), 0};
    s->request = request;
    s->result = result;
    s->result_capacity = request->result.length;
    JDSTEP step;
    JRESULT code = jd_decomp_step(&s->jpeg, output_block, max_mcus,
                                  max_mcus * EPD_JPEG_MCU_PIXELS, control, &step);
    s->request = NULL;
    s->result = NULL;
    result->code = code;
    result->state = step.state;
    if (code == JDR_INTR) return EPD_COMPUTE_CANCELLED;
    if (code != JDR_OK) return EPD_COMPUTE_COMPONENT_ERROR;
    return step.state == JD_STEP_DONE ? EPD_COMPUTE_OK : EPD_COMPUTE_MORE;
}

static void close_jpeg(void *state, const epd_compute_wire_t *request)
{
    (void)request;
    jpeg_remote_t *s = state;
    /* The pool was exclusively remote-owned after OPEN. Complete its final
     * writes before HCPU returns it to the original allocator. */
    epd_compute_cache_clean(s->pool.address, s->pool.capacity);
    epd_compute_cache_invalidate(s->pool.address, s->pool.capacity);
    rt_free(s);
}

const epd_compute_ops_t epd_jpeg_remote_ops = {open_jpeg, step_jpeg, NULL, close_jpeg};
