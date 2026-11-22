#ifndef MUSIC_PROGRESS_HOST_H
#define MUSIC_PROGRESS_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "music_store.h"

#define STORAGE_PATH_MAX 512
#define STORAGE_SD_ROOT "/sd"
#define STORAGE_APP_DATA 1
typedef int intent_t;
struct music_host_stat { int64_t st_size; unsigned st_mode; };
#define S_ISDIR(mode) ((mode) == 1)
#define stat music_host_stat
int music_host_stat(const char *path, struct music_host_stat *out);
bool storage_app_path(char *, size_t, const char *, int, const char *);
bool storage_record_save(const char *, uint32_t, uint32_t, const void *, size_t);
bool storage_record_load(const char *, uint32_t, uint32_t, void *, size_t);
bool storage_file_recover(const char *);
uint32_t storage_crc32(const void *, size_t);
bool storage_mkdirs(const char *);
bool host_queue_valid(const music_queue_t *);
#endif
