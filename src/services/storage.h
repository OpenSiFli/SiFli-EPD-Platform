#ifndef EPD_STORAGE_H
#define EPD_STORAGE_H
#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_SD_ROOT "/sdcard"
#define STORAGE_PATH_MAX 512
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef struct { bool mounted; uint64_t total, used; } storage_info_t;

/* Root strings are static. relative_path() borrows from path; neither allocates. */
const char *storage_root(storage_volume_t volume);
const char *storage_path_root(const char *path);
const char *storage_relative_path(const char *path);
bool storage_info(storage_volume_t volume, storage_info_t *info);
bool storage_available(storage_volume_t volume);
bool storage_path_available(const char *path);
/* A removed card invalidates its session before a replacement can be mounted. */
uint32_t storage_card_session(void);
bool storage_session_valid(storage_volume_t volume, uint32_t session);
uint32_t storage_revision(void);
bool storage_changing(void);
/* Thread context. Pair lock/unlock on the same thread around raw filesystem
 * access that must be serialized with storage handoff. */
void storage_lock(void);
void storage_unlock(void);
#endif
