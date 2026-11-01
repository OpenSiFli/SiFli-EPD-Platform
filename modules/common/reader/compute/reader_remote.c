/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_wire.h"
#include "epd_compute_protocol.h"
#include "epd_compute_memory.h"
#include <string.h>

typedef struct {
    epd_reader_batch_t *batch;
    uint32_t epoch, sequence, record;
} reader_slot_t;
typedef struct {
    reader_slot_t slots[READER_SLOTS];
    epd_reader_batch_t work;
    epd_reader_page_t before;
} reader_session_t;

static int32_t open_reader(void **state, const epd_compute_wire_t *request)
{
    (void)request;
    *state = epd_compute_calloc(1, sizeof(reader_session_t));
    return *state ? EPD_COMPUTE_OK : EPD_COMPUTE_NO_MEMORY;
}

static int32_t step_reader(void *state, const epd_compute_wire_t *request)
{
    reader_session_t *s = state;
    if (!s || !request || request->region_count != 2 || request->params.region != 0 || request->params.offset ||
        request->result.region != 1 || request->result.offset || request->params.length < sizeof(reader_wire_request_t) ||
        request->params.length > READER_TX_MAX || request->result.length < sizeof(reader_wire_reply_t) ||
        request->result.length > READER_RX_MAX || !request->step_limit) return EPD_COMPUTE_INVALID;
    const reader_wire_request_t *q = epd_compute_resolve(request, request->params, EPD_COMPUTE_READ);
    void *reply = epd_compute_resolve(request, request->result, EPD_COMPUTE_WRITE);
    if (!q || !reply || q->size != request->params.length || q->version != READER_WIRE_VERSION ||
        q->slot >= READER_SLOTS || !q->epoch || !q->sequence || q->limit > request->step_limit) return EPD_COMPUTE_INVALID;
    if (epd_compute_signals(request) & EPD_COMPUTE_CANCEL) return EPD_COMPUTE_CANCELLED;
    reader_slot_t *slot = &s->slots[q->slot];
    if (q->flags & READER_SEED_PAGE) {
        if (!(q->flags & READER_SEED_RECORD) || q->sequence != 1 ||
            (slot->batch && (int32_t)(q->epoch - slot->epoch) <= 0)) return EPD_COMPUTE_INVALID;
        memset(&s->work, 0, sizeof(s->work));
    } else {
        uint32_t next = slot->sequence + 1;
        if (!next) ++next;
        if (!slot->batch || q->epoch != slot->epoch || q->sequence != next ||
            q->generation != slot->batch->generation ||
            (!(q->flags & READER_SEED_RECORD) && q->cursor.record != slot->record)) return EPD_COMPUTE_INVALID;
        if (!epd_reader_batch_copy(&s->work, slot->batch)) return EPD_COMPUTE_INVALID;
    }
    if (!reader_wire_load(&s->work, q) || !epd_reader_page_copy(&s->before, &s->work.page)) return EPD_COMPUTE_INVALID;
    if (epd_reader_layout(&s->work)) return EPD_COMPUTE_COMPONENT_ERROR;
    if (epd_compute_signals(request) & EPD_COMPUTE_CANCEL) return EPD_COMPUTE_CANCELLED;
    if (!slot->batch) {
        slot->batch = epd_compute_calloc(1, sizeof(*slot->batch));
        if (!slot->batch) return EPD_COMPUTE_NO_MEMORY;
    }
    if (!reader_wire_reply(reply, request->result.length, q, &s->work, &s->before)) return EPD_COMPUTE_INVALID;
    if (!epd_reader_batch_copy(slot->batch, &s->work)) return EPD_COMPUTE_INVALID;
    slot->epoch = q->epoch; slot->sequence = q->sequence;
    if (q->flags & READER_SEED_RECORD) slot->record = q->cursor.record;
    return EPD_COMPUTE_OK;
}

static void close_reader(void *state, const epd_compute_wire_t *request)
{
    (void)request;
    reader_session_t *s = state;
    if (!s) return;
    for (unsigned i = 0; i < READER_SLOTS; ++i) epd_compute_free(s->slots[i].batch);
    epd_compute_free(s);
}

const epd_compute_ops_t epd_reader_remote_ops = {open_reader, step_reader, NULL, close_reader};
