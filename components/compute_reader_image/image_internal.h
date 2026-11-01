#ifndef EPD_RI_RASTER_H
#define EPD_RI_RASTER_H
#if defined(__GNUC__) && !defined(__EXCEPTIONS)
#error "reader-image requires -fexceptions after any inherited -fno-exceptions"
#endif
#include "reader_image.h"
#include <string.h>
#include <assert.h>
#include <limits.h>
typedef struct
{
    const ri_io *io;
    const char *resource;
    int status;
    unsigned max_width, max_height;
    /* Keep enough samples for a centered, aspect-preserving cover crop. */
    bool cover;
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
    size_t position, file_size;
    char error[96];
} ri_raster_decoder_t;

typedef enum
{
    RI_RASTER_GRAY8, RI_RASTER_GRAY_ALPHA8, RI_RASTER_INDEX8,
    RI_RASTER_RGB888, RI_RASTER_RGBA8888, RI_RASTER_RGB565
} ri_raster_pixel_format_t;

bool ri_raster_cancelled(const ri_raster_decoder_t *decoder);
bool ri_raster_read(ri_raster_decoder_t *decoder, void *data, size_t bytes);
size_t ri_raster_read_some(ri_raster_decoder_t *decoder, void *data, size_t bytes);
bool ri_raster_seek(ri_raster_decoder_t *decoder, size_t position);
void *ri_raster_fast_alloc(ri_raster_decoder_t *decoder, size_t bytes);
void *ri_raster_fast_alloc_priority(ri_raster_decoder_t *decoder, size_t bytes);
void ri_raster_fast_free(ri_raster_decoder_t *decoder, void *pointer);
void ri_raster_fit(unsigned sw, unsigned sh, unsigned max_width, unsigned max_height,
                unsigned *width, unsigned *height, bool enlarge);
void ri_raster_target_size(const ri_raster_decoder_t *decoder, unsigned sw, unsigned sh,
                        unsigned *width, unsigned *height);
bool ri_raster_sink_init(ri_raster_decoder_t *decoder, unsigned width, unsigned height, bool ordered);
bool ri_raster_sink_init_banded(ri_raster_decoder_t *decoder, unsigned width, unsigned height, unsigned band_rows);
void ri_raster_sink_end_band(ri_raster_decoder_t *decoder, unsigned source_end);
void ri_raster_sink_row(ri_raster_decoder_t *decoder, unsigned x, unsigned y, unsigned step,
                     const uint8_t *pixels, unsigned count, ri_raster_pixel_format_t format);
bool ri_raster_decode_png(ri_raster_decoder_t *decoder);
bool ri_raster_decode_jpeg(ri_raster_decoder_t *decoder);
bool ri_raster_jpeg_memory(ri_raster_decoder_t *decoder, const uint8_t *memory, size_t size);
#define RI_RASTER_JPEG_INPUT_LIMIT (64u * 1024u) /* replayable header only */
bool ri_raster_jpeg_extended(ri_raster_decoder_t *decoder, uint8_t *prefix, size_t length);
#endif

void *ri_work_alloc(size_t);
void *ri_work_realloc(void *, size_t);
void ri_work_free(void *);
bool ri_scope_enter(const ri_io *);
void ri_scope_leave(void);
int ri_scope_status(void);

bool ri_raster_finish_gray(ri_raster_decoder_t *);
int ri_decode_active(const ri_io *, const char *, unsigned, unsigned, ri_bitmap *);

void *ri_output_alloc(size_t);
