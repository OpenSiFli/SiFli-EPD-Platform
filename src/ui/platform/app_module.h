#ifndef APP_MODULE_H
#define APP_MODULE_H

#include "storage.h"
#include "app_package.h"

/* UI and worker references are serialized by an independent lifecycle mutex.
 * Call without holding storage_lock. An application and its private libraries
 * retain their installation identity until the last module is destroyed. */
void *app_module_open(const char *path, const char *id);
void app_module_close(void *module);
bool app_module_loaded(const char *id);
bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *session);
bool app_modules_present(void);
/* Thread-safe metadata snapshot: 1 matching resident code, 0 not resident,
 * -1 resident installation changed. This query does not acquire a reference. */
int app_module_verified(const char *root, const char *id, const char *library, size_t *peak);
/* UI thread only; the SDK already initializes newly loaded module languages. */
void app_modules_set_locale(const char *locale);

#endif
