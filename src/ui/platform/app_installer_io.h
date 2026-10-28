/* Short filesystem operations serialize with storage handoff. The installer
 * owns one transaction at a time; its session stays fixed through cleanup. */
static uint32_t installer_session;
static bool installer_io_active;

static bool installer_io_ready(void)
{
    return !storage_release_requested() && !storage_changing() &&
           (!installer_io_active || installer_session == storage_card_session());
}

#define INSTALLER_IO(type, name, args, call, failure) \
    static type installer_##name args \
    { \
        storage_lock(); \
        type value; \
        if (installer_io_ready()) value = call; \
        else { errno = -EIO; value = failure; } \
        storage_unlock(); \
        return value; \
    }
INSTALLER_IO(int, open, (const char *path, int flags, ...), open(path, flags, 0), -1)
INSTALLER_IO(int, stat, (const char *path, struct stat *st), stat(path, st), -1)
INSTALLER_IO(int, fstat, (int fd, struct stat *st), fstat(fd, st), -1)
INSTALLER_IO(int, read, (int fd, void *buffer, size_t size), read(fd, buffer, size), -1)
INSTALLER_IO(int, write, (int fd, const void *buffer, size_t size), write(fd, buffer, size), -1)
INSTALLER_IO(int, fsync, (int fd), fsync(fd), -1)
INSTALLER_IO(int, mkdir, (const char *path, mode_t mode), mkdir(path, mode), -1)
INSTALLER_IO(int, rename, (const char *from, const char *to), rename(from, to), -1)
INSTALLER_IO(int, unlink, (const char *path), unlink(path), -1)
INSTALLER_IO(int, rmdir, (const char *path), rmdir(path), -1)
INSTALLER_IO(DIR *, opendir, (const char *path), opendir(path), NULL)
INSTALLER_IO(struct dirent *, readdir, (DIR *dir), readdir(dir), NULL)
INSTALLER_IO(bool, mkdirs, (const char *path), storage_mkdirs(path), false)
INSTALLER_IO(int, statfs, (const char *path, struct statfs *st), dfs_statfs(path, st), -1)
#undef INSTALLER_IO

/* Retire handles even after a session has ended. */
static int installer_close(int fd)
{
    storage_lock();
    int value = close(fd);
    storage_unlock();
    return value;
}
static int installer_closedir(DIR *dir)
{
    storage_lock();
    int value = closedir(dir);
    storage_unlock();
    return value;
}
