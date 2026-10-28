#ifndef APP_CONTENT_H
#define APP_CONTENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <rtthread.h>
#include "gui_app_fwk.h"

typedef struct
{
    char id[GUI_APP_ID_MAX_LEN];
    char name[48];
} app_content_handler_t;

/* UI thread only. Copy a page of installed handlers; count receives the total
 * number of matches. Discovery never loads a module. */
rt_err_t app_content_query(const char *path, unsigned first,
                           app_content_handler_t *items, unsigned capacity, unsigned *count);
/* RT_EOK means the SDK intent was queued. Loading and entry are asynchronous. */
rt_err_t app_content_open(const char *id, const char *path);
/* Copy an incoming path before retaining it. -ENOENT means a normal launch.
 * Removed/replaced media and insufficient output capacity are errors. */
rt_err_t app_content_receive(intent_t intent, char *path, size_t size);
const char *app_content_error(rt_err_t error);

#endif
