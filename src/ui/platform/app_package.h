#ifndef EPD_APP_PACKAGE_H
#define EPD_APP_PACKAGE_H

#include "app_catalog.h"

#define EPD_PACKAGE_FORMAT 1u
#define EPD_PACKAGE_PATH_MAX 160
#define EPD_PACKAGE_IO_SIZE (32u * 1024u)
typedef void (*app_package_progress_t)(uint64_t completed, uint64_t total, void *user);

typedef enum
{
    APP_PACKAGE_OK,
    APP_PACKAGE_FORMAT_ERROR,
    APP_PACKAGE_INCOMPATIBLE,
    APP_PACKAGE_IO_ERROR,
    APP_PACKAGE_INTEGRITY_ERROR
} app_package_status_t;

/* In-memory identity only: never persist these counters as a content digest. */
typedef struct
{
    uint32_t session, state, paths[6];
} app_package_revision_t;
bool app_package_revision(const char *root, const char *id, const char *library,
                          app_package_revision_t *revision);

/* Receipts are consumed only after every record's streamed SHA-256, write,
 * sync and close succeeded. The extraction owner keeps the staging tree
 * private. This verifier takes storage_lock only around individual I/O calls. */
typedef struct
{
    char name[EPD_PACKAGE_PATH_MAX];
    unsigned char digest[32];
    uint64_t size;
} app_package_record_t;
app_package_status_t app_package_verify_extracted(const char *root, const char *id,
                                                  const app_package_record_t *records, uint32_t count);

/* These functions never load or execute a module. Individual I/O calls take
 * storage_lock; avoid an outer lock around verification or metadata parsing.
 * Verification-cache and resident-module metadata are copied under short
 * interrupt guards; hashing never holds that guard. */
bool app_package_metadata(const char *root, const char *id, epd_app_entry_t *entry);
bool app_package_metadata_locale(const char *root, const char *id, epd_app_entry_t *entry, const char *locale);
bool app_package_handles_path(const char *root, const char *path);
/* Optional load_peak is the main ELF file plus its LOAD span, for reclaiming
 * cache before dlopen. It is not an allocation admission limit. */
app_package_status_t app_package_verify(const char *root, const char *id, bool all_files,
                                        size_t *load_peak);
app_package_status_t app_package_verify_progress(const char *root, const char *id,
                                                app_package_progress_t progress, void *user);
app_package_status_t app_package_verify_library(const char *root, const char *id,
                                               const char *name, size_t *load_peak);
const char *app_package_error(app_package_status_t status);

#endif
