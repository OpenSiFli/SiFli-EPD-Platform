/* SPDX-License-Identifier: Apache-2.0 */
#include "ui_reader.h"
#include "ui_bookshelf_data.h"
#include "document/document_view.h"
#include "../common/reader/reader.h"
#include <limits.h>

static unsigned opened_book = UINT_MAX;

bool ui_reader_open(unsigned book)
{
    ui_reader_close();
    opened_book = book;
    return document_view_open(book);
}
const char *ui_reader_identity(void)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book(opened_book);
    return book ? book->file.path : "";
}
void ui_reader_close(void) { document_view_close(); opened_book = UINT_MAX; }
bool ui_reader_save(void) { return document_view_save(); }
bool ui_reader_reflow(void) { return document_view_reflow(); }
bool ui_reader_settings_changed(void) { return document_view_settings_changed(); }
ui_reader_work_t ui_reader_step(uint32_t budget, bool foreground, bool commit)
{ return document_view_step(budget, foreground, commit); }
void ui_reader_background_pause(bool paused) { epd_reader_background_pause(paused); }
void ui_reader_cancel_pending(void) { document_view_cancel_pending(); }
void ui_reader_seek(unsigned page) { document_view_seek(page); }
void ui_reader_turn(int direction) { epd_reader_turn(direction); }
const ui_reader_view_t *ui_reader_view(void) { return document_view_get(); }
bool ui_reader_waiting(void) { return document_view_waiting(); }
