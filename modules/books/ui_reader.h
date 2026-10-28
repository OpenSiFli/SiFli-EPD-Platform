#ifndef UI_READER_H
#define UI_READER_H

#include "../common/reader/reader_types.h"

/* Navigation computes page deltas with signed int. */
#define UI_READER_PAGE_MAX READER_PAGE_MAX

typedef reader_view_t ui_reader_view_t;

/* Prepare resources without activating a page or changing reading history. */
bool ui_reader_open(unsigned book);
const char *ui_reader_identity(void);
void ui_reader_close(void);
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
