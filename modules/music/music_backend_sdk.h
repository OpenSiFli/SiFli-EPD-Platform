#ifndef MUSIC_BACKEND_SDK_H
#define MUSIC_BACKEND_SDK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct music_backend music_backend_t;
typedef void (*music_backend_wake_fn)(void *userdata);

typedef enum {
    MUSIC_BACKEND_IDLE,
    MUSIC_BACKEND_PREPARED,
    MUSIC_BACKEND_PLAYING,
    MUSIC_BACKEND_PAUSED,
    MUSIC_BACKEND_DRAINING,
    MUSIC_BACKEND_SUSPENDED,
    MUSIC_BACKEND_ENDED,
    MUSIC_BACKEND_ERROR
} music_backend_state_t;

typedef enum {
    MUSIC_BACKEND_OK,
    MUSIC_BACKEND_INVALID,
    MUSIC_BACKEND_NO_MEMORY,
    MUSIC_BACKEND_IO,
    MUSIC_BACKEND_UNSUPPORTED,
    MUSIC_BACKEND_DECODE,
    MUSIC_BACKEND_OUTPUT,
    MUSIC_BACKEND_SEEK,
    MUSIC_BACKEND_CANCELLED
} music_backend_error_t;

typedef enum {
    MUSIC_BACKEND_FORMAT_UNKNOWN,
    MUSIC_BACKEND_FORMAT_MP3,
    MUSIC_BACKEND_FORMAT_WAV,
    MUSIC_BACKEND_FORMAT_FLAC
} music_backend_format_t;

typedef struct {
    music_backend_state_t state;
    music_backend_error_t error;
    int error_detail;
    music_backend_format_t format;
    uint64_t session_id;
    /* MP3/WAV use the SDK progress callback; FLAC subtracts its PCM cache. */
    uint64_t position_ms;
    uint64_t duration_ms;
    /* FLAC counters and rate use source samples, including when output is
     * downsampled. The SDK controller does not expose these counters. */
    uint64_t decoded_samples;
    uint64_t submitted_samples;
    uint64_t pending_samples;
    uint32_t sample_rate;
    uint8_t channels; /* Source channels. */
    uint8_t bits_per_sample;
    bool duration_known;
    bool duration_estimated;
    bool want_play;
    bool can_seek;
} music_backend_snapshot_t;

/* The music service owns control calls. MP3/WAV call the SDK controller
 * directly; FLAC alone has a decoding worker. Callbacks only publish state
 * and signal the service. Keep the backend alive until close has returned. */
music_backend_t *music_backend_create(music_backend_wake_fn wake, void *userdata);
void music_backend_destroy(music_backend_t *backend);

/* Return 0 or a negative music_backend_error_t. Open owns a copy of path.
 * MP3/WAV seek is available after the first progress callback while playing;
 * FLAC also supports prepared and paused seek. Check snapshot.can_seek. */
int music_backend_open(music_backend_t *backend, const char *path);
int music_backend_play(music_backend_t *backend);
int music_backend_pause(music_backend_t *backend);
int music_backend_seek(music_backend_t *backend, uint64_t position_ms);
int music_backend_close(music_backend_t *backend);

/* Status poll only. SDK/FLAC threads advance playback independently. */
int music_backend_pump(music_backend_t *backend);
void music_backend_snapshot(music_backend_t *backend, music_backend_snapshot_t *snapshot);

/* A producer may request cancellation without waiting. The service then closes
 * the controller. SDK calls already in progress finish under the SDK contract;
 * the project does not interrupt its decoder thread. Stop producers before
 * destroying the backend. */
void music_backend_request_cancel(music_backend_t *backend);

#ifdef __cplusplus
}
#endif
#endif
