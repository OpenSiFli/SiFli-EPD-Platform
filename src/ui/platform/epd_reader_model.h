/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_READER_MODEL_H
#define EPD_READER_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../../modules/common/reader/compute/include/epd_reader_paragraph.h"

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

#define BOOK_CODEC_ABI 3u
enum { BOOK_OK = 0, BOOK_ERROR = -1, BOOK_CANCELLED = -2,
       BOOK_NO_MEMORY = -3, BOOK_UNSUPPORTED = -4, BOOK_IO_ERROR = -5,
       BOOK_NO_SPACE = -6 };
enum { BOOK_STYLE_NORMAL = 0, BOOK_STYLE_BOLD = 1, BOOK_STYLE_ITALIC = 2,
       BOOK_STYLE_PREFORMAT = 4, BOOK_STYLE_CENTER = 8, BOOK_STYLE_RIGHT = 16,
       BOOK_STYLE_HEADING = 32, BOOK_STYLE_LINK = 64,
       BOOK_STYLE_NOTEREF = 128, BOOK_STYLE_PAGE_START = 256, BOOK_STYLE_PARAGRAPH_START = 512 };

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

/* Positive lengths are pixels. Negative lengths are tenths of a percent. */
#define BOOK_PICTURE_AUTO INT16_MAX
#define BOOK_PICTURE_ZERO INT16_MIN
enum { BOOK_PICTURE_WIDTH, BOOK_PICTURE_HEIGHT, BOOK_PICTURE_MAX_WIDTH,
       BOOK_PICTURE_MAX_HEIGHT, BOOK_PICTURE_LENGTHS };
typedef struct {
    int16_t lengths[BOOK_PICTURE_LENGTHS];
} book_picture_style_t;

typedef struct {
    uint16_t width, height;
    book_picture_style_t style;
} book_picture_layout_t;

static inline unsigned book_picture_length(int16_t length, unsigned span, unsigned fallback)
{
    if (!length || length == BOOK_PICTURE_AUTO) return fallback;
    if (length == BOOK_PICTURE_ZERO) return 0;
    if (length > 0) return (unsigned)length;
    unsigned pixels = ((uint64_t)(-length) * span + 500u) / 1000u;
    return pixels ? pixels : 1;
}

static inline void book_picture_size(const book_picture_layout_t *picture,
                                     unsigned page_width, unsigned page_height,
                                     unsigned *width, unsigned *height)
{
    const int16_t *lengths = picture->style.lengths;
    unsigned w = book_picture_length(lengths[BOOK_PICTURE_WIDTH], page_width, 0);
    unsigned h = book_picture_length(lengths[BOOK_PICTURE_HEIGHT], page_height, 0);
    unsigned max_w = book_picture_length(lengths[BOOK_PICTURE_MAX_WIDTH], page_width, page_width);
    unsigned max_h = book_picture_length(lengths[BOOK_PICTURE_MAX_HEIGHT], page_height, page_height);
    *width = *height = 0;
    if (!picture->width || !picture->height || !page_width || !page_height ||
        !max_w || !max_h || lengths[BOOK_PICTURE_WIDTH] == BOOK_PICTURE_ZERO ||
        lengths[BOOK_PICTURE_HEIGHT] == BOOK_PICTURE_ZERO)
        return;
    if (max_w > page_width) max_w = page_width;
    if (max_h > page_height) max_h = page_height;
    if (!w && !h) { w = picture->width; h = picture->height; }
    else if (!w) w = ((uint64_t)h * picture->width + picture->height / 2u) / picture->height;
    else if (!h) h = ((uint64_t)w * picture->height + picture->width / 2u) / picture->width;
    if (!w) w = 1;
    if (!h) h = 1;
    if (w > max_w) { h = ((uint64_t)h * max_w + w / 2u) / w; w = max_w; }
    if (h > max_h) { w = ((uint64_t)w * max_h + h / 2u) / h; h = max_h; }
    *width = w ? w : 1;
    *height = h ? h : 1;
}

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
    /* Applies to the next picture callback in the current markup element. */
    void (*picture_style)(void *user, const book_picture_style_t *style);
    void (*paragraph_style)(void *user, const epd_paragraph_style_t *style);
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
