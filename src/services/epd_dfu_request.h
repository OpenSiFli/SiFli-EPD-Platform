/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_DFU_REQUEST_H
#define EPD_DFU_REQUEST_H

#include <rtthread.h>

/* A NULL path enters recovery mode without a preselected package. */
rt_err_t epd_dfu_request(const char *path);
rt_err_t epd_dfu_font_request(const char *path, uint32_t signature, const char *name);

#endif
