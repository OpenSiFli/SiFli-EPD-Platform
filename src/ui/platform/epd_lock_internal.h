#ifndef EPD_LOCK_INTERNAL_H
#define EPD_LOCK_INTERNAL_H

#include "epd_lock.h"

void epd_lock_init(void);

/* Reserve built-in page state before navigation; release any unused reserve. */
bool epd_lock_builtin_prepare(void);
void epd_lock_builtin_cancel(void);
bool epd_lock_builtin_ready(void);

/* Outside app_fwk/LVGL callbacks; false during storage handoff. */
bool epd_lock_enter(void);

/* Call from the UI loop, outside app_fwk and LVGL callbacks. */
void epd_lock_process(void);

#endif
