#ifndef EPD_READER_RENDER_IMAGE_H
#define EPD_READER_RENDER_IMAGE_H
#include <stdbool.h>

/* Call on the LVGL thread after lv_init(), before using L8 variable images.
 * Registers an opaque gray palette for native EPIC without copying pixels.
 * The source image and its pixels must remain alive until drawing completes. */
bool epd_reader_render_image_init(void);
#endif
