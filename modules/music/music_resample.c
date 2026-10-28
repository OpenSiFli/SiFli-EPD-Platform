/*
 * Copyright (c) 2011, The WebRTC project authors. All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Google nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "music_resample.h"
#include <limits.h>

/* Generic WebRtcSpl_DownsampleBy2 from the SDK's
 * external/WebRTC_audio_processing/webrtc/common_audio/signal_processing/
 * resample_by_2.c. Coefficients, state recurrence and rounding are unchanged.
 * A widened multiply replaces SCALEDIFF32; multiplication by 1024 replaces
 * signed left shift. Only the streaming/odd-input wrapper is application code. */
static int32_t scale_diff(uint16_t coefficient, int32_t difference, int32_t state)
{
    return state + (int32_t)(((int64_t)difference * coefficient) >> 16);
}

static int16_t downsample_pair(music_resample_t *state, int16_t first, int16_t second)
{
    static const uint16_t allpass1[3] = {3284, 24441, 49528};
    static const uint16_t allpass2[3] = {12199, 37471, 60255};
    int32_t *s = state->filter;
    int32_t in = (int32_t)first * 1024;
    int32_t tmp1 = scale_diff(allpass2[0], in - s[1], s[0]);
    s[0] = in;
    int32_t tmp2 = scale_diff(allpass2[1], tmp1 - s[2], s[1]);
    s[1] = tmp1;
    s[3] = scale_diff(allpass2[2], tmp2 - s[3], s[2]);
    s[2] = tmp2;

    in = (int32_t)second * 1024;
    tmp1 = scale_diff(allpass1[0], in - s[5], s[4]);
    s[4] = in;
    tmp2 = scale_diff(allpass1[1], tmp1 - s[6], s[5]);
    s[5] = tmp1;
    s[7] = scale_diff(allpass1[2], tmp2 - s[7], s[6]);
    s[6] = tmp2;

    int32_t out = (s[3] + s[7] + 1024) >> 11;
    if (out > INT16_MAX) out = INT16_MAX;
    if (out < INT16_MIN) out = INT16_MIN;
    return (int16_t)out;
}

size_t music_resample_by2(music_resample_t *state, int16_t *pcm, size_t count)
{
    size_t input = 0, output = 0;
    if (state->has_pending && count) {
        int16_t first = state->pending, second = pcm[input++];
        pcm[output++] = downsample_pair(state, first, second);
        state->has_pending = false;
    }
    while (count - input >= 2) {
        int16_t first = pcm[input], second = pcm[input + 1];
        input += 2;
        pcm[output++] = downsample_pair(state, first, second);
    }
    if (input < count) {
        state->pending = pcm[input];
        state->has_pending = true;
    }
    return output;
}

size_t music_resample_finish(music_resample_t *state, int16_t *pcm)
{
    if (!state->has_pending) return 0;
    pcm[0] = downsample_pair(state, state->pending, state->pending);
    state->has_pending = false;
    return 1;
}
