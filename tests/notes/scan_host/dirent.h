#ifndef NOTES_SCAN_DIRENT_H
#define NOTES_SCAN_DIRENT_H
#include <stdint.h>
/* SDK fields used by the unmodified DFS readdir, plus host backing storage. */
struct dirent { uint8_t d_type, d_namlen; uint16_t d_reclen; char d_name[256]; };
typedef struct {
    int fd; char buf[512]; int num, cur;
    unsigned index; char path[512]; struct dirent entry;
} DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#endif
