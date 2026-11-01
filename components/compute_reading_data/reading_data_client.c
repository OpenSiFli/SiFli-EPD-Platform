/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_reading_data.h"
#include "reading_data_wire.h"
#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#ifndef EPD_READING_DATA_TRACE
#define EPD_READING_DATA_TRACE 0
#endif
#ifndef BUILD_DLMODULE
#include <rtm.h>
#endif

struct epd_reading_data_client {
    epd_compute_capability_t capability;
    epd_compute_session_t session;
    epd_compute_wait_t wait;
    epd_compute_buffer_t input, output;
    epd_compute_purpose_t purpose;
    uint32_t sequence, limit;
    bool cancelled, closing;
    int status;
    uint64_t tx_bytes, rx_bytes;
};

int epd_reading_data_status(epd_reading_data_t c)
{
    if (!c) return EPD_COMPUTE_INVALID;
    rt_base_t key = rt_hw_interrupt_disable();
    int status = c->cancelled ? EPD_COMPUTE_CANCELLED : c->status;
    rt_hw_interrupt_enable(key);
    return status;
}

static int record_status(epd_reading_data_t c, int status)
{
    rt_base_t key = rt_hw_interrupt_disable();
    c->status = c->cancelled ? EPD_COMPUTE_CANCELLED : status;
    status = c->status;
    rt_hw_interrupt_enable(key);
    return status;
}

static bool cancelled(void *user)
{ return epd_reading_data_status(user) == EPD_COMPUTE_CANCELLED; }

void epd_reading_data_cancel(epd_reading_data_t c)
{
    if (!c) return;
    rt_base_t key = rt_hw_interrupt_disable();
    c->cancelled = true;
    epd_compute_wait_cancel(c->wait);
    if (c->session) epd_compute_cancel(c->session);
    rt_hw_interrupt_enable(key);
}

int epd_reading_data_create(epd_compute_purpose_t purpose, epd_reading_data_t *out)
{
    if (!out) return EPD_COMPUTE_INVALID;
    *out = NULL;
    if (purpose != EPD_COMPUTE_FOREGROUND && purpose != EPD_COMPUTE_BACKGROUND) return EPD_COMPUTE_INVALID;
    epd_compute_capability_t capability;
    int status = epd_compute_query(EPD_READING_DATA_COMPONENT, EPD_READING_DATA_VERSION, &capability);
    if (status != EPD_COMPUTE_OK) return status;
    if (!capability.ready || !capability.registration.step_limit) return EPD_COMPUTE_UNAVAILABLE;
    if (!(capability.registration.flags & EPD_COMPUTE_STEP_SHARED)) return EPD_COMPUTE_UNSUPPORTED;
    epd_reading_data_t c = rt_calloc(1, sizeof(*c));
    if (!c) return EPD_COMPUTE_NO_MEMORY;
    c->capability = capability; c->purpose = purpose;
    c->limit = capability.registration.step_limit < RD_CHUNK ? capability.registration.step_limit : RD_CHUNK;
    status = epd_compute_wait_create(&c->wait);
    if (status == EPD_COMPUTE_OK &&
        (!epd_compute_buffer_alloc(&c->input, sizeof(rd_request_t) + RD_CHUNK) ||
         !epd_compute_buffer_alloc(&c->output, sizeof(rd_reply_t)))) status = EPD_COMPUTE_NO_MEMORY;
    if (status != EPD_COMPUTE_OK) {
        epd_compute_buffer_free(&c->input); epd_compute_buffer_free(&c->output);
        if (c->wait) { int closed = epd_compute_wait_destroy(c->wait); RT_ASSERT(closed == EPD_COMPUTE_OK); }
        rt_free(c); return status;
    }
    *out = c;
    return EPD_COMPUTE_OK;
}

static int open_client(epd_reading_data_t c, bool wait)
{
    int status = epd_reading_data_status(c);
    if (status != EPD_COMPUTE_OK) return status;
    if (c->closing) return EPD_COMPUTE_INVALID;
    if (c->session) return EPD_COMPUTE_OK;
    epd_compute_requirements_t requirements = {c->purpose, 2};
    epd_compute_request_t request = {0};
    request.params.region = request.result.region = EPD_COMPUTE_REGION_NONE;
    request.cancelled = cancelled; request.context = c;
    epd_compute_session_t opened = NULL;
    status = wait ? epd_compute_open_wait(&c->capability, &requirements, &request, c->wait, &opened) :
                    epd_compute_open(&c->capability, &requirements, &request, &opened);
    rt_base_t key = rt_hw_interrupt_disable();
    c->session = opened;
    if (c->cancelled && opened) epd_compute_cancel(opened);
    rt_hw_interrupt_enable(key);
    if (!wait && status == EPD_COMPUTE_BUSY && !opened) return cancelled(c) ? EPD_COMPUTE_CANCELLED : status;
    return record_status(c, status);
}

int epd_reading_data_try_open(epd_reading_data_t c)
{ return c ? open_client(c, false) : EPD_COMPUTE_INVALID; }

int epd_reading_data_set_purpose(epd_reading_data_t c, epd_compute_purpose_t purpose)
{
    if (!c || (purpose != EPD_COMPUTE_FOREGROUND && purpose != EPD_COMPUTE_BACKGROUND)) return EPD_COMPUTE_INVALID;
    int status = epd_reading_data_status(c);
    if (status != EPD_COMPUTE_OK) return status;
    if (c->closing) return EPD_COMPUTE_INVALID;
    status = c->session ? epd_compute_set_purpose(c->session, purpose) : EPD_COMPUTE_OK;
    if (status == EPD_COMPUTE_OK) c->purpose = purpose;
    return status;
}

int epd_reading_data_close(epd_reading_data_t *client)
{
    if (!client) return EPD_COMPUTE_INVALID;
    epd_reading_data_t c = *client;
    if (!c) return EPD_COMPUTE_OK;
    c->closing = true;
    if (c->session) {
        int status = epd_compute_close(c->session);
        if (status != EPD_COMPUTE_OK) return status;
        c->session = NULL;
    }
    int status = epd_compute_wait_destroy(c->wait);
    if (status != EPD_COMPUTE_OK) return status;
    epd_compute_buffer_free(&c->input); epd_compute_buffer_free(&c->output);
    rt_free(c); *client = NULL;
    return EPD_COMPUTE_OK;
}

static int transfer(epd_reading_data_t c, rd_request_t *q, const void *data, rd_reply_t *reply)
{
    int status = open_client(c, true);
    if (status != EPD_COMPUTE_OK) return status;
    q->version = RD_VERSION; q->size = (uint32_t)sizeof(*q) + q->length;
    q->sequence = c->sequence + 1u;
    if (!q->sequence) q->sequence = 1;
    memcpy(c->input.data, q, sizeof(*q));
    if (q->length) memcpy((char *)c->input.data + sizeof(*q), data, q->length);
    memset(c->output.data, 0, sizeof(*reply));
    epd_compute_region_t regions[2] = {
        epd_compute_buffer_region(&c->input, q->size, EPD_COMPUTE_READ),
        epd_compute_buffer_region(&c->output, 0, EPD_COMPUTE_WRITE)
    };
    regions[0].capacity = (uint32_t)RT_ALIGN(q->size, EPD_COMPUTE_CACHE_LINE);
    epd_compute_request_t request = {regions, 2, {0, 0, q->size}, {1, 0, sizeof(*reply)}, cancelled, c};
    status = epd_compute_step(c->session, &request);
    c->tx_bytes += regions[0].capacity; c->rx_bytes += regions[1].capacity;
    if (cancelled(c)) status = EPD_COMPUTE_CANCELLED;
    if (status == EPD_COMPUTE_OK || status == EPD_COMPUTE_MORE) {
        memcpy(reply, c->output.data, sizeof(*reply));
        if (reply->version != RD_VERSION || reply->size != sizeof(*reply) ||
            reply->sequence != q->sequence || reply->operation != q->operation ||
            reply->backend != RD_BACKEND_ACPU || reply->reserved || reply->processed != q->length ||
            (q->operation != RD_INDEX_DATA && q->operation != RD_CACHE_DATA && status != EPD_COMPUTE_OK) ||
            ((q->operation == RD_INDEX_DATA || q->operation == RD_CACHE_DATA) &&
             status != (q->offset + q->length == q->total ? EPD_COMPUTE_OK : EPD_COMPUTE_MORE)))
            status = EPD_COMPUTE_COMPONENT_ERROR;
    }
    if (status == EPD_COMPUTE_OK || status == EPD_COMPUTE_MORE) c->sequence = q->sequence;
    else { epd_compute_cancel(c->session); return record_status(c, status); }
    return status;
}

static void log_bytes(epd_reading_data_t c, const char *operation, size_t size, uint64_t tx, uint64_t rx)
{
    rt_kprintf("[reading-data] backend=ACPU op=%s bytes=%lu tx_bytes=%lu rx_bytes=%lu\n", operation,
               (unsigned long)size, (unsigned long)(c->tx_bytes - tx), (unsigned long)(c->rx_bytes - rx));
}

int epd_reading_data_hash_skip(epd_reading_data_t c, const void *data, size_t size,
                               size_t skip_offset, size_t skip_size, uint32_t *hash)
{
    if (!c || !hash || (size && !data) || size > UINT32_MAX || skip_offset > size || skip_size > size - skip_offset)
        return EPD_COMPUTE_INVALID;
    int status = epd_reading_data_status(c);
    if (status != EPD_COMPUTE_OK) return status;
    if (c->closing) return EPD_COMPUTE_INVALID;
    uint32_t value = *hash;
#if EPD_READING_DATA_TRACE
    uint64_t tx = c->tx_bytes, rx = c->rx_bytes;
#endif
    for (size_t offset = 0; offset < size;) {
        uint32_t length = (uint32_t)(size - offset < c->limit ? size - offset : c->limit);
        rd_request_t q = {0}; rd_reply_t reply;
        q.operation = RD_HASH; q.length = length; q.seed = value;
        size_t low = skip_offset > offset ? skip_offset : offset;
        size_t high = skip_offset + skip_size < offset + length ? skip_offset + skip_size : offset + length;
        if (high > low) { q.skip_offset = (uint32_t)(low - offset); q.skip_size = (uint32_t)(high - low); }
        status = transfer(c, &q, (const char *)data + offset, &reply);
        if (status != EPD_COMPUTE_OK) return status;
        value = reply.value; offset += length;
    }
    rt_base_t key = rt_hw_interrupt_disable();
    status = c->cancelled ? EPD_COMPUTE_CANCELLED : EPD_COMPUTE_OK;
    if (status == EPD_COMPUTE_OK) *hash = value;
    rt_hw_interrupt_enable(key);
#if EPD_READING_DATA_TRACE
    if (size && status == EPD_COMPUTE_OK) log_bytes(c, "fnv32", size, tx, rx);
#endif
    return status;
}

int epd_reading_data_hash(epd_reading_data_t c, const void *data, size_t size, uint32_t *hash)
{ return epd_reading_data_hash_skip(c, data, size, 0, 0, hash); }

bool epd_reading_data_signature_update(void *c, const void *data, size_t size, uint32_t *hash)
{ return epd_reading_data_hash(c, data, size, hash) == EPD_COMPUTE_OK; }

int epd_reading_data_cache_check(epd_reading_data_t c, const void *blob, size_t size,
    const epd_reading_cache_spec_t *spec, uint32_t *checksum)
{
    if (!c || !blob || !spec || !checksum || size > EPD_READING_CACHE_MAX_BYTES ||
        !spec->source_bytes || spec->source_bytes > 256 || !spec->title_bytes || spec->title_bytes > 128 ||
        (spec->flags & ~EPD_READING_CACHE_EXPLICIT_ID) || size < 48u + spec->source_bytes + spec->title_bytes)
        return EPD_COMPUTE_INVALID;
    rd_request_t q = {0}; rd_reply_t reply;
    uint64_t tx = c->tx_bytes, rx = c->rx_bytes;
    q.operation = RD_CACHE_BEGIN; q.total = (uint32_t)size;
    q.magic = spec->source_bytes; q.index_version = spec->title_bytes; q.layout = spec->flags;
    int status = transfer(c, &q, NULL, &reply);
    if (status != EPD_COMPUTE_OK) return status;
    memset(&q, 0, sizeof(q)); q.operation = RD_CACHE_DATA; q.total = (uint32_t)size;
    while (q.offset < q.total) {
        q.length = q.total - q.offset < c->limit ? q.total - q.offset : c->limit;
        status = transfer(c, &q, (const char *)blob + q.offset, &reply);
        if (status != EPD_COMPUTE_OK && status != EPD_COMPUTE_MORE) return status;
        q.offset += q.length;
    }
    rt_base_t key = rt_hw_interrupt_disable();
    status = c->cancelled ? EPD_COMPUTE_CANCELLED : EPD_COMPUTE_OK;
    if (status == EPD_COMPUTE_OK) *checksum = reply.value;
    rt_hw_interrupt_enable(key);
    if (status == EPD_COMPUTE_OK) log_bytes(c, "cache", size, tx, rx);
    return status;
}

int epd_reading_data_index_check(epd_reading_data_t c, const void *blob, size_t size,
                                 const epd_reading_index_spec_t *spec)
{
    if (!c || !blob || !spec || size < sizeof(epd_reading_index_header_t) || size > UINT32_MAX)
        return EPD_COMPUTE_INVALID;
    rd_request_t q = {0}; rd_reply_t reply;
    uint64_t tx = c->tx_bytes, rx = c->rx_bytes;
    q.operation = RD_INDEX_BEGIN; q.total = (uint32_t)size;
    q.magic = spec->magic; q.index_version = spec->version; q.layout = spec->layout;
    q.end = spec->end; q.page_max = spec->page_max; q.text_limit = spec->text_limit;
    int status = transfer(c, &q, NULL, &reply);
    if (status != EPD_COMPUTE_OK) return status;
    memset(&q, 0, sizeof(q)); q.operation = RD_INDEX_DATA; q.total = (uint32_t)size;
    while (q.offset < q.total) {
        q.length = q.total - q.offset < c->limit ? q.total - q.offset : c->limit;
        status = transfer(c, &q, (const char *)blob + q.offset, &reply);
        if (status != EPD_COMPUTE_OK && status != EPD_COMPUTE_MORE) return status;
        q.offset += q.length;
    }
    if (cancelled(c)) return EPD_COMPUTE_CANCELLED;
    log_bytes(c, "index", size, tx, rx);
    return status;
}

#ifndef BUILD_DLMODULE
RTM_EXPORT(epd_reading_data_create);
RTM_EXPORT(epd_reading_data_try_open);
RTM_EXPORT(epd_reading_data_cancel);
RTM_EXPORT(epd_reading_data_set_purpose);
RTM_EXPORT(epd_reading_data_status);
RTM_EXPORT(epd_reading_data_close);
RTM_EXPORT(epd_reading_data_hash);
RTM_EXPORT(epd_reading_data_hash_skip);
RTM_EXPORT(epd_reading_data_signature_update);
RTM_EXPORT(epd_reading_data_cache_check);
RTM_EXPORT(epd_reading_data_index_check);
#endif
