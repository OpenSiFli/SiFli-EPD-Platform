#ifndef UI_READER_H
#define UI_READER_H

#include "../common/reader/reader_types.h"
#include "bookshelf.h"

/* Navigation computes page deltas with signed int. */
#define UI_READER_PAGE_MAX READER_PAGE_MAX

typedef reader_view_t ui_reader_view_t;

/* Queue an open without document/font preparation; show cached pixels or loading. */
bool ui_reader_open(unsigned book);
const char *ui_reader_identity(void);
const bookshelf_item_t *ui_reader_file(void);
void ui_reader_close(void);
/* Final module teardown releases the bounded entry-picture cache. */
void ui_reader_shutdown(void);
/* UI queues visible/selected resume records; only the books worker reads files. */
void ui_reader_cache_preload(unsigned first, unsigned count);
bool ui_reader_cache_process(void);
struct epd_service;
bool ui_reader_cache_worker_step(struct epd_service *service, bool reader_worked);
/* Selected preview only: at most 64 KiB or 8 ms before one document step. */
bool ui_reader_cache_worker_priority(struct epd_service *service);
void ui_reader_cache_worker_shutdown(void);
/* UI: idle capture is debounced; a new bound page only marks it dirty. */
void ui_reader_cache_idle(lv_obj_t *body, bool idle);
/* Discard queued work and request active cleanup; busy covers active only. */
void ui_reader_cache_quiesce(void);
bool ui_reader_cache_busy(void);
/* UI: signal actual first-screen refresh submission, not an elapsed delay. */
void ui_reader_first_frame(void);
bool ui_reader_preview_active(void);
void ui_reader_cache_diagnostic(void);
bool ui_reader_open_pending(void);
bool ui_reader_bind(lv_obj_t *body);
/* Cache the last bound page at pause, before its widgets are detached. */
void ui_reader_cache_entry(lv_obj_t *body);
bool ui_reader_save(void);
bool ui_reader_reflow(void);
bool ui_reader_settings_changed(void);
typedef reader_work_t ui_reader_work_t;
#define UI_READER_WORK_IDLE READER_WORK_IDLE
#define UI_READER_WORK_BACKGROUND READER_WORK_BACKGROUND
#define UI_READER_WORK_TARGET READER_WORK_TARGET

/* One shared soft budget, in milliseconds. Only commit publishes view state;
 * only foreground && commit activates/saves a page. A ready target is committed
 * before optional preparation. The caller yields while the display is busy. */
ui_reader_work_t ui_reader_step(uint32_t budget_ms, bool foreground, bool commit);
/* Keep the visible page/cache and submitted work while suspending optional
 * layout, glyph/image preparation and automatic index checkpoints. */
void ui_reader_background_pause(bool paused);
/* Cancel a queued seek while retaining the last visible page or restore anchor. */
void ui_reader_cancel_pending(void);
void ui_reader_seek(unsigned page);
void ui_reader_turn(int direction);
const ui_reader_view_t *ui_reader_view(void);
bool ui_reader_waiting(void);

#define UI_READER_TOP READER_TOP
#define UI_READER_BOTTOM READER_BOTTOM

#endif
