#ifndef EPD_RECORD_H
#define EPD_RECORD_H
#include "epd_speech.h"
typedef struct epd_record epd_record_t;
typedef bool (*epd_record_cancel_fn)(void *user);
typedef struct {
    uint32_t milliseconds, bytes;
    bool paused, finished;
    epd_speech_error_t error;
} epd_record_status_t;
/* One application worker owns each handle. Pump at least every 100 ms.
 * The path must be on the current TF-card session. Capture uses bounded PSRAM
 * and the platform audio arbiter; it never restores paused playback. */
epd_speech_error_t epd_record_open(const char *path, uint32_t card_session,
    epd_record_cancel_fn cancel, void *user, epd_record_t **out);
epd_speech_error_t epd_record_pump(epd_record_t *record);
epd_speech_error_t epd_record_pause(epd_record_t *record);
epd_speech_error_t epd_record_resume(epd_record_t *record);
void epd_record_status(epd_record_t *record, epd_record_status_t *out);
epd_speech_error_t epd_record_close(epd_record_t *record);
/* Repair a platform-recorded WAV from its actual persisted length. */
epd_speech_error_t epd_record_recover(const char *path, uint32_t card_session,
                                     uint32_t *milliseconds);
/* Worker-only, synchronous, at most 120 seconds of mono S16LE/16 kHz WAV.
 * Existing streaming ASR providers receive file PCM at source rate. The caller
 * persists completed segments and explicitly schedules further segments.
 * No retry and no microphone capture. Output is a NUL-terminated final text. */
epd_speech_error_t epd_record_transcribe(const char *path, uint32_t card_session,
    uint32_t begin_ms, uint32_t duration_ms, epd_record_cancel_fn cancel, void *user,
    char *text, size_t capacity);
#endif
