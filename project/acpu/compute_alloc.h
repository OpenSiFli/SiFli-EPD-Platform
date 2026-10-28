/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_ACPU_COMPUTE_ALLOC_H
#define EPD_ACPU_COMPUTE_ALLOC_H

#include <stdlib.h>
#include <rtthread.h>
#include "epd_compute_memory.h"

/* Applied only to the selected SDK algorithm library groups. Device buffers
 * and RT-Thread control objects keep their existing SRAM allocation. */
#define malloc epd_compute_malloc
#define calloc epd_compute_calloc
#define realloc epd_compute_realloc
#define free epd_compute_free
#define rt_malloc epd_compute_malloc
#define rt_calloc epd_compute_calloc
#define rt_realloc epd_compute_realloc
#define rt_free epd_compute_free

#endif
