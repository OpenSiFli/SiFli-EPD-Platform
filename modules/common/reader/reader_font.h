/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_FONT_H
#define BOOK_DOCUMENT_FONT_H
#include "lvgl.h"
/* Owned asynchronous ACPU face; retains the source generation. Use the same
 * status/prepare/ready/page-pin APIs as a normal reading font. */
lv_font_t *document_font_oblique(const lv_font_t *source);
void document_font_free(lv_font_t *font);
#endif
