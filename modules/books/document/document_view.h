/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_VIEW_H
#define BOOK_DOCUMENT_VIEW_H
#include "../ui_reader.h"
#include "document.h"

bool document_view_open(unsigned book);
void document_view_close(void);
bool document_view_active(void);
const ui_reader_view_t *document_view_get(void);
/* Same scheduling result as TXT; waiting for data/images is not target work.
 * commit=false preserves the visible revision/position. */
ui_reader_work_t document_view_step(uint32_t budget_ms, bool foreground, bool commit);
bool document_view_save(void);
bool document_view_reflow(void);
bool document_view_settings_changed(void);
void document_view_seek(unsigned page);
void document_view_cancel_pending(void);
bool document_view_waiting(void);
/* Bind prepared content only when the screen is idle; reuse existing objects. */
bool document_view_bind(lv_obj_t *body);
void document_view_detach(void);
unsigned document_view_toc_count(void);
bool document_view_toc_title(unsigned index, char *title, size_t size, unsigned *level);
bool document_view_toc_seek(unsigned index);
bool document_view_follow_link(uint32_t record);
#endif
