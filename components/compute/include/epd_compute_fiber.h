/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_FIBER_H
#define EPD_COMPUTE_FIBER_H

#include <stdbool.h>
#include <stddef.h>
#include "epd_compute_protocol.h"

typedef struct epd_compute_fiber epd_compute_fiber_t;
typedef int32_t (*epd_compute_fiber_entry_t)(epd_compute_fiber_t *, void *);

/* The stack and algorithm state stay private to ACPU between component steps.
 * Only step/close run on the dispatcher; entry/yield run on the private stack. */
epd_compute_fiber_t *epd_compute_fiber_create(const char *name, size_t stack_size,
    epd_compute_fiber_entry_t entry, void *context);
int32_t epd_compute_fiber_step(epd_compute_fiber_t *fiber, const epd_compute_wire_t *request);
bool epd_compute_fiber_yield(epd_compute_fiber_t *fiber);
bool epd_compute_fiber_cancelled(const epd_compute_fiber_t *fiber);
bool epd_compute_fiber_should_yield(const epd_compute_fiber_t *fiber);
const epd_compute_wire_t *epd_compute_fiber_request(const epd_compute_fiber_t *fiber);
void epd_compute_fiber_close(epd_compute_fiber_t *fiber);

#endif
