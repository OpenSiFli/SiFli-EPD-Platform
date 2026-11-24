#include "host.h"
#undef open
#undef close
#undef write
#undef unlink
#undef rename
#define STORAGE_PATH_MAX 512
#define DFS_FS_FLAG_FULLPATH 1
#define RT_FIOFTRUNCATE 0x52540000u
#define O_ACCMODE 1
#define O_TRUNC 8
#define RTM_EXPORT(symbol)
struct dfs_fd;
struct dfs_filesystem;
struct dfs_file_ops {
    int (*open)(struct dfs_fd *);
    int (*close)(struct dfs_fd *);
    int (*ioctl)(struct dfs_fd *, int, void *);
    int (*write)(struct dfs_fd *, const void *, size_t);
};
struct dfs_filesystem_ops {
    uint32_t flags;
    const struct dfs_file_ops *fops;
    int (*unlink)(struct dfs_filesystem *, const char *);
    int (*rename)(struct dfs_filesystem *, const char *, const char *);
};
struct dfs_filesystem { const char *path; const struct dfs_filesystem_ops *ops; };
struct dfs_fd { struct dfs_filesystem *fs; const char *path; unsigned flags; };
static struct dfs_filesystem volumes[2];
static struct dfs_filesystem *dfs_filesystem_lookup(const char *path) { return &volumes[!strcmp(path, STORAGE_SD_ROOT)]; }
static char *dfs_normalize_path(const char *base, const char *path) { (void)base; return _strdup(path); }
static int raw_open(struct dfs_fd *fd) { (void)fd; return 0; }
static int raw_close(struct dfs_fd *fd) { (void)fd; return 0; }
static int raw_ioctl(struct dfs_fd *fd, int cmd, void *arg) { (void)fd; (void)cmd; (void)arg; return 0; }
static int raw_write(struct dfs_fd *fd, const void *data, size_t size) { (void)fd; (void)data; return (int)size; }
static int raw_unlink(struct dfs_filesystem *fs, const char *path) { (void)fs; (void)path; return 0; }
static int raw_rename(struct dfs_filesystem *fs, const char *from, const char *to) { (void)fs; (void)from; (void)to; return 0; }
enum { EPD_SERVICE_UI_FILES = 1u << 11 };
static void app_service_ui_signal(uint32_t events) { assert(events == EPD_SERVICE_UI_FILES); }
#include "watch_actual.inc"

void host_watch_reset(void)
{
    static const struct dfs_file_ops fops = {raw_open, raw_close, raw_ioctl, raw_write};
    static const struct dfs_filesystem_ops ops = {DFS_FS_FLAG_FULLPATH, &fops, raw_unlink, raw_rename};
    for (storage_volume_t v = STORAGE_FLASH; v < STORAGE_COUNT; ++v) {
        volumes[v].path = storage_root(v); volumes[v].ops = &ops;
        storage_watch_mount(v);
    }
}
void host_watch_remount(storage_volume_t volume)
{
    volumes[volume].ops = watches[volume].original;
    storage_watch_mount(volume);
}
static struct dfs_filesystem *volume_for(const char *path) { return dfs_filesystem_lookup(storage_path_root(path)); }
void host_watch_changed(const char *path, bool directory) { changed(volume_for(path), path, directory, false); }
void host_watch_pin(const char *path, int flags, bool acquire)
{
    struct dfs_fd fd = {volume_for(path), path, flags};
    if (acquire) assert(watched_open(&fd) == 0);
    else assert(watched_close(&fd) == 0);
}
void host_watch_rename(const char *from, const char *to) { assert(watched_rename(volume_for(from), from, to) == 0); }
unsigned host_watch_pin_count(void)
{
    unsigned count = 0;
    for (unsigned v = 0; v < STORAGE_COUNT; ++v)
        if (watches[v].slots)
            for (unsigned i = 0; i < WATCH_SLOTS; ++i) count += watches[v].slots[i].pins;
    return count;
}
