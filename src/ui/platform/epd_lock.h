#ifndef EPD_LOCK_H
#define EPD_LOCK_H

#include <stdbool.h>

/* All functions run on the UI thread. An empty selected ID means built-in. */
const char *epd_lock_selected(void);
bool epd_lock_select(const char *id);
bool epd_lock_active(void);
/* Request unlock from an application callback. The platform exits the lock
 * application only after the callback has returned. */
bool epd_lock_unlock(void);

#endif
