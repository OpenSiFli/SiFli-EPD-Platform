/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_HTTP_H
#define EPD_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char *name;
    const char *value;
} epd_http_header_t;

typedef bool (*epd_http_cancelled_fn)(void *user);
/* Called once per final-response header and trailer, including repeated
 * Set-Cookie and Date. Name/value preserve case; surrounding value whitespace
 * is removed. Strings are valid only during this call. Copy retained values.
 * Returning false cancels the operation. Callbacks run on the calling thread. */
typedef bool (*epd_http_header_fn)(void *user, const char *name, const char *value);

typedef struct
{
    /* HTTPS only, port 443. Bare DNS hostname or IPv4 address; no scheme,
     * port, userinfo, slash, or whitespace. No redirects are followed. */
    const char *host;
    const char *path;                 /* Origin-form path beginning with '/'. */
    const char *method;               /* "GET" (also NULL) or "POST". */
    const epd_http_header_t *headers;
    size_t header_count;
    /* Host, Connection, Content-Length and Transfer-Encoding are owned by
     * the transport and cannot appear in headers. Expect is unsupported.
     * CR/LF injection is rejected.
     * Request data and all strings remain caller-owned until perform returns. */
    const void *body;
    size_t body_size;
    uint32_t timeout_ms;              /* 0 = 30000; total elapsed time. */
    size_t body_limit;                /* 0 = no policy cap; wire AND decoded. */
    epd_http_cancelled_fn cancelled;
    void *user;                      /* Shared by cancelled and on_header. */
    epd_http_header_fn on_header;
} epd_http_request_t;

typedef struct
{
    int status;                      /* HTTP status, or 0 before status line. */
    /* On success, body is NUL-terminated (size excludes NUL), including empty
     * responses. Binary data may contain NULs. Owned by this response and
     * allocated with epd_app_alloc(..., EPD_APP_PSRAM), freed by
     * epd_http_response_clear(), or epd_app_free() after detaching body.
     * On failure body is NULL and size is 0; diagnostics remain available. */
    char *body;
    size_t size;
    const char *stage;                /* Static diagnostic string. */
    int detail;                      /* Native TLS/socket or negative RT error. */
    uint32_t verify_flags;            /* mbedTLS certificate verification flags. */
} epd_http_response_t;

/* Synchronous API intended for a worker thread. Network socket I/O and TLS
 * handshake use nonblocking retries with cancellation and network-generation
 * checks. SDK DNS resolution is synchronous: cancellation/timeout is checked
 * immediately before and after DNS but cannot interrupt the SDK resolver.
 * Strict certificate/hostname validation is required. Gzip is decoded and its
 * CRC/length checked. EOF-framed bodies require TLS close_notify. Response
 * header/trailer lines are limited to 4094 bytes, combined blocks to 32768.
 * HTTP error statuses are successful transport results;
 * callers inspect response.status. No automatic retries or redirects.
 * Initialize response to {0}; clear it before reuse. Returns true only after a
 * complete response. Failure releases partial bodies and transport resources. */
bool epd_http_perform(const epd_http_request_t *request, epd_http_response_t *response);
void epd_http_response_clear(epd_http_response_t *response);

#ifdef __cplusplus
}
#endif
#endif
