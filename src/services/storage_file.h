#ifndef EPD_STORAGE_FILE_H
#define EPD_STORAGE_FILE_H

#include "storage.h"

typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;

/* Thread context. Writes the installed application's code/data/cache path to
 * the caller's buffer; no directory is created. This is a directory convention,
 * not an access-control boundary for native modules. */
bool storage_app_path(char *out, size_t size, const char *id,
                      storage_app_area_t area, const char *relative);
bool storage_app_available(const char *id);
bool storage_mkdirs(const char *path);
uint32_t storage_crc32(const void *data, size_t size);
/* Synchronous I/O. The caller owns the buffers; save() does not retain data. */
bool storage_record_load(const char *path, uint32_t magic, uint32_t version, void *data, size_t size);
bool storage_record_save(const char *path, uint32_t magic, uint32_t version, const void *data, size_t size);
/* Serialized replacement. FAT keeps a backup across its two renames; callers
 * opening raw files recover an interrupted replacement before open(). */
bool storage_file_recover(const char *path);
bool storage_file_commit(const char *temporary, const char *path);
bool storage_file_replace(const char *path, const void *data, size_t size);
/* Background replacement: at most 4 KiB per lock; retain the expected card session. */
bool storage_file_replace_session(const char *path, const void *data, size_t size, uint32_t session);
bool storage_file_import(const char *source, const char *destination);
bool storage_file_import_session(const char *source, const char *destination,
                                 storage_volume_t volume, uint32_t expected_session);
/* In-memory change tokens; combine with the card session. Tokens do not survive
 * reboot. Open files pin their entry; evicted entries receive a new token. */
uint32_t storage_path_revision(const char *path);
uint32_t storage_directory_revision(const char *path);
uint32_t storage_volume_revision(storage_volume_t volume);
uint32_t storage_file_writers(const char *path);
/* Memory-only token retention across fd lifetimes. Pin before taking the
 * token; pair each successful pin with one unpin before retiring its mount.
 * Full/unavailable tracking tables return false without acquiring a pin. */
bool storage_path_pin(const char *path);
void storage_path_unpin(const char *path);
/* LittleFS has no mtime in this SDK. Content tag replaces it for reader caches. */
bool storage_file_signature(const char *path, uint32_t *signature);
/* Optional incremental FNV-1a engine, called outside the storage lock. */
typedef bool (*storage_signature_update_t)(void *user, const void *data, size_t size, uint32_t *hash);
bool storage_file_signature_with(const char *path, uint32_t *signature,
                                 storage_signature_update_t update, void *user);

#endif
