#ifndef EPD_PCM_OUTPUT_H
#define EPD_PCM_OUTPUT_H

#include <stdint.h>
#include <audio_server.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct epd_pcm_output epd_pcm_output_t;

#ifndef EPD_PCM_STARTUP_SILENCE_MS
#define EPD_PCM_STARTUP_SILENCE_MS 100u
#endif
#ifndef EPD_PCM_TAIL_SILENCE_MS
#define EPD_PCM_TAIL_SILENCE_MS 100u
#endif
#ifndef EPD_PCM_SPEAKER_PIPELINE_MS
#define EPD_PCM_SPEAKER_PIPELINE_MS 20u
#endif
#ifndef EPD_PCM_A2DP_PIPELINE_MS
#define EPD_PCM_A2DP_PIPELINE_MS 120u
#endif

/* Input is signed 16-bit mono PCM at rate Hz. Calls are serialized by the caller. */
epd_pcm_output_t *epd_pcm_output_open(audio_type_t type, uint32_t rate,
    audio_server_callback_func callback, void *data);

/* Returns accepted input bytes (possibly a prefix), 0 for a full cache,
 * -1 for suspension, or -2 for invalid input/output state. On 0, retry the same input;
 * the converted block is retained. Negative results do not accept input. */
int epd_pcm_output_write(epd_pcm_output_t *output, const void *mono_pcm,
    uint32_t bytes);

/* Estimated duration of PCM accepted into the SDK cache; excludes pending input. */
uint32_t epd_pcm_output_cached_ms(epd_pcm_output_t *output);

/* Closes immediately; the caller owns any wait for queued audio. */
int epd_pcm_output_close(epd_pcm_output_t *output);

/* Optional fixed-route output. Existing open/write callers retain their route
 * behavior. Controlled writes return -3 before accepting a new block when the
 * preferred route changes; -4 means the old device can no longer drain. */
epd_pcm_output_t *epd_pcm_output_open_controlled(audio_type_t type, uint32_t rate,
    audio_server_callback_func callback, void *data);
bool epd_pcm_output_route_changed(epd_pcm_output_t *output);
/* Poll from the supply thread. 1: source PCM drained and pipeline settled,
 * 0: pending, -2: uncommitted source input, -4: interrupted or cache query failed.
 * Source retry must finish first. A sub-block tail of appended silence may remain.
 * This estimate includes 100 ms padding plus 20 ms speaker / 120 ms A2DP margin;
 * it does not measure the remote headset's actual playback position. */
int epd_pcm_output_drain_step(epd_pcm_output_t *output, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
