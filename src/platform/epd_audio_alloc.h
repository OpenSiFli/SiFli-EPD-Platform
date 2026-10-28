#ifndef EPD_AUDIO_ALLOC_H
#define EPD_AUDIO_ALLOC_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <audio_mem.h>
#include "epd_memory.h"

/* CPU workspaces use PSRAM; device buffers retain their original allocator.
 * Decoder libraries and the resampler use these mappings directly. Controller
 * and Audio Manager objects use the narrower epd_audio_control_alloc.h mapping. */
static inline void *epd_audio_malloc(size_t size)
{
    return epd_app_alloc(size, EPD_APP_PSRAM);
}

static inline void *epd_audio_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    void *memory = epd_audio_malloc(bytes);
    if (memory) memset(memory, 0, bytes);
    return memory;
}

static inline void *epd_audio_realloc(void *memory, size_t size)
{
    return epd_app_realloc(memory, size, EPD_APP_PSRAM);
}

#define malloc epd_audio_malloc
#define calloc epd_audio_calloc
#define realloc epd_audio_realloc
#define free epd_app_free
#define audio_mem_malloc epd_audio_malloc
#define audio_mem_calloc epd_audio_calloc
#define audio_mem_realloc epd_audio_realloc
#define audio_mem_free epd_app_free
#endif
