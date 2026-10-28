#ifndef EPD_PORTAL_PACKAGE_H
#define EPD_PORTAL_PACKAGE_H
#include "storage.h"
#include <stddef.h>

#define PORTAL_PACKAGE_PATH_MAX 256
#define PORTAL_PACKAGE_ID_SIZE 33
/* One upload owns its scratch file until the installer has closed it. All
 * functions serialize state internally; process/start run on the UI. */
bool portal_package_init(void);
int portal_package_begin(const char *id, storage_volume_t volume, uint64_t length);
int portal_package_write(const void *data, size_t size);
int portal_package_finish(char path[PORTAL_PACKAGE_PATH_MAX]);
void portal_package_abort(const char *message);
bool portal_package_queue(const char *path, storage_volume_t volume);
bool portal_package_start(const char *path, storage_volume_t volume, uint32_t *ticket);
void portal_package_process(void);
void portal_package_block(bool blocked);
bool portal_package_storage_idle(void);
/* Current result plus eight completed results. cleanup_failed reports a retained
 * file after bounded cleanup; cleanup_path/volume/session identify its owner. */
bool portal_package_result(const char *id, char *json, size_t capacity);
#endif
