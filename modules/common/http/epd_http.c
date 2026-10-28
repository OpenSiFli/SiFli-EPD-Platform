/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_http_internal.h"
#include <miniz.h>
#include <string.h>

static bool gzip_decode(http_reader_t *reader, char **body, size_t *size)
{
    const unsigned char *src = (const unsigned char *)*body;
    size_t offset = 10, end, expected, output_used = 0;
    uint32_t expected_crc = 0, expected_size = 0;
    char *output = NULL;
    tinfl_decompressor *decoder = NULL;
    bool ok = false;
    reader->response->stage = "gzip-header";
    if (*size < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8 || (src[3] & 0xe0))
        return fail(reader, "gzip-header", -RT_EINVAL);
    end = *size - 8;
    if (src[3] & 4)
    {
        if (end < 12) return fail(reader, "gzip-header", -RT_EINVAL);
        offset = 12 + src[10] + ((size_t)src[11] << 8);
    }
    for (unsigned flag = 8; flag <= 16; flag *= 2)
    {
        if (!(src[3] & flag)) continue;
        while (offset < end && src[offset])
        {
            if (expired(reader)) return false;
            ++offset;
        }
        if (offset >= end) return fail(reader, "gzip-header", -RT_EINVAL);
        ++offset;
    }
    if (offset >= end || ((src[3] & 2) && end - offset < 2))
        return fail(reader, "gzip-header", -RT_EINVAL);
    if (src[3] & 2)
    {
        uint32_t crc = 0;
        for (size_t i = 0; i < offset;)
        {
            size_t count = offset - i > IO_SLICE ? IO_SLICE : offset - i;
            if (expired(reader)) return false;
            crc = (uint32_t)mz_crc32(crc, src + i, count);
            i += count;
        }
        if ((crc & 0xffffu) != (src[offset] | ((uint32_t)src[offset + 1] << 8)))
            return fail(reader, "gzip-header-crc", -RT_EINVAL);
        offset += 2;
    }
    for (unsigned i = 0; i < 4; ++i)
    {
        expected_crc |= (uint32_t)src[end + i] << (8 * i);
        expected_size |= (uint32_t)src[end + 4 + i] << (8 * i);
    }
    expected = (size_t)expected_size;
    if (expected > reader->limit) return fail(reader, "body-limit", -RT_EFULL);
    if (expired(reader)) return false;
    output = epd_app_alloc(expected + 1, EPD_APP_PSRAM);
    if (!output) return fail(reader, "gzip-output-alloc", -RT_ENOMEM);
    decoder = epd_app_alloc(sizeof(*decoder), EPD_APP_PSRAM);
    if (!decoder) { fail(reader, "gzip-decoder-alloc", -RT_ENOMEM); goto finish; }
    tinfl_init(decoder);
    reader->response->stage = "gzip-inflate";
    uint32_t crc = 0;
    for (;;)
    {
        size_t input_size = end - offset;
        size_t output_size = expected + 1 - output_used;
        if (expired(reader)) goto finish;
        if (input_size > IO_SLICE) input_size = IO_SLICE;
        if (output_size > IO_SLICE) output_size = IO_SLICE;
        unsigned flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if (input_size < end - offset) flags |= TINFL_FLAG_HAS_MORE_INPUT;
        tinfl_status result = tinfl_decompress(decoder, src + offset, &input_size,
            (unsigned char *)output, (unsigned char *)output + output_used, &output_size,
            flags);
        crc = (uint32_t)mz_crc32(crc, (unsigned char *)output + output_used, output_size);
        output_used += output_size;
        offset += input_size;
        if (output_used > expected) { fail(reader, "gzip-length", -RT_EINVAL); goto finish; }
        if (result == TINFL_STATUS_DONE) break;
        if ((result != TINFL_STATUS_HAS_MORE_OUTPUT && result != TINFL_STATUS_NEEDS_MORE_INPUT) ||
            (!input_size && !output_size))
        { fail(reader, "gzip-inflate", (int)result); goto finish; }
    }
    if (output_used != expected || offset != end) { fail(reader, "gzip-length", -RT_EINVAL); goto finish; }
    if (crc != expected_crc) { fail(reader, "gzip-crc", -RT_EINVAL); goto finish; }
    if (expired(reader)) goto finish;
    output[output_used] = '\0';
    epd_app_free(*body);
    *body = output;
    *size = output_used;
    output = NULL;
    ok = true;
finish:
    epd_app_free(decoder);
    epd_app_free(output);
    return ok;
}

void epd_http_response_clear(epd_http_response_t *response)
{
    if (!response) return;
    epd_app_free(response->body);
    memset(response, 0, sizeof(*response));
}

bool epd_http_perform(const epd_http_request_t *request, epd_http_response_t *response)
{
    http_reader_t reader = {0};
    char *body = NULL;
    size_t used = 0;
    bool gzip = false, complete;
    if (!response) return false;
    memset(response, 0, sizeof(*response));
    reader.response = response;
    reader.request = request;
    complete = epd_http_core_perform(&reader, &body, &used, &gzip);
    if (complete)
    {
        body[used] = '\0';
        if (gzip || (used >= 2 && (unsigned char)body[0] == 0x1f && (unsigned char)body[1] == 0x8b))
            complete = gzip_decode(&reader, &body, &used);
        if (complete) complete = !expired(&reader);
    }
    if (!complete)
    {
        epd_app_free(body);
        if (!response->detail) response->detail = -RT_ERROR;
        return false;
    }
    response->body = body;
    response->size = used;
    response->stage = "complete";
    response->detail = 0;
    return true;
}
