/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_HTTP_INTERNAL_H
#define EPD_HTTP_INTERNAL_H
#include "epd_http_stream.h"
#include "epd_memory.h"
#include "network.h"
#include <tls_client.h>

#define IO_SLICE 1024u
typedef struct
{
    const epd_http_request_t *request;
    epd_http_response_t *response;
    const epd_http_stream_request_t *stream;
    epd_http_stream_response_t *stream_response;
    MbedTLSSession *tls;
    network_snapshot_t route;
    network_interface_snapshot_t local_interface;
    bool local_route;
    rt_tick_t start, phase_start, progress;
    uint64_t timeout_ticks, connect_ticks, send_ticks, first_ticks, idle_ticks;
    size_t limit;
    unsigned phase;
    uint16_t port;
    unsigned char *buffer;
    bool valid_data, stopped, http_complete, error_status;
} http_reader_t;

bool epd_http_core_fail(http_reader_t *reader, const char *stage, int detail);
bool epd_http_core_expired(http_reader_t *reader);
bool epd_http_core_perform(http_reader_t *reader, char **body, size_t *used, bool *gzip);
#define fail epd_http_core_fail
#define expired epd_http_core_expired
#endif
