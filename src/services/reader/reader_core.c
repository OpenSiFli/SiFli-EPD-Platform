/* SPDX-License-Identifier: Apache-2.0 */
#include "reader_core.h"
#include <string.h>

typedef struct {
    reader_core_t *core;
    reader_open_t request;
    epd_read_provider_t api;
    reader_provider_lease_t provider;
    char app_id[64], identity[256], path[EPD_READER_PATH_MAX], format[64];
    int error;
} reader_content_t;

static bool copy(char *out, size_t size, const char *text)
{
    if (!text || !*text || strlen(text) >= size) return false;
    memcpy(out, text, strlen(text) + 1); return true;
}
static bool retain(void *owner)
{
    reader_content_t *c = owner;
    return c->request.owner.retain(c->request.owner.user);
}
static void release(void *owner)
{
    reader_content_t *c = owner;
    reader_core_t *core = c->core;
    reader_provider_release(core->providers, &c->provider);
    c->request.owner.release(c->request.owner.user);
    core->memory.free(c);
}
static int save(void *owner, reader_session_id_t id, uint64_t request, const epd_read_anchor_t *position)
{
    reader_content_t *c = owner;
    return c->request.owner.save(c->request.owner.user, id, request, position);
}
static reader_content_t *active(reader_core_t *core, reader_session_id_t id, bool allow_paused)
{
    const reader_session_slot_t *s = reader_sessions_get(&core->sessions, id);
    if (!s || core->sessions.active != id ||
        (s->phase != READER_SESSION_ACTIVE && !(allow_paused && s->phase == READER_SESSION_PAUSED))) return NULL;
    return s->owner;
}
int reader_core_init(reader_core_t *core, reader_providers_t *providers,
                      const reader_pipeline_sync_t *sync, const book_allocator_t *memory,
                      size_t window_limit, size_t block_size)
{
    if (!core || !providers || !memory || !memory->alloc || !memory->free) return EPD_READ_INVALID;
    memset(core, 0, sizeof(*core)); core->providers = providers; core->memory = *memory;
    reader_session_hooks_t hooks = {retain, release, save};
    reader_sessions_init(&core->sessions, &hooks);
    return reader_pipeline_init(&core->pipeline, sync, memory, window_limit, block_size);
}
int reader_core_open(reader_core_t *core, const reader_open_t *request, reader_session_id_t *id)
{
    if (id) *id = 0;
    if (!core || !request || !id || !request->owner.retain || !request->owner.release || !request->owner.save ||
        (request->restore.version && request->restore.version != EPD_READER_POSITION_VERSION)) return EPD_READ_INVALID;
    reader_content_t *c = core->memory.alloc(sizeof(*c));
    if (!c) return EPD_READ_NO_MEMORY;
    memset(c, 0, sizeof(*c)); c->core = core; c->request = *request;
    if (!copy(c->app_id, sizeof(c->app_id), request->source.app_id) ||
        !copy(c->identity, sizeof(c->identity), request->source.identity) ||
        !copy(c->format, sizeof(c->format), request->format) ||
        (request->source.path && !copy(c->path, sizeof(c->path), request->source.path)))
    { core->memory.free(c); return EPD_READ_INVALID; }
    c->request.source.app_id = c->app_id; c->request.source.identity = c->identity;
    c->request.source.path = c->path; c->request.source.memory = core->memory;
    c->request.format = c->format;
    int result = EPD_READ_OK;
    if (request->provider)
    {
        const epd_read_provider_t *api = request->provider;
        if (api->abi != EPD_READER_SOURCE_ABI || api->size < sizeof(*api) || !api->format ||
            strcmp(api->format, c->format) || !api->content_version || !api->open || !api->next || !api->seek ||
            !api->close || (request->source.content_version && request->source.content_version != api->content_version))
            result = EPD_READ_UNSUPPORTED;
        else c->api = *api;
    }
    else
    {
        result = reader_provider_acquire(core->providers, c->format, request->source.content_version, &c->provider);
        if (!result) c->api = *c->provider.api;
    }
    c->api.format = c->format;
    if (!result)
    {
        reader_session_open_t open = {c->app_id, c->identity, c};
        result = reader_sessions_open(&core->sessions, &open, id);
    }
    if (result)
    {
        reader_provider_release(core->providers, &c->provider);
        core->memory.free(c); return result;
    }
    reader_core_process(core);
    return EPD_READ_OK;
}
void reader_core_process(reader_core_t *core)
{
    if (core->worker.session)
    {
        const reader_session_slot_t *s = reader_sessions_get(&core->sessions, core->worker.session);
        if (!s || s->phase == READER_SESSION_CLOSING)
        {
            reader_pipeline_close(&core->pipeline);
            if (reader_pipeline_collect(&core->pipeline))
            {
                reader_sessions_work_end(&core->sessions, core->worker);
                core->worker = (reader_work_token_t){0};
            }
        }
    }
    reader_session_id_t next = reader_sessions_process(&core->sessions);
    if (!next) return;
    reader_content_t *c = active(core, next, false);
    c->error = reader_sessions_work_begin(&core->sessions, next, &core->worker);
    if (!c->error) c->error = reader_pipeline_open(&core->pipeline, next, &c->api, &c->request.source);
    if (c->error)
    {
        if (core->worker.session) reader_sessions_work_end(&core->sessions, core->worker);
        core->worker = (reader_work_token_t){0};
    }
    else if (c->request.restore.version)
        c->error = reader_core_seek(core, next, &c->request.restore);
}
bool reader_core_step(reader_core_t *core) { return reader_pipeline_step(&core->pipeline); }
int reader_core_pause(reader_core_t *core, reader_session_id_t id, bool paused)
{
    int result = reader_sessions_pause(&core->sessions, id, paused);
    if (!result) reader_pipeline_pause(&core->pipeline, paused);
    return result;
}
int reader_core_close(reader_core_t *core, reader_session_id_t id)
{
    int result = reader_sessions_close(&core->sessions, id);
    if (!result) reader_core_process(core);
    return result;
}
int reader_core_seek(reader_core_t *core, reader_session_id_t id, const epd_read_anchor_t *position)
{
    if (!position || position->version != EPD_READER_POSITION_VERSION) return EPD_READ_INVALID;
    reader_content_t *content = active(core, id, true);
    if (!content) return EPD_READ_STALE;
    if (content->error) return content->error;
    int result = reader_sessions_invalidate(&core->sessions, id);
    if (!result) result = reader_pipeline_seek(&core->pipeline, position);
    return result;
}
int reader_core_next(reader_core_t *core, reader_session_id_t id)
{
    if (!active(core, id, false)) return EPD_READ_STALE;
    return reader_pipeline_next(&core->pipeline);
}
int reader_core_ready(reader_core_t *core, reader_session_id_t id)
{
    if (core->sessions.in_hook) return EPD_READ_BUSY;
    reader_content_t *content = active(core, id, true);
    if (!content) return EPD_READ_STALE;
    return content->error ? content->error : reader_pipeline_next(&core->pipeline);
}
int reader_core_take(reader_core_t *core, reader_session_id_t id, reader_window_lease_t *lease)
{
    if (!active(core, id, false)) return EPD_READ_STALE;
    return reader_pipeline_take(&core->pipeline, lease);
}
int reader_core_return(reader_core_t *core, reader_session_id_t id, reader_window_lease_t *lease)
{
    if (!lease || lease->session != id || !reader_sessions_get(&core->sessions, id)) return EPD_READ_STALE;
    return reader_pipeline_return(&core->pipeline, lease);
}
int reader_core_error(reader_core_t *core, reader_session_id_t id)
{
    reader_content_t *c = active(core, id, true);
    if (!c) return EPD_READ_STALE;
    return c->error ? c->error : reader_pipeline_status(&core->pipeline).error;
}
void reader_core_stop(reader_core_t *core)
{
    reader_sessions_stop(&core->sessions); reader_pipeline_close(&core->pipeline);
    reader_core_process(core);
}
bool reader_core_empty(reader_core_t *core)
{ return reader_sessions_empty(&core->sessions) && reader_pipeline_closed(&core->pipeline); }
