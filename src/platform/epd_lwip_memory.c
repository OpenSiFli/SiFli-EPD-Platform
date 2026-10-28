#include "rtconfig.h"
#if defined(RT_USING_LWIP) && defined(EPD_USE_LVGL)
#include "epd_memory.h"
#include <lwip/mem.h>
#include <stdint.h>
#include <string.h>

/* The SDK port exposes these weak hooks for protocol heap allocation.
 * Packet payloads use the project PSRAM heap; RT objects keep their allocator. */
void *mem_malloc(mem_size_t size)
{
    return epd_app_alloc(size,EPD_APP_PSRAM);
}

void *mem_calloc(mem_size_t count,mem_size_t size)
{
    if (size && (size_t)count>SIZE_MAX/(size_t)size) return NULL;
    size_t bytes=(size_t)count*(size_t)size;
    void *result=epd_app_alloc(bytes,EPD_APP_PSRAM);
    if (result) memset(result,0,bytes);
    return result;
}

void mem_free(void *memory)
{
    epd_app_free(memory);
}
#endif
