/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_HTTP_STREAM_H
#define EPD_HTTP_STREAM_H

#include "epd_http.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_HTTP_STREAM_VERSION 1u
#define EPD_HTTP_STREAM_ERROR_BODY_LIMIT 4096u
/* Use the first decoded body byte as meaningful data (non-streaming JSON).
 * Otherwise on_body must set *valid_data after parsing a valid protocol event. */
#define EPD_HTTP_STREAM_FIRST_BODY_DATA 0x00000001u

typedef enum
{
    EPD_HTTP_STREAM_CONTINUE = 0,
    EPD_HTTP_STREAM_COMPLETE,
    EPD_HTTP_STREAM_CANCEL,
    EPD_HTTP_STREAM_ERROR
} epd_http_stream_action_t;

/* Called synchronously on the perform caller's thread with dechunked bytes.
 * data is borrowed for this call only; neither TLS records nor HTTP chunks
 * define callback boundaries. Set *valid_data=true only after a valid protocol
 * event. COMPLETE closes the connection without waiting for the remaining HTTP
 * body; CANCEL and ERROR are distinct failures. No callbacks occur after return.
 * Non-2xx bodies are delivered here as well, capped at 4096 bytes; status is
 * available in response before header/body callbacks. Keep callbacks bounded. */
typedef epd_http_stream_action_t (*epd_http_stream_body_fn)(
    void *user, const unsigned char *data, size_t size, bool *valid_data);

typedef struct
{
    uint32_t struct_size;              /* sizeof(epd_http_stream_request_t) */
    uint32_t version;                  /* EPD_HTTP_STREAM_VERSION */
    uint32_t options;                  /* Unknown bits are rejected. */
    const char *host;                  /* Bare DNS name or IPv4, as in epd_http.h. */
    uint16_t port;                     /* 0 = 443; non-default port enters Host. */
    const char *path;
    const char *method;                /* NULL/GET or POST. */
    const epd_http_header_t *headers;
    size_t header_count;
    const void *body;
    size_t body_size;
    size_t body_limit;                 /* 0 = no aggregate transport cap. */
    uint32_t connect_timeout_ms;        /* 0 = 30000, includes DNS and TLS. */
    uint32_t send_idle_timeout_ms;      /* 0 = 30000 since last write progress. */
    uint32_t first_data_timeout_ms;     /* 0 = 90000 since request sent. */
    uint32_t idle_timeout_ms;           /* 0 = 60000, enabled after valid data. */
    uint32_t total_timeout_ms;          /* 0 = 300000; maximum 900000. */
    epd_http_cancelled_fn cancelled;
    epd_http_header_fn on_header;
    epd_http_stream_body_fn on_body;    /* Required. */
    void *user;                        /* Shared callback context. */
    /* Called once after headers and the whole request body have been sent.
     * May release body; HTTP never accesses body again after this callback.
     * Other request fields remain borrowed until perform returns. */
    void (*on_sent)(void *user);
} epd_http_stream_request_t;

typedef struct
{
    uint32_t struct_size;              /* Initialize before perform. */
    uint32_t version;                  /* Initialize to STREAM_VERSION. */
    int status;                       /* Preserved even on callback/I/O failure. */
    const char *stage;                 /* Static diagnostic, never body text. */
    int detail;                       /* Native TLS/socket or negative RT error. */
    uint32_t verify_flags;
    size_t body_bytes;                 /* Bytes delivered to on_body. */
    bool protocol_complete;           /* on_body returned COMPLETE. */
    bool http_complete;               /* HTTP framing was fully consumed. */
    bool first_data_received;
    bool error_body_truncated;        /* Diagnostic cap ended reception. */
    epd_http_stream_action_t action;   /* Last callback action. */
} epd_http_stream_response_t;

/* Requests and pointed-to data remain valid until return, except that on_sent
 * may release the request body after sending completes. Response owns no
 * memory. Initialize its size/version before every call. True means complete
 * HTTP framing, protocol COMPLETE, or a capped non-2xx diagnostic response;
 * inspect status and the two completion flags independently of this return.
 * HTTP status is never a protocol-success signal. Non-2xx status remains the
 * primary service error even when collecting diagnostics fails.
 * Accept-Encoding: identity is sent by the transport; a supplied identity
 * header is allowed and deduplicated, other encodings are rejected. Compressed
 * responses fail with stage content-encoding and retain HTTP status. Media type
 * and protocol terminal validation belong to the caller via on_header/on_body.
 * Uses strict certificate and hostname verification; no retries or redirects.
 * DNS is synchronous and cannot be interrupted: cancellation, route generation
 * and deadlines are checked immediately before and after the resolver returns.
 * Connect/send/receive waits use periodic cancellation/deadline checks. Socket
 * close uses lwIP's bounded send-timeout policy, subject to its timer scheduling;
 * cancellation, route and total time are checked again after cleanup. Time is monotonic.
 * TLS session, host and transport buffers use PSRAM without SRAM fallback. */
bool epd_http_stream_perform(const epd_http_stream_request_t *request,
                             epd_http_stream_response_t *response);

#ifdef __cplusplus
}
#endif
#endif
