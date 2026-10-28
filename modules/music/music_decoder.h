#ifndef MUSIC_DECODER_H
#define MUSIC_DECODER_H

#include "music_backend_sdk.h"

/* The FLAC worker owns this decoder. Only cancellation may cross threads;
 * callers must keep the decoder alive until the worker has exited. */
typedef struct music_decoder music_decoder_t;
music_decoder_t *music_decoder_create(music_backend_wake_fn wake, void *userdata);
void music_decoder_destroy(music_decoder_t *decoder);
int music_decoder_open(music_decoder_t *decoder, const char *path);
int music_decoder_play(music_decoder_t *decoder);
int music_decoder_pause(music_decoder_t *decoder);
int music_decoder_seek(music_decoder_t *decoder, uint64_t position_ms);
int music_decoder_close(music_decoder_t *decoder);
int music_decoder_pump(music_decoder_t *decoder);
void music_decoder_snapshot(music_decoder_t *decoder, music_backend_snapshot_t *snapshot);
void music_decoder_request_cancel(music_decoder_t *decoder);

#endif
