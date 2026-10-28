#ifndef EPD_READING_FONT_VALIDATE_H
#define EPD_READING_FONT_VALIDATE_H
#include "reading_font.h"

typedef bool (*reading_font_cancel_t)(void *context);
/* Bounded-memory, seekable static TrueType validator. Runs on a worker.
 * Checks table/glyph references and the supported outline format before the
 * file can enter the catalog. Writes a UTF-8 family/style display name. */
rt_err_t reading_font_validate(const char *path, reading_font_info_t *info,
                               reading_font_cancel_t cancelled, void *context);
#endif
