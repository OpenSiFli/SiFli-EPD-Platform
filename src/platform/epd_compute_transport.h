/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_TRANSPORT_H
#define EPD_COMPUTE_TRANSPORT_H
#include <rtthread.h>
#include <stdint.h>

int epd_compute_transport_init(rt_thread_t owner);
void epd_compute_transport_detach(void);
int epd_compute_transport_run(void *parameter, uint32_t size, uint8_t *error);
#endif
