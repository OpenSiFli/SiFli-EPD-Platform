#ifndef EPD_DFU_SYSTEM_FONT_H
#define EPD_DFU_SYSTEM_FONT_H
#include "dfu_records.h"
#include "dfu_package.h"
enum { DFU_FONT_OK, DFU_FONT_TOO_LARGE, DFU_FONT_SOURCE, DFU_FONT_IO, DFU_FONT_RESOURCE };
unsigned dfu_system_font_prepare(const dfu_request_t *request, bool recovering,
    dfu_package_t *package, dfu_package_progress_t progress);
void dfu_system_font_cleanup(void);
#endif
