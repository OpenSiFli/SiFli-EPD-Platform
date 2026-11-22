#ifndef APP_MODULE_H
#define APP_MODULE_H

#include "storage.h"
#include "app_package.h"
#include "platform/epd_memory_manager.h"

/* UI and worker references are serialized by an independent lifecycle mutex.
 * Call without holding storage_lock. An application and its private libraries
 * retain their installation identity until the last module is destroyed. */
void *app_module_open(const char *path, const char *id);
void app_module_close(void *module);
/* The returned owner holds a reference; release it after creating the child
 * scope or after the guarded call. Closing instances reject new borrowers. */
epd_memory_owner_t app_module_owner_acquire(const char *id);
/* GUI-loop retirement of code still borrowed by callbacks or workers. */
void app_modules_process(void);
/* UI memory pressure: schedule one idle STATE instance for durable eviction. */
bool app_modules_reclaim(const char *foreground_id);
/* Nonblocking retirement observation; a busy lifecycle mutex remains pending. */
bool app_modules_retiring(void);
void app_memory_context_process(void);
bool app_module_loaded(const char *id);
/* UI thread, after pages leave: withdraw retention and schedule normal drain. */
epd_memory_result_t app_module_request_stop(const char *id);
/* System teardown can also stop retained modules that have no GUI pages. */
void app_modules_request_stop(void);
bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *session);
bool app_modules_present(void);
/* Thread-safe metadata snapshot: 1 matching resident code, 0 not resident,
 * -1 resident installation changed. This query does not acquire a reference. */
int app_module_verified(const char *root, const char *id, const char *library, size_t *peak);
/* UI thread only; the SDK already initializes newly loaded module languages. */
void app_modules_set_locale(const char *locale);

#endif
