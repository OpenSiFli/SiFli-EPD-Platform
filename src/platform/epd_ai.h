/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_AI_H
#define EPD_AI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct epd_service epd_service_t;
#define EPD_AI_VERSION 1u
#define EPD_AI_ID_SIZE 32u
#define EPD_AI_NAME_SIZE 64u
#define EPD_AI_MODEL_SIZE 128u
#define EPD_AI_MAX_MODELS 8u
#define EPD_AI_MAX_PROFILES 8u
#define EPD_AI_MAX_TOOLS 8u
#define EPD_AI_REQUEST_LIMIT (128u * 1024u)
#define EPD_AI_TEXT_LIMIT (64u * 1024u)
#define EPD_AI_EVENT_LIMIT (512u * 1024u)
#define EPD_AI_NATIVE_LIMIT (256u * 1024u)
#define EPD_AI_ARGUMENT_LIMIT (16u * 1024u)

typedef enum { EPD_AI_RESPONSES, EPD_AI_CHAT, EPD_AI_MESSAGES } epd_ai_protocol_t;
typedef enum {
    EPD_AI_OK, EPD_AI_INVALID, EPD_AI_UNSUPPORTED, EPD_AI_UNAUTHORIZED,
    EPD_AI_BUSY, EPD_AI_NO_MEMORY, EPD_AI_NO_NETWORK, EPD_AI_DNS,
    EPD_AI_CONNECT, EPD_AI_TLS, EPD_AI_AUTH, EPD_AI_RATE_LIMIT,
    EPD_AI_SERVER, EPD_AI_PROTOCOL, EPD_AI_LIMIT, EPD_AI_TIMEOUT,
    EPD_AI_CANCELLED, EPD_AI_INCOMPLETE, EPD_AI_STORAGE, EPD_AI_NOT_FOUND
} epd_ai_error_t;
enum {
    EPD_AI_CAP_TEXT = 1u, EPD_AI_CAP_STREAM = 2u, EPD_AI_CAP_TOOLS = 4u,
    EPD_AI_CAP_JSON = 8u, EPD_AI_CAP_SCHEMA = 16u, EPD_AI_CAP_NATIVE = 32u,
    EPD_AI_CAP_WEB_SEARCH = 64u
};
typedef enum { EPD_AI_MAX_TOKENS, EPD_AI_MAX_COMPLETION_TOKENS } epd_ai_token_field_t;
enum {
    EPD_AI_FEATURE_OPENAI = 1u, EPD_AI_FEATURE_DEEPSEEK = 2u,
    EPD_AI_FEATURE_ENCRYPTED_REASONING = 4u
};
typedef struct {
    char id[EPD_AI_MODEL_SIZE];
    uint32_t capabilities;
    uint32_t max_output_tokens;
    epd_ai_token_field_t token_field;
} epd_ai_model_t;
/* Redacted, copied snapshot. No credential or caller-owned DOM pointers. */
typedef struct {
    uint32_t struct_size, version;
    char id[EPD_AI_ID_SIZE], name[EPD_AI_NAME_SIZE];
    epd_ai_protocol_t protocol;
    uint32_t identity_revision, features;
    bool enabled, is_default;
    uint8_t model_count;
    epd_ai_model_t models[EPD_AI_MAX_MODELS];
} epd_ai_profile_t;

typedef struct { volatile uint32_t cancelled; } epd_ai_cancel_t;
typedef enum {
    EPD_AI_USER, EPD_AI_ASSISTANT, EPD_AI_NATIVE_ASSISTANT, EPD_AI_TOOL_RESULT
} epd_ai_message_kind_t;
typedef struct {
    epd_ai_message_kind_t kind;
    const char *text;
    size_t text_size;
    /* Native records are self-contained JSON envelopes returned by perform. */
    const char *native;
    size_t native_size;
    const char *call_id;
    bool tool_error;
} epd_ai_message_t;
typedef struct {
    const char *name, *description, *parameters_json;
    /* Upper bound after JSON encoding, reserved before any tool effect. */
    size_t max_result_encoded_bytes;
    bool strict;
} epd_ai_tool_t;
typedef enum { EPD_AI_FORMAT_TEXT, EPD_AI_FORMAT_JSON, EPD_AI_FORMAT_SCHEMA } epd_ai_format_t;
typedef struct {
    uint32_t struct_size, version;
    const char *profile_id, *model, *instructions;
    const epd_ai_message_t *messages;
    size_t message_count;
    const epd_ai_tool_t *tools;
    size_t tool_count;
    uint32_t max_output_tokens;
    bool stream;
    epd_ai_format_t format;
    const char *schema_name, *schema_json;
    bool has_temperature, has_top_p;
    double temperature, top_p;
    const char *const *stop;
    size_t stop_count;
    const char *extensions_json;
    /* Full-width tail keeps this field outside the padding of older requests. */
    uint64_t web_search;
} epd_ai_request_t;
/* Requests from existing applications end before web_search. */
#define EPD_AI_REQUEST_V1_SIZE offsetof(epd_ai_request_t, web_search)
enum {
    EPD_AI_USAGE_INPUT = 1u, EPD_AI_USAGE_OUTPUT = 2u,
    EPD_AI_USAGE_CACHED = 4u, EPD_AI_USAGE_REASONING = 8u
};
typedef struct {
    uint32_t valid;
    uint64_t input, output, cached, reasoning;
} epd_ai_usage_t;
typedef enum {
    EPD_AI_FINISH_UNKNOWN, EPD_AI_FINISH_STOP, EPD_AI_FINISH_TOOLS,
    EPD_AI_FINISH_LENGTH, EPD_AI_FINISH_REFUSAL, EPD_AI_FINISH_PAUSE
} epd_ai_finish_t;
typedef struct {
    char *id, *name, *arguments;
    size_t arguments_size;
} epd_ai_tool_call_t;
typedef enum {
    EPD_AI_EVENT_TEXT, EPD_AI_EVENT_TOOL_START, EPD_AI_EVENT_TOOL_ARGUMENTS,
    EPD_AI_EVENT_TOOL_END, EPD_AI_EVENT_USAGE,
    EPD_AI_EVENT_SEARCH_START, EPD_AI_EVENT_SEARCH_DONE, EPD_AI_EVENT_SEARCH_ERROR
} epd_ai_event_kind_t;
typedef struct {
    epd_ai_event_kind_t kind;
    const char *text;
    size_t text_size;
    size_t tool_index;
    const char *call_id, *tool_name;
    epd_ai_usage_t usage;
} epd_ai_event_t;
/* Borrowed event fields expire on return. false cancels; never execute tools
 * or wait for the GUI from this callback. It runs on the service worker. */
typedef bool (*epd_ai_event_cb_t)(const epd_ai_event_t *event, void *user);
typedef struct {
    uint32_t struct_size, version;
    epd_ai_error_t error;
    int http_status, detail;
    uint32_t tls_verify_flags;
    char error_code[96], request_id[128], retry_after[64], stage[32];
    epd_ai_finish_t finish;
    epd_ai_usage_t usage;
    char *text, *native;
    size_t text_size, native_size, tool_count;
    epd_ai_tool_call_t tools[EPD_AI_MAX_TOOLS];
    bool complete, replayable;
} epd_ai_result_t;

void epd_ai_cancel_init(epd_ai_cancel_t *cancel);
void epd_ai_cancel(epd_ai_cancel_t *cancel);
epd_ai_error_t epd_ai_profile_list(epd_service_t *owner, epd_ai_profile_t *profiles,
                                 size_t capacity, size_t *count);
epd_ai_error_t epd_ai_profile_get(epd_service_t *owner, const char *id,
                                epd_ai_profile_t *profile);
/* Performs one request on the owner's registered worker. Caller keeps all
 * inputs and cancel alive until return. Failed POSTs are never retried.
 * Server-requested pause_turn continuation is bounded by count and deadline. */
epd_ai_error_t epd_ai_perform(epd_service_t *owner, const epd_ai_request_t *request,
                            epd_ai_cancel_t *cancel, epd_ai_event_cb_t callback,
                            void *user, epd_ai_result_t *result);
/* Uses the actual encoder, without network access. reserve_encoded_bytes is
 * the upper bound for not-yet-executed tool results, including their wrappers.
 * Success requires encoded_size + reserve <= EPD_AI_REQUEST_LIMIT. */
epd_ai_error_t epd_ai_request_measure(epd_service_t *owner,
                                    const epd_ai_request_t *request,
                                    size_t reserve_encoded_bytes, size_t *encoded_size);
void epd_ai_result_clear(epd_ai_result_t *result);
/* UI thread only: opens the platform's explicit service authorization page. */
bool epd_ai_select_service(epd_service_t *owner);
/* UI thread, after an explicit authorization action. Grants the default
 * enabled text service, or the first enabled text service if none is default.
 * Poll the ticket until it returns a result other than BUSY. */
epd_ai_error_t epd_ai_authorize_service(epd_service_t *owner, uint32_t *ticket);
epd_ai_error_t epd_ai_authorization_poll(uint32_t ticket);
const char *epd_ai_error_name(epd_ai_error_t error);
#ifdef __cplusplus
}
#endif
#endif
