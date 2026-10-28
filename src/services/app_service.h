#ifndef EPD_APP_SERVICE_H
#define EPD_APP_SERVICE_H
#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

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
void epd_service_wake(epd_service_t *service);
/* Idempotent per service. Acquire before accepting work; false means stopping
 * or committed sleep. Release after all owned work and resource cleanup ends. */
bool epd_service_sleep_acquire(epd_service_t *service);
void epd_service_sleep_release(epd_service_t *service);
/* Worker or UI callback: request the next step after delay_ms; repeated
 * requests keep the earliest deadline. Consumed before the next callback and
 * cleared on stop. Cleanup callbacks can request another stopping step.
 * Explicit requests also run during sleep; ordinary UI polling is suspended. */
void epd_service_ui_request(epd_service_t *service, uint32_t delay_ms);
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
bool app_service_running(const char *id);
/* True only on the registered service's own unfinished worker. Cancellation
 * is independent: check epd_service_cancelled() before accepting new work. */
bool epd_service_is_worker(epd_service_t *service);
#endif
