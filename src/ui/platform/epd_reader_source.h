/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_READER_SOURCE_H
#define EPD_READER_SOURCE_H
#include "epd_reader_model.h"

#define EPD_READER_SOURCE_ABI 1u
#define EPD_READER_POSITION_VERSION 1u
#define EPD_READER_RECORD_TEXT_MAX 1024u

typedef enum {
    EPD_READ_OK = 0, EPD_READ_END = 1, EPD_READ_WAIT = 2, EPD_READ_FULL = 3,
    EPD_READ_YIELD = 4,
    EPD_READ_ERROR = -1, EPD_READ_CANCELLED = -2, EPD_READ_NO_MEMORY = -3,
    EPD_READ_UNSUPPORTED = -4, EPD_READ_IO = -5, EPD_READ_INVALID = -6,
    EPD_READ_BUSY = -7, EPD_READ_STALE = -8
} epd_read_result_t;

/* unit is a stable spine/part index within an unchanged content identity.
 * offset is a source byte/text offset interpreted by the selected parser.
 * byte selects a UTF-8 display fragment within that source position.
 * encoding identifies the original TXT encoding, not the UI font. */
typedef struct {
    uint32_t version, unit, encoding, byte;
    uint64_t offset;
} epd_read_anchor_t;

typedef enum {
    EPD_READ_TEXT = 1, EPD_READ_PARAGRAPH, EPD_READ_ANCHOR,
    EPD_READ_LINK, EPD_READ_IMAGE, EPD_READ_METADATA
} epd_read_record_kind_t;

/* Data follows this header in the batch, padded to eight bytes. Headers are
 * copied with memcpy; callers must not assume the input buffer is aligned.
 * TEXT contains UTF-8 bytes without NUL; IMAGE contains a resource identifier,
 * never decoded pixels. ANCHOR/LINK/METADATA use NUL-separated UTF-8 fields. */
typedef struct {
    uint32_t kind, bytes, style, link;
    epd_read_anchor_t source, next;
} epd_read_record_t;

typedef struct {
    uint8_t *data;
    size_t capacity, used;
    uint32_t count;
} epd_read_batch_t;

typedef struct {
    const char *app_id, *identity, *path;
    uint32_t content_version;
    uint32_t encoding; /* TXT: 0 auto, 1 UTF-8, 2 GBK. */
    book_resources_t resources;
    book_allocator_t memory;
    void *user;
    bool (*cancelled)(void *user);
    int (*open_stream)(void *user, const char *path, book_stream_t *stream);
    uint16_t (*legacy_to_unicode)(uint16_t code, uint16_t code_page);
} epd_read_source_options_t;

/* All operations run on the platform reader worker. next obeys the supplied
 * byte budget and may return FULL only after publishing at least one record.
 * WAIT means an external resource is pending, not EOF; an explicit readiness
 * notification resumes it. seek/next return YIELD after a bounded unit of
 * replay or parsing with no output, and resume on the next worker iteration.
 * YIELD requires forward progress or cancellation; it is not an I/O poll.
 * open initializes an instance with OK, or returns WAIT with no instance.
 * The caller retains
 * options, callback owners and module code until close and all streams end.
 * open failure leaves *instance NULL and releases temporary allocations.
 * A returned stream belongs to its caller, who closes it before close(). */
typedef struct {
    uint32_t abi, size, content_version;
    const char *format;
    int (*open)(const epd_read_source_options_t *options, void **instance);
    int (*next)(void *instance, epd_read_batch_t *batch);
    int (*seek)(void *instance, const epd_read_anchor_t *position);
    int (*resource)(void *instance, const char *identifier, book_stream_t *stream);
    void (*close)(void *instance);
} epd_read_provider_t;

#endif
