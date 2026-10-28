/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMMON_READER_H
#define COMMON_READER_H
#include "app_service.h"
#include "reader_types.h"
#include "../../../src/ui/platform/epd_reader_model.h"
#include "platform/epd_app.h"

typedef enum { READER_SOURCE_TXT, READER_SOURCE_XHTML } reader_source_kind_t;
typedef struct {
    document_cursor_t cursor;
    uint32_t conversion_version, layout;
    unsigned page, pages, percent;
} reader_position_t;
typedef bool (*reader_save_fn)(void *user, const reader_position_t *position);
typedef struct {
    const char *app_id;           /* 1..63 ASCII letters/digits, '_', '-' or '.'. */
    const char *cache_path;       /* Absolute directory for derived documents. */
    const char *content_identity; /* Stable account/book/chapter + content revision.
                                    strlen(app_id) + strlen(identity) < 254. */
    const char *path;             /* Complete UTF-8 TXT/XHTML chapter; <256 bytes. */
    const char *title;
    reader_source_kind_t kind;
    reader_position_t restore;
    book_resources_t resources;
    bool (*cancelled)(void *user); /* Nonblocking, thread-safe state read. */
    reader_save_fn save;
    void (*release_user)(void *user);
    void *user;
} reader_options_t;

/* One visible reader per linked component instance. open copies strings and
 * callback structs and queues conversion; it never parses chapter content.
 * resources.open runs only in the worker and may fetch/cache images. Returned
 * streams are consumed and closed by the converter. BOOK_IO_ERROR, BOOK_ERROR
 * and BOOK_UNSUPPORTED from resources.open produce a missing-image placeholder
 * or skip unavailable CSS, and disable persistence for that conversion.
 * Cancellation/OOM and errors while reading an opened resource still fail.
 * Callback user objects must
 * survive close AND completion of the worker call that used the old chapter.
 * save runs in the UI and must enqueue/copy any asynchronous persistence work.
 * restore must belong to the same content_identity/conversion_version.
 * With non-NULL options, open takes ownership of user: release_user is called
 * exactly once, even on failure, either immediately or in a later UI collect
 * after all worker use ends. release_user may free resources.user when owned
 * by user. collect must continue after close, or run after worker shutdown. */
bool epd_reader_open(const reader_options_t *options);

/* Except worker functions, these calls are UI-thread only. step has one soft
 * millisecond budget. commit publishes a prepared page; foreground && commit
 * saves its position. Call bind while the display is idle. get borrows state.
 * pages is zero until pagination completes. During reflow, page/ready still
 * describe the old visible page; wait for the restored page to bind before
 * deriving a new seek target from that page number. */
const reader_view_t *epd_reader_get(void);
/* Copy the bound page's text; never includes a prepared but unpublished page. */
bool epd_reader_page_text(char *text, size_t capacity);
bool epd_reader_active(void);
reader_work_t epd_reader_step(uint32_t budget_ms, bool foreground, bool commit);
/* Pause optional preparation without cancelling submitted work or clearing
 * caches. First-page/seek preparation and explicit save remain available. */
void epd_reader_background_pause(bool paused);
bool epd_reader_bind(lv_obj_t *body);
void epd_reader_detach(void);
void epd_reader_seek(unsigned page); /* One-based page within this chapter. */
void epd_reader_seek_cursor(document_cursor_t cursor);
void epd_reader_cancel_pending(void);
bool epd_reader_waiting(void);
bool epd_reader_save(void);
bool epd_reader_reflow(void);
bool epd_reader_settings_changed(void);
void epd_reader_close(void);
void epd_reader_collect(void); /* Reap closed sessions after worker progress/join. */
unsigned epd_reader_toc_count(void);
bool epd_reader_toc_title(unsigned index, char *title, size_t size, unsigned *level);
bool epd_reader_toc_seek(unsigned index);
bool epd_reader_chapter_move(int direction);
unsigned epd_reader_toc_level(unsigned index);
bool epd_reader_chapter_title(char *title, size_t capacity, unsigned *toc_index);
bool epd_reader_follow_link(uint32_t record);

/* Serial worker only. A step may synchronously convert the entire chapter;
 * true means work was performed. service may be NULL; its cancellation is
 * checked during conversion. These functions make no LVGL calls.
 * The service loop must poll/wake itself when UI enqueues work.
 * Shutdown: stop admission, cancel the service/user context, let step return,
 * call worker_shutdown on that worker, join, then UI close + collect.
 * worker_shutdown does not destroy UI-owned readers, widgets or fonts. */
bool epd_reader_worker_step(epd_service_t *service);
void epd_reader_worker_shutdown(void);
void epd_reader_worker(epd_service_t *service);
#endif
