/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute_protocol.h"

int epd_compute_validate_regions(const epd_compute_region_t *regions, uint32_t count)
{
    if (count && !regions) return EPD_COMPUTE_INVALID;
    for (uint32_t i = 0; i < count; ++i)
    {
        const epd_compute_region_t *r = &regions[i];
        if (!r->address || !r->capacity || r->length > r->capacity ||
            (r->address & (EPD_COMPUTE_CACHE_LINE - 1)) ||
            (r->capacity & (EPD_COMPUTE_CACHE_LINE - 1)) ||
            r->capacity > UINT32_MAX - r->address ||
            !(r->access & (EPD_COMPUTE_READ | EPD_COMPUTE_WRITE)) ||
            (r->access & ~(EPD_COMPUTE_READ | EPD_COMPUTE_WRITE)))
            return EPD_COMPUTE_INVALID;
        for (uint32_t j = 0; j < i; ++j)
        {
            const epd_compute_region_t *p = &regions[j];
            if (r->address < p->address + p->capacity &&
                p->address < r->address + r->capacity)
                return EPD_COMPUTE_INVALID;
        }
    }
    return EPD_COMPUTE_OK;
}

void *epd_compute_resolve(const epd_compute_wire_t *request,
                          epd_compute_ref_t ref, uint32_t access)
{
    if (!request || ref.region >= request->region_count || !request->regions) return NULL;
    const epd_compute_region_t *regions = (const void *)(uintptr_t)request->regions;
    const epd_compute_region_t *r = &regions[ref.region];
    uint32_t bound = (access & EPD_COMPUTE_READ) ? r->length : r->capacity;
    if ((r->access & access) != access || ref.offset > bound || ref.length > bound - ref.offset)
        return NULL;
    return (void *)(uintptr_t)(r->address + ref.offset);
}

uint32_t epd_compute_signals(const epd_compute_wire_t *request)
{
    if (!request || !request->signals) return 0;
    return *(volatile const uint32_t *)(uintptr_t)request->signals;
}
