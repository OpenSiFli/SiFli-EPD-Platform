/* SPDX-License-Identifier: Apache-2.0 */
#include "sound_tone_decode.h"
#include "../platform/epd_memory.h"
#include "../thirdparty/miniz/miniz.h"
#include <string.h>

#define TONE_BITMAP_BYTES ((SOUND_TONE_PCM_LIMIT / 2u + 7u) / 8u)
#define TONE_WORK_BYTES (sizeof(tinfl_decompressor) > TONE_BITMAP_BYTES ? \
                        sizeof(tinfl_decompressor) : TONE_BITMAP_BYTES)
typedef char tone_memory_bound[(TONE_WORK_BYTES + SOUND_TONE_PCM_LIMIT <=
                               SOUND_TONE_MEMORY_LIMIT) ? 1 : -1];

bool sound_tone_asset_valid(const epd_tone_asset_t *asset)
{
    return asset && asset->packed && asset->packed_bytes &&
        asset->packed_bytes <= SOUND_TONE_MEMORY_LIMIT && asset->bytes &&
        asset->bytes <= SOUND_TONE_PCM_LIMIT && !(asset->bytes & 1u) &&
        asset->sample_rate == 32000 && asset->encoding == EPD_TONE_DIFF_PLANES_ZLIB_V1 &&
        asset->difference_order >= 1 && asset->difference_order <= 4;
}

static int restore_pcm(const epd_tone_asset_t *asset, unsigned char *pcm,
                       unsigned char *visited, bool (*cancelled)(void *), void *user)
{
    uint32_t samples = asset->bytes / 2u, operations = 0;
    uint32_t history[4] = {0}, crc = MZ_CRC32_INIT;
    memset(visited, 0, (samples + 7u) / 8u);
    /* In-place planar-to-interleaved permutation. Every nontrivial cycle
     * reaches the low plane: indices in the high plane strictly decrease
     * until then, except the final byte which is already in place.
     * Only low-plane indices need visited bits; no second PCM buffer. */
    for (uint32_t start = 0; start < samples; ++start)
    {
        if (visited[start / 8u] & (1u << (start % 8u))) continue;
        uint32_t position = start;
        unsigned char carry = pcm[position];
        do
        {
            if (!(operations++ % 4096u) && cancelled && cancelled(user)) return SOUND_TONE_CANCELLED;
            if (position < samples) visited[position / 8u] |= (unsigned char)(1u << (position % 8u));
            uint32_t next = position < samples ? 2u * position : 2u * (position - samples) + 1u;
            unsigned char saved = pcm[next];
            pcm[next] = carry; carry = saved; position = next;
        } while (position != start);
    }
    for (uint32_t first = 0; first < samples; first += 2048u)
    {
        if (cancelled && cancelled(user)) return SOUND_TONE_CANCELLED;
        uint32_t end = samples - first > 2048u ? first + 2048u : samples;
        for (uint32_t i = first; i < end; ++i)
        {
            uint32_t code = pcm[2u * i] | ((uint32_t)pcm[2u * i + 1u] << 8);
            uint32_t value = ((code >> 1) ^ (0u - (code & 1u))) & 65535u;
            for (uint32_t level = 0; level < asset->difference_order; ++level)
            {
                history[level] = (history[level] + value) & 65535u;
                value = history[level];
            }
            pcm[2u * i] = (unsigned char)value;
            pcm[2u * i + 1u] = (unsigned char)(value >> 8);
        }
        crc = (uint32_t)mz_crc32(crc, pcm + 2u * first, 2u * (end - first));
    }
    return crc == asset->pcm_crc32 ? SOUND_TONE_OK : SOUND_TONE_INVALID;
}

void sound_tone_decode_release(sound_tone_pcm_t *pcm)
{
    if (!pcm) return;
    epd_app_free(pcm->storage);
    memset(pcm, 0, sizeof(*pcm));
}

int sound_tone_decode(const epd_tone_asset_t *asset, bool (*cancelled)(void *),
                      void *user, sound_tone_pcm_t *out)
{
    tinfl_decompressor *state;
    unsigned char *pcm;
    size_t input = 0, output = 0;
    int result = SOUND_TONE_INVALID;
    if (!out) return SOUND_TONE_INVALID;
    memset(out, 0, sizeof(*out));
    if (!sound_tone_asset_valid(asset)) return SOUND_TONE_INVALID;
    if (cancelled && cancelled(user)) return SOUND_TONE_CANCELLED;
    size_t work_bytes = (asset->bytes / 2u + 7u) / 8u;
    if (work_bytes < sizeof(*state)) work_bytes = sizeof(*state);
    state = epd_app_alloc(work_bytes + asset->bytes, EPD_APP_PSRAM);
    if (!state) return SOUND_TONE_NOMEM;
    pcm = (unsigned char *)state + work_bytes;
    tinfl_init(state);
    for (;;)
    {
        if (cancelled && cancelled(user)) { result = SOUND_TONE_CANCELLED; break; }
        size_t in = asset->packed_bytes - input, count = asset->bytes - output;
        if (count > 4096) count = 4096;
        tinfl_status status = tinfl_decompress(state, asset->packed + input, &in,
            pcm, pcm + output, &count,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        input += in; output += count;
        if (status == TINFL_STATUS_DONE)
        {
            if (input != asset->packed_bytes || output != asset->bytes) break;
            if (cancelled && cancelled(user)) { result = SOUND_TONE_CANCELLED; break; }
            result = restore_pcm(asset, pcm, (unsigned char *)state, cancelled, user);
            if (result != SOUND_TONE_OK) break;
            if (cancelled && cancelled(user)) { result = SOUND_TONE_CANCELLED; break; }
            out->storage = state; out->pcm = pcm;
            return SOUND_TONE_OK;
        }
        if (status != TINFL_STATUS_HAS_MORE_OUTPUT || (!in && !count)) break;
    }
    epd_app_free(state);
    return result;
}
