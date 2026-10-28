#ifndef UI_READING_FONT_SOURCE_H
#define UI_READING_FONT_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ui_reading_font_source ui_reading_font_source_t;

/* UI thread owns sources. Destroy the TinyTTF font before releasing its source. */
ui_reading_font_source_t *ui_reading_font_source_acquire(uint32_t id);
void ui_reading_font_source_release(ui_reading_font_source_t *source);
const char *ui_reading_font_source_path(const ui_reading_font_source_t *source);
uint32_t ui_reading_font_source_signature(const ui_reading_font_source_t *source);
bool ui_reading_font_source_valid(ui_reading_font_source_t *source);
void ui_reading_font_source_fault(ui_reading_font_source_t *source);
/* UI thread, after drawing finishes. Invalidates streams without freeing the
 * cursor still owned by TinyTTF. A later close cannot close the file twice. */
void ui_reading_font_source_revoke(uint32_t id);
void ui_reading_font_source_revoke_storage(void);

#endif
