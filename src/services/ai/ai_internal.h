/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_AI_INTERNAL_H
#define EPD_AI_INTERNAL_H
#include "epd_ai.h"
#include "ai_capabilities.h"
#include <rtthread.h>

#define AI_MEMORY_LIMIT (1536u * 1024u)
#define AI_CONFIG_LIMIT (64u * 1024u)
#define AI_MAX_GRANTS 64u
#define AI_SETTINGS_OWNER "ai_cfg"
#define AI_ENDPOINT_SIZE 512u
#define AI_SECRET_SIZE 1024u
#define AI_EXTRA_HEADERS 4u
typedef struct { char name[64], value[192]; } ai_header_t;
typedef enum { AI_AUTH_BEARER, AI_AUTH_API_KEY } ai_auth_t;
typedef struct {
    epd_ai_profile_t public;
    char endpoint[AI_ENDPOINT_SIZE], secret[AI_SECRET_SIZE];
    ai_auth_t auth;
    uint8_t header_count;
    ai_header_t headers[AI_EXTRA_HEADERS];
    uint32_t connect_ms, send_idle_ms, first_event_ms, read_idle_ms, total_ms;
} ai_profile_t;
typedef struct { char app[32], profile[EPD_AI_ID_SIZE]; } ai_grant_t;
typedef struct {
    uint32_t version, generation;
    size_t profile_count, grant_count;
    char default_id[EPD_AI_ID_SIZE];
    ai_profile_t profiles[EPD_AI_MAX_PROFILES];
    ai_grant_t grants[AI_MAX_GRANTS];
} ai_config_t;
typedef struct { char host[256], path[AI_ENDPOINT_SIZE]; uint16_t port; } ai_endpoint_t;

/* All private storage, JSON and returned results share this PSRAM budget.
 * Free remains valid after the request's stack has gone away. */
void *ai_alloc(size_t size);
void *ai_calloc(size_t count, size_t size);
void *ai_realloc(void *old, size_t size);
void ai_free(void *memory);
char *ai_strndup(const char *text, size_t size);
void ai_memory_usage(size_t *used, size_t *peak);
bool ai_cancelled(const epd_ai_cancel_t *cancel);
epd_ai_error_t ai_endpoint_parse(const char *url, ai_endpoint_t *endpoint);

/* Configuration owns a serialized writer and publishes complete snapshots.
 * snapshot/get allocate copies; caller ai_free()s them (including secrets). */
epd_ai_error_t ai_config_init(void);
epd_ai_error_t ai_config_snapshot(ai_config_t **out);
epd_ai_error_t ai_config_acquire(const char *app, const char *id, ai_profile_t **out);
bool ai_config_current(const char *app, const ai_profile_t *profile);
epd_ai_error_t ai_config_list(const char *app, epd_ai_profile_t *out,
                            size_t capacity, size_t *count);
epd_ai_error_t ai_config_commit(const ai_config_t *candidate);
/* Prepared imports own immutable, validated content. Commit never rereads path.
 * Both phases and exports must use the card session captured by the UI. */
epd_ai_error_t ai_config_import_prepare(const char *path, uint32_t session, ai_config_t **out);
epd_ai_error_t ai_config_import_commit(const ai_config_t *candidate, uint32_t session);
epd_ai_error_t ai_config_export(const char *path, uint32_t session);
epd_ai_error_t ai_config_revoke_app(const char *app);
/* Called after publication while the config writer and mutex own both snapshots.
 * The hook must not allocate, block, retain snapshots or call config APIs. */
void ai_config_changed(const ai_config_t *previous, const ai_config_t *next);

/* The protocol engine does not own a connection or service. Callbacks always
 * run outside its private JSON lock; completion requires a protocol terminal. */
typedef struct ai_protocol ai_protocol_t;
static inline bool ai_search_requested(const epd_ai_request_t *request)
{
    return request && request->struct_size >= sizeof(*request) && request->web_search;
}
epd_ai_error_t ai_protocol_encode(const ai_profile_t *profile,
                                const epd_ai_request_t *request,
                                char **body, size_t *size);
epd_ai_error_t ai_protocol_open(const ai_profile_t *profile,
                               const epd_ai_request_t *request,
                               epd_ai_event_cb_t callback, void *user,
                               epd_ai_result_t *result, ai_protocol_t **out);
epd_ai_error_t ai_protocol_feed(ai_protocol_t *state, const void *data, size_t size);
bool ai_protocol_has_event(const ai_protocol_t *state);
bool ai_protocol_complete(const ai_protocol_t *state);
epd_ai_error_t ai_protocol_finish(ai_protocol_t *state);
void ai_protocol_close(ai_protocol_t *state);
epd_ai_error_t ai_protocol_merge(epd_ai_result_t *previous, epd_ai_result_t *current);
/* Consumes previous even on failure; reserved text preserves the bounded prefix
 * without allocation. capacity is the actual allocation size of previous->text. */
epd_ai_error_t ai_protocol_merge_reserved(epd_ai_result_t *previous,
                                         epd_ai_result_t *current, size_t capacity);
#endif
