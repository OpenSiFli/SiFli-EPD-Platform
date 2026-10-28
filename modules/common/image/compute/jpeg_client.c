/* SPDX-License-Identifier: Apache-2.0 */
#include "raster.h"
#include "epd_compute.h"
#include "epd_jpeg_compute.h"
#include <string.h>

/* 0: local path still owns the original decoder, 1: complete, -1: failed. */
int raster_jpeg_remote(raster_decoder_t *decoder, unsigned scale)
{
    epd_compute_capability_t capability;
    if (decoder->forward_only ||
        epd_compute_query(EPD_JPEG_CAPABILITY, EPD_JPEG_VERSION, &capability) != EPD_COMPUTE_OK || !capability.ready)
        return 0;
    uint32_t batch = capability.registration.step_limit;
    if (!batch || batch > (UINT32_MAX - sizeof(epd_jpeg_result_t)) / (sizeof(epd_jpeg_block_t) + EPD_JPEG_MCU_PIXELS))
        return 0;
    epd_compute_buffer_t input = {0}, pool = {0}, params = {0}, output = {0};
    epd_compute_session_t session = NULL;
    int result = 0;
    size_t position = decoder->position;
    if (!epd_compute_buffer_alloc(&input, decoder->file_size) ||
        !epd_compute_buffer_alloc(&pool, EPD_JPEG_POOL_BYTES) ||
        !epd_compute_buffer_alloc(&params, sizeof(epd_jpeg_open_t)) ||
        !epd_compute_buffer_alloc(&output, sizeof(epd_jpeg_result_t) + batch * (sizeof(epd_jpeg_block_t) + EPD_JPEG_MCU_PIXELS)))
        goto cleanup;
    if (!raster_seek(decoder, 0) || !raster_read(decoder, input.data, decoder->file_size))
    { result = -1; goto cleanup; }
    if (raster_cancelled(decoder)) { result = -1; goto cleanup; }
    *(epd_jpeg_open_t *)params.data = (epd_jpeg_open_t){{0, 0, decoder->file_size}, {1, 0, EPD_JPEG_POOL_BYTES}, scale};
    epd_compute_region_t regions[] = {
        epd_compute_buffer_region(&input, decoder->file_size, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&pool, 0, EPD_COMPUTE_WRITE),
        epd_compute_buffer_region(&params, sizeof(epd_jpeg_open_t), EPD_COMPUTE_READ),
        epd_compute_buffer_region(&output, 0, EPD_COMPUTE_WRITE)
    };
    epd_compute_requirements_t requirements = {
        decoder->background ? EPD_COMPUTE_BACKGROUND : EPD_COMPUTE_FOREGROUND, 4};
    epd_compute_request_t request = {regions, 4, {2, 0, sizeof(epd_jpeg_open_t)}, {3, 0, output.capacity}, decoder->cancel, decoder->context};
    int status = epd_compute_open(&capability, &requirements, &request, &session);
    if (status != EPD_COMPUTE_OK)
    {
        if (status != EPD_COMPUTE_BUSY && status != EPD_COMPUTE_UNAVAILABLE && status != EPD_COMPUTE_UNSUPPORTED &&
            status != EPD_COMPUTE_NO_MEMORY) result = -1;
        goto cleanup;
    }
    epd_jpeg_result_t *reply = output.data;
    result = -1;
    if (!reply->width || !reply->height || !reply->msy || reply->msy > 2 ||
        !raster_sink_init_banded(decoder, reply->width >> scale, reply->height >> scale, (reply->msy * 8u) >> scale))
        goto cleanup;
    /* The compressed input and work pool remain remote-owned through CLOSE. */
    request.region_count = 1;
    request.regions = &regions[3];
    request.params = (epd_compute_ref_t){EPD_COMPUTE_REGION_NONE, 0, 0};
    request.result = (epd_compute_ref_t){0, 0, output.capacity};
    do
    {
        if (raster_cancelled(decoder)) epd_compute_cancel(session);
        status = epd_compute_step(session, &request);
        if (status != EPD_COMPUTE_OK && status != EPD_COMPUTE_MORE) goto cleanup;
        if (reply->blocks > batch || reply->bytes < sizeof(*reply) || reply->bytes > output.capacity) goto cleanup;
        uint32_t offset = sizeof(*reply);
        for (uint32_t i = 0; i < reply->blocks; ++i)
        {
            if (sizeof(epd_jpeg_block_t) > reply->bytes - offset) goto cleanup;
            epd_jpeg_block_t *block = (void *)((uint8_t *)reply + offset);
            if (block->left > block->right || block->top > block->bottom ||
                block->right >= decoder->sample_width || block->bottom >= decoder->sample_height ||
                block->bytes != (block->right - block->left + 1) * (block->bottom - block->top + 1) ||
                block->bytes > EPD_JPEG_MCU_PIXELS || block->bytes > reply->bytes - offset - sizeof(*block)) goto cleanup;
            unsigned width = block->right - block->left + 1;
            const uint8_t *pixels = (const void *)(block + 1);
            for (uint32_t y = block->top; y <= block->bottom; ++y, pixels += width)
                raster_sink_row(decoder, block->left, y, 1, pixels, width, RASTER_GRAY8);
            if (block->right + 1u == decoder->sample_width)
                raster_sink_end_band(decoder, block->bottom + 1u);
            offset += (sizeof(*block) + block->bytes + 3u) & ~3u;
            if (offset > reply->bytes) goto cleanup;
        }
    } while (status == EPD_COMPUTE_MORE);
    result = 1;
cleanup:
    if (session)
    {
        int closed = epd_compute_close(session);
        RT_ASSERT(closed == EPD_COMPUTE_OK);
    }
    epd_compute_buffer_free(&output);
    epd_compute_buffer_free(&params);
    epd_compute_buffer_free(&pool);
    epd_compute_buffer_free(&input);
    if (!raster_seek(decoder, position) || decoder->io_error || raster_cancelled(decoder)) result = -1;
    return result;
}
