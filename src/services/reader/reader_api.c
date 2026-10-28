/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/epd_reader_session.h"
#include "platform/epd_memory.h"
#include "reader_service.h"
#include "reader_runtime.h"
#include "reader_file.h"
#include <rtm.h>
#include <string.h>

typedef struct {
    epd_read_owner_t owner;
    epd_read_source_options_t source;
    reader_file_access_t file;
    void *module;
    bool retained;
} callback_owner_t;
extern unsigned short ff_oem2uni(unsigned short code, unsigned short code_page);
static bool cancelled(void *user)
{
    callback_owner_t *owner = user;
    return (!owner->source.open_stream && !reader_file_valid(&owner->file)) ||
        (owner->source.cancelled && owner->source.cancelled(owner->source.user));
}
static int open_stream(void *user, const char *path, book_stream_t *stream)
{
    callback_owner_t *owner = user;
    return owner->source.open_stream ? owner->source.open_stream(owner->source.user, path, stream) :
                                      reader_file_open(&owner->file, path, stream);
}
static bool retain(void *user)
{
    callback_owner_t *owner = user;
    owner->retained = owner->owner.retain(owner->owner.user);
    return owner->retained;
}
static void release(void *user)
{
    callback_owner_t *owner = user;
    owner->owner.release(owner->owner.user);
    reader_runtime_drop(owner->module);
    epd_app_free(owner);
}
static int save(void *user, reader_session_id_t session, uint64_t request, const epd_read_anchor_t *position)
{
    callback_owner_t *owner = user;
    return owner->owner.save(owner->owner.user, session, request, position);
}
bool epd_read_supports(const char *format, uint32_t version)
{ return reader_service_supports(format, version); }
RTM_EXPORT(epd_read_supports);
int epd_read_open(const epd_read_request_t *request, epd_read_session_t *session)
{
    if (session) *session = 0;
    if (!request || !session || request->abi != EPD_READ_SESSION_ABI || request->size < sizeof(*request) ||
        !request->owner.retain || !request->owner.release || !request->owner.save) return EPD_READ_INVALID;
    reader_core_t *core = reader_service_core();
    if (!reader_runtime_accept() || (core && core->sessions.in_hook)) return EPD_READ_BUSY;
    callback_owner_t *owner = epd_app_alloc(sizeof(*owner), EPD_APP_PSRAM);
    if (!owner) return EPD_READ_NO_MEMORY;
    memset(owner, 0, sizeof(*owner)); owner->owner = request->owner; owner->source = request->source;
    int result = request->source.open_stream ? EPD_READ_OK : reader_file_capture(request->source.path, &owner->file);
    if (!result) result = reader_runtime_pin(request->source.app_id, &owner->module);
    if (!result)
    {
        reader_open_t open = {.source = request->source, .format = request->format,
            .provider = request->provider, .owner = {owner, retain, release, save}, .restore = request->restore};
        open.source.user = owner; open.source.cancelled = cancelled; open.source.open_stream = open_stream;
        if (!open.source.legacy_to_unicode) open.source.legacy_to_unicode = ff_oem2uni;
        result = reader_service_open(&open, session);
    }
    if (result)
    {
        /* Admission failure never consumes the caller's owner reference. */
        if (owner->retained) owner->owner.release(owner->owner.user);
        reader_runtime_drop(owner->module); epd_app_free(owner);
    }
    return result;
}
RTM_EXPORT(epd_read_open);
int epd_read_pause(epd_read_session_t session, bool paused)
{
    reader_core_t *core = reader_service_core();
    int result = core ? reader_core_pause(core, session, paused) : EPD_READ_STALE;
    if (!result) reader_service_notify();
    return result;
}
RTM_EXPORT(epd_read_pause);
int epd_read_close(epd_read_session_t session)
{
    reader_core_t *core = reader_service_core();
    int result = core ? reader_core_close(core, session) : EPD_READ_STALE;
    if (!result) reader_service_notify();
    return result;
}
RTM_EXPORT(epd_read_close);
int epd_read_seek(epd_read_session_t session, const epd_read_anchor_t *position)
{
    reader_core_t *core = reader_service_core();
    int result = core ? reader_core_seek(core, session, position) : EPD_READ_STALE;
    if (!result) reader_service_notify();
    return result;
}
RTM_EXPORT(epd_read_seek);
int epd_read_status(epd_read_session_t session, epd_read_status_t *status)
{
    if (!status) return EPD_READ_INVALID;
    memset(status, 0, sizeof(*status));
    reader_core_t *core = reader_service_core();
    const reader_session_slot_t *slot = core ? reader_sessions_get(&core->sessions, session) : NULL;
    if (!slot) return EPD_READ_STALE;
    switch (slot->phase)
    {
    case READER_SESSION_WAITING: status->phase = EPD_READ_QUEUED; break;
    case READER_SESSION_ACTIVE: status->phase = EPD_READ_ACTIVE; break;
    case READER_SESSION_PAUSED: status->phase = EPD_READ_PAUSED; break;
    case READER_SESSION_CLOSING: status->phase = EPD_READ_CLOSING; break;
    case READER_SESSION_SAVING: status->phase = EPD_READ_SAVING; break;
    default: return EPD_READ_STALE;
    }
    status->generation = slot->generation; status->position = slot->visible;
    status->save_pending = slot->visible_revision != slot->saved_revision;
    status->save_error = slot->save_error;
    if (core->sessions.active == session)
    {
        reader_pipeline_status_t pipeline = reader_pipeline_status(&core->pipeline);
        status->buffer_bytes = pipeline.buffer_bytes; status->peak_buffer_bytes = pipeline.peak_buffer_bytes;
        status->content_ready = pipeline.ready; status->ended = pipeline.ended;
        status->error = reader_core_error(core, session);
        if (slot->phase == READER_SESSION_CLOSING) status->error = 0;
    }
    return EPD_READ_OK;
}
RTM_EXPORT(epd_read_status);
int epd_read_content_ready(epd_read_session_t session)
{
    reader_core_t *core = reader_service_core();
    return core ? reader_core_ready(core, session) : EPD_READ_STALE;
}
RTM_EXPORT(epd_read_content_ready);
int epd_read_saved(epd_read_session_t session, uint64_t request, int result)
{
    reader_core_t *core = reader_service_core();
    int accepted = core ? reader_sessions_saved(&core->sessions, session, request, result) : EPD_READ_STALE;
    if (!accepted) reader_service_notify();
    return accepted;
}
RTM_EXPORT(epd_read_saved);
int epd_read_retry_save(epd_read_session_t session)
{
    reader_core_t *core = reader_service_core();
    int result = core ? reader_sessions_retry_save(&core->sessions, session) : EPD_READ_STALE;
    if (!result) reader_service_notify();
    return result;
}
RTM_EXPORT(epd_read_retry_save);
