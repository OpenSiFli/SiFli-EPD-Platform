/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_MP3_H
#define EPD_MP3_H

#include "mp3dec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct epd_mp3_client *epd_mp3_t;

/* Calls are synchronous and serialized by the owning worker. Open chooses
 * the compute session or local Helix decoder for the handle's lifetime.
 * Open/reset/close return 0 on success or a negative Helix error code.
 * Open sets *client to NULL on failure. Reset preserves the handle, including
 * on failure; a failed reset can be retried or the handle can be closed. */
int epd_mp3_open(epd_mp3_t *client);
int epd_mp3_reset(epd_mp3_t client);
/* A failed close preserves the handle and its resources for retry.
 * Successful close invalidates the handle. Closing NULL succeeds. */
int epd_mp3_close(epd_mp3_t client);

/* PCM holds MAX_NGRAN * MAX_NCHAN * MAX_NSAMP shorts. Input advancement,
 * remaining bytes, use_size and is_seeking follow MP3Decode semantics. */
int epd_mp3_decode(epd_mp3_t client, unsigned char **input, int *bytes_left,
                   short *pcm, int use_size, int is_seeking);
void epd_mp3_last_info(epd_mp3_t client, MP3FrameInfo *info);
/* Input contains at least four header bytes, or six for a CRC header. */
int epd_mp3_next_info(epd_mp3_t client, MP3FrameInfo *info, unsigned char *input);

#ifdef __cplusplus
}
#endif
#endif
