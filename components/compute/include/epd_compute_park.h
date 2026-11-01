/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_PARK_H
#define EPD_COMPUTE_PARK_H
#include <stdint.h>

/* Shared uncached SRAM; each core writes only its own sequence. */
typedef struct
{
    volatile uint32_t requested;
    volatile uint32_t parked;
} epd_compute_park_t;
#endif
