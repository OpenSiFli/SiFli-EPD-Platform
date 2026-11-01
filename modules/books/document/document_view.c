/* SPDX-License-Identifier: Apache-2.0 */
#include "document_view.h"
#include "../../common/reader/reader_internal.h"
#include "../ui_bookshelf_data.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

static unsigned active_book = UINT_MAX;
static bool save_rejected;

static bool position_compatible(const ui_bookshelf_book_t *book)
{
    if (book->position.document_kind != book->file.format) return false;
    if (book->position.document_version == DOCUMENT_CONVERSION_VERSION) return true;
    /* Only EPUB record ordering changed; MD/MOBI spool coordinates are unchanged. */
    return book->position.document_version == 9 &&
        (((book->file.format == DOCUMENT_EPUB || book->file.format == DOCUMENT_SPLIT_EPUB) &&
          book->position.source.unit == 2) ||
         (book->file.format == DOCUMENT_TXT && book->position.source.unit == 1) ||
         ((book->file.format == DOCUMENT_MD || book->file.format == DOCUMENT_MOBI) &&
          book->position.source.unit == DOCUMENT_SOURCE_SPOOL));
}

static document_t *reopen_book(void *user, char *error, size_t size)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book((unsigned)(uintptr_t)user - 1);
    if (!book) return NULL;
    const document_anchor_t *source = position_compatible(book) ? &book->position.source : NULL;
    return document_open_at(&book->file, source, error, size);
}
static bool save_book(void *user, const reader_position_t *position)
{
    unsigned index = (unsigned)(uintptr_t)user - 1;
    const ui_bookshelf_book_t *book = ui_bookshelf_book(index);
    if (!book) { save_rejected = true; return false; }
    ui_reading_position_t p = {0};
    p.current_page = position->page; p.total_pages = position->pages;
    p.progress = position->percent; p.layout = position->layout;
    p.document_version = position->conversion_version; p.document_kind = book->file.format;
    p.source = position->source;
    ui_reading_position_t old = book->position;
    old.last_read = 0;
    if (epd_reader_get()->saved && !memcmp(&old, &p, sizeof(p))) return true;
    time_t now = time(NULL);
    if (now >= 1704067200) p.last_read = (uint32_t)now;
    bool saved = ui_bookshelf_save(index, &p);
    save_rejected = !saved;
    epd_app_set_recent_reading(epd_reader_get()->title, p.progress);
    return saved;
}
bool document_view_open(unsigned index)
{
    epd_reader_close();
    active_book = UINT_MAX;
    save_rejected = false;
    const ui_bookshelf_book_t *book = ui_bookshelf_book(index);
    if (!book) return false;
    char error[96] = {0};
    const document_anchor_t *source = position_compatible(book) ? &book->position.source : NULL;
    document_t *document = document_open_at(&book->file, source, error, sizeof(error));
    if (!document) { epd_reader_set_error_code(document_open_error()); return false; }
    reader_position_t restore = {0};
    if (source)
    {
        restore.conversion_version = DOCUMENT_CONVERSION_VERSION;
        restore.source = *source;
    }
    bool opened = epd_reader_attach(document, book->file.name, &restore, save_book, reopen_book,
                                   (void *)(uintptr_t)(index + 1));
    if (opened) active_book = index;
    return opened;
}
void document_view_close(void)
{
    epd_reader_close();
    active_book = UINT_MAX;
    save_rejected = false;
}
bool document_view_save_failed(void)
{
    return active_book != UINT_MAX &&
           (save_rejected || ui_bookshelf_save_failed(active_book));
}
bool document_view_active(void) { return epd_reader_active(); }
const ui_reader_view_t *document_view_get(void) { return epd_reader_get(); }
ui_reader_work_t document_view_step(uint32_t budget, bool foreground, bool commit)
{ return epd_reader_step(budget, foreground, commit); }
bool document_view_save(void)
{
    bool saved = epd_reader_save();
    bool flushed = ui_bookshelf_flush();
    return saved && flushed;
}
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
