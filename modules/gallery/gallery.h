#ifndef GALLERY_H
#define GALLERY_H

#include "storage.h"
#include "platform/epd_app.h"
#include "../common/image/raster.h"
#include "../common/memory_retry.h"

#define GALLERY_ROWS 8
#define GALLERY_WIDTH 620
#define GALLERY_HEIGHT 966
#define GALLERY_MAX_SIDE 16384u
#define GALLERY_MAX_PIXELS (64u * 1024u * 1024u)

typedef enum {
    GALLERY_STATUS_NONE,
    GALLERY_STATUS_LOADING,
    GALLERY_STATUS_EMPTY,
    GALLERY_STATUS_STORAGE,
    GALLERY_STATUS_REMOVED,
    GALLERY_STATUS_PATH,
    GALLERY_STATUS_DEPTH,
    GALLERY_STATUS_PAGE,
    GALLERY_STATUS_UNAVAILABLE,
    GALLERY_STATUS_SERVICE,
    GALLERY_STATUS_MEMORY,
    GALLERY_STATUS_OPEN,
    GALLERY_STATUS_DECODE,
    GALLERY_STATUS_CONTENT,
} gallery_status_t;

typedef enum { GALLERY_SCAN, GALLERY_IMAGE } gallery_job_kind_t;
typedef struct
{
    uint32_t serial;
    gallery_job_kind_t kind;
    char path[STORAGE_PATH_MAX];
    unsigned first;
} gallery_job_t;

typedef struct
{
    char name[256];
    bool directory;
} gallery_entry_t;

typedef struct
{
    uint32_t serial;
    unsigned references;
    epd_memory_retry_t memory_retry;
    gallery_job_kind_t kind;
    gallery_status_t error_code;
    char error[96]; /* Decoder diagnostic; UI uses error_code. */
    unsigned count;
    bool more;
    gallery_entry_t entries[GALLERY_ROWS];
    unsigned source_width, source_height;
    lv_image_dsc_t image;
    void *pixels;
} gallery_result_t;

typedef raster_decoder_t gallery_decoder_t;
#define gallery_read raster_read
#define gallery_read_some raster_read_some
#define gallery_seek raster_seek
#define gallery_fingerprint raster_fingerprint
#define gallery_decoder_clear raster_decoder_clear
static inline const gallery_job_t *gallery_decoder_job(const gallery_decoder_t *decoder)
{ return (const gallery_job_t *)decoder->context; }

bool gallery_start(void);
void gallery_stop(void);
uint32_t gallery_submit(gallery_job_kind_t kind, const char *path, unsigned first);
void gallery_cancel(void);
void gallery_cancel_request(uint32_t serial);
bool gallery_failed(uint32_t serial);
bool gallery_cancelled(uint32_t serial);
gallery_result_t *gallery_take(uint32_t serial);
void gallery_result_free(gallery_result_t *result);
bool gallery_read(gallery_decoder_t *decoder, void *data, size_t bytes);
size_t gallery_read_some(gallery_decoder_t *decoder, void *data, size_t bytes);
bool gallery_seek(gallery_decoder_t *decoder, size_t position);
bool gallery_fingerprint(gallery_decoder_t *decoder);
void gallery_fit(unsigned sw, unsigned sh, unsigned *width, unsigned *height, bool enlarge);
extern const uint16_t gallery_gray16_colors[16];
bool gallery_image_alloc(gallery_result_t *result, unsigned width, unsigned height);
void gallery_image_clean(const gallery_result_t *result);
bool gallery_decode(const gallery_job_t *job, gallery_decoder_t *decoder, gallery_result_t *result);
bool gallery_render(gallery_decoder_t *decoder, gallery_result_t *result, const gallery_job_t *job);
void gallery_decoder_clear(gallery_decoder_t *decoder);
bool gallery_cache_load(gallery_decoder_t *decoder, gallery_result_t *result);
void *gallery_cache_pack(const gallery_decoder_t *decoder, const gallery_result_t *result, size_t *size);
void gallery_cache_save(const gallery_job_t *job, const void *data, size_t size);

#endif
