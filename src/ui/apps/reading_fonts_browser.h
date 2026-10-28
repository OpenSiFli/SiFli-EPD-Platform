#ifndef UI_READING_FONTS_BROWSER_H
#define UI_READING_FONTS_BROWSER_H

#include "storage.h"

#define READING_FONTS_FILES 12
typedef struct
{
    char name[256];
    uint32_t size;
    bool directory;
} reading_fonts_file_t;

typedef struct
{
    char path[STORAGE_PATH_MAX];
    reading_fonts_file_t files[READING_FONTS_FILES];
    unsigned first, count;
    uint32_t session;
    bool more;
    rt_err_t error;
} reading_fonts_browser_t;

bool reading_fonts_is_ttf(const char *name);
void reading_fonts_browser_read(reading_fonts_browser_t *browser);
bool reading_fonts_browser_path(const reading_fonts_browser_t *browser, unsigned row,
                                char *path, size_t capacity);
bool reading_fonts_browser_up(reading_fonts_browser_t *browser);

#endif
