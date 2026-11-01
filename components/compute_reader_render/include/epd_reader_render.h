#ifndef EPD_READER_RENDER_H
#define EPD_READER_RENDER_H
#include "reader_render.h"
#include "epd_compute.h"

typedef struct epd_reader_render_client *epd_reader_render_t;

/* Worker-only synchronous API. Borrowed frame must be a cache-line-aligned
 * compute buffer with capacity>=stride*height, and outlive successful close.
 * Each session exclusively borrows its entire frame, even though STEP syncs
 * only its row window. FINISH success permits readers after command returns;
 * they must finish before another INIT. Failed CLOSE keeps this storage live.
 * No fallback.
 * Admission may return BUSY; open does not spin waiting for another session. */
int epd_reader_render_open(const epd_compute_buffer_t *frame, rr_frame format,
                           epd_compute_purpose_t purpose, epd_reader_render_t *out);
/* Worker-only queued admission, using the common scheduler's arrival order.
 * Create and publish wait with epd_compute_wait_create before starting this
 * call. While admission/remote OPEN is pending, the cancellation owner calls
 * epd_compute_wait_cancel(wait). A wait serves one call; cancellation is sticky.
 * After return, synchronize its cancellation users before wait_destroy. On
 * success, use epd_reader_render_cancel(client) for subsequent cancellation.
 * Frame remains borrowed while waiting. Both open variants may fail with a
 * non-NULL terminal client if rollback CLOSE fails. Keep that client AND the
 * borrowed frame until a later shutdown succeeds. A NULL output means this
 * function has released its allocations. There is no BUSY polling/reopen loop. */
int epd_reader_render_open_wait(const epd_compute_buffer_t *frame, rr_frame format,
                                epd_compute_purpose_t purpose, epd_compute_wait_t wait,
                                epd_reader_render_t *out);
/* Assigns version/bytes/id, copies <=16KiB input into shared staging, then
 * submits bounded row steps until complete. Other command fields are preserved.
 * Never call on the UI thread. result may be NULL. */
int epd_reader_render_command(epd_reader_render_t client, const rr_command *command,
                              const void *source, size_t source_bytes,
                              bool (*cancelled)(void *), void *user, rr_result *result);
/* For larger GRAY4/RGB565 images, borrow an immutable shared source instead of copying it.
 * source_bytes must match rr_source_bytes. A8 remains limited to 16KiB.
 * The shared source must remain live and unchanged until this call returns. */
int epd_reader_render_command_shared(epd_reader_render_t client, const rr_command *command,
                                     const epd_compute_buffer_t *source, uint32_t source_bytes,
                                     bool (*cancelled)(void *), void *user, rr_result *result);
/* Copy an A8 glyph into existing staging; OK means consumed, MORE means
 * flush and retry (or use command for a glyph exceeding the batch limits).
 * Source may be released after return. Flush before any non-batch command.
 * At most eight glyphs / 16KiB per flush; no implicit cross-core calls here. */
int epd_reader_render_a8_enqueue(epd_reader_render_t client, const rr_command *command,
                                 const void *source, size_t source_bytes);
int epd_reader_render_a8_flush(epd_reader_render_t client,
                               bool (*cancelled)(void *), void *user);
/* May be called from a cancellation owner while a command waits. Caller must
 * synchronize lifetime with close. All other calls are serialized by worker. */
void epd_reader_render_cancel(epd_reader_render_t client);
int epd_reader_render_set_purpose(epd_reader_render_t client, epd_compute_purpose_t purpose);
/* Call after any outstanding command has returned; does not free borrowed frame.
 * A close attempt makes the client terminal even if CLOSE fails. On failure
 * retain client + borrowed frame, report the status, and retry cleanup later. */
int epd_reader_render_close(epd_reader_render_t client);
/* Cancel then close a terminal client; set *client=NULL ONLY on successful
 * close. On failure keep the exact handle for the worker's cleanup queue.
 * Caller owns and retains borrowed buffers until successful shutdown; this
 * function does not spin or force-free a session whose CLOSE was rejected.
 * Use after outstanding command/open calls and cancellation users quiesce. */
int epd_reader_render_shutdown(epd_reader_render_t *client);
#endif
