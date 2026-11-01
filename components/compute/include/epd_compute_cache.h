/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_CACHE_H
#define EPD_COMPUTE_CACHE_H
#include "epd_compute_protocol.h"

void epd_compute_barrier(void);
void epd_compute_cache_clean(uint32_t address, uint32_t capacity);
void epd_compute_cache_invalidate(uint32_t address, uint32_t capacity);

/* Same whole-line ownership contract as clean/invalidate. */
void epd_compute_cache_clean_invalidate(uint32_t address, uint32_t capacity);

#endif
