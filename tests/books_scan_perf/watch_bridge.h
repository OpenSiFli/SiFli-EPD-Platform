/* Minimal DFS backend for the production mutation registry. */
#define DFS_FS_FLAG_FULLPATH 1
#define RT_FIOFTRUNCATE 0x52540000u
#define RTM_EXPORT(symbol)
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif
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
static int raw_open(struct dfs_fd *fd) { (void)fd; return 0; }
static int raw_close(struct dfs_fd *fd) { (void)fd; return 0; }
static int raw_ioctl(struct dfs_fd *fd, int cmd, void *arg) { (void)fd; (void)cmd; (void)arg; return 0; }
static int raw_write(struct dfs_fd *fd, const void *data, size_t size) { (void)fd; (void)data; return (int)size; }
static int raw_unlink(struct dfs_filesystem *fs, const char *path) { (void)fs; (void)path; return 0; }
static int raw_rename(struct dfs_filesystem *fs, const char *from, const char *to) { (void)fs; (void)from; (void)to; return 0; }
#define full_path watch_full_path
#include "watch.inc"
#undef full_path
static const struct dfs_file_ops raw_files = {raw_open, raw_close, raw_ioctl, raw_write};
static const struct dfs_filesystem_ops raw_ops = {DFS_FS_FLAG_FULLPATH, &raw_files, raw_unlink, raw_rename};
static void host_watch_init(void)
{
    for (storage_volume_t v = STORAGE_FLASH; v < STORAGE_COUNT; ++v) {
        volumes[v].path = storage_root(v); volumes[v].ops = &raw_ops; storage_watch_mount(v);
    }
}
static void host_watch_changed(const char *path, bool directory)
{ changed(dfs_filesystem_lookup(storage_path_root(path)), path, directory); }
static void host_watch_pin(const char *path, bool acquire)
{
    struct dfs_fd fd = {dfs_filesystem_lookup(storage_path_root(path)), path, O_RDONLY};
    CHECK(acquire ? watched_open(&fd) == 0 : watched_close(&fd) == 0);
}
