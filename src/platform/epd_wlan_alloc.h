/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_WLAN_ALLOC_H
#define EPD_WLAN_ALLOC_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "epd_memory.h"

/* Applied only to the official Wi-Fi port. Keep RT objects and thread stacks
 * on the native allocator; payloads and driver workspaces share RAM1. The SDK
 * SDIO driver retains its DMA alignment and cache-maintenance path. */
static inline void *epd_wlan_malloc(size_t bytes)
{
    return epd_app_alloc(bytes, EPD_APP_PSRAM);
}

static inline void *epd_wlan_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    void *memory = epd_wlan_malloc(bytes);
    if (memory) memset(memory, 0, bytes);
    return memory;
}

static inline void *epd_wlan_realloc(void *memory, size_t bytes)
{
    return epd_app_realloc(memory, bytes, EPD_APP_PSRAM);
}

#define malloc epd_wlan_malloc
#define calloc epd_wlan_calloc
#define realloc epd_wlan_realloc
#define free epd_app_free
#endif
