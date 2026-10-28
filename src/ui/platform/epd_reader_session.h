/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_READER_SESSION_H
#define EPD_READER_SESSION_H
#include "epd_reader_source.h"

#define EPD_READ_SESSION_ABI 1u
typedef uint64_t epd_read_session_t;
typedef struct {
    void *user;
    bool (*retain)(void *user);
    void (*release)(void *user);
    /* UI thread: queue a copy and return WAIT, or return OK after durable
     * storage. Complete WAIT with epd_read_saved() on the UI thread. Callbacks
     * must not reenter session operations. The module stays pinned through
     * the final release callback, including delayed save completion. */
    int (*save)(void *user, epd_read_session_t session, uint64_t request,
                const epd_read_anchor_t *position);
} epd_read_owner_t;
typedef struct {
    uint32_t abi, size;
    epd_read_source_options_t source;
    const char *format;
    /* NULL selects the platform provider. A custom provider uses the same
     * bounded-record contract. Its callbacks must belong to the owner module
     * or to dependencies kept alive by owner.retain/release. */
    const epd_read_provider_t *provider;
    epd_read_owner_t owner;
    epd_read_anchor_t restore; /* version=0 starts at the beginning. */
} epd_read_request_t;
typedef enum {
    EPD_READ_QUEUED, EPD_READ_ACTIVE, EPD_READ_PAUSED,
    EPD_READ_CLOSING, EPD_READ_SAVING
} epd_read_phase_t;
typedef struct {
    epd_read_phase_t phase;
    uint32_t generation;
    int error, save_error;
    bool content_ready, ended, save_pending;
    size_t buffer_bytes, peak_buffer_bytes;
    epd_read_anchor_t position; /* Last presented position, not read-ahead. */
} epd_read_status_t;

/* UI-thread calls only. Source operations run on the platform worker.
 * Open copies strings and function tables; owner.retain pins callback data.
 * Source memory is supplied by the platform. With no open_stream callback,
 * the platform opens source.path on its worker and retains the storage session.
 * Default GBK conversion uses the existing firmware code-page converter.
 * On failure ownership is unchanged.
 * Current built-in capability: text/plain, content version 1. */
bool epd_read_supports(const char *format, uint32_t content_version);
int epd_read_open(const epd_read_request_t *request, epd_read_session_t *session);
int epd_read_pause(epd_read_session_t session, bool paused);
int epd_read_close(epd_read_session_t session);
int epd_read_seek(epd_read_session_t session, const epd_read_anchor_t *position);
int epd_read_status(epd_read_session_t session, epd_read_status_t *status);
/* After an external source returned WAIT, notify from its UI completion. */
int epd_read_content_ready(epd_read_session_t session);
int epd_read_saved(epd_read_session_t session, uint64_t request, int result);
int epd_read_retry_save(epd_read_session_t session);
#endif
