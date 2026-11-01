#ifndef COMPUTE_READER_FONT_H
#define COMPUTE_READER_FONT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRF_API_VERSION 1u
#define CRF_CACHE_BLOCK_BYTES 4096u
#define CRF_MAX_CACHE_BYTES (256u * 1024u)
#define CRF_MAX_MEMORY_BYTES (2u * 1024u * 1024u)
#define CRF_MAX_FILE_BYTES (32u * 1024u * 1024u)
#define CRF_MAX_BATCH 1024u
#define CRF_MAX_SIZE_PX 256u
#define CRF_MAX_BOX 512u
#define CRF_NO_BITMAP UINT32_MAX

typedef struct crf_context crf_context;

typedef enum {
    CRF_OK = 0, CRF_INVALID_ARGUMENT, CRF_NOT_OPEN, CRF_BUSY,
    CRF_NEED_DATA, CRF_IO_ERROR, CRF_SHORT_READ, CRF_STALE_SOURCE,
    CRF_INVALID_FONT, CRF_UNSUPPORTED_FONT, CRF_NO_MEMORY,
    CRF_OUTPUT_FULL, CRF_CANCELLED, CRF_LIMIT
} crf_status;

typedef struct {
    uint32_t source_id, signature, generation, file_size, face_index;
} crf_font_identity;

/* Identity is supplied by the owner of immutable font content. The callback
 * must reject a revoked generation even when the numeric source_id is reused.
 * A synchronous callback may suspend/resume its caller with the C stack intact.
 * OK requires exactly length bytes. NEED_DATA is returned without any retry.
 * Allowed failures: NEED_DATA, IO_ERROR, SHORT_READ, STALE_SOURCE, CANCELLED. */
typedef crf_status (*crf_read_at_fn)(void *user, const crf_font_identity *font,
                                   uint32_t offset, void *dst, size_t length,
                                   size_t *bytes_read);
typedef void *(*crf_alloc_fn)(void *user, size_t bytes);
typedef void (*crf_free_fn)(void *user, void *pointer);
/* Return nonzero to cancel. Called on cache hits as well as storage reads.
 * Shared cancellation state must use the platform's synchronization rules. */
typedef int (*crf_cancel_fn)(void *user);

typedef struct {
    crf_read_at_fn read_at;
    void *read_user;
    crf_alloc_fn alloc;
    crf_free_fn free;
    void *alloc_user;
    crf_cancel_fn cancelled;
    void *cancel_user;
    size_t memory_limit;       /* All allocator request bytes, including context/cache. */
    uint32_t cache_bytes;      /* 1..CRF_MAX_CACHE_BYTES / CRF_CACHE_BLOCK_BYTES blocks. */
    uint32_t operation_limit;  /* Stream seek/read checks per open or glyph; 1..1048576. */
} crf_config;

enum { CRF_WEIGHT_NORMAL = 0, CRF_WEIGHT_BOLD = 1, CRF_WEIGHT_LIGHT = 2 };
typedef struct {
    uint16_t size_px;          /* em pixels, as in TinyTTF. */
    uint8_t weight;            /* Reader's synthetic weight, not an OpenType axis. */
    uint8_t kerning;           /* 0: off; 1: include next_codepoint's pair adjustment. */
    uint8_t oblique;           /* 0/1: horizontal row shear (box_h - 1 - row) / 4. */
    uint8_t reserved[3];       /* Must be zero. */
} crf_style;

typedef struct { uint32_t codepoint, next_codepoint; } crf_codepoint;
enum { CRF_GLYPH_MISSING = 1u, CRF_GLYPH_CONTROL = 2u };
typedef struct {
    uint32_t codepoint, glyph_index;
    uint32_t advance_px, box_w, box_h, stride;
    int32_t ofs_x, ofs_y;      /* TinyTTF convention: x_min, -y_max. */
    uint32_t flags;
    uint32_t bitmap_offset, bitmap_size; /* Relative to caller's A8 output buffer. */
} crf_glyph;

typedef struct { int32_t line_height, base_line, ascent, descent, line_gap; } crf_metrics;
typedef struct {
    crf_status status;
    crf_font_identity font;
    crf_style style;
    crf_metrics metrics;
    uint32_t completed;       /* Only this prefix of glyph records is committed. */
    uint32_t bitmap_bytes;    /* Only this prefix of bitmap bytes is committed. */
    uint32_t required_bytes;  /* Total buffer capacity needed through failing glyph. */
    uint32_t wait_offset, wait_length; /* Exact outstanding read, on NEED_DATA. */
} crf_result;

typedef struct {
    size_t memory_current, memory_peak;
    uint64_t read_calls, read_bytes, cache_hits;
} crf_stats;

/* Defaults: 16 KiB cache, 256 KiB total memory, 1048576 stream operations.
 * Caller sets read_at/alloc/free before create. Allocations must be aligned for
 * pointers and long double (malloc-equivalent alignment). No C library heap or
 * filesystem calls are made by this component. */
void crf_config_init(crf_config *config);
size_t crf_context_bytes(void);
crf_status crf_create(const crf_config *config, crf_context **out);
void crf_destroy(crf_context *context);
crf_status crf_open(crf_context *context, const crf_font_identity *font, crf_result *result);

/* Calls on one context must be serialized; distinct contexts share no state.
 * Callbacks may cooperatively yield, but must not destroy/reenter this context.
 * Arrays and output storage must be disjoint and live until the call returns.
 * render=0 measures; render=1 also emits A8 rows with four-byte stride alignment.
 * A failed current glyph never commits its record; its bitmap tail is scratch.
 * No automatic fallback, shaping, UTF-8 decoding, or .notdef substitution. */
crf_status crf_batch(crf_context *context, const crf_font_identity *expected,
                     crf_style style, const crf_codepoint *input, uint32_t count,
                     int render, crf_glyph *glyphs, uint32_t glyph_capacity,
                     uint8_t *pixels, uint32_t pixel_capacity, crf_result *result);
crf_status crf_measure_pair(crf_context *context, const crf_font_identity *expected,
                            crf_style style, uint32_t codepoint, uint32_t next_codepoint,
                            crf_glyph *glyph, crf_result *result);
void crf_get_stats(const crf_context *context, crf_stats *stats);
const char *crf_status_string(crf_status status);

#ifdef __cplusplus
}
#endif
#endif
