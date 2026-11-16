/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_READER_FONT_H
#define EPD_READER_FONT_H
#include <stdint.h>
#include <stdbool.h>
/* Memory-only, durable font content key, including the built-in fallback.
 * Unlike the runtime revision this key is stable across restart. */
bool epd_reader_font_fingerprint(uint32_t id, uint32_t *fingerprint);
/* UI, memory-only: 1 accepted, 0 service busy, -1 service unavailable. */
int epd_reader_font_begin(void);
#endif
