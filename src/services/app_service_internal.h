#ifndef EPD_APP_SERVICE_INTERNAL_H
#define EPD_APP_SERVICE_INTERNAL_H

#include "app_service.h"

/* Optional shell command; holds the module alive until the call returns. */
int app_service_command(const char *id, int argc, char **argv);


/* Resident state publishers validate a live service and copy its stable ID. */
bool epd_service_identity(epd_service_t *service, char *id, size_t capacity);

/* Resident asynchronous work pins the service/module independently of the
 * application's worker. Release exactly once after no borrowed owner remains.
 * Retain rejects a stopping or retired storage session. */
bool epd_service_retain(epd_service_t *service);
void epd_service_release(epd_service_t *service);
bool app_service_stopping(void);
/* UI thread: a 5 ms callback budget (rounded up to ticks), with at least one
 * eligible callback when the remaining batch contains ready work.
 * Continue from the next service after exhausting the budget. Each callback
 * must bound its own work because a running callback cannot be preempted here. */
void app_service_process(void);
/* Same batch budget/cursor; stopping cleanup and due explicit requests only. */
void app_service_maintenance_process(void);
/* Normal UI iteration: advance the foreground app before drawing, then give
 * the remaining services their bounded turn. A callback runs once per iteration. */
void app_service_ui_begin_iteration(void);
void app_service_ui_process_foreground(const char *app_id);
void app_service_ui_process_deferred(void);
/* UI thread, after sleep_guard_cancel(): retry retained work on live workers
 * using their existing WAKE event. Does not restart or call module code. */
void app_service_resume(void);
/* UI thread: min(default_ms, remaining service deadlines), in milliseconds.
 * Expired deadlines and unfinished batches return zero. Reading does not
 * consume a request or advance the batch cursor. */
uint32_t app_service_ui_delay(uint32_t default_ms);
bool app_service_flush(void);
/* UI thread: persistence barrier for one application's services only.
 * False leaves its pages and workers alive so asynchronous saving can finish. */
bool app_service_flush_app(const char *app_id);
/* UI owner supplies the installed package's background metadata. */
void app_service_set_background(const char *app_id, bool background);
bool app_service_app_stopping(const char *app_id);
typedef enum {
    APP_SERVICE_RECLAIM_NONE,
    APP_SERVICE_RECLAIM_WAITING,
    APP_SERVICE_RECLAIM_PROGRESS
} app_service_reclaim_result_t;
/* UI thread: visit one eligible root, call at most one bounded flush callback,
 * or signal cancellation. Reclamation itself uses the normal deferred cleanup.
 * A missing foreground identity protects all services. generation must be nonzero. */
app_service_reclaim_result_t app_service_reclaim_step(const char *foreground_id,
                                                      uint32_t generation);
void app_service_stop_modules(void);
bool app_service_modules_running(void);

#endif
