#ifndef READER_RENDER_H
#define READER_RENDER_H
#include <stddef.h>
#include <stdint.h>

#define RR_VERSION 3u
#define RR_ROWS_PER_STEP 32u
#define RR_INPUT_BYTES (16u * 1024u)
#define RR_MAX_DIM 8192u
#define RR_MAX_BUFFER_BYTES (32u * 1024u * 1024u)
#define RR_REMOTE_STATE_MAX 256u

enum { RR_INIT = 1, RR_BLIT_A8, RR_BLIT_GRAY4, RR_FINISH,
       RR_SCALE_GRAY4_TO_RGB565, RR_FILLRECT, RR_BLIT_RGB565, RR_BATCH_A8 };
enum { RR_OK = 0, RR_MORE = 1, RR_CANCELLED = -1, RR_INVALID = -2, RR_BAD_STATE = -3 };
#define RR_A8_WHITE_BG 1u
/* Gray4 with no per-row padding, pixel index=y*src_width+x. Only this flag
 * permits src_stride=0. Otherwise every row starts in the high nibble. */
#define RR_GRAY4_CONTIGUOUS 2u

/* L8 is opaque luminance: 0=black, 255=white. Zero selects RGB565. */
enum { RR_FORMAT_RGB565 = 0, RR_FORMAT_L8 = 1 };
typedef struct { uint32_t width, height, stride, format; } rr_frame;
static inline uint32_t rr_pixel_bytes(rr_frame frame)
{ return frame.format == RR_FORMAT_L8 ? 1u : 2u; }
typedef struct { uint32_t left, top, right, rows; } rr_bounds;
typedef struct { uint32_t offset, bytes; } rr_window;
typedef struct {
    uint32_t version, bytes, id, operation, flags;
    int32_t x, y;
    uint32_t width, height;
    int32_t clip_x, clip_y;
    uint32_t clip_w, clip_h;
    uint32_t src_width, src_height, src_stride;
    uint32_t fill_rgb565;
} rr_command;
/* RR_BLIT_RGB565 takes little-endian pixels without alpha.
 * flags=0, src_stride>=src_width*2; width/height must fit source dimensions.
 * Source rows may have padding. Command/wire layouts and existing opcodes are
 * unchanged; RGB565 is opcode 7. L8 destinations receive luminance directly. */
typedef struct {
    uint32_t version, bytes, id;
    int32_t status;
    uint32_t rows_done, rows_total, rows_this_step;
} rr_result;
typedef int (*rr_cancel_fn)(void *user);

/* Fixed-size state; no allocator or private frame. All source/frame storage
 * must be disjoint and remain stable across MORE. A cancelled/partial frame
 * must be discarded or reinitialized; FINISH only succeeds after full INIT. */
typedef struct {
    rr_frame frame;
    uint8_t *pixels;
    size_t capacity;
    rr_command command;
    const uint8_t *source;
    size_t source_bytes;
    uint32_t row, rows, left, top, right, last_id;
    int active, initialized, finished, poisoned;
} rr_kernel;

/* RGB565 is little-endian. Stride is a multiple of 4 and >=width*pixel_bytes.
 * INIT writes white to every frame byte, including row padding. */
int rr_kernel_bind(rr_kernel *kernel, rr_frame frame, void *pixels, size_t capacity);
/* Defaults the clipping rectangle to the complete frame, flags/source to zero.
 * Caller supplies a nonzero id, destination rectangle and source dimensions. */
void rr_command_init(rr_command *command, uint32_t operation, rr_frame frame);
int rr_source_bytes(const rr_command *command, size_t *bytes);
/* Validate header, flags, destination/source dimensions and source length
 * before deriving any cache window. Does not dereference pixel storage. */
int rr_validate_command(const rr_command *command, size_t *source_bytes);
rr_bounds rr_command_bounds(rr_frame frame, const rr_command *command);
/* One contiguous, 32-byte-aligned envelope for the next bounded row batch.
 * Requires a bound frame and rr_validate_command success. Empty/FINISH commands
 * have no frame window. INIT includes row padding. Source covers sampled rows
 * (including row padding); a clipped source command carries its first line. */
void rr_sync_windows(rr_frame frame, const rr_command *command, uint32_t row,
                     rr_window *pixels, rr_window *source);
int rr_kernel_step(rr_kernel *kernel, const rr_command *command,
                   const void *source, size_t source_bytes,
                   rr_cancel_fn cancelled, void *user, rr_result *result);

typedef char rr_command_layout[sizeof(rr_command) == 68 ? 1 : -1];
typedef char rr_result_layout[sizeof(rr_result) == 28 ? 1 : -1];
#endif
