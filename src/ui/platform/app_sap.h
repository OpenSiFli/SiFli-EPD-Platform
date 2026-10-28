#ifndef EPD_APP_SAP_H
#define EPD_APP_SAP_H

#include "app_package.h"

#define APP_SAP_PATH_MAX 256
#define APP_SAP_SIZE_MAX UINT64_C(1073741824)
#define APP_SAP_FILE_MAX 8192u

/* I/O takes storage_lock in bounded chunks. Extraction requires a new, empty
 * destination owned exclusively by the caller. */
app_package_status_t app_sap_metadata(const char *path, epd_app_entry_t *entry);
app_package_status_t app_sap_inspect(const char *path, uint32_t block_size,
                                    uint64_t *allocated, uint64_t *payload);
app_package_status_t app_sap_extract(const char *path, const char *destination,
                                    app_package_progress_t progress, void *user);
/* Extract and validate manifest/index against the streamed record hashes. */
app_package_status_t app_sap_install(const char *path, const char *destination, const char *id,
                                    app_package_progress_t progress, void *user);

#endif
