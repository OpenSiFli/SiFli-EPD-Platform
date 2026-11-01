#ifndef READER_NOTE_UI_H
#define READER_NOTE_UI_H
#include "reader.h"

/* UI thread. The request retains the document until completion or cancellation. */
void reader_note_ui_open(lv_obj_t *screen, document_t *document,
                         uint32_t link_record, const lv_area_t *anchor);
#endif
