#ifndef CLEANUP_RASTER_H
#define CLEANUP_RASTER_H
#include "epd_compute.h"
#include "epd_memory.h"
#include <stddef.h>
#include <stdbool.h>
enum { RASTER_GRAY8 };
typedef struct {
    bool forward_only, background, io_error;
    size_t position, file_size;
    bool (*cancel)(void *);
    void *context;
    unsigned sample_width, sample_height;
} raster_decoder_t;
bool raster_seek(raster_decoder_t *, size_t);
bool raster_read(raster_decoder_t *, void *, size_t);
bool raster_cancelled(const raster_decoder_t *);
bool raster_sink_init_banded(raster_decoder_t *, unsigned, unsigned, unsigned);
void raster_sink_row(raster_decoder_t *, unsigned, unsigned, unsigned, const void *, unsigned, int);
void raster_sink_end_band(raster_decoder_t *, unsigned);
int raster_jpeg_remote(raster_decoder_t *, unsigned);
#endif
