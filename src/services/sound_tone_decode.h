/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SOUND_TONE_DECODE_H
#define SOUND_TONE_DECODE_H
#include <stdbool.h>
#include "../../assets/sounds/epd_tone_assets.h"

#define SOUND_TONE_PCM_LIMIT (180u * 1024u)
#define SOUND_TONE_MEMORY_LIMIT (192u * 1024u)
enum { SOUND_TONE_OK, SOUND_TONE_INVALID = -1, SOUND_TONE_NOMEM = -2,
       SOUND_TONE_CANCELLED = -3 };
typedef struct { unsigned char *pcm; void *storage; } sound_tone_pcm_t;
bool sound_tone_asset_valid(const epd_tone_asset_t *asset);
/* One PSRAM allocation holds the current tone and workspace, at most 192 KiB
 * excluding heap bookkeeping. The inflater workspace is reused as a visited
 * bitmap for in-place plane restoration. Cancellation is checked per 4096
 * inflated/restored bytes or permutation moves. Success publishes only a
 * complete, exact-length stream with verified transformed Adler and PCM CRC.
 * Failure leaves out empty. The serialized tone worker owns the result. */
int sound_tone_decode(const epd_tone_asset_t *asset, bool (*cancelled)(void *),
                      void *user, sound_tone_pcm_t *out);
void sound_tone_decode_release(sound_tone_pcm_t *pcm);
#endif
