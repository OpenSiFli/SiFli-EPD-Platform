#ifndef EPD_RASTER_H
#define EPD_RASTER_H
#include "platform/epd_app.h"
typedef struct
{
    void *context;
    const char *path;
    bool (*cancel)(void *context);
    int (*read_at)(void *context, size_t position, void *data, size_t size);
    unsigned max_width, max_height;
    /* Keep enough samples for a centered, aspect-preserving cover crop. */
    bool cover;
    bool forward_only;
    /* Background cover generation yields to user-facing image work. */
    bool background;
    unsigned source_width, source_height, width, height;
    unsigned sample_width, sample_height;
    unsigned sum_rows, sum_first_row;
    uint32_t *sum;
    bool wide_sum;
    uint16_t *x_map;
    uint8_t *gray;
    uint8_t palette[256];
    bool ordered_rows;
    bool banded_rows;
    bool cache_hit;
    int fd;
    bool io_error;
    void *read_memory;
    uint8_t *read_buffer;
    size_t read_start, read_count, position, file_size;
    uint32_t content_crc;
    size_t crc_end;
    size_t fast_used;
    char error[96];
} raster_decoder_t;

typedef enum
{
    RASTER_GRAY8, RASTER_GRAY_ALPHA8, RASTER_INDEX8,
    RASTER_RGB888, RASTER_RGBA8888, RASTER_RGB565
} raster_pixel_format_t;

bool raster_cancelled(const raster_decoder_t *decoder);
bool raster_io_open(raster_decoder_t *decoder);
bool raster_read(raster_decoder_t *decoder, void *data, size_t bytes);
size_t raster_read_some(raster_decoder_t *decoder, void *data, size_t bytes);
bool raster_seek(raster_decoder_t *decoder, size_t position);
bool raster_fingerprint(raster_decoder_t *decoder);
void *raster_fast_alloc(raster_decoder_t *decoder, size_t bytes);
void *raster_fast_alloc_priority(raster_decoder_t *decoder, size_t bytes);
void raster_fast_free(raster_decoder_t *decoder, void *pointer);
void raster_fit(unsigned sw, unsigned sh, unsigned max_width, unsigned max_height,
                unsigned *width, unsigned *height, bool enlarge);
void raster_target_size(const raster_decoder_t *decoder, unsigned sw, unsigned sh,
                        unsigned *width, unsigned *height);
bool raster_sink_init(raster_decoder_t *decoder, unsigned width, unsigned height, bool ordered);
bool raster_sink_init_banded(raster_decoder_t *decoder, unsigned width, unsigned height, unsigned band_rows);
void raster_sink_end_band(raster_decoder_t *decoder, unsigned source_end);
void raster_sink_row(raster_decoder_t *decoder, unsigned x, unsigned y, unsigned step,
                     const uint8_t *pixels, unsigned count, raster_pixel_format_t format);
bool raster_decode_png(raster_decoder_t *decoder);
bool raster_decode_jpeg(raster_decoder_t *decoder);
#define RASTER_JPEG_INPUT_LIMIT (2u * 1024u * 1024u)
bool raster_jpeg_extended(raster_decoder_t *decoder, uint8_t *prefix, size_t length);
bool raster_decode(raster_decoder_t *decoder);
void raster_decoder_clear(raster_decoder_t *decoder);
#endif
