#ifndef MUSIC_RESAMPLE_H
#define MUSIC_RESAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t filter[8];
    int16_t pending;
    bool has_pending;
} music_resample_t;

/* Zero-initialize on open/seek. Convert in place, preserving an odd last input
 * until the next call. Returns output samples, at most ceil(count / 2). */
size_t music_resample_by2(music_resample_t *state, int16_t *pcm, size_t count);
/* Emit an odd final input by repeating it as the second sample of the pair.
 * Padding is not included in the decoder's source-sample progress. */
size_t music_resample_finish(music_resample_t *state, int16_t *pcm);

#endif
