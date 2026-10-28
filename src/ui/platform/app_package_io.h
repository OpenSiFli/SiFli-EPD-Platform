/* Metadata parsing and hashing run outside these individual I/O sections. */
#define PACKAGE_IO(type, name, args, call) \
    static type package_io_##name args \
    { storage_lock(); type value = call; storage_unlock(); return value; }
PACKAGE_IO(int, open, (const char *path, int flags), open(path, flags))
PACKAGE_IO(int, close, (int fd), close(fd))
PACKAGE_IO(int, read, (int fd, void *buffer, size_t size), read(fd, buffer, size))
PACKAGE_IO(int, stat, (const char *path, struct stat *st), stat(path, st))
PACKAGE_IO(int, fstat, (int fd, struct stat *st), fstat(fd, st))
PACKAGE_IO(off_t, lseek, (int fd, off_t offset, int origin), lseek(fd, offset, origin))
PACKAGE_IO(FILE *, fopen, (const char *path, const char *mode), fopen(path, mode))
PACKAGE_IO(char *, fgets, (char *line, int size, FILE *file), fgets(line, size, file))
PACKAGE_IO(int, fclose, (FILE *file), fclose(file))
PACKAGE_IO(DIR *, opendir, (const char *path), opendir(path))
PACKAGE_IO(struct dirent *, readdir, (DIR *dir), readdir(dir))
PACKAGE_IO(int, closedir, (DIR *dir), closedir(dir))
#undef PACKAGE_IO
