/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_FONT_H
#define BOOK_DOCUMENT_FONT_H
#include "lvgl.h"
/* The wrapper borrows its source. Destroy it after all draw tasks finish and
 * before destroying the source font. */
lv_font_t *document_font_oblique(const lv_font_t *source);
void document_font_free(lv_font_t *font);
#endif
