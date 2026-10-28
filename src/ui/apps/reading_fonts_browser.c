#include "reading_fonts_browser.h"
#include <dfs_posix.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

bool reading_fonts_is_ttf(const char *name)
{
    const char *suffix = name ? strrchr(name, '.') : NULL;
    return suffix && strlen(suffix) == 4 && tolower((unsigned char)suffix[1]) == 't' &&
           tolower((unsigned char)suffix[2]) == 't' && tolower((unsigned char)suffix[3]) == 'f';
}

static bool card_path(const char *path)
{
    size_t length = strlen(STORAGE_SD_ROOT);
    return !strncmp(path, STORAGE_SD_ROOT, length) && (!path[length] || path[length] == '/');
}

bool reading_fonts_browser_path(const reading_fonts_browser_t *browser, unsigned row,
                                char *path, size_t capacity)
{
    if (!browser || row >= browser->count || !card_path(browser->path)) return false;
    int n = snprintf(path, capacity, "%s/%s", browser->path, browser->files[row].name);
    return n > 0 && (size_t)n < capacity;
}

void reading_fonts_browser_read(reading_fonts_browser_t *browser)
{
    browser->count = 0;
    browser->more = false;
    browser->error = -RT_EIO;
    if (!card_path(browser->path)) return;
    storage_lock();
    browser->session = storage_card_session();
    DIR *directory = !storage_changing() && storage_available(STORAGE_SD) ? opendir(browser->path) : NULL;
    if (directory)
    {
        unsigned skipped = 0;
        browser->error = RT_EOK;
        for (;;)
        {
            if (storage_changing() || !storage_session_valid(STORAGE_SD, browser->session))
            { browser->error = -RT_EIO; break; }
            errno = 0;
            struct dirent *entry = readdir(directory);
            if (!entry) { if (errno) browser->error = -RT_EIO; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
                strchr(entry->d_name, '/') || strchr(entry->d_name, '\\')) continue;
            char path[STORAGE_PATH_MAX];
            int n = snprintf(path, sizeof(path), "%s/%s", browser->path, entry->d_name);
            struct stat st;
            if (n < 0 || (size_t)n >= sizeof(path) || stat(path, &st)) continue;
            bool folder = S_ISDIR(st.st_mode);
            if (!folder && (!S_ISREG(st.st_mode) || !reading_fonts_is_ttf(entry->d_name))) continue;
            if (strlen(entry->d_name) >= sizeof(browser->files[0].name)) continue;
            if (skipped++ < browser->first) continue;
            if (browser->count == READING_FONTS_FILES) { browser->more = true; break; }
            reading_fonts_file_t *file = &browser->files[browser->count++];
            strcpy(file->name, entry->d_name);
            file->directory = folder;
            file->size = st.st_size > 0 && (uint64_t)st.st_size <= UINT32_MAX ? (uint32_t)st.st_size : 0;
        }
        closedir(directory);
    }
    if (storage_changing() || !storage_session_valid(STORAGE_SD, browser->session)) browser->error = -RT_EIO;
    if (browser->error) { browser->count = 0; browser->more = false; }
    storage_unlock();
}

bool reading_fonts_browser_up(reading_fonts_browser_t *browser)
{
    if (!card_path(browser->path) || !strcmp(browser->path, STORAGE_SD_ROOT)) return false;
    char *slash = strrchr(browser->path, '/');
    if (!slash || slash < browser->path + strlen(STORAGE_SD_ROOT)) return false;
    *slash = 0;
    browser->first = 0;
    return true;
}
