#ifndef EPD_RASTER_BUDGET_H
#define EPD_RASTER_BUDGET_H

/* Optional whole-file JPEG acceleration must leave room for streaming/UI work. */
#define RASTER_JPEG_BUFFER_BUDGET (1024u * 1024u)
#define RASTER_PSRAM_RESERVE (1024u * 1024u)
/* Source-width workspace: libpng's two rows, the output row and the x map. */
#define RASTER_PNG_ROW_BUDGET (512u * 1024u)

#endif
