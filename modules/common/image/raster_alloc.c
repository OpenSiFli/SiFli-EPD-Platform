#include "epd_memory.h"

/* Module-local defaults for codec paths without a per-session allocator. */
void *raster_codec_malloc(size_t size)
{
    return epd_app_alloc(size, EPD_APP_PSRAM);
}

void raster_codec_free(void *memory)
{
    epd_app_free(memory);
}
