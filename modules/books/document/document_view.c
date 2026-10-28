/* SPDX-License-Identifier: Apache-2.0 */
#include "document_view.h"
#include "../../common/reader/reader_internal.h"
#include "../ui_bookshelf_data.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

static document_t *reopen_book(void *user, char *error, size_t size)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book((unsigned)(uintptr_t)user - 1);
    return book ? document_open(&book->file, error, size) : NULL;
}
static bool save_book(void *user, const reader_position_t *position)
{
    unsigned index = (unsigned)(uintptr_t)user - 1;
    const ui_bookshelf_book_t *book = ui_bookshelf_book(index);
    if (!book) return false;
    ui_reading_position_t p = {0};
    p.current_page = position->page; p.total_pages = position->pages;
    p.progress = position->percent; p.layout = position->layout;
    p.document_version = position->conversion_version; p.document_kind = book->file.format;
    p.offset = position->cursor.record; p.document_byte = position->cursor.byte;
    ui_reading_position_t old = book->position;
    old.last_read = 0;
    if (epd_reader_get()->saved && !memcmp(&old, &p, sizeof(p))) return true;
    time_t now = time(NULL);
    if (now >= 1704067200) p.last_read = (uint32_t)now;
    bool saved = ui_bookshelf_save(index, &p);
    epd_app_set_recent_reading(epd_reader_get()->title, p.progress);
    return saved;
}
bool document_view_open(unsigned index)
{
    epd_reader_close();
    const ui_bookshelf_book_t *book = ui_bookshelf_book(index);
    if (!book) return false;
    char error[96] = {0};
    document_t *document = document_open(&book->file, error, sizeof(error));
    if (!document) { epd_reader_set_error_code(document_open_error()); return false; }
    reader_position_t restore = {0};
    if (book->position.document_kind == book->file.format)
    {
        restore.conversion_version = book->position.document_version;
        restore.cursor = (document_cursor_t){book->position.offset, book->position.document_byte};
    }
    return epd_reader_attach(document, book->file.name, &restore, save_book, reopen_book,
                         (void *)(uintptr_t)(index + 1));
}
void document_view_close(void) { epd_reader_close(); }
bool document_view_active(void) { return epd_reader_active(); }
const ui_reader_view_t *document_view_get(void) { return epd_reader_get(); }
ui_reader_work_t document_view_step(uint32_t budget, bool foreground, bool commit)
{ return epd_reader_step(budget, foreground, commit); }
bool document_view_save(void) { epd_reader_save(); return ui_bookshelf_flush(); }
bool document_view_reflow(void) { return epd_reader_reflow(); }
bool document_view_settings_changed(void) { return epd_reader_settings_changed(); }
void document_view_seek(unsigned page) { epd_reader_seek(page); }
void document_view_cancel_pending(void) { epd_reader_cancel_pending(); }
bool document_view_waiting(void) { return epd_reader_waiting(); }
bool document_view_bind(lv_obj_t *body) { return epd_reader_bind(body); }
void document_view_detach(void) { epd_reader_detach(); }
unsigned document_view_toc_count(void) { return epd_reader_toc_count(); }
bool document_view_toc_title(unsigned index, char *title, size_t size, unsigned *level)
{ return epd_reader_toc_title(index, title, size, level); }
bool document_view_toc_seek(unsigned index) { return epd_reader_toc_seek(index); }
bool document_view_follow_link(uint32_t record) { return epd_reader_follow_link(record); }
