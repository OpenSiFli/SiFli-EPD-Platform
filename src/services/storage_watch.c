#include "storage_file_internal.h"
#include "epd_memory.h"
#include <dfs_file.h>
#include <dfs_posix.h>
#include <rthw.h>
#include <rtm.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#ifdef RT_USING_DFS_ELMFAT
#define DIR FAT_DIR
#include <ff.h>
#undef DIR
#endif

#if defined(FF_DEFINED) && FF_USE_LFN && FF_LFN_UNICODE == 2 && !FF_FS_EXFAT
#define WATCH_FAT_ALIASES 1
typedef struct
{
    char name[STORAGE_PATH_MAX], short_name[STORAGE_PATH_MAX];
} alias_route_t;
typedef struct alias_change
{
    struct alias_change *next;
    struct dfs_fd *fd;
    alias_route_t route;
    bool subtree;
} alias_change_t;
typedef struct
{
    alias_route_t route;
    uint32_t used;
    bool directory;
} alias_directory_t;
#define ALIAS_DIRECTORIES 8u
#endif

#define WATCH_SLOTS 128u
typedef struct
{
    char path[STORAGE_PATH_MAX];
    uint32_t hash, content, directory, used;
    unsigned pins, writers;
} change_slot_t;
typedef struct
{
    struct dfs_filesystem_ops ops;
    struct dfs_file_ops files;
    const struct dfs_filesystem_ops *original;
    uint32_t epoch, revision, writers;
    bool renamed_writer, resetting;
    change_slot_t *slots;
#ifdef WATCH_FAT_ALIASES
    bool fat;
    alias_change_t *aliases;
    alias_directory_t *directories;
    uint32_t directory_use;
#endif
} volume_watch_t;
static volume_watch_t watches[STORAGE_COUNT];
static uint32_t sequence = 1, access_sequence;

static uint32_t next_revision(void)
{
    if (++sequence == 0)
    {
        sequence = 1;
        for (unsigned v = 0; v < STORAGE_COUNT; ++v)
        {
            watches[v].epoch = watches[v].revision = 1;
            if (watches[v].slots)
                for (unsigned i = 0; i < WATCH_SLOTS; ++i)
                    watches[v].slots[i].content = watches[v].slots[i].directory = 1;
        }
    }
    return sequence;
}
static uint32_t path_hash(const char *path)
{
    uint32_t h = 2166136261u;
    while (*path) h = (h ^ (unsigned char)*path++) * 16777619u;
    return h;
}
/* Caller holds the short interrupt guard; open files keep their exact entry. */
static change_slot_t *find_slot(volume_watch_t *watch, const char *path, bool create)
{
    if (watch->resetting || !watch->slots || strlen(path) >= STORAGE_PATH_MAX) return NULL;
    uint32_t hash = path_hash(path);
    change_slot_t *victim = NULL;
    for (unsigned i = 0; i < WATCH_SLOTS; ++i)
    {
        change_slot_t *slot = &watch->slots[i];
        if (slot->hash == hash && !strcmp(slot->path, path))
        {
            slot->used = ++access_sequence;
            return slot;
        }
        if (!slot->pins && (!victim || !slot->path[0] ||
            (victim->path[0] && slot->used < victim->used))) victim = slot;
    }
    if (!create || !victim) return NULL;
    memset(victim, 0, sizeof(*victim));
    strcpy(victim->path, path);
    victim->hash = hash;
    victim->content = victim->directory = next_revision();
    victim->used = ++access_sequence;
    return victim;
}
static volume_watch_t *watch_for_path(const char *path)
{
    return &watches[!strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH];
}
static volume_watch_t *watch_for_fs(struct dfs_filesystem *fs)
{ return watch_for_path(fs->path); }

static bool fold_fat_path(char path[STORAGE_PATH_MAX])
{
    unsigned char *p = (unsigned char *)path + strlen(STORAGE_SD_ROOT);
#if defined(RT_USING_DFS_ELMFAT) && FF_USE_LFN && FF_LFN_UNICODE == 2
    size_t length = strlen(path);
    while (*p)
    {
        uint32_t code = *p;
        unsigned n = code < 0x80 ? 1 : code >= 0xc2 && code <= 0xdf ? 2 :
            code >= 0xe0 && code <= 0xef ? 3 : code >= 0xf0 && code <= 0xf4 ? 4 : 0;
        if (!n) return false;
        if (n > 1)
        {
            code &= (1u << (7u - n)) - 1u;
            for (unsigned i = 1; i < n; ++i)
            {
                if ((p[i] & 0xc0) != 0x80) return false;
                code = (code << 6) | (p[i] & 0x3f);
            }
            if ((n == 2 && code < 0x80) || (n == 3 && code < 0x800) ||
                (n == 4 && code < 0x10000) || code > 0x10ffff ||
                (code >= 0xd800 && code <= 0xdfff)) return false;
        }
        code = ff_wtoupper(code);
        unsigned width = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        size_t new_length = length - n + width;
        if (new_length >= STORAGE_PATH_MAX) return false;
        if (width != n)
            memmove(p + width, p + n, length - (size_t)(p - (unsigned char *)path) - n + 1);
        length = new_length;
        for (unsigned i = width - 1; i > 0; --i) { p[i] = 0x80 | (code & 0x3f); code >>= 6; }
        p[0] = width == 1 ? code : (width == 2 ? 0xc0 : width == 3 ? 0xe0 : 0xf0) | code;
        p += width;
    }
#else
    while (*p)
    {
#if (!defined(RT_DFS_ELM_LFN_UNICODE) || RT_DFS_ELM_LFN_UNICODE == 0) && \
    (!defined(RT_DFS_ELM_CODE_PAGE) || RT_DFS_ELM_CODE_PAGE == 936)
        if (*p >= 0x81 && *p <= 0xfe && p[1]) { p += 2; continue; }
#endif
        if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
        ++p;
    }
#endif
    return true;
}
/* Bounded, allocation-free DFS path normalization. Memory-only callers can
 * use aliases even when the heap is exhausted or interrupts are disabled. */
static bool normalize_path(const char *input, char output[STORAGE_PATH_MAX])
{
    if (!input || input[0] != '/' || strlen(input) >= STORAGE_PATH_MAX) return false;
    size_t used = 1;
    output[0] = '/';
    while (*input)
    {
        while (*input == '/') ++input;
        const char *start = input;
        while (*input && *input != '/') ++input;
        size_t n = (size_t)(input - start);
        if (!n || (n == 1 && start[0] == '.')) continue;
        if (n == 2 && start[0] == '.' && start[1] == '.')
        {
            if (used <= 1) return false;
            while (used > 1 && output[used - 1] != '/') --used;
            if (used > 1) --used;
            continue;
        }
        if (used > 1) output[used++] = '/';
        memmove(output + used, start, n);
        used += n;
    }
    output[used] = 0;
    return strcmp(storage_path_root(output), STORAGE_SD_ROOT) || fold_fat_path(output);
}

static int full_path(struct dfs_filesystem *fs, const char *relative, char path[STORAGE_PATH_MAX])
{
    size_t prefix = fs->ops->flags & DFS_FS_FLAG_FULLPATH ? 0 : strlen(fs->path);
    size_t length = strlen(relative);
    if (prefix >= STORAGE_PATH_MAX || length >= STORAGE_PATH_MAX - prefix) return -1;
    if (prefix) memcpy(path, fs->path, prefix);
    memcpy(path + prefix, relative, length + 1);
    if (!normalize_path(path, path)) return -1;
    return (int)strlen(path);
}
static void changed(struct dfs_filesystem *fs, const char *relative, bool directory, bool renamed)
{
    char path[STORAGE_PATH_MAX];
    int length = full_path(fs, relative, path);
    volume_watch_t *watch = watch_for_fs(fs);
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t revision = next_revision();
    watch->revision = revision;
    if (length < 0 || length >= STORAGE_PATH_MAX || !watch->slots)
    {
        watch->epoch = revision;
        if (renamed && watch->writers) watch->renamed_writer = true;
    }
    else
    {
        for (unsigned i = 0; i < WATCH_SLOTS; ++i)
        {
            change_slot_t *slot = &watch->slots[i];
            if (!strcmp(slot->path, path) ||
                (directory && !strncmp(slot->path, path, length) && slot->path[length] == '/'))
            {
                slot->content = revision;
                if (directory) slot->directory = revision;
                if (renamed && slot->writers) watch->renamed_writer = true;
            }
        }
        if (directory)
        {
            char *slash = strrchr(path, '/');
            if (slash)
            {
                *slash = 0;
                change_slot_t *parent = find_slot(watch, path, false);
                if (parent) parent->directory = revision;
            }
        }
    }
    rt_hw_interrupt_enable(level);
}
static bool writable(struct dfs_fd *fd)
{ return (fd->flags & O_ACCMODE) != O_RDONLY || (fd->flags & O_TRUNC); }
static bool pin_file(struct dfs_fd *fd, bool acquire)
{
    char path[STORAGE_PATH_MAX];
    int length = full_path(fd->fs, fd->path, path);
    volume_watch_t *watch = watch_for_fs(fd->fs);
    rt_base_t level = rt_hw_interrupt_disable();
    change_slot_t *slot = length >= 0 && length < STORAGE_PATH_MAX ?
        find_slot(watch, path, acquire) : NULL;
    if (acquire)
    {
        /* Do not open an untracked handle in an otherwise exact table. A
         * later lookup could allocate a slot with a false zero writer count. */
        if (watch->slots && (!slot || slot->pins == UINT32_MAX))
        {
            rt_hw_interrupt_enable(level);
            return false;
        }
        if (slot) { ++slot->pins; if (writable(fd)) ++slot->writers; }
        if (writable(fd)) ++watch->writers;
    }
    else
    {
        if (slot && slot->pins) --slot->pins;
        if (writable(fd))
        {
            if (slot && slot->writers) --slot->writers;
            if (watch->writers) --watch->writers;
            if (!watch->writers && watch->renamed_writer)
            {
                watch->renamed_writer = false;
                watch->epoch = watch->revision = next_revision();
            }
        }
    }
    rt_hw_interrupt_enable(level);
    return true;
}
#ifdef WATCH_FAT_ALIASES
/* One pair of component sequences represents every mixed LFN/SFN spelling;
 * matching is linear in path length, without expanding path combinations. */
static const char *alias_tail(const alias_route_t *route, const char *path, bool parent)
{
    const char *long_name = route->name, *short_name = route->short_name;
    while (*long_name)
    {
        const char *a = strchr(long_name + 1, '/'), *b = strchr(short_name + 1, '/');
        const char *p = *path ? strchr(path + 1, '/') : NULL;
        if (parent && !a) return path;
        size_t na = a ? (size_t)(a - long_name) : strlen(long_name);
        size_t nb = b ? (size_t)(b - short_name) : strlen(short_name);
        size_t np = p ? (size_t)(p - path) : strlen(path);
        if (!((na == np && !memcmp(long_name, path, na)) ||
              (nb == np && !memcmp(short_name, path, nb)))) return NULL;
        long_name += na; short_name += nb; path += np;
    }
    return path;
}
static void alias_changed(volume_watch_t *watch, const alias_route_t *route, bool directory)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t revision = watch->revision = next_revision();
    if (watch->slots)
        for (unsigned i = 0; i < WATCH_SLOTS; ++i)
        {
            change_slot_t *slot = &watch->slots[i];
            const char *tail = alias_tail(route, slot->path, false);
            if (tail && (!*tail || directory))
            {
                slot->content = revision;
                if (directory) slot->directory = revision;
            }
            tail = directory ? alias_tail(route, slot->path, true) : NULL;
            if (tail && !*tail) slot->directory = revision;
        }
    rt_hw_interrupt_enable(level);
}
static void alias_parent(alias_route_t *route)
{
    *strrchr(route->name, '/') = 0;
    *strrchr(route->short_name, '/') = 0;
}
static void alias_forget(volume_watch_t *watch, const alias_route_t *route)
{
    if (watch->directories)
        for (unsigned i = 0; i < ALIAS_DIRECTORIES; ++i)
            if (alias_tail(route, watch->directories[i].route.name, false))
                watch->directories[i].used = 0;
}
static void alias_remember(volume_watch_t *watch, const alias_route_t *route, bool directory)
{
    if (!watch->directories)
    {
        watch->directories = epd_app_alloc(sizeof(*watch->directories) * ALIAS_DIRECTORIES, EPD_APP_PSRAM);
        if (!watch->directories) return;
        memset(watch->directories, 0, sizeof(*watch->directories) * ALIAS_DIRECTORIES);
    }
    unsigned victim = 0;
    for (unsigned i = 0; i < ALIAS_DIRECTORIES; ++i)
    {
        if (!strcmp(watch->directories[i].route.name, route->name)) { victim = i; break; }
        if (watch->directories[i].used < watch->directories[victim].used) victim = i;
    }
    watch->directories[victim].route = *route;
    watch->directories[victim].used = ++watch->directory_use;
    watch->directories[victim].directory = directory;
}
static bool alias_names(struct dfs_filesystem *fs, const FILINFO *info, char *name, char *short_name)
{
    int a = snprintf(name, STORAGE_PATH_MAX, "%s/%s", fs->path, info->fname);
    int b = snprintf(short_name, STORAGE_PATH_MAX, "%s/%s", fs->path,
                     info->altname[0] ? info->altname : info->fname);
    return a >= 0 && a < STORAGE_PATH_MAX && b >= 0 && b < STORAGE_PATH_MAX &&
           fold_fat_path(name) && fold_fat_path(short_name);
}
/* FatFS f_stat(SFN) may return only the SFN. Enumerate the actual entry so
 * both names, including aliases of ancestor directories, are available. */
static int alias_resolve(struct dfs_filesystem *fs, const char *path,
                         alias_route_t *route, bool missing_leaf, bool *exists)
{
    typedef struct
    {
        FAT_DIR dir;
        FILINFO info;
        char input[STORAGE_PATH_MAX], disk[STORAGE_PATH_MAX];
        char name[STORAGE_PATH_MAX], short_name[STORAGE_PATH_MAX];
    } work_t;
    extern int elm_get_vol(FATFS *fat);
    int volume = elm_get_vol((FATFS *)fs->data), result = -EIO;
    work_t *work = epd_app_alloc(sizeof(*work), EPD_APP_PSRAM);
    if (!work) return -ENOMEM;
    if (volume < 0 || full_path(fs, path, work->input) < 0) goto done;
    strcpy(route->name, fs->path); strcpy(route->short_name, fs->path);
    const char *part = work->input + strlen(fs->path);
    volume_watch_t *watch = watch_for_fs(fs);
    if (watch->directories)
        for (unsigned i = 0; i < ALIAS_DIRECTORIES; ++i)
        {
            alias_directory_t *cached = &watch->directories[i];
            const char *tail = cached->used ? alias_tail(&cached->route, work->input, false) : NULL;
            if (tail && (cached->directory || !*tail) && tail > part)
            { *route = cached->route; part = tail; cached->used = ++watch->directory_use; }
        }
    *exists = true;
    while (*part)
    {
        const char *end = strchr(part + 1, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        int n = snprintf(work->disk, sizeof(work->disk), "%d:%s%.*s", volume,
                         route->name + strlen(fs->path), (int)length, part);
        if (n < 0 || n >= sizeof(work->disk)) goto done;
        FRESULT status = f_stat(work->disk, &work->info);
        bool found = false, valid = true;
        if (status == FR_OK)
        {
            if (!alias_names(fs, &work->info, work->name, work->short_name)) goto done;
            found = strcmp(work->name, work->short_name) != 0;
            if (!found)
            {
                /* An 8.3 result alone cannot distinguish a plain short file
                 * from an external card's arbitrary (not necessarily '~') SFN. */
                n = snprintf(work->disk, sizeof(work->disk), "%d:%s", volume,
                             route->name + strlen(fs->path));
                if (n < 0 || n >= sizeof(work->disk) || f_opendir(&work->dir, work->disk) != FR_OK) goto done;
                for (;;)
                {
                    if (f_readdir(&work->dir, &work->info) != FR_OK) { valid = false; break; }
                    if (!work->info.fname[0]) break;
                    /* Reject incomplete UTF-8 FILINFO names rather than
                     * silently overlooking another spelling of an entry. */
                    if (work->dir.blk_ofs != 0xffffffffu && !work->info.altname[0])
                    { valid = false; break; }
                    if (!alias_names(fs, &work->info, work->name, work->short_name)) { valid = false; break; }
                    const char *a = work->name + strlen(fs->path), *b = work->short_name + strlen(fs->path);
                    if ((strlen(a) == length && !memcmp(a, part, length)) ||
                        (strlen(b) == length && !memcmp(b, part, length))) { found = true; break; }
                }
                if (f_closedir(&work->dir) != FR_OK) valid = false;
                if (!found) valid = false;
            }
        }
        else if (status != FR_NO_FILE) goto done;
        if (!valid) goto done;
        const char *a = work->name + strlen(fs->path), *b = work->short_name + strlen(fs->path);
        if (!found)
        {
            if (end || !missing_leaf) { result = -ENOENT; goto done; }
            a = b = part; *exists = false;
        }
        else if (end && !(work->info.fattrib & AM_DIR)) { result = -ENOTDIR; goto done; }
        if (strlen(route->name) + strlen(a) >= sizeof(route->name) ||
            strlen(route->short_name) + strlen(b) >= sizeof(route->short_name)) goto done;
        strcat(route->name, a); strcat(route->short_name, b);
        if (found) alias_remember(watch, route, (work->info.fattrib & AM_DIR) != 0);
        part += length;
    }
    result = 0;
done:
    epd_app_free(work);
    return result;
}
static alias_change_t *alias_find(volume_watch_t *watch, struct dfs_fd *fd)
{
    for (alias_change_t *change = watch->aliases; change; change = change->next)
        if (change->fd == fd) return change;
    return NULL;
}
static void alias_link(volume_watch_t *watch, alias_change_t *change, bool add)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (add) { change->next = watch->aliases; watch->aliases = change; }
    else
    {
        alias_change_t **at = &watch->aliases;
        while (*at && *at != change) at = &(*at)->next;
        if (*at) *at = change->next;
    }
    rt_hw_interrupt_enable(level);
}
static unsigned alias_writers(volume_watch_t *watch, const char *path)
{
    unsigned writers = 0;
    for (alias_change_t *change = watch->aliases; change; change = change->next)
    {
        const char *tail = alias_tail(&change->route, path, false);
        if (tail && (!*tail || change->subtree)) ++writers;
    }
    return writers;
}
static int alias_open(struct dfs_fd *fd)
{
    volume_watch_t *watch = watch_for_fs(fd->fs);
    alias_change_t *change = epd_app_alloc(sizeof(*change), EPD_APP_PSRAM);
    if (!change) return -ENOMEM;
    memset(change, 0, sizeof(*change)); change->fd = fd;
    storage_lock();
    unsigned count = 0;
    for (alias_change_t *c = watch->aliases; c; c = c->next) ++count;
    if (count >= WATCH_SLOTS) { storage_unlock(); epd_app_free(change); return -ENOMEM; }
    bool exists;
    int result = alias_resolve(fd->fs, fd->path, &change->route, (fd->flags & O_CREAT) != 0, &exists);
    if (result < 0) goto done;
    if (!pin_file(fd, true)) { result = -ENOMEM; goto done; }
    alias_link(watch, change, true);
    alias_changed(watch, &change->route, !exists);
    result = watch->original->fops->open(fd);
    if (result >= 0 && !exists)
    {
        /* Resolve the newly allocated SFN. If media/allocation fails after
         * creation, conservatively track only the affected parent subtree. */
        alias_route_t *resolved = epd_app_alloc(sizeof(*resolved), EPD_APP_PSRAM);
        bool present;
        bool ok = resolved && alias_resolve(fd->fs, fd->path, resolved, false, &present) == 0;
        rt_base_t level = rt_hw_interrupt_disable();
        if (ok) change->route = *resolved;
        else { alias_parent(&change->route); change->subtree = true; }
        rt_hw_interrupt_enable(level);
        epd_app_free(resolved);
    }
    alias_changed(watch, &change->route, !exists || change->subtree);
    if (result < 0) pin_file(fd, false);
    if (result >= 0 && writable(fd)) { storage_unlock(); return result; }
    alias_link(watch, change, false);
done:
    storage_unlock(); epd_app_free(change);
    return result;
}
static int alias_close(struct dfs_fd *fd)
{
    volume_watch_t *watch = watch_for_fs(fd->fs);
    storage_lock();
    alias_change_t *change = alias_find(watch, fd);
    int result = watch->original->fops->close(fd);
    if (result >= 0)
    {
        if (change) { alias_changed(watch, &change->route, change->subtree); alias_link(watch, change, false); }
        pin_file(fd, false);
        epd_app_free(change);
    }
    storage_unlock();
    return result;
}
static int alias_write(struct dfs_fd *fd, const void *data, size_t size, bool truncate, void *argument)
{
    volume_watch_t *watch = watch_for_fs(fd->fs);
    storage_lock();
    alias_change_t *change = alias_find(watch, fd);
    int result = -EIO;
    if (change)
    {
        alias_changed(watch, &change->route, change->subtree);
        result = truncate ? watch->original->fops->ioctl(fd, RT_FIOFTRUNCATE, argument) :
                            watch->original->fops->write(fd, data, size);
    }
    storage_unlock();
    return result;
}
static int alias_namespace(struct dfs_filesystem *fs, const char *from, const char *to)
{
    volume_watch_t *watch = watch_for_fs(fs);
    /* Two pending namespace routes also block new alias readers during I/O. */
    alias_change_t *changes = epd_app_alloc(sizeof(*changes) * 2, EPD_APP_PSRAM);
    alias_route_t *resolved = epd_app_alloc(sizeof(*resolved), EPD_APP_PSRAM);
    if (!changes || !resolved) { epd_app_free(changes); epd_app_free(resolved); return -ENOMEM; }
    memset(changes, 0, sizeof(*changes) * 2);
    changes[0].subtree = changes[1].subtree = true;
    storage_lock();
    bool exists;
    int result = alias_resolve(fs, from, &changes[0].route, false, &exists);
    if (result < 0) goto done;
    if (to && (result = alias_resolve(fs, to, &changes[1].route, true, &exists)) < 0) goto done;
    unsigned count = to ? 2 : 1;
    for (unsigned i = 0; i < count; ++i)
    {
        alias_forget(watch, &changes[i].route);
        alias_link(watch, &changes[i], true); alias_changed(watch, &changes[i].route, true);
    }
    result = to ? watch->original->rename(fs, from, to) : watch->original->unlink(fs, from);
    if (to && result >= 0)
    {
        bool ok = alias_resolve(fs, to, resolved, false, &exists) == 0;
        if (!ok) { *resolved = changes[1].route; alias_parent(resolved); }
        /* Existing descriptors retain their original pin key, while writes
         * follow the new object name. No whole-volume renamed-writer epoch. */
        rt_base_t level = rt_hw_interrupt_disable();
        for (alias_change_t *c = watch->aliases; c; c = c->next)
        {
            if (!c->fd) continue;
            const char *a = alias_tail(&changes[0].route, c->route.name, false);
            const char *b = alias_tail(&changes[0].route, c->route.short_name, false);
            if (!a || !b) continue;
            if (ok && strlen(resolved->name) + strlen(a) < sizeof(c->route.name) &&
                strlen(resolved->short_name) + strlen(b) < sizeof(c->route.short_name))
            {
                size_t n = strlen(resolved->name), m = strlen(resolved->short_name);
                memmove(c->route.name + n, a, strlen(a) + 1); memcpy(c->route.name, resolved->name, n);
                memmove(c->route.short_name + m, b, strlen(b) + 1); memcpy(c->route.short_name, resolved->short_name, m);
            }
            else { c->route = *resolved; c->subtree = true; }
        }
        rt_hw_interrupt_enable(level);
        alias_changed(watch, resolved, true);
    }
    for (unsigned i = 0; i < count; ++i)
    { alias_changed(watch, &changes[i].route, true); alias_link(watch, &changes[i], false); }
done:
    storage_unlock(); epd_app_free(resolved); epd_app_free(changes);
    return result;
}
#endif

static uint32_t path_token(const char *path, bool directory, bool writers)
{
    if (!path || !*path) return 0;
    char normalized[STORAGE_PATH_MAX];
    if (!normalize_path(path, normalized)) return writers ? UINT32_MAX : 0;
    path = normalized;
    volume_watch_t *watch = watch_for_path(path);
    rt_base_t level = rt_hw_interrupt_disable();
    if (watch->resetting)
    {
        rt_hw_interrupt_enable(level);
        return writers ? UINT32_MAX : 0;
    }
    change_slot_t *slot = find_slot(watch, path, !writers);
    uint32_t result;
    if (writers)
    {
#ifdef WATCH_FAT_ALIASES
        if (watch->fat) result = alias_writers(watch, path);
        else
#endif
        result = watch->renamed_writer || !slot ? watch->writers : slot->writers;
    }
    else
    {
        result = slot ? (directory ? slot->directory : slot->content) : watch->revision;
        if (result < watch->epoch) result = watch->epoch;
    }
    rt_hw_interrupt_enable(level);
    return result;
}
uint32_t storage_path_revision(const char *path) { return path_token(path, false, false); }
uint32_t storage_directory_revision(const char *path) { return path_token(path, true, false); }
uint32_t storage_file_writers(const char *path) { return path_token(path, false, true); }

bool storage_path_pin(const char *path)
{
    char normalized[STORAGE_PATH_MAX];
    if (!normalize_path(path, normalized)) return false;
    volume_watch_t *watch = watch_for_path(normalized);
    rt_base_t level = rt_hw_interrupt_disable();
    change_slot_t *slot = find_slot(watch, normalized, true);
    bool ok = slot && slot->pins != UINT32_MAX;
    if (ok) ++slot->pins;
    rt_hw_interrupt_enable(level);
    return ok;
}

void storage_path_unpin(const char *path)
{
    char normalized[STORAGE_PATH_MAX];
    if (!normalize_path(path, normalized)) return;
    volume_watch_t *watch = watch_for_path(normalized);
    rt_base_t level = rt_hw_interrupt_disable();
    change_slot_t *slot = find_slot(watch, normalized, false);
    if (slot && slot->pins) --slot->pins;
    rt_hw_interrupt_enable(level);
}
uint32_t storage_volume_revision(storage_volume_t volume)
{
    if ((unsigned)volume >= STORAGE_COUNT) return 0;
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t result = watches[volume].revision;
    rt_hw_interrupt_enable(level);
    return result;
}
static int watched_open(struct dfs_fd *fd)
{
    volume_watch_t *watch = watch_for_fs(fd->fs);
#ifdef WATCH_FAT_ALIASES
    if (watch->fat && (writable(fd) || (fd->flags & O_CREAT))) return alias_open(fd);
#endif
    if (!pin_file(fd, true)) return -ENOMEM;
    if (writable(fd) || (fd->flags & O_CREAT))
        changed(fd->fs, fd->path, (fd->flags & O_CREAT) != 0, false);
    int result = watch->original->fops->open(fd);
    if (writable(fd) || (fd->flags & O_CREAT))
        changed(fd->fs, fd->path, (fd->flags & O_CREAT) != 0, false);
    if (result < 0) pin_file(fd, false);
    return result;
}
static int watched_close(struct dfs_fd *fd)
{
#ifdef WATCH_FAT_ALIASES
    if (watch_for_fs(fd->fs)->fat) return alias_close(fd);
#endif
    int result = watch_for_fs(fd->fs)->original->fops->close(fd);
    /* DFS retains the descriptor/path on close failure; FatFS also retains
     * its FIL. Keep the pin/writer until a successful retry retires the fd. */
    if (result >= 0)
    {
        if (writable(fd)) changed(fd->fs, fd->path, false, false);
        pin_file(fd, false);
    }
    return result;
}
static int watched_write(struct dfs_fd *fd, const void *data, size_t size)
{
#ifdef WATCH_FAT_ALIASES
    if (watch_for_fs(fd->fs)->fat) return alias_write(fd, data, size, false, NULL);
#endif
    changed(fd->fs, fd->path, false, false);
    return watch_for_fs(fd->fs)->original->fops->write(fd, data, size);
}
static int watched_ioctl(struct dfs_fd *fd, int command, void *argument)
{
#ifdef WATCH_FAT_ALIASES
    if (watch_for_fs(fd->fs)->fat && (unsigned)command == RT_FIOFTRUNCATE)
        return alias_write(fd, NULL, 0, true, argument);
#endif
    if ((unsigned)command == RT_FIOFTRUNCATE) changed(fd->fs, fd->path, false, false);
    return watch_for_fs(fd->fs)->original->fops->ioctl(fd, command, argument);
}
static int watched_unlink(struct dfs_filesystem *fs, const char *path)
{
#ifdef WATCH_FAT_ALIASES
    if (watch_for_fs(fs)->fat) return alias_namespace(fs, path, NULL);
#endif
    changed(fs, path, true, false);
    int result = watch_for_fs(fs)->original->unlink(fs, path);
    changed(fs, path, true, false);
    return result;
}
static int watched_rename(struct dfs_filesystem *fs, const char *from, const char *to)
{
    volume_watch_t *watch = watch_for_fs(fs);
#ifdef WATCH_FAT_ALIASES
    if (watch->fat) return alias_namespace(fs, from, to);
#endif
    changed(fs, from, true, true);
    changed(fs, to, true, true);
    int result = watch->original->rename(fs, from, to);
    changed(fs, from, true, false);
    changed(fs, to, true, false);
    return result;
}
void storage_watch_mount(storage_volume_t volume)
{
    if ((unsigned)volume >= STORAGE_COUNT) return;
    struct dfs_filesystem *fs = dfs_filesystem_lookup(storage_root(volume));
    if (!fs || !fs->path || strcmp(fs->path, storage_root(volume)) || !fs->ops->fops) return;
    volume_watch_t *watch = &watches[volume];
    if (fs->ops == &watch->ops) return;
    rt_base_t level = rt_hw_interrupt_disable();
    watch->resetting = true;
    rt_hw_interrupt_enable(level);
    if (!watch->slots) watch->slots = epd_app_alloc(sizeof(change_slot_t) * WATCH_SLOTS, EPD_APP_PSRAM);
    watch->original = fs->ops;
#ifdef WATCH_FAT_ALIASES
    while (watch->aliases)
    {
        alias_change_t *next = watch->aliases->next;
        epd_app_free(watch->aliases); watch->aliases = next;
    }
    watch->fat = !strcmp(fs->path, STORAGE_SD_ROOT) && !strcmp(fs->ops->name, "elm");
    if (watch->directories) memset(watch->directories, 0, sizeof(*watch->directories) * ALIAS_DIRECTORIES);
    watch->directory_use = 0;
#endif
    watch->ops = *fs->ops;
    watch->files = *fs->ops->fops;
    if (watch->files.open) watch->files.open = watched_open;
    if (watch->files.close) watch->files.close = watched_close;
    if (watch->files.write) watch->files.write = watched_write;
    if (watch->files.ioctl) watch->files.ioctl = watched_ioctl;
    if (watch->ops.unlink) watch->ops.unlink = watched_unlink;
    if (watch->ops.rename) watch->ops.rename = watched_rename;
    watch->ops.fops = &watch->files;
    if (watch->slots) memset(watch->slots, 0, sizeof(change_slot_t) * WATCH_SLOTS);
    level = rt_hw_interrupt_disable();
    watch->writers = 0;
    watch->renamed_writer = false;
    watch->epoch = watch->revision = next_revision();
    watch->resetting = false;
    fs->ops = &watch->ops;
    rt_hw_interrupt_enable(level);
}
RTM_EXPORT(storage_path_revision);
RTM_EXPORT(storage_directory_revision);
RTM_EXPORT(storage_volume_revision);
RTM_EXPORT(storage_file_writers);
RTM_EXPORT(storage_path_pin);
RTM_EXPORT(storage_path_unpin);
