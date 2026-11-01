#ifndef EPD_READER_FONT_METADATA_H
#define EPD_READER_FONT_METADATA_H
#include "compute_reader_font.h"

/* Internal read hint from the already validated SFNT directory. This is safe
 * during the engine's provider callback and does not perform I/O. */
int crf_read_is_metadata(const crf_context *context, uint32_t offset, size_t length);
#endif
