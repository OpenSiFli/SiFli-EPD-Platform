/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_MEMORY_H
#define EPD_COMPUTE_MEMORY_H

#include <stddef.h>

/* ACPU-owned algorithm workspace. Never free these allocations on HCPU. */
void *epd_compute_malloc(size_t size);
void *epd_compute_calloc(size_t count, size_t size);
void *epd_compute_realloc(void *memory, size_t size);
void epd_compute_free(void *memory);

#endif
