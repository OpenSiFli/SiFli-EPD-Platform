/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_FS_H
#define BOOK_FS_H
#ifdef BOOK_FORMAT_TARGET
#include <dfs_posix.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#include <stdint.h>
#include <stdio.h>

typedef struct bf_file_handle bf_FILE;
typedef struct bf_directory
{
    DIR *dir;
    uint32_t revision;
    const char *path;
    int status;
    struct dirent entry;
} bf_directory;
bf_FILE *bf_fopen(const char *path, const char *mode);
size_t bf_fread(void *buffer, size_t size, size_t count, bf_FILE *file);
int bf_fseek(bf_FILE *file, long offset, int origin);
int bf_fclose(bf_FILE *file);
int bf_ferror(bf_FILE *file);
int bf_fgetc(bf_FILE *file);
int bf_ungetc(int c, bf_FILE *file);
int bf_stat(const char *path, struct stat *st);
int bf_dir_open(bf_directory *d, const char *path);
struct dirent *bf_dir_next(bf_directory *d);
void bf_dir_close(bf_directory *d);
#endif
