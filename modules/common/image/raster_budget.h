#ifndef EPD_RASTER_BUDGET_H
#define EPD_RASTER_BUDGET_H

#include "epd_resource_runtime.h"

/* Bound optional whole-file JPEG acceleration; streaming remains available. */
#define RASTER_JPEG_BUFFER_BUDGET (1024u * 1024u)
/* Source-width workspace: libpng's two rows, the output row and the x map. */
#define RASTER_PNG_ROW_BUDGET (512u * 1024u)

/* Each allocation has its own heap header, rounding and unsplittable tail. */
static inline bool raster_reserve(epd_memory_region_t region, const size_t *sizes, size_t count)
{
    size_t peak = 0;
    for (size_t i = 0; i < count; ++i)
    {
        size_t part = epd_app_allocation_peak(sizes[i]);
        if (part == SIZE_MAX || part > SIZE_MAX - peak) return false;
        peak += part;
    }
    return epd_resource_context_reserve(region, peak) == EPD_RESOURCE_OK;
}
#endif
