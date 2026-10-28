/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SPEECH_AUDIO_H
#define SPEECH_AUDIO_H
#include "speech_internal.h"

#define SPEECH_AUDIO_PACKET_BYTES 6400u
#define SPEECH_AUDIO_RAW_BYTES (32u * 1024u)
#define SPEECH_AUDIO_SEND_BYTES (128u * 1024u)

typedef struct speech_audio_input speech_audio_input_t;
typedef struct speech_audio_output speech_audio_output_t;
typedef struct {
    uint32_t samples, elapsed_ms, raw_high_water, send_high_water;
    bool finished, duration_limit;
    epd_speech_error_t error;
} speech_audio_input_status_t;

/* Worker-only open waits for optional DSP initialization and 200 ms microphone settling.
 * Caller has already obtained exclusive media ownership. Capture progresses on its
 * own thread, independent of network reads. cancel/user remain valid until close. */
bool speech_audio_input_available(void);
epd_speech_error_t speech_audio_input_open(speech_cancel_fn cancel, void *user,
                                         speech_audio_input_t **out);
epd_speech_error_t speech_audio_input_open_limited(speech_cancel_fn cancel, void *user,
                                                  uint32_t limit_ms, speech_audio_input_t **out);
/* Nonblocking: accepted mono S16LE bytes, zero if empty, or -epd_speech_error_t.
 * capacity must be even. A 6400-byte buffer holds 200 ms. finished means capture
 * and processing have stopped; read until zero to consume the final short packet. */
int speech_audio_input_read(speech_audio_input_t *input, void *pcm, size_t capacity);
/* Call only after the network accepted these bytes. Reads may still be waiting
 * in a TLS write; they remain part of the two-second unsent-audio budget. */
void speech_audio_input_ack(speech_audio_input_t *input, size_t bytes);
void speech_audio_input_status(speech_audio_input_t *input, speech_audio_input_status_t *out);
/* Nonblocking graceful stop; read remains valid until close. */
void speech_audio_input_finish(speech_audio_input_t *input);
/* Stop/discard, join the capture thread and release resources; never frees live callbacks. */
void speech_audio_input_close(speech_audio_input_t *input);

/* All output calls belong to one supply thread. Source PCM is never modified. */
epd_speech_error_t speech_audio_output_open(speech_cancel_fn cancel, void *user,
                                          speech_audio_output_t **out);
/* Nonblocking: accepted source bytes, zero for backpressure, or -error.
 * On zero retry exactly the same pointer and length, including across pause.
 * Advance the source cursor only by a positive result. -EPD_SPEECH_BUSY requests
 * an explicit route handoff; call output_handoff then retry the same block. */
int speech_audio_output_write(speech_audio_output_t *output, const void *pcm, size_t bytes);
/* Worker-only route change: drain the reachable old route, close, and reopen.
 * Sudden device loss returns OUTPUT_INTERRUPTED without advancing the cursor. */
epd_speech_error_t speech_audio_output_handoff(speech_audio_output_t *output);
/* Completes only after pending source has been retried successfully. Uses bounded
 * software-cache/pipeline settling, not a hardware playback-position counter.
 * Cancellation is checked while waiting; returns within a 2 second deadline. */
epd_speech_error_t speech_audio_output_drain(speech_audio_output_t *output);
/* Stop without draining; waits for the SDK's close acknowledgement before free.
 * Call drain successfully first for pause or natural completion. */
void speech_audio_output_close(speech_audio_output_t *output);
#endif
