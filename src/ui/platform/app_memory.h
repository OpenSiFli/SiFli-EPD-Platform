#ifndef APP_MEMORY_H
#define APP_MEMORY_H

#include "epd_memory.h"
#include "epd_memory_manager.h"
#include <stdbool.h>

struct _lv_draw_buf_handlers_t;
const struct _lv_draw_buf_handlers_t *app_memory_font_cache_handlers(void);
epd_memory_result_t app_memory_init(void);
epd_memory_owner_t app_memory_foreground_page(void *context, epd_memory_owner_t account);

/* Stack-owned observation of one synchronous GUI load attempt. Allocator
 * failure reporting only records the calling thread; it never reclaims/waits. */
typedef struct app_memory_load_watch {
    struct app_memory_load_watch *previous;
    const void *thread;
    size_t peak, contiguous;
    const char *target;
    int start_error;
    bool busy;
} app_memory_load_watch_t;
void app_memory_load_begin(app_memory_load_watch_t *watch);
void app_memory_load_end(app_memory_load_watch_t *watch);
app_memory_load_watch_t *app_memory_load_current(void);
void app_memory_load_failed(size_t physical);

#endif
