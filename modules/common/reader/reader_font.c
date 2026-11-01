/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/epd_app_font.h"
#include "reader_font.h"

lv_font_t *document_font_oblique(const lv_font_t *source)
{
    return epd_app_font_oblique(source);
}

void document_font_free(lv_font_t *font)
{
    epd_app_font_destroy(font);
}
