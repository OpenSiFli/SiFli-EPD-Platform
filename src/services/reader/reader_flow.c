/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/epd_reader_session.h"
#include "reader_flow.h"
#include "../../../components/compute/include/epd_compute_protocol.h"
#include <string.h>

static void return_input(reader_flow_t *f)
{
    if (f->input.key.serial) reader_core_return(f->core, f->session, &f->input);
    f->offset = 0;
}
static void discard(reader_flow_t *f)
{
    return_input(f);
    if (f->job) reader_compute_release(f->job);
    f->job = NULL; f->generation = 0; f->ready = f->presented = false;
    memset(&f->page, 0, sizeof(f->page));
}
int reader_flow_init(reader_flow_t *f, reader_core_t *core, reader_session_id_t id,
                     const reader_page_metrics_t *metrics)
{
    if (!f || !core || !id || !metrics || !reader_sessions_get(&core->sessions, id)) return EPD_READ_INVALID;
    memset(f, 0, sizeof(*f)); f->core = core; f->session = id; f->metrics = *metrics;
    f->end.version = EPD_READER_POSITION_VERSION;
    return EPD_READ_OK;
}
static int begin(reader_flow_t *f, uint32_t generation)
{
    int result = 0;
    f->job = reader_compute_create(&result);
    if (!f->job) return result == EPD_COMPUTE_NO_MEMORY ? EPD_READ_NO_MEMORY : EPD_READ_BUSY;
    result = reader_page_init(&f->page, reader_compute_data(f->job), &f->metrics, generation);
    if (result) { reader_compute_release(f->job); f->job = NULL; return result; }
    f->generation = generation;
    return EPD_READ_OK;
}
static int consume(reader_flow_t *f)
{
    if (!f->input.key.serial)
    {
        int result = reader_core_take(f->core, f->session, &f->input);
        if (result == EPD_READ_END) return reader_page_end(&f->page, &f->end);
        if (result) return result;
    }
    epd_read_record_t record;
    const void *payload = NULL;
    int result = reader_batch_next(&f->input.batch, &f->offset, &record, &payload);
    if (result == EPD_READ_END)
    {
        bool ended = f->input.result == EPD_READ_END;
        return_input(f);
        if (ended) return reader_page_end(&f->page, &f->end);
        result = reader_core_next(f->core, f->session);
        return result == EPD_READ_BUSY ? EPD_READ_WAIT : result;
    }
    if (result) return result;
    /* Image resource preparation supplies dimensions before layout. Until
     * that path is attached, retain the current page and report unsupported. */
    if (record.kind == EPD_READ_IMAGE) return EPD_READ_UNSUPPORTED;
    result = reader_page_load(&f->page, &record, payload, 0, 0);
    if (!result) f->end = record.next;
    return result;
}
bool reader_flow_step(reader_flow_t *f)
{
    if (!f || !f->core || !f->session) return false;
    const reader_session_slot_t *s = reader_sessions_get(&f->core->sessions, f->session);
    bool retired = !s || s->phase == READER_SESSION_CLOSING || s->phase == READER_SESSION_SAVING;
    bool invalid = f->closing || retired || (f->generation && s && s->generation != f->generation);
    if (f->work.session)
    {
        int result;
        if (!reader_compute_poll(f->job, &result)) return false;
        reader_sessions_work_end(&f->core->sessions, f->work);
        f->work = (reader_work_token_t){0};
        if (!invalid) f->error = reader_page_complete(&f->page, result);
    }
    if (invalid)
    {
        discard(f); f->error = 0;
        if (f->closing || retired) { f->closing = true; return false; }
    }
    if (!s || s->phase != READER_SESSION_ACTIVE || f->error || f->ready) return false;
    if (!f->job)
    {
        int result = begin(f, s->generation);
        if (result == EPD_READ_BUSY) return true;
        if (result) { f->error = result; return false; }
    }
    if (f->page.batch->page.done)
    { f->ready = true; reader_compute_idle(); return false; }
    if (!f->page.loaded && !f->page.eof)
    {
        int result = consume(f);
        if (result == EPD_READ_WAIT || result == EPD_READ_BUSY) return false;
        if (result) { f->error = result; return false; }
        return true;
    }
    int result = reader_page_prepare(&f->page, 32);
    if (result == EPD_READ_WAIT) return true;
    if (result) { f->error = result; return false; }
    result = reader_sessions_work_begin(&f->core->sessions, f->session, &f->work);
    if (result)
    { f->page.in_flight = false; f->error = result; return false; }
    reader_compute_submit_target(f->job, true);
    return false;
}
const epd_reader_page_t *reader_flow_page(const reader_flow_t *f)
{ return f && f->ready && !f->closing && !f->error ? &f->page.batch->page : NULL; }
int reader_flow_present(reader_flow_t *f)
{
    if (!reader_flow_page(f)) return EPD_READ_WAIT;
    int result = reader_sessions_present(&f->core->sessions, f->session, f->generation, &f->page.start);
    if (!result) f->presented = true;
    return result;
}
int reader_flow_next(reader_flow_t *f)
{
    if (!reader_flow_page(f) || !f->presented) return EPD_READ_WAIT;
    int result = reader_page_turn(&f->page);
    if (!result) f->ready = f->presented = false;
    return result;
}
int reader_flow_seek(reader_flow_t *f, const epd_read_anchor_t *position)
{
    if (!f || f->closing) return EPD_READ_STALE;
    int result = reader_core_seek(f->core, f->session, position);
    if (!result) { f->ready = f->presented = false; f->end = *position; }
    return result;
}
bool reader_flow_close(reader_flow_t *f)
{
    if (!f) return true;
    f->closing = true;
    reader_flow_step(f);
    return reader_flow_closed(f);
}
bool reader_flow_closed(const reader_flow_t *f)
{ return !f || (f->closing && !f->job && !f->input.key.serial && !f->work.session); }
