#ifndef EPD_STORAGE_FILE_INTERNAL_H
#define EPD_STORAGE_FILE_INTERNAL_H

#include "storage_file.h"

#define STORAGE_FLASH_APPS STORAGE_FLASH_ROOT "/apps"
#define STORAGE_SD_APPS STORAGE_SD_ROOT "/.epd/apps"
const char *storage_app_directory(storage_volume_t volume);
bool storage_app_locate(const char *id, storage_volume_t *volume);


void storage_prepare_directories(void);
/* Called after mount, before publishing the volume to file users. */
void storage_watch_mount(storage_volume_t volume);
/* Drain owned failed-close handles before acknowledging a volume retirement.
 * Each nonzero release ID permits one attempt per retryable handle; polling the
 * same ID never retries indefinitely. False keeps active/quarantined fds owned. */
bool storage_file_release(storage_volume_t volume, uint32_t release_id);

#endif
