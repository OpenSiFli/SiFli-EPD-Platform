#ifndef EPD_APP_SERVICE_H
#define EPD_APP_SERVICE_H
#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "app_background.h"

typedef struct epd_service epd_service_t;
/* Service ID capacity including NUL, independent of kernel object names. */
#define EPD_SERVICE_ID_SIZE 64u
#define EPD_SERVICE_WAKE 1u
#define EPD_SERVICE_NETWORK 2u
#define EPD_SERVICE_STOP 4u
typedef struct
{
    void (*run)(epd_service_t *service);
    uint32_t stack_size;
    uint8_t priority;
    bool network;
    int (*command)(int argc, char **argv);
    /* One bounded UI-thread step, optionally alongside run. stopping becomes
     * true only after run and shell calls have returned. Return true only after
     * page teardown and all UI-owned resources have been released. */
    bool (*ui_process)(epd_service_t *service, bool stopping);
    /* UI-thread persistence barrier before power-off or storage handoff.
     * false means not yet saved (pending or failed); callers must not retire
     * the storage session or stop the worker until true. */
    bool (*flush)(void);
} epd_background_t;

/* A module exports epd_app_background. Its run function must return on stop,
 * close its requests/handles, and never retain LVGL page objects. */
uint32_t epd_service_wait(epd_service_t *service, rt_int32_t ticks);
bool epd_service_cancelled(epd_service_t *service);
/* Background explicit RELEASE/STATE requests cooperative business completion.
 * Default policy and foreground work do not cancel. Finish accepted changes and
 * saving before returning; this signal leaves cleanup admission available. */
bool epd_service_background_cancelled(epd_service_t *service);
void epd_service_wake(epd_service_t *service);
/* Replace the application's single next business deadline. -1 cancels it.
 * A network requirement waits for connectivity after the deadline. The record
 * survives idle/pressure unload without retaining module code. Explicit stop
 * or RELEASE cancels it; sleep and memory pressure do not start new work. */
bool epd_service_schedule(epd_service_t *service, rt_int32_t delay_ticks, bool require_network);
/* Register one nonblocking cancellation signal while the worker is alive.
 * Called outside the publication guard in the service cleanup scope. Keep the
 * signal nonblocking; worker/UI cleanup releases the owned resources.
 * A stopped service invokes a newly registered callback immediately and returns
 * false. Pass NULL to unregister before releasing the callback's context. */
bool epd_service_on_stop(epd_service_t *service, void (*notify)(void *), void *context);
/* Idempotent per service. Acquire before accepting work; false means stopping
 * or committed sleep. Release after all owned work and resource cleanup ends. */
bool epd_service_sleep_acquire(epd_service_t *service);
void epd_service_sleep_release(epd_service_t *service);
/* Worker or UI callback: request the next step after delay_ms; repeated
 * requests keep the earliest deadline. Consumed before the next callback and
 * cleared on stop. Cleanup callbacks can request another stopping step.
 * Explicit requests also run during sleep; ordinary UI polling is suspended. */
void epd_service_ui_request(epd_service_t *service, uint32_t delay_ms);
/* Subscribe a live service to shared UI state transitions. Delivery is a
 * coalesced UI step through its existing callback and ownership scope. */
enum {
    EPD_SERVICE_UI_DISPLAY = 1u << 0,
    EPD_SERVICE_UI_FONT = 1u << 1,
    EPD_SERVICE_UI_LIFECYCLE = 1u << 2,
    EPD_SERVICE_UI_SUMMARY = 1u << 3,
    EPD_SERVICE_UI_SETTINGS = 1u << 4,
    EPD_SERVICE_UI_STORAGE = 1u << 5,
    EPD_SERVICE_UI_OPERATION = 1u << 6,
    EPD_SERVICE_UI_WALLPAPER = 1u << 7,
    EPD_SERVICE_UI_AI = 1u << 8,
    EPD_SERVICE_UI_SPEECH = 1u << 9,
    EPD_SERVICE_UI_FONT_PREVIEW = 1u << 10,
    EPD_SERVICE_UI_FILES = 1u << 11
};
void epd_service_ui_watch(epd_service_t *service, uint32_t events);
/* The worker publishes a copy; the UI reads it without calling module code. */
void epd_service_publish_summary(epd_service_t *service, const char *text);
/* Resident snapshots: no module callback or borrowed pointer. The revision
 * changes on publication/removal, not on repeated identical publications. */
uint32_t epd_app_summary_revision(void);
bool epd_app_summary_read(const char *id, char *text, size_t capacity);
/* UI thread. Start/stop application-owned services. Dynamic applications pass
 * their SDK module handle; the platform retains its code until work and cleanup
 * finish. A NULL module is for resident firmware services. Stop requests
 * cancellation; running stays true until the service has been retired. */
epd_service_t *app_service_start(const char *id, const epd_background_t *definition, void *module);
/* UI thread. An auxiliary worker shares its application's authorization and
 * storage lifetime, retains the module, and stops with the owning service. */
epd_service_t *app_service_start_auxiliary(epd_service_t *owner, const char *name,
                                          const epd_background_t *definition);
void app_service_stop(const char *id);
/* UI thread, after pages leave. Repeated calls withdraw application retention,
 * flush accepted work, and wait for services/resources/module ownership to drain. */
epd_memory_result_t app_service_terminate(const char *app_id);
bool app_service_running(const char *id);
/* True only on the registered service's own unfinished worker. Cancellation
 * is independent: check epd_service_cancelled() before accepting new work. */
bool epd_service_is_worker(epd_service_t *service);
/* Opt in an application's root service to low-priority memory reclamation.
 * Only catalog background applications with cooperative workers are eligible;
 * default is protected. UI-only services in the app also prevent reclamation.
 * The worker must finish writes and release resources before returning on stop.
 * Its flush/UI callbacks must be bounded, nonblocking cooperative steps. */
void epd_service_set_reclaimable(epd_service_t *service, bool reclaimable);
/* Acquire before accepting data-changing work (including queued work). A false
 * result means cancellation has won; do not accept the work. Balanced holds
 * protect the entire application from memory-pressure cancellation. */
bool epd_service_reclaim_hold(epd_service_t *service);
void epd_service_reclaim_release(epd_service_t *service);
/* Existing dirty state may finish saving after RELEASE. Acquire only inside
 * this service's cleanup owner or flush callback; release after the async write.
 * This pins service/code and sleep, without admitting TASKS. Failed saves retry
 * with a 500 ms to 5 s backoff; successful flush acknowledgement is immediate. */
bool epd_service_cleanup_hold(epd_service_t *service);
void epd_service_cleanup_release(epd_service_t *service);
/* Mark a newly published data change before releasing its accepted-work hold.
 * Flush callbacks acknowledge only the revision observed before they ran. */
bool epd_service_mark_dirty(epd_service_t *service);
/* A retained accounting scope for a registered asynchronous callback. This
 * does not pin module code: keep the existing service retain/release lifetime.
 * Enter/leave on the callback thread and release the returned owner afterward. */
epd_memory_owner_t epd_service_owner_acquire(epd_service_t *service, bool cleanup);
#endif
