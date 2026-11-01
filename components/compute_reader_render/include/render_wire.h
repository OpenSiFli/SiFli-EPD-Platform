#ifndef READER_RENDER_WIRE_H
#define READER_RENDER_WIRE_H
#include "reader_render.h"
#include "epd_compute_protocol.h"

#define EPD_READER_RENDER_COMPONENT "epd/reader-render"
#define EPD_READER_RENDER_VERSION 3u
#define RR_CONTROL_BYTES 128u
#define RR_RESULT_OFFSET 96u
#define RR_A8_BATCH_MAX 8u
#define RR_A8_BATCH_HEIGHT 64u
#define RR_A8_BATCH_HEADER (RR_A8_BATCH_MAX * sizeof(rr_command))
/* Staging contains eight command slots followed by contiguous A8 bitmaps.
 * Batch commands use consecutive IDs. width=count, src_width=staging bytes.
 * One STEP completes the batch, with row-level cancellation in the kernel. */
int rr_a8_batch_window(rr_frame frame, const void *data, uint32_t bytes,
                       uint32_t count, rr_window *window);

typedef struct {
    uint32_t version, bytes;
    epd_compute_ref_t frame;
    rr_frame format;
} rr_open_wire;
typedef struct {
    rr_command command;
    epd_compute_ref_t frame, source;
} rr_command_wire;

/* Render v3 adds an explicit frame format, using the existing compute protocol.
 * OPEN has control(0), whole frame(1), staging source(2); frame ref offset=0,
 * length=stride*height. Frame address/geometry and control address stay bound.
 * STEP has control(0), then a frame window if nonempty, then a source window
 * if needed. Each window is a 32-byte-aligned region with capacity=length;
 * its ref has offset=0 and length=window bytes. Absent refs are NONE,0,0.
 * rr_sync_windows derives both envelopes from the validated command and row
 * progress. The source base inferred from its window must stay fixed over MORE.
 * Control/frame are READ|WRITE; source is READ. Params offset=0, result=96.
 * Each STEP completes the ordinary cache handoff and echoes id/version. Single
 * commands advance at most 32 rows; RR_BATCH_A8 uses the bounded format above.
 * FINISH needs no frame region and
 * makes the completed frame readable; a later INIT requires exclusive access.
 * OPEN is 36 bytes; STEP remains 92 bytes. Both peers must use render v3.
 * All metadata is little-endian fixed-width, no pointers or size_t on wire. */
extern const epd_compute_ops_t epd_reader_render_remote_ops;
typedef char rr_open_layout[sizeof(rr_open_wire) == 36 ? 1 : -1];
typedef char rr_wire_layout[sizeof(rr_command_wire) == 92 ? 1 : -1];
#endif
