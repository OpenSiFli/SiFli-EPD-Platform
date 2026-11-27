/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_OPERATION_TEST_DFS_POSIX_H
#define EPD_OPERATION_TEST_DFS_POSIX_H

#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include "storage.h"

/* Each case gets its own working directory and a private system directory. */
#undef STORAGE_FLASH_ROOT
#define STORAGE_FLASH_ROOT "."

#ifdef _WIN32
#include <io.h>
/* The firmware reads binary records. Keep Windows text mode out of the fixture. */
int test_open(const char *path, int flags);
int test_read(int fd, void *data, size_t size);
#define open test_open
#define read test_read
#define close _close
#define unlink _unlink
#define stat _stat64
#define fstat _fstat64
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#else
#include <unistd.h>
#endif

#endif
