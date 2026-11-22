/* Actual callback dispatch without a core/runtime or thread-context adapter. */
#include "epd_activity_work.h"
#include "epd_http_stream.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) abort(); } while (0)
#define RT_ERROR 1
#define RT_EINTR 4
#define RT_EFULL 28
typedef struct {
    const epd_http_request_t *request;
    epd_http_response_t *response;
    const epd_http_stream_request_t *stream;
    epd_http_stream_response_t *stream_response;
    unsigned char *buffer;
    size_t limit;
    bool valid_data, error_status, stopped;
} http_reader_t;
static bool cancelled_flag, header_ok = true;
static unsigned calls;
static epd_http_stream_action_t action;
static bool fail(http_reader_t *r, const char *stage, int error)
{ r->response->stage = stage; r->response->detail = error; return false; }
static bool callback_cancelled(http_reader_t *);
static bool expired(http_reader_t *r) { return callback_cancelled(r); }
#include "http_callbacks_actual.inc"
static bool cancel(void *p) { CHECK(p == &calls); ++calls; return cancelled_flag; }
static bool header(void *p, const char *name, const char *value)
{ CHECK(p == &calls && name && value); ++calls; return header_ok; }
static void sent(void *p) { CHECK(p == &calls); ++calls; }
static epd_http_stream_action_t body(void *p, const unsigned char *data, size_t size, bool *valid)
{ CHECK(p == &calls && data && size == 1); *valid = true; ++calls; return action; }
int main(void) {
    epd_http_request_t request = {0}; epd_http_response_t diagnostics = {0};
    epd_http_stream_request_t stream = {0}; epd_http_stream_response_t response = {0};
    http_reader_t reader = {0}; size_t used = 0;
    request.cancelled = cancel; request.on_header = header; request.user = &calls;
    stream.on_body = body; stream.on_sent = sent; stream.user = &calls;
    reader.request = &request; reader.response = &diagnostics;
    reader.stream = &stream; reader.stream_response = &response;
    reader.buffer = (unsigned char *)"x"; reader.limit = 20;
    CHECK(header_callback(&reader, "name", "value")); CHECK(sent_callback(&reader));
    CHECK(deliver(&reader, 1, &used) && reader.valid_data && response.first_data_received);
    action = EPD_HTTP_STREAM_COMPLETE; CHECK(deliver(&reader, 1, &used) && reader.stopped);
    action = EPD_HTTP_STREAM_CANCEL; CHECK(!deliver(&reader, 1, &used));
    action = EPD_HTTP_STREAM_ERROR; CHECK(!deliver(&reader, 1, &used));
    header_ok = false; CHECK(!header_callback(&reader, "name", "value"));
    cancelled_flag = true; CHECK(callback_cancelled(&reader)); CHECK(calls >= 8);
    puts("PASS HTTP callback dispatch without ownership adapter"); return 0;
}
