/* SPDX-License-Identifier: Apache-2.0 */
#include "app_service_internal.h"
#include "storage_internal.h"
#include "ai_internal.h"
#include "ai_json.h"
#include "epd_http_stream.h"
#include "app_service.h"
#include "sleep_guard.h"
#include "storage.h"
#include "board_service.h"
#include "system_reset_guard.h"
#include "network.h"
#include <rthw.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    epd_service_t *owner;
    const epd_ai_cancel_t *cancel;
    const epd_ai_request_t *request;
    epd_ai_result_t *result;
    ai_profile_t *profile;
    ai_protocol_t *protocol;
    epd_http_stream_response_t response;
    ai_endpoint_t endpoint;
    char app[32], authorization[AI_SECRET_SIZE + 8];
    char *encoded, *error_body;
    size_t error_size;
    epd_ai_event_cb_t callback;
    void *callback_user;
    sleep_guard_owner_t sleep;
    epd_ai_error_t error;
    bool media_seen, media_valid;
    epd_ai_result_t previous;
    epd_ai_request_t continued;
    epd_ai_message_t *continuation_messages;
} ai_call_t;
static bool active;
typedef struct {
    char app[32], profile[EPD_AI_ID_SIZE];
    uint64_t serial;
    bool registered, cancelled;
} ai_config_watch_t;
static ai_config_watch_t config_watch;

static void secure_clear(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}
static void profile_clear(ai_profile_t *profile)
{
    if (profile) { secure_clear(profile, sizeof(*profile)); ai_free(profile); }
}
static bool equal_ci(const char *a, const char *b)
{
    while (*a && *b)
    {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return !*a && !*b;
}
static bool blocked(void)
{
    return board_power_pending() || system_reset_blocks_apps() ||
           storage_release_requested() || storage_changing();
}
static bool network_available(void)
{
    if (network_ready()) return true;
    /* A local service does not require a default gateway. */
    for (network_source_t source = NETWORK_PAN; source <= NETWORK_WIFI_AP; ++source)
    {
        network_interface_snapshot_t state;
        if (network_interface_snapshot(source, &state) == RT_EOK && state.ready)
            return true;
    }
    return false;
}
void epd_ai_cancel_init(epd_ai_cancel_t *cancel)
{
    if (!cancel) return;
    rt_base_t level = rt_hw_interrupt_disable();
    cancel->cancelled = 0;
    rt_hw_interrupt_enable(level);
}
void epd_ai_cancel(epd_ai_cancel_t *cancel)
{
    if (!cancel) return;
    rt_base_t level = rt_hw_interrupt_disable();
    cancel->cancelled = 1;
    rt_hw_interrupt_enable(level);
}
bool ai_cancelled(const epd_ai_cancel_t *cancel)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool value = cancel && cancel->cancelled != 0;
    rt_hw_interrupt_enable(level);
    return value;
}
static bool watch_config(const ai_call_t *call)
{
    const char *id = call->request->profile_id;
    if (!id) return false;
    size_t size = 0;
    while (size < sizeof(config_watch.profile) && id[size]) ++size;
    if (!size || size == sizeof(config_watch.profile)) return false;
    rt_base_t level = rt_hw_interrupt_disable();
    ++config_watch.serial;
    memcpy(config_watch.app, call->app, sizeof(config_watch.app));
    memcpy(config_watch.profile, id, size + 1);
    config_watch.cancelled = false;
    config_watch.registered = true;
    rt_hw_interrupt_enable(level);
    return true;
}

static const ai_profile_t *watch_profile(const ai_config_t *config, const char *id)
{
    for (size_t i = 0; i < config->profile_count; ++i)
        if (!strcmp(config->profiles[i].public.id, id)) return &config->profiles[i];
    return NULL;
}

void ai_config_changed(const ai_config_t *previous, const ai_config_t *next)
{
    rt_base_t level = rt_hw_interrupt_disable();
    ai_config_watch_t watch = config_watch;
    rt_hw_interrupt_enable(level);
    if (!watch.registered || watch.cancelled) return;
    /* Both snapshots remain immutable while the config writer owns its lock.
     * Scan outside the interrupt guard; retain no request or profile pointers. */
    const ai_profile_t *before = watch_profile(previous, watch.profile);
    const ai_profile_t *after = watch_profile(next, watch.profile);
    bool granted = false;
    for (size_t i = 0; i < next->grant_count; ++i)
        if (!strcmp(next->grants[i].app, watch.app) && !strcmp(next->grants[i].profile, watch.profile))
        { granted = true; break; }
    if (after && after->public.enabled && granted &&
        (!before || before->public.identity_revision == after->public.identity_revision)) return;
    level = rt_hw_interrupt_disable();
    if (config_watch.registered && config_watch.serial == watch.serial)
        config_watch.cancelled = true;
    rt_hw_interrupt_enable(level);
}
static bool request_cancelled(void *user)
{
    ai_call_t *call = user;
    rt_base_t level = rt_hw_interrupt_disable();
    bool revoked = config_watch.registered && config_watch.cancelled;
    rt_hw_interrupt_enable(level);
    if (revoked || ai_cancelled(call->cancel) || epd_service_cancelled(call->owner) || blocked())
    { call->error = EPD_AI_CANCELLED; return true; }
    bool current = ai_config_current(call->app, call->profile);
    level = rt_hw_interrupt_disable();
    revoked = config_watch.registered && config_watch.cancelled;
    rt_hw_interrupt_enable(level);
    if (!current || revoked)
    { call->error = EPD_AI_CANCELLED; return true; }
    return false;
}

static bool protocol_event(const epd_ai_event_t *event, void *user)
{
    ai_call_t *call = user;
    if (request_cancelled(call)) return false;
    if (call->callback && !call->callback(event, call->callback_user))
    { call->error = EPD_AI_CANCELLED; return false; }
    return !request_cancelled(call);
}

epd_ai_error_t epd_ai_profile_list(epd_service_t *owner, epd_ai_profile_t *out,
                                 size_t capacity, size_t *count)
{
    char id[32];
    if (!count || (capacity && !out) || !epd_service_identity(owner, id, sizeof(id)))
        return EPD_AI_INVALID;
    return ai_config_list(id, out, capacity, count);
}
epd_ai_error_t epd_ai_profile_get(epd_service_t *owner, const char *id, epd_ai_profile_t *out)
{
    char app[32];
    ai_profile_t *profile = NULL;
    if (!id || !out || !epd_service_identity(owner, app, sizeof(app))) return EPD_AI_INVALID;
    epd_ai_error_t error = ai_config_acquire(app, id, &profile);
    if (error == EPD_AI_OK) *out = profile->public;
    profile_clear(profile);
    return error;
}
epd_ai_error_t epd_ai_request_measure(epd_service_t *owner,
                                    const epd_ai_request_t *request,
                                    size_t reserve, size_t *size)
{
    char app[32];
    ai_profile_t *profile = NULL;
    if (size) *size = 0;
    if (!size || !request || request->struct_size < EPD_AI_REQUEST_V1_SIZE ||
        request->version != EPD_AI_VERSION || !epd_service_is_worker(owner) ||
        !epd_service_identity(owner, app, sizeof(app))) return EPD_AI_INVALID;
    if (reserve > EPD_AI_REQUEST_LIMIT) return EPD_AI_LIMIT;
    epd_ai_error_t error = ai_config_init();
    if (error == EPD_AI_OK) error = ai_config_acquire(app, request->profile_id, &profile);
    if (error == EPD_AI_OK) error = ai_protocol_encode(profile, request, NULL, size);
    if (error == EPD_AI_OK && *size > EPD_AI_REQUEST_LIMIT - reserve) error = EPD_AI_LIMIT;
    profile_clear(profile);
    return error;
}

static bool response_header(void *user, const char *name, const char *value)
{
    ai_call_t *call = user;
    if (equal_ci(name, "content-type") && call->response.status >= 200 && call->response.status < 300)
    {
        char media[96];
        size_t length = strcspn(value, "; \t");
        if (length >= sizeof(media)) { call->error = EPD_AI_PROTOCOL; return false; }
        memcpy(media, value, length); media[length] = 0;
        bool valid = call->request->stream ? equal_ci(media, "text/event-stream") :
                     equal_ci(media, "application/json");
        if (call->media_seen && call->media_valid != valid)
        { call->error = EPD_AI_PROTOCOL; return false; }
        call->media_seen = true; call->media_valid = valid;
    }
    else if (equal_ci(name, "x-request-id") || equal_ci(name, "request-id"))
        snprintf(call->result->request_id, sizeof(call->result->request_id), "%s", value);
    else if (equal_ci(name, "retry-after"))
        snprintf(call->result->retry_after, sizeof(call->result->retry_after), "%s", value);
    return !request_cancelled(call);
}
static epd_http_stream_action_t response_body(void *user, const unsigned char *data,
                                            size_t size, bool *valid_data)
{
    ai_call_t *call = user;
    if (request_cancelled(call)) return EPD_HTTP_STREAM_CANCEL;
    if (call->response.status < 200 || call->response.status >= 300)
    {
        if (size > EPD_HTTP_STREAM_ERROR_BODY_LIMIT - call->error_size)
            size = EPD_HTTP_STREAM_ERROR_BODY_LIMIT - call->error_size;
        if (size)
        {
            char *next = ai_realloc(call->error_body, call->error_size + size + 1);
            if (!next) return EPD_HTTP_STREAM_ERROR;
            call->error_body = next;
            memcpy(next + call->error_size, data, size);
            call->error_size += size; next[call->error_size] = 0;
        }
        return EPD_HTTP_STREAM_CONTINUE;
    }
    if (!call->media_seen || !call->media_valid)
    { call->error = EPD_AI_PROTOCOL; return EPD_HTTP_STREAM_ERROR; }
    call->error = ai_protocol_feed(call->protocol, data, size);
    *valid_data = ai_protocol_has_event(call->protocol);
    if (call->error == EPD_AI_CANCELLED) return EPD_HTTP_STREAM_CANCEL;
    if (call->error != EPD_AI_OK) return EPD_HTTP_STREAM_ERROR;
    return ai_protocol_complete(call->protocol) ? EPD_HTTP_STREAM_COMPLETE : EPD_HTTP_STREAM_CONTINUE;
}
static void request_sent(void *user)
{
    ai_call_t *call = user;
    ai_free(call->encoded);
    call->encoded = NULL;
}
static epd_ai_error_t http_error(ai_call_t *call)
{
    int status = call->response.status;
    if (status == 401 || status == 403) return EPD_AI_AUTH;
    if (status == 429) return EPD_AI_RATE_LIMIT;
    if (status >= 500) return EPD_AI_SERVER;
    if (status >= 300) return EPD_AI_PROTOCOL;
    if (call->error != EPD_AI_OK) return call->error;
    const char *stage = call->response.stage ? call->response.stage : "";
    if (strstr(stage, "timeout") && call->response.detail == -RT_ETIMEOUT) return EPD_AI_TIMEOUT;
    if (strstr(stage, "cancel")) return EPD_AI_CANCELLED;
    if (strstr(stage, "network")) return EPD_AI_NO_NETWORK;
    if (strstr(stage, "dns")) return EPD_AI_DNS;
    if (strstr(stage, "alloc") || strstr(stage, "memory")) return EPD_AI_NO_MEMORY;
    if (strstr(stage, "limit") || !strcmp(stage, "request-size")) return EPD_AI_LIMIT;
    if (!strcmp(stage, "content-encoding")) return EPD_AI_UNSUPPORTED;
    if (!strncmp(stage, "request-", 8) && strcmp(stage, "request-write")) return EPD_AI_INVALID;
    if (strstr(stage, "tls") || strstr(stage, "cert") || strstr(stage, "clock")) return EPD_AI_TLS;
    if (!strncmp(stage, "tcp-", 4) || strstr(stage, "connect") || strstr(stage, "socket")) return EPD_AI_CONNECT;
    if (call->response.detail == -RT_EINVAL || !strcmp(stage, "http-status")) return EPD_AI_PROTOCOL;
    return EPD_AI_INCOMPLETE;
}
static void error_code(ai_call_t *call)
{
    if (!call->error_size || !ai_json_begin()) return;
    epd_ai_error_t error;
    ai_cjson *root = ai_json_parse(call->error_body, call->error_size, &error);
    if (root)
    {
        const ai_cjson *object = ai_cjson_GetObjectItemCaseSensitive(root, "error");
        const ai_cjson *code = ai_cjson_GetObjectItemCaseSensitive(object, "code");
        if (!ai_cjson_IsString(code)) code = ai_cjson_GetObjectItemCaseSensitive(object, "type");
        if (ai_cjson_IsString(code))
        {
            size_t n = 0;
            const unsigned char *p = (const unsigned char *)code->valuestring;
            while (*p && n + 1 < sizeof(call->result->error_code))
            {
                unsigned char c = *p++;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
                    call->result->error_code[n++] = (char)c;
                else break;
            }
            call->result->error_code[n] = 0;
        }
        ai_cjson_Delete(root);
    }
    ai_json_end();
}

epd_ai_error_t epd_ai_perform(epd_service_t *owner, const epd_ai_request_t *request,
                            epd_ai_cancel_t *cancel, epd_ai_event_cb_t callback,
                            void *user, epd_ai_result_t *result)
{
    if (!result) return EPD_AI_INVALID;
    memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result); result->version = EPD_AI_VERSION;
    if (!request || request->struct_size < EPD_AI_REQUEST_V1_SIZE || request->version != EPD_AI_VERSION ||
        !epd_service_is_worker(owner)) return result->error = EPD_AI_INVALID;
    if (ai_cancelled(cancel) || epd_service_cancelled(owner)) return result->error = EPD_AI_CANCELLED;
    if (blocked()) return result->error = EPD_AI_BUSY;
    rt_base_t level = rt_hw_interrupt_disable();
    if (active) { rt_hw_interrupt_enable(level); return result->error = EPD_AI_BUSY; }
    active = true;
    rt_hw_interrupt_enable(level);
    ai_call_t *call = ai_calloc(1, sizeof(*call));
    epd_ai_error_t error = EPD_AI_NO_MEMORY;
    if (!call) goto release;
    call->owner = owner; call->cancel = cancel; call->request = request; call->result = result;
    call->callback = callback; call->callback_user = user;
    call->sleep.id = "ai_request";
    if (!epd_service_identity(owner, call->app, sizeof(call->app)))
    { error = epd_service_cancelled(owner) ? EPD_AI_CANCELLED : EPD_AI_INVALID; goto done; }
    if (!sleep_guard_acquire(&call->sleep)) { error = EPD_AI_BUSY; goto done; }
    error = ai_config_init();
    if (error != EPD_AI_OK) goto done;
    /* Register before acquisition so a revoke/regrant cannot fall between
     * copying the profile and attaching its cancellation watch. */
    if (!watch_config(call)) { error = EPD_AI_INVALID; goto done; }
    error = ai_config_acquire(call->app, request->profile_id, &call->profile);
    if (error != EPD_AI_OK) goto done;
    if (request_cancelled(call))
    { error = EPD_AI_CANCELLED; goto done; }
    if (!network_available()) { error = EPD_AI_NO_NETWORK; goto done; }
    error = ai_endpoint_parse(call->profile->endpoint, &call->endpoint);
    if (error != EPD_AI_OK) goto done;
    const epd_ai_request_t *original = request;
    rt_tick_t started = rt_tick_get();
    for (unsigned continuation = 0; ; ++continuation) {
        bool text_reserved = false;
        uint64_t elapsed = (uint64_t)(rt_tick_t)(rt_tick_get() - started) * 1000u / RT_TICK_PER_SECOND;
        if (elapsed >= call->profile->total_ms) { error = EPD_AI_TIMEOUT; goto done; }
        size_t encoded_size = 0;
        error = ai_protocol_encode(call->profile, request, &call->encoded, &encoded_size);
        if (error != EPD_AI_OK) goto done;
        if (request_cancelled(call)) { error = EPD_AI_CANCELLED; goto done; }
        error = ai_protocol_open(call->profile, request, protocol_event, call, result, &call->protocol);
        if (error != EPD_AI_OK) goto done;
        epd_http_header_t headers[AI_EXTRA_HEADERS + 4];
        size_t count = 0;
        headers[count++] = (epd_http_header_t){"Content-Type", "application/json"};
        headers[count++] = (epd_http_header_t){"Accept", request->stream ? "text/event-stream" : "application/json"};
        if (call->profile->auth == AI_AUTH_BEARER)
        {
            snprintf(call->authorization, sizeof(call->authorization), "Bearer %s", call->profile->secret);
            headers[count++] = (epd_http_header_t){"Authorization", call->authorization};
        }
        else headers[count++] = (epd_http_header_t){"x-api-key", call->profile->secret};
        for (size_t i = 0; i < call->profile->header_count; ++i)
            headers[count++] = (epd_http_header_t){call->profile->headers[i].name, call->profile->headers[i].value};
        elapsed = (uint64_t)(rt_tick_t)(rt_tick_get() - started) * 1000u / RT_TICK_PER_SECOND;
        if (elapsed >= call->profile->total_ms) { error = EPD_AI_TIMEOUT; goto done; }
        epd_http_stream_request_t transport = {
            .struct_size = sizeof(transport), .version = EPD_HTTP_STREAM_VERSION,
            .options = request->stream ? 0 : EPD_HTTP_STREAM_FIRST_BODY_DATA,
            .host = call->endpoint.host, .port = call->endpoint.port, .path = call->endpoint.path,
            .method = "POST", .headers = headers, .header_count = count,
            .body = call->encoded, .body_size = encoded_size,
            .body_limit = request->stream ? 0 : EPD_AI_EVENT_LIMIT,
            .connect_timeout_ms = call->profile->connect_ms,
            .send_idle_timeout_ms = call->profile->send_idle_ms,
            .first_data_timeout_ms = call->profile->first_event_ms,
            .idle_timeout_ms = call->profile->read_idle_ms,
            .total_timeout_ms = call->profile->total_ms - (uint32_t)elapsed,
            .cancelled = request_cancelled, .on_header = response_header, .on_body = response_body,
            .user = call, .on_sent = request_sent
        };
        call->response.struct_size = sizeof(call->response);
        call->response.version = EPD_HTTP_STREAM_VERSION;
        bool received = epd_http_stream_perform(&transport, &call->response);
        result->http_status = call->response.status;
        result->detail = call->response.detail;
        result->tls_verify_flags = call->response.verify_flags;
        snprintf(result->stage, sizeof(result->stage), "%s", call->response.stage ? call->response.stage : "protocol");
        if (result->http_status < 200 || result->http_status >= 300 || !received)
        { error = http_error(call); error_code(call); }
        else if (request_cancelled(call)) error = EPD_AI_CANCELLED;
        else if (!call->media_seen || !call->media_valid) error = EPD_AI_PROTOCOL;
        else
        {
            error = ai_protocol_finish(call->protocol);
            if (error == EPD_AI_OK && request_cancelled(call)) error = EPD_AI_CANCELLED;
        }
        ai_protocol_close(call->protocol); call->protocol = NULL;
        result->error = error;
        if (call->previous.native || call->previous.text) {
            epd_ai_error_t merged = ai_protocol_merge_reserved(&call->previous, result, EPD_AI_TEXT_LIMIT + 1u);
            text_reserved = true;
            if (merged) { if (!error) error = merged; goto done; }
        }
        if (error || result->finish != EPD_AI_FINISH_PAUSE) break;
        if (!ai_search_requested(original) || continuation >= 3 || result->error_code[0] ||
            !result->native || !result->replayable || result->tool_count) {
            snprintf(result->stage, sizeof(result->stage), "search-continuation-limit");
            error = EPD_AI_INCOMPLETE; break;
        }
        if (!call->continuation_messages) {
            if (original->message_count >= 4096) { error = EPD_AI_LIMIT; break; }
            call->continuation_messages = ai_calloc(original->message_count + 1, sizeof(epd_ai_message_t));
            if (!call->continuation_messages) { error = EPD_AI_NO_MEMORY; break; }
            memcpy(call->continuation_messages, original->messages, original->message_count * sizeof(epd_ai_message_t));
            memcpy(&call->continued, original, sizeof(call->continued));
            call->continued.messages = call->continuation_messages;
            call->continued.message_count = original->message_count + 1;
        }
        /* Reserve before another request can deliver text, so even a later
         * allocation failure can return the full prefix within the text cap. */
        if (!text_reserved) {
            char *text = ai_realloc(result->text, EPD_AI_TEXT_LIMIT + 1u);
            if (!text) { error = EPD_AI_NO_MEMORY; break; }
            result->text = text; result->text[result->text_size] = 0;
        }
        call->previous = *result;
        memset(result, 0, sizeof(*result));
        call->continuation_messages[original->message_count] = (epd_ai_message_t){
            .kind = EPD_AI_NATIVE_ASSISTANT, .native = call->previous.native,
            .native_size = call->previous.native_size
        };
        request = &call->continued; call->request = request;
        ai_protocol_close(call->protocol); call->protocol = NULL;
        ai_free(call->encoded); call->encoded = NULL;
        ai_free(call->error_body); call->error_body = NULL; call->error_size = 0;
        memset(&call->response, 0, sizeof(call->response));
        call->media_seen = call->media_valid = false; call->error = EPD_AI_OK;
    }
done:
    ai_protocol_close(call->protocol); call->protocol = NULL;
    result->error = error;
    if (call->previous.native || call->previous.text) {
        epd_ai_error_t merged = ai_protocol_merge_reserved(&call->previous, result, EPD_AI_TEXT_LIMIT + 1u);
        if (!error) error = merged;
    }
    epd_ai_result_clear(&call->previous);
    ai_free(call->continuation_messages);
    ai_free(call->encoded); ai_free(call->error_body);
    profile_clear(call->profile);
    secure_clear(call->authorization, sizeof(call->authorization));
    sleep_guard_release(&call->sleep);
    ai_free(call);
release:
    level = rt_hw_interrupt_disable();
    if (config_watch.registered && config_watch.cancelled && error == EPD_AI_OK) error = EPD_AI_CANCELLED;
    config_watch.registered = false;
    active = false;
    rt_hw_interrupt_enable(level);
    result->struct_size = sizeof(*result); result->version = EPD_AI_VERSION;
    result->error = error;
    if (error != EPD_AI_OK) { result->complete = false; result->replayable = false; }
    return error;
}

void epd_ai_result_clear(epd_ai_result_t *result)
{
    if (!result) return;
    ai_free(result->text); ai_free(result->native);
    for (size_t i = 0; i < EPD_AI_MAX_TOOLS; ++i)
    { ai_free(result->tools[i].id); ai_free(result->tools[i].name); ai_free(result->tools[i].arguments); }
    memset(result, 0, sizeof(*result));
}

const char *epd_ai_error_name(epd_ai_error_t error)
{
    static const char *const names[] = {"ok", "invalid", "unsupported", "unauthorized", "busy",
        "no-memory", "no-network", "dns", "connect", "tls", "authentication", "rate-limit",
        "server", "protocol", "limit", "timeout", "cancelled", "incomplete", "storage", "not-found"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "unknown";
}

RTM_EXPORT(epd_ai_cancel_init);
RTM_EXPORT(epd_ai_cancel);
RTM_EXPORT(epd_ai_profile_list);
RTM_EXPORT(epd_ai_profile_get);
RTM_EXPORT(epd_ai_request_measure);
RTM_EXPORT(epd_ai_perform);
RTM_EXPORT(epd_ai_result_clear);
RTM_EXPORT(epd_ai_error_name);
