#ifndef MUSIC_FLAC_WORKER_H
#define MUSIC_FLAC_WORKER_H

#include "music_backend_sdk.h"

/* FLAC uses libFLAC and the PCM output API; mp3ctrl owns MP3/WAV playback. */
typedef struct music_flac_worker music_flac_worker_t;
music_flac_worker_t *music_flac_create(music_backend_wake_fn wake, void *userdata);
void music_flac_destroy(music_flac_worker_t *worker);
int music_flac_open(music_flac_worker_t *worker, const char *path);
int music_flac_play(music_flac_worker_t *worker);
int music_flac_pause(music_flac_worker_t *worker);
int music_flac_seek(music_flac_worker_t *worker, uint64_t position_ms);
int music_flac_close(music_flac_worker_t *worker);
int music_flac_pump(music_flac_worker_t *worker);
void music_flac_snapshot(music_flac_worker_t *worker, music_backend_snapshot_t *snapshot);
void music_flac_request_cancel(music_flac_worker_t *worker);

#endif
