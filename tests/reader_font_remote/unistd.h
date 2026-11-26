#ifndef READER_FONT_REMOTE_UNISTD_H
#define READER_FONT_REMOTE_UNISTD_H

#include <sys/types.h>
#include <sys/stat.h>

/* The fixture supplies a regular file (mode 1) and its own descriptors. */
#define S_ISREG(mode) ((mode) == 1)
#define open(...) remote_file_open(__VA_ARGS__)
#define read(...) remote_file_read(__VA_ARGS__)
#define lseek(...) remote_file_seek(__VA_ARGS__)
#define fstat(...) remote_file_stat(__VA_ARGS__)
#define close(...) remote_file_close(__VA_ARGS__)

int open(const char *path, int flags);
int read(int fd, void *buffer, unsigned size);
off_t lseek(int fd, off_t offset, int from);
int fstat(int fd, struct stat *status);
int close(int fd);

#endif
