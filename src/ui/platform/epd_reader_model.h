/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_READER_MODEL_H
#define EPD_READER_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EPD_READER_PATH_MAX 256u
#define EPD_READER_TITLE_MAX 128u

/* The layout of scanned file metadata also preserves existing book histories. */
typedef struct {
    char path[EPD_READER_PATH_MAX], name[EPD_READER_TITLE_MAX];
    uint32_t size, modified;
    int progress;
    uint32_t format;
} epd_reader_file_t;

/* Legacy record-stream cursor, used while migrating version-2 histories. */
typedef struct { uint32_t record, byte; } document_cursor_t;

#define BOOK_CODEC_ABI 2u
enum { BOOK_OK = 0, BOOK_ERROR = -1, BOOK_CANCELLED = -2,
       BOOK_NO_MEMORY = -3, BOOK_UNSUPPORTED = -4, BOOK_IO_ERROR = -5,
       BOOK_NO_SPACE = -6 };
enum { BOOK_STYLE_NORMAL = 0, BOOK_STYLE_BOLD = 1, BOOK_STYLE_ITALIC = 2,
       BOOK_STYLE_PREFORMAT = 4, BOOK_STYLE_CENTER = 8, BOOK_STYLE_RIGHT = 16,
       BOOK_STYLE_HEADING = 32, BOOK_STYLE_LINK = 64,
       BOOK_STYLE_NOTEREF = 128, BOOK_STYLE_PAGE_START = 256 };

/* Input streams are owned by their opener. Reads return bytes, zero at EOF,
 * or a negative BOOK_* result. seek uses an absolute source byte offset. */
typedef struct book_stream {
    void *user;
    uint64_t size;
    int (*read)(void *user, void *buffer, size_t length);
    int (*seek)(void *user, uint64_t offset);
    void (*close)(void *user);
} book_stream_t;

typedef struct book_resources {
    void *user;
    int (*open)(void *user, const char *base, const char *reference, book_stream_t *stream);
} book_resources_t;

typedef struct book_sink book_sink_t;
struct book_sink {
    void *user;
    bool (*cancelled)(void *user);
    /* All callbacks consume/copy borrowed arguments before returning. */
    int (*text)(void *user, const char *utf8, size_t length, unsigned style);
    int (*paragraph)(void *user);
    int (*anchor)(void *user, const char *id, const char *title, unsigned level);
    /* NULL requests the language-independent missing-image graphic. */
    int (*image)(void *user, const char *id, book_stream_t *stream);
    int (*metadata)(void *user, const char *title, const char *author);
    int (*link)(void *user, const char *target, bool begin);
    int (*markup)(const book_sink_t *sink, book_stream_t *stream,
                  const char *base, const book_resources_t *resources);
    int (*svg)(void *user, const char *xml, size_t length, const char *base,
               const book_resources_t *resources);
    bool (*heading_target)(void *user, const char *id);
};

typedef struct book_allocator {
    void *(*alloc)(size_t size);
    void *(*realloc)(void *memory, size_t size);
    void (*free)(void *memory);
} book_allocator_t;

/* Legacy synchronous converter interface. All callbacks end with convert(). */
typedef struct book_convert_api {
    uint32_t abi, size;
    int (*convert)(const char *path, const book_sink_t *sink,
                   const book_allocator_t *allocator, char *error, size_t error_size);
} book_convert_api_t;
#endif
