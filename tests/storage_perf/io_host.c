#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define STORAGE_PATH_MAX 512
#define STORAGE_SD_ROOT "/sdcard"
#define STORAGE_FLASH_ROOT "/flash"
#define STORAGE_SD_APPS "/sdcard/.epd/apps"
#define STORAGE_FLASH_APPS "/flash/apps"
#define DFS_FS_FLAG_FULLPATH 1
#define RT_FIOFTRUNCATE 0x52540000u
#define RT_NAME_MAX 32
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x200000
#endif
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define RT_USING_DFS_ELMFAT
#define FF_USE_LFN 1
#define FF_LFN_UNICODE 2
#include "fat_upper_actual.inc"
#define RTM_EXPORT(x)
#define rt_snprintf snprintf
#define rt_kprintf printf
typedef int rt_base_t;
typedef enum { STORAGE_FLASH, STORAGE_SD, STORAGE_COUNT } storage_volume_t;
typedef enum { STORAGE_APP_CODE, STORAGE_APP_DATA, STORAGE_APP_CACHE } storage_app_area_t;
typedef enum { STORAGE_REPLACE_UNAVAILABLE, STORAGE_REPLACE_ATOMIC, STORAGE_REPLACE_BACKUP } storage_replace_mode_t;
typedef enum { EPD_APP_SRAM, EPD_APP_PSRAM } epd_app_memory_t;
typedef bool (*storage_signature_update_t)(void *, const void *, size_t, uint32_t *);
bool storage_file_signature_with(const char *, uint32_t *, storage_signature_update_t, void *);
struct dfs_fd;
struct dfs_filesystem;
struct dfs_file_ops {
    int (*open)(struct dfs_fd *);
    int (*close)(struct dfs_fd *);
    int (*ioctl)(struct dfs_fd *, int, void *);
    int (*write)(struct dfs_fd *, const void *, size_t);
};
struct dfs_filesystem_ops {
    const char *name; uint32_t flags;
    const struct dfs_file_ops *fops;
    int (*unlink)(struct dfs_filesystem *, const char *);
    int (*rename)(struct dfs_filesystem *, const char *, const char *);
};
struct dfs_filesystem { const char *path; const struct dfs_filesystem_ops *ops; };
struct dfs_fd {
    struct dfs_filesystem *fs;
    const struct dfs_file_ops *fops;
    const char *path;
    unsigned flags;
    int index;
    size_t position;
    bool used, backend_freed;
    char full[STORAGE_PATH_MAX];
};
#define FILE_LIMIT 128
#define DATA_LIMIT 20000
static struct { char path[STORAGE_PATH_MAX]; uint8_t data[DATA_LIMIT]; size_t size; bool used, directory; } entries[FILE_LIMIT];
static struct dfs_fd handles[16];
static struct dfs_filesystem volumes[STORAGE_COUNT];
static bool mounted[STORAGE_COUNT] = {true,true}, allow_write = true;
static uint32_t session = 1;
static unsigned depth, locks, segment_reads, segment_writes, max_reads, max_writes;
static unsigned read_calls, write_calls, close_calls, yields, alloc_calls, cache_allocations;
static unsigned fail_read, fail_write, fail_close, short_write;
static bool fail_buffer_alloc, fail_cache_alloc, fail_stat, fail_sync, fail_rename;
static bool fail_close_always, lfs_consumes_failed_close;
static const char *fail_close_path;
static size_t live_bytes, largest_allocation, baseline_bytes;
static void (*yield_hook)(void);
static void (*stat_hook)(void);
static const char *storage_root(storage_volume_t v) { return v == STORAGE_SD ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static const char *storage_path_root(const char *p)
{ return !strncmp(p,"/sdcard/",8) || !strcmp(p,"/sdcard") ? STORAGE_SD_ROOT : STORAGE_FLASH_ROOT; }
static struct dfs_filesystem *dfs_filesystem_lookup(const char *p) { return &volumes[!strcmp(storage_path_root(p), STORAGE_SD_ROOT)]; }
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t n) { (void)n; }
static bool storage_available(storage_volume_t v) { return mounted[v]; }
static bool storage_path_available(const char *p) { return storage_available(!strcmp(storage_path_root(p),STORAGE_SD_ROOT)); }
static uint32_t storage_card_session(void) { return session; }
static bool storage_session_valid(storage_volume_t v, uint32_t s) { return mounted[v] && (v != STORAGE_SD || s == session); }
static bool system_reset_write_allowed(void) { return allow_write; }
static storage_replace_mode_t storage_replace_mode(storage_volume_t v)
{ return !mounted[v] ? STORAGE_REPLACE_UNAVAILABLE : v == STORAGE_SD ? STORAGE_REPLACE_BACKUP : STORAGE_REPLACE_ATOMIC; }
static bool app_module_storage(const char *id, storage_volume_t *v, uint32_t *s) { (void)id;(void)v;(void)s; return false; }
static void storage_lock(void)
{ if(!depth++) { ++locks; segment_reads = segment_writes = 0; } }
static void storage_unlock(void)
{
    CHECK(depth);
    if(!--depth) {
        if(segment_reads > max_reads) max_reads = segment_reads;
        if(segment_writes > max_writes) max_writes = segment_writes;
    }
}
static void rt_thread_yield(void)
{ CHECK(!depth); ++yields; if(yield_hook) yield_hook(); }
static void *epd_app_alloc(size_t n, epd_app_memory_t region)
{
    (void)region; ++alloc_calls;
    if((n == 4096 && fail_buffer_alloc) || (n > 4096 && fail_cache_alloc)) return NULL;
    size_t *p = malloc(n + sizeof(*p)); CHECK(p); *p = n; live_bytes += n;
    if(n > largest_allocation) largest_allocation = n;
    if(n > 4096) ++cache_allocations;
    return p + 1;
}
static void epd_app_free(void *data)
{ if(data) { size_t *p = (size_t *)data - 1; CHECK(live_bytes >= *p); live_bytes -= *p; free(p); } }
static int lookup(const char *path)
{
    bool fat = !strcmp(storage_path_root(path), STORAGE_SD_ROOT);
    for(unsigned i = 0; i < FILE_LIMIT; ++i)
        if(entries[i].used && !(fat ? _stricmp(entries[i].path,path) : strcmp(entries[i].path,path))) return i;
    return -1;
}
static int seed(const char *path, size_t size, unsigned byte)
{
    int i = lookup(path);
    if(i < 0) for(i = 0; i < FILE_LIMIT && entries[i].used; ++i) {}
    CHECK(i < FILE_LIMIT && size <= DATA_LIMIT);
    entries[i].used = true; entries[i].directory = false; entries[i].size = size;
    strcpy(entries[i].path,path); memset(entries[i].data,byte,size); return i;
}
static int backend_open(struct dfs_fd *fd)
{
    int i = lookup(fd->full);
    if(i >= 0 && (fd->flags & O_EXCL)) return -1;
    if(i < 0 && (fd->flags & O_CREAT)) i = seed(fd->full,0,0);
    if(i < 0) { errno = -ENOENT; return -1; }
    if(fd->flags & O_DIRECTORY) entries[i].directory = true;
    if(fd->flags & O_TRUNC) entries[i].size = 0;
    fd->index = i; fd->position = 0; return 0;
}
static int backend_close(struct dfs_fd *fd)
{
    CHECK(!fd->backend_freed);
    ++close_calls;
    bool fail = (fail_close && close_calls == fail_close) ||
                (fail_close_always && (!fail_close_path || !strcmp(fail_close_path,fd->full)));
    if(fail && lfs_consumes_failed_close && !strcmp(fd->fs->ops->name,"lfs")) fd->backend_freed = true;
    return fail ? -1 : 0;
}
static int backend_write(struct dfs_fd *fd, const void *data, size_t n)
{
    ++write_calls;
    if(fail_write && write_calls == fail_write) return -1;
    if(short_write && n > short_write) n = short_write;
    CHECK(fd->position + n <= DATA_LIMIT);
    memcpy(entries[fd->index].data + fd->position,data,n); fd->position += n;
    if(fd->position > entries[fd->index].size) entries[fd->index].size = fd->position;
    segment_writes += (unsigned)n;
    return (int)n;
}
static int backend_ioctl(struct dfs_fd *fd, int command, void *arg)
{ if((unsigned)command == RT_FIOFTRUNCATE) entries[fd->index].size = *(unsigned *)arg; return 0; }
static int backend_unlink(struct dfs_filesystem *fs, const char *relative)
{
    char path[STORAGE_PATH_MAX]; snprintf(path,sizeof(path),"%s%s",fs->path,relative);
    int i = lookup(path); if(i < 0) { errno = -ENOENT; return -1; }
    for(unsigned h = 0; h < 16; ++h) if(handles[h].used && handles[h].index == i) return -1;
    entries[i].used = false; return 0;
}
static int backend_rename(struct dfs_filesystem *fs, const char *from, const char *to)
{
    if(fail_rename) return -1;
    char a[STORAGE_PATH_MAX],b[STORAGE_PATH_MAX];
    snprintf(a,sizeof(a),"%s%s",fs->path,from); snprintf(b,sizeof(b),"%s%s",fs->path,to);
    int i = lookup(a), j = lookup(b); if(i < 0) return -1;
    if(j >= 0) { if(fs == &volumes[STORAGE_SD]) return -1; entries[j].used = false; }
    strcpy(entries[i].path,b); return 0;
}
enum { EPD_SERVICE_UI_FILES = 1u };
static void app_service_ui_signal(uint32_t mask) { (void)mask; }
#include "storage_watch_actual.inc"

static int io_open(const char *path, int flags, ...)
{
    if(!storage_path_available(path)) return -1;
    for(unsigned i = 0; i < 16; ++i) if(!handles[i].used) {
        struct dfs_fd *fd = &handles[i]; memset(fd,0,sizeof(*fd));
        strcpy(fd->full,path); fd->fs = dfs_filesystem_lookup(path);
        fd->path = fd->full + strlen(fd->fs->path); fd->flags = flags; fd->fops = fd->fs->ops->fops;
        if(fd->fops->open(fd) < 0) return -1;
        fd->used = true; return i;
    }
    return -1;
}
static int io_close(int i)
{ CHECK(i >= 0 && handles[i].used); int result = handles[i].fops->close(&handles[i]); if(!result) handles[i].used = false; return result; }
static int io_read(int i, void *out, size_t n)
{
    CHECK(depth && handles[i].used); ++read_calls;
    if(fail_read && read_calls == fail_read) return -1;
    struct dfs_fd *fd = &handles[i];
    if(!storage_path_available(fd->full)) return -1;
    if(n > entries[fd->index].size - fd->position) n = entries[fd->index].size - fd->position;
    memcpy(out,entries[fd->index].data + fd->position,n); fd->position += n;
    segment_reads += (unsigned)n; return (int)n;
}
static int io_write(int i, const void *data, size_t n)
{ CHECK(handles[i].used); return handles[i].fops->write(&handles[i],data,n); }
static int io_stat(const char *p, struct stat *out)
{
    if(stat_hook) stat_hook();
    int i = lookup(p); if(i < 0 || fail_stat) { errno = -ENOENT; return -1; }
    memset(out,0,sizeof(*out)); out->st_mode = entries[i].directory ? _S_IFDIR : _S_IFREG;
    out->st_size = (off_t)entries[i].size; out->st_mtime = 0; return 0;
}
static int io_fstat(int i, struct stat *out) { return io_stat(entries[handles[i].index].path,out); }
static int io_unlink(const char *p)
{ struct dfs_filesystem *fs = dfs_filesystem_lookup(p); return fs->ops->unlink(fs,p+strlen(fs->path)); }
static int io_rename(const char *a, const char *b)
{ struct dfs_filesystem *fs = dfs_filesystem_lookup(a); return fs->ops->rename(fs,a+strlen(fs->path),b+strlen(fs->path)); }
static int io_mkdir(const char *p, int mode)
{ (void)mode; int fd = io_open(p,O_CREAT|O_DIRECTORY); return fd >= 0 ? io_close(fd) : -1; }
static int io_fsync(int fd) { (void)fd; return fail_sync ? -1 : 0; }
#define open io_open
#define close io_close
#define read io_read
#define write io_write
#define stat(...) io_stat(__VA_ARGS__)
#define fstat io_fstat
#define unlink io_unlink
#define rename io_rename
#define mkdir io_mkdir
#define fsync io_fsync
bool storage_file_recover(const char *);
bool storage_file_commit(const char *, const char *);
#define RT_EOK 0
#include "../cold_static_psram/host_sections.h"
#include "storage_file_actual.inc"

#define PKG_USING_EPD_SDCARD
#define RT_EINVAL 10
#define RT_EBUSY 7
typedef int rt_err_t;
static uint32_t release_request;
static unsigned release_acks;
static uint32_t storage_release_request_id(void) { return release_request; }
static rt_err_t sdcard_release_complete(uint32_t id)
{
    CHECK(id && id == release_request);
    for(unsigned i = 0; i < 16; ++i) CHECK(!handles[i].used || handles[i].fs != &volumes[STORAGE_SD]);
    ++release_acks;
    return RT_EOK;
}
#include "storage_release_actual.inc"

static unsigned open_handles(void) { unsigned n = 0; for(unsigned i = 0; i < 16; ++i) n += handles[i].used; return n; }
static uint32_t expected_hash(int i)
{ uint32_t h = 2166136261u; for(size_t j = 0; j < entries[i].size; ++j) h = (h ^ entries[i].data[j]) * 16777619u; return h; }
static void overwrite(const char *path, unsigned char value)
{
    int fd = io_open(path,O_WRONLY); CHECK(fd >= 0);
    unsigned char data[DATA_LIMIT]; size_t n = entries[handles[fd].index].size; memset(data,value,n);
    CHECK(io_write(fd,data,n) == (int)n); CHECK(!io_close(fd));
}
static void setup(void)
{
    CHECK(host_init_storage_owned_files_init() == RT_EOK);
    static const struct dfs_file_ops files = {backend_open,backend_close,backend_ioctl,backend_write};
    static const struct dfs_filesystem_ops lfs = {"lfs",0,&files,backend_unlink,backend_rename};
    static const struct dfs_filesystem_ops fat = {"elm",0,&files,backend_unlink,backend_rename};
    volumes[0] = (struct dfs_filesystem){"/flash",&lfs}; volumes[1] = (struct dfs_filesystem){"/sdcard",&fat};
    storage_watch_mount(STORAGE_FLASH); storage_watch_mount(STORAGE_SD);
    const char *dirs[] = {"/flash","/sdcard","/flash/books","/sdcard/books"};
    for(unsigned j = 0; j < 4; ++j) entries[seed(dirs[j],0,0)].directory = true;
    seed("/flash/books/source.bin",16507,17); seed("/sdcard/books/source.bin",16507,31);
    seed("/flash/books/dest.bin",7,66); seed("/sdcard/books/dest.bin",7,66);
    baseline_bytes = live_bytes; cache_allocations = 0; largest_allocation = 0;
}
static void signature_hit_and_rewrite(void)
{
    const char *p = "/flash/books/source.bin"; uint32_t a,b;
    CHECK(storage_file_signature(p,&a) && a == expected_hash(lookup(p)));
    unsigned reads = read_calls; CHECK(reads > 1 && max_reads <= 4096);
    CHECK(storage_file_signature(p,&b) && b == a && reads == read_calls);
    overwrite(p,21);
    CHECK(storage_file_signature(p,&b) && b != a && b == expected_hash(lookup(p)));
    CHECK(read_calls > reads && !open_handles());
}

static bool delegated_hash(void *user, const void *data, size_t size, uint32_t *hash)
{
    CHECK(depth == 0 && size <= 4096);
    unsigned *calls = user;
    if (++*calls == UINT32_MAX) return false;
    const uint8_t *p = data;
    while (size--) *hash = (*hash ^ *p++) * 16777619u;
    return true;
}
static void signature_delegation(void)
{
    const char *p = "/flash/books/source.bin";
    unsigned calls = 0; uint32_t value = 42;
    CHECK(storage_file_signature_with(p, &value, delegated_hash, &calls));
    CHECK(calls > 1 && value == expected_hash(lookup(p)) && !open_handles());
    calls = 0;
    CHECK(storage_file_signature_with(p, &value, delegated_hash, &calls) && !calls);
    overwrite(p, 29); calls = UINT32_MAX - 1; value = 42;
    CHECK(!storage_file_signature_with(p, &value, delegated_hash, &calls));
    CHECK(value == 42 && !open_handles());
    calls = 0;
    CHECK(storage_file_signature_with(p, &value, delegated_hash, &calls) && calls > 1);
    CHECK(value == expected_hash(lookup(p)));
}
static void signature_cache_bound(void)
{
    char p[80]; uint32_t hash;
    for(unsigned i = 0; i < 70; ++i) {
        snprintf(p,sizeof(p),"/flash/books/cache%u",i); seed(p,3,i);
        CHECK(storage_file_signature(p,&hash));
    }
    CHECK(cache_allocations == 1 && live_bytes - baseline_bytes == largest_allocation && largest_allocation < 40u * 1024u);
    unsigned reads = read_calls; CHECK(storage_file_signature(p,&hash) && reads == read_calls);
    CHECK(storage_file_signature("/flash/books/cache0",&hash) && read_calls > reads);
    printf("signature cache allocation: %zu bytes\n",live_bytes - baseline_bytes);
}
static void signature_allocation_failures(void)
{
    uint32_t hash = 42; fail_buffer_alloc = true;
    CHECK(!storage_file_signature("/flash/books/source.bin",&hash) && hash == 42 && !open_handles() && live_bytes == baseline_bytes);
    fail_buffer_alloc = false; fail_cache_alloc = true;
    CHECK(storage_file_signature("/flash/books/source.bin",&hash)); unsigned reads = read_calls;
    CHECK(storage_file_signature("/flash/books/source.bin",&hash) && read_calls > reads && live_bytes == baseline_bytes);
    fail_cache_alloc = false; CHECK(storage_file_signature("/flash/books/source.bin",&hash));
}
static void signature_read_failure(void)
{
    uint32_t hash = 42; fail_read = 2;
    CHECK(!storage_file_signature("/flash/books/source.bin",&hash) && hash == 42 && !open_handles());
    fail_read = 0; CHECK(storage_file_signature("/flash/books/source.bin",&hash));
}
static void remove_card(void) { mounted[STORAGE_SD] = false; ++session; yield_hook = NULL; }
static void signature_removal(void)
{
    uint32_t hash = 42; yield_hook = remove_card;
    CHECK(!storage_file_signature("/sdcard/books/source.bin",&hash) && hash == 42 && !open_handles());
    mounted[STORAGE_SD] = true;
    CHECK(storage_file_signature("/sdcard/books/source.bin",&hash)); unsigned reads = read_calls;
    ++session; CHECK(storage_file_signature("/sdcard/books/source.bin",&hash) && read_calls > reads);
}
static void change_source(void) { overwrite("/flash/books/source.bin",9); yield_hook = NULL; }
static void signature_mutation_during_read(void)
{
    uint32_t hash = 42; yield_hook = change_source;
    CHECK(!storage_file_signature("/flash/books/source.bin",&hash) && hash == 42 && !open_handles());
    CHECK(storage_file_signature("/flash/books/source.bin",&hash));
}
static void signature_case_alias(void)
{
    uint32_t a,b; CHECK(storage_file_signature("/sdcard/books/source.bin",&a));
    overwrite("/sdcard/BOOKS/SOURCE.BIN",21);
    CHECK(storage_file_signature("/sdcard/books/source.bin",&b) && b != a);
}
static void mutate_in_stat(void) { stat_hook = NULL; overwrite("/flash/books/source.bin",21); }
static void signature_hit_stat_race(void)
{
    uint32_t a,b = 42; CHECK(storage_file_signature("/flash/books/source.bin",&a));
    stat_hook = mutate_in_stat;
    bool ok = storage_file_signature("/flash/books/source.bin",&b);
    CHECK(!ok || b == expected_hash(lookup("/flash/books/source.bin")));
}
static void import_segmented(void)
{
    short_write = 511;
    CHECK(storage_file_import("/sdcard/books/source.bin","/flash/books/dest.bin"));
    int a = lookup("/sdcard/books/source.bin"), b = lookup("/flash/books/dest.bin");
    CHECK(entries[a].size == entries[b].size && !memcmp(entries[a].data,entries[b].data,entries[a].size));
    CHECK(max_reads <= 4096 && max_writes <= 4096 && yields >= 4 && !open_handles() && live_bytes == baseline_bytes);
    printf("import max bytes per lock: read=%u write=%u\n",max_reads,max_writes);
}
static void import_failed(unsigned which)
{
    if(which == 0) fail_read = 2;
    if(which == 1) fail_write = 2;
    if(which == 2) fail_sync = true;
    if(which == 3) fail_rename = true;
    CHECK(!storage_file_import("/sdcard/books/source.bin","/flash/books/dest.bin"));
    int i = lookup("/flash/books/dest.bin"); CHECK(i >= 0 && entries[i].size == 7 && entries[i].data[0] == 66);
    CHECK(!open_handles() && live_bytes == baseline_bytes);
    for(unsigned j = 0; j < FILE_LIMIT; ++j) CHECK(!entries[j].used || !strstr(entries[j].path,".tmp"));
}
static void import_read_failure(void) { import_failed(0); }
static void import_write_failure(void) { import_failed(1); }
static void import_sync_failure(void) { import_failed(2); }
static void import_rename_failure(void) { import_failed(3); }
static void import_removal(void)
{
    yield_hook = remove_card;
    CHECK(!storage_file_import("/flash/books/source.bin","/sdcard/books/dest.bin"));
    int i = lookup("/sdcard/books/dest.bin"); CHECK(entries[i].size == 7 && entries[i].data[0] == 66);
    CHECK(!open_handles() && live_bytes == baseline_bytes);
}
static void import_source_mutation(void)
{
    yield_hook = change_source;
    CHECK(!storage_file_import("/flash/books/source.bin","/sdcard/books/dest.bin"));
    CHECK(entries[lookup("/sdcard/books/dest.bin")].size == 7 && !open_handles());
}
static void mkdir_invalidates_directory(void)
{
    uint32_t revision = storage_directory_revision("/flash/books");
    CHECK(!io_mkdir("/flash/books/newdir",0));
    CHECK(storage_directory_revision("/flash/books") != revision);
}
static void signature_close_failure(void)
{
    uint32_t hash = 42; fail_close = 1;
    CHECK(!storage_file_signature("/sdcard/books/source.bin",&hash) && hash == 42);
    CHECK(!open_handles());
}
static unsigned legacy_slot(const char *p)
{ uint32_t h = 2166136261u; while(*p) h = (h ^ (unsigned char)*p++) * 16777619u; return h % 256u; }
static void import_same_volume_collision(void)
{
    char source[100];
    unsigned slot = legacy_slot("/flash/books/dest.bin.00000001.tmp");
    for(unsigned i = 0; ; ++i) {
        CHECK(i < 100000); snprintf(source,sizeof(source),"/flash/books/collision%u.bin",i);
        if(legacy_slot(source) == slot) break;
    }
    seed(source,12000,11);
    CHECK(storage_file_import(source,"/flash/books/dest.bin"));
    CHECK(entries[lookup("/flash/books/dest.bin")].size == 12000 && !open_handles());
}
static void unrelated_rename_with_writer(void)
{
    seed("/flash/background.log",10,0); seed("/flash/unrelated.tmp",10,0);
    int writer = io_open("/flash/background.log",O_WRONLY); CHECK(writer >= 0);
    CHECK(!io_rename("/flash/unrelated.tmp","/flash/unrelated.dat"));
    uint32_t hash;
    CHECK(storage_file_signature("/flash/books/source.bin",&hash));
    CHECK(!io_close(writer));
}
static void import_close_failure(void)
{
    fail_close = 1;
    CHECK(!storage_file_import("/sdcard/books/source.bin","/flash/books/dest.bin"));
    CHECK(entries[lookup("/flash/books/dest.bin")].size == 7);
    CHECK(!open_handles());
}
static void watch_aliases_no_allocations(void)
{
    unsigned allocations = alloc_calls;
    uint32_t revision = storage_path_revision("/sdcard/books/source.bin");
    CHECK(storage_path_revision("/flash/../sdcard//books/./source.bin") == revision);
    CHECK(storage_path_revision("/sdcard/BOOKS/SOURCE.BIN") == revision);
    int fd = io_open("/sdcard/BOOKS/SOURCE.BIN",O_WRONLY); CHECK(fd >= 0);
    CHECK(storage_file_writers("/sdcard/books/../books/source.bin") == 1);
    CHECK(alloc_calls == allocations); CHECK(!io_close(fd));
    CHECK(storage_file_writers("relative/path") != 0);
    CHECK(storage_path_revision("/../../sdcard/source.bin") == 0);
}
static void watch_close_retry_keeps_pin(void)
{
    int fd = io_open("/flash/books/source.bin",O_WRONLY); CHECK(fd >= 0);
    CHECK(!storage_file_writers("/flash/books/independent.bin.tmp"));
    CHECK(storage_file_replace("/flash/books/independent.bin", "mark", 4));
    fail_close = close_calls + 1;
    CHECK(io_close(fd) < 0 && open_handles() == 1);
    CHECK(storage_file_writers("/flash/books/source.bin") == 1);
    uint32_t revision = storage_path_revision("/flash/books/source.bin");
    for(unsigned i = 0; i < 512; ++i) {
        char p[64]; snprintf(p,sizeof(p),"/flash/other%u",i); storage_path_revision(p);
    }
    CHECK(storage_path_revision("/flash/books/source.bin") == revision);
    fail_close = 0; CHECK(!io_close(fd) && !open_handles());
    CHECK(!storage_file_writers("/flash/books/source.bin"));
}
static void watch_slot_exhaustion(void)
{
    struct dfs_fd pinned[WATCH_SLOTS]; char names[WATCH_SLOTS][64];
    for(unsigned i = 0; i < WATCH_SLOTS; ++i) {
        snprintf(names[i],sizeof(names[i]),"/pinned%u",i);
        pinned[i] = (struct dfs_fd){.fs=&volumes[0],.path=names[i],.flags=O_RDONLY};
        CHECK(pin_file(&pinned[i],true));
    }
    CHECK(io_open("/flash/books/source.bin",O_WRONLY) < 0);
    CHECK(!open_handles() && !storage_file_writers("/flash/books/source.bin"));
    for(unsigned i = 0; i < WATCH_SLOTS; ++i) CHECK(pin_file(&pinned[i],false));
    int fd = io_open("/flash/books/source.bin",O_WRONLY); CHECK(fd >= 0);
    CHECK(storage_file_writers("/flash/books/source.bin") == 1); CHECK(!io_close(fd));
}
static void import_expected_session(void)
{
    CHECK(!storage_file_import_session("/sdcard/books/source.bin","/flash/books/dest.bin",STORAGE_SD,session+1));
    CHECK(!open_handles() && !read_calls && !write_calls && live_bytes == baseline_bytes);
    CHECK(entries[lookup("/flash/books/dest.bin")].size == 7);
    CHECK(storage_file_import_session("/sdcard/books/source.bin","/flash/books/dest.bin",STORAGE_SD,session));
}
static void watch_allocation_failure(void)
{
    CHECK(!watches[0].slots && !watches[1].slots);
    CHECK(!storage_path_pin("/flash/books/source.bin"));
    storage_path_unpin("/flash/books/source.bin");
    fail_cache_alloc = false;
    uint32_t initial = storage_path_revision("/flash/books/source.bin");
    int fd = io_open("/flash/books/source.bin",O_WRONLY); CHECK(fd >= 0);
    CHECK(storage_file_writers("/flash/books/source.bin") != 0);
    CHECK(storage_file_writers("/flash/unknown.bin") != 0);
    CHECK(storage_path_revision("/flash/books/source.bin") != initial);
    CHECK(!io_close(fd) && !storage_file_writers("/flash/books/source.bin"));
    uint32_t hash; CHECK(storage_file_signature("/flash/books/source.bin",&hash));
    CHECK(hash == expected_hash(lookup("/flash/books/source.bin")));
}
static void retained_path_pins(void)
{
    const char *path = "/flash/books/source.bin";
    CHECK(storage_path_pin(path) && storage_path_pin(path));
    uint32_t token = storage_path_revision(path);
    int fd = io_open(path,O_RDONLY); CHECK(fd >= 0); CHECK(!io_close(fd));
    storage_path_unpin(path);
    for(unsigned i = 0; i < 512; ++i) {
        char p[64]; snprintf(p,sizeof(p),"/flash/pin-churn%u",i); storage_path_revision(p);
    }
    CHECK(storage_path_revision(path) == token);
    overwrite(path,8);
    CHECK(storage_path_revision(path) != token);
    token = storage_path_revision(path); storage_path_unpin(path);
    for(unsigned i = 0; i < 512; ++i) {
        char p[64]; snprintf(p,sizeof(p),"/flash/unpin-churn%u",i); storage_path_revision(p);
    }
    CHECK(storage_path_revision(path) != token);
    char paths[WATCH_SLOTS][64];
    for(unsigned i = 0; i < WATCH_SLOTS; ++i) {
        snprintf(paths[i],sizeof(paths[i]),"/flash/retained%u",i); CHECK(storage_path_pin(paths[i]));
    }
    CHECK(!storage_path_pin("/flash/full-table"));
    storage_path_unpin("/flash/full-table");
    for(unsigned i = 0; i < WATCH_SLOTS; ++i) storage_path_unpin(paths[i]);
    CHECK(storage_path_pin("/flash/full-table")); storage_path_unpin("/flash/full-table");
    for(unsigned i = 0; i < WATCH_SLOTS; ++i) CHECK(!watches[0].slots[i].pins);
}
static void unicode_path_tokens(void)
{
    const char *a = "/sdcard/books/\xc3\xa9-\xce\xb1-中文.ttf";
    const char *b = "/sdcard/BOOKS/\xc3\x89-\xce\x91-中文.TTF";
    CHECK(storage_path_pin(a)); uint32_t token = storage_path_revision(a);
    CHECK(storage_path_revision(b) == token);
    changed(&volumes[STORAGE_SD],b + strlen(STORAGE_SD_ROOT),false,false);
    CHECK(storage_path_revision(a) != token);
    storage_path_unpin(b);
    /* This SDK maps U+023A to U+2C65, growing from two UTF-8 bytes to three. */
    CHECK(storage_path_revision("/sdcard/\xc8\xba.ttf") == storage_path_revision("/sdcard/\xe2\xb1\xa5.TTF"));
    CHECK(!storage_path_pin("/sdcard/\xed\xa0\x80"));
    CHECK(!storage_path_pin("/sdcard/\xc0\xaf"));
    CHECK(!storage_path_pin("/sdcard/\xe2"));
    for(unsigned i = 0; i < WATCH_SLOTS; ++i) CHECK(!watches[STORAGE_SD].slots[i].pins);
}

static unsigned stat_race_call, stat_race_at, stat_race_kind;
static int stat_race_writer = -1;
static void race_in_stat(void)
{
    if(++stat_race_call != stat_race_at) return;
    stat_hook = NULL;
    if(stat_race_kind == 0) overwrite("/sdcard/books/source.bin",23);
    if(stat_race_kind == 1) ++session;
    if(stat_race_kind == 2) {
        stat_race_writer = io_open("/sdcard/books/source.bin",O_WRONLY);
        CHECK(stat_race_writer >= 0);
    }
}
static void signature_stat_race(bool cached, unsigned kind)
{
    const char *path = "/sdcard/books/source.bin"; uint32_t hash = 42;
    if(cached) CHECK(storage_file_signature(path,&hash));
    unsigned reads = read_calls;
    hash = 42; stat_race_at = cached ? 1 : 2; stat_race_kind = kind; stat_hook = race_in_stat;
    CHECK(!storage_file_signature(path,&hash) && hash == 42 && !stat_hook);
    if(cached) CHECK(read_calls == reads);
    if(stat_race_writer >= 0) CHECK(!io_close(stat_race_writer));
    CHECK(!open_handles());
    CHECK(storage_file_signature(path,&hash) && hash == expected_hash(lookup(path)));
}
static void signature_hit_stat_session(void) { signature_stat_race(true,1); }
static void signature_hit_stat_writer(void) { signature_stat_race(true,2); }
static void signature_final_stat_revision(void) { signature_stat_race(false,0); }
static void signature_final_stat_session(void) { signature_stat_race(false,1); }
static void signature_final_stat_writer(void) { signature_stat_race(false,2); }

static void close_recovered_by_operation(void)
{
    const char *path = "/sdcard/books/source.bin"; uint32_t hash = 42;
    fail_close_always = true;
    CHECK(!storage_file_signature(path,&hash) && hash == 42);
    CHECK(close_calls == 2 && open_handles() == 1 && live_bytes == baseline_bytes);
    uint32_t token = storage_path_revision(path);
    for(unsigned i = 0; i < 512; ++i) {
        char p[64]; snprintf(p,sizeof(p),"/sdcard/churn%u",i); storage_path_revision(p);
    }
    CHECK(storage_path_revision(path) == token);
    fail_close_always = false;
    CHECK(storage_file_signature(path,&hash) && hash == expected_hash(lookup(path)));
    CHECK(close_calls == 4 && !open_handles());
}
static void close_release_poll_bound(void)
{
    uint32_t hash = 42; fail_close_always = true;
    CHECK(!storage_file_signature("/sdcard/books/source.bin",&hash));
    release_request = 41;
    CHECK(storage_release_complete(0) == -RT_EINVAL);
    CHECK(storage_release_complete(40) == -RT_EINVAL && close_calls == 2);
    CHECK(storage_release_complete(41) == -RT_EBUSY && close_calls == 3);
    for(unsigned i = 0; i < 100; ++i) CHECK(storage_release_complete(41) == -RT_EBUSY);
    CHECK(close_calls == 3 && open_handles() == 1 && !release_acks);
    fail_close_always = false;
    CHECK(storage_release_complete(41) == -RT_EBUSY && close_calls == 3);
    /* A subsequent operation still has its single opportunistic attempt. */
    CHECK(storage_file_signature("/flash/books/source.bin",&hash));
    CHECK(!open_handles() && close_calls == 5);
    CHECK(storage_release_complete(41) == RT_EOK && release_acks == 1);
}
static void release_while_active(void)
{
    yield_hook = NULL;
    CHECK(open_handles() == 1 && close_calls == 0);
    CHECK(storage_release_complete(release_request) == -RT_EBUSY);
    CHECK(open_handles() == 1 && close_calls == 0 && !release_acks);
}
static void close_release_waits_for_active(void)
{
    uint32_t hash; release_request = 51; yield_hook = release_while_active;
    CHECK(storage_file_signature("/sdcard/books/source.bin",&hash));
    CHECK(!yield_hook && !open_handles());
    CHECK(storage_release_complete(51) == RT_EOK && release_acks == 1);
}
static void close_capacity_bounded(void)
{
    uint32_t hash = 42; fail_close_always = true;
    for(unsigned i = 0; i < 16; ++i) {
        char path[80];snprintf(path,sizeof(path),"/sdcard/books/held-%u.bin",i%8);
        if(i<8)seed(path,7,31);
        CHECK(!storage_file_signature(path,&hash));
    }
    CHECK(hash == 42 && open_handles() == 8 && close_calls == 24 && live_bytes == baseline_bytes);
    unsigned allocations = alloc_calls, reads = read_calls;
    for(unsigned i = 0; i < 100; ++i) CHECK(!storage_file_signature("/sdcard/books/source.bin",&hash));
    CHECK(open_handles() == 8 && close_calls == 24 && alloc_calls == allocations && read_calls == reads);
    release_request = 61;
    CHECK(storage_release_complete(61) == -RT_EBUSY && close_calls == 32);
    for(unsigned i = 0; i < 100; ++i) CHECK(storage_release_complete(61) == -RT_EBUSY);
    CHECK(close_calls == 32 && !release_acks);
    fail_close_always = false; release_request = 62;
    CHECK(storage_release_complete(62) == RT_EOK && close_calls == 40);
    CHECK(!open_handles() && release_acks == 1);
    CHECK(storage_file_signature("/sdcard/books/source.bin",&hash));
}
static void import_output_close_transient(void)
{
    fail_close = 2;
    CHECK(!storage_file_import("/flash/books/source.bin","/sdcard/books/dest.bin"));
    CHECK(close_calls == 3 && !open_handles());
    CHECK(entries[lookup("/sdcard/books/dest.bin")].size == 7);
    CHECK(lookup("/sdcard/books/dest.bin.00000001.tmp") < 0);
}
static void import_output_close_pending(void)
{
    const char *temporary = "/sdcard/books/dest.bin.00000001.tmp";
    fail_close_always = true; fail_close_path = temporary;
    CHECK(!storage_file_import("/flash/books/source.bin","/sdcard/books/dest.bin"));
    CHECK(close_calls == 3 && open_handles() == 1 && storage_file_writers(temporary) == 1);
    CHECK(entries[lookup("/sdcard/books/dest.bin")].size == 7 && lookup(temporary) >= 0);
    fail_close_always = false;
    uint32_t hash; CHECK(storage_file_signature("/flash/books/source.bin",&hash));
    CHECK(!open_handles() && !storage_file_writers(temporary) && lookup(temporary) < 0);
    CHECK(entries[lookup("/sdcard/books/dest.bin")].size == 7);
}
static void import_close_retired_session(void)
{
    const char *temporary = "/sdcard/books/dest.bin.00000001.tmp";
    fail_close_always = true; fail_close_path = temporary;
    CHECK(!storage_file_import("/flash/books/source.bin","/sdcard/books/dest.bin"));
    CHECK(open_handles() == 1 && storage_file_writers(temporary) == 1);
    ++session; mounted[STORAGE_SD] = false; fail_close_always = false; release_request = 71;
    CHECK(storage_release_complete(71) == RT_EOK && !open_handles());
    CHECK(lookup(temporary) >= 0 && !storage_file_writers(temporary));
    CHECK(entries[lookup("/sdcard/books/dest.bin")].size == 7);
}
static void replace_close_pending(void)
{
    const char *path = "/sdcard/books/dest.bin", *temporary = "/sdcard/books/dest.bin.tmp";
    const char data[] = "new contents";
    fail_close_always = true; fail_close_path = temporary;
    CHECK(!storage_file_replace(path,data,sizeof(data)));
    CHECK(open_handles() == 1 && close_calls == 2 && storage_file_writers(temporary) == 1);
    CHECK(entries[lookup(path)].size == 7);
    CHECK(!storage_file_replace(path,data,sizeof(data)) && close_calls == 3);
    for(unsigned i = 0; i < 20; ++i) CHECK(!storage_file_replace(path,data,sizeof(data)));
    CHECK(close_calls == 3 && open_handles() == 1 && entries[lookup(path)].size == 7);
    fail_close_always = false; release_request = 81;
    CHECK(storage_release_complete(81) == RT_EOK && !open_handles() && lookup(temporary) < 0);
    CHECK(storage_file_replace(path,data,sizeof(data)));
    CHECK(entries[lookup(path)].size == sizeof(data) && !memcmp(entries[lookup(path)].data,data,sizeof(data)));
}
static void replace_close_recovered_by_operation(void)
{
    const char *path = "/sdcard/books/dest.bin", *temporary = "/sdcard/books/dest.bin.tmp";
    const char data[] = "new contents";
    fail_close_always = true; fail_close_path = temporary;
    CHECK(!storage_file_replace(path,data,sizeof(data)) && open_handles() == 1);
    fail_close_always = false;
    CHECK(storage_file_replace(path,data,sizeof(data)) && !open_handles());
    CHECK(close_calls == 4 && entries[lookup(path)].size == sizeof(data));
}
static void lfs_close_quarantined(void)
{
    const char *temporary = "/flash/books/dest.bin.tmp";
    const char data[] = "new contents";
    lfs_consumes_failed_close = true; fail_close = 1;
    CHECK(!storage_file_replace("/flash/books/dest.bin",data,sizeof(data)));
    CHECK(open_handles() == 1 && close_calls == 1 && storage_file_writers(temporary) == 1);
    CHECK(entries[lookup("/flash/books/dest.bin")].size == 7);
    for(unsigned i = 0; i < 20; ++i) CHECK(!storage_file_replace("/flash/books/dest.bin",data,sizeof(data)));
    for(unsigned i = 1; i < 20; ++i) CHECK(!storage_file_release(STORAGE_FLASH,i));
    CHECK(open_handles() == 1 && close_calls == 1 && storage_file_writers(temporary) == 1);
    uint32_t hash; CHECK(storage_file_signature("/sdcard/books/source.bin",&hash));
    CHECK(open_handles() == 1 && close_calls == 2);
    release_request = 91;
    CHECK(storage_release_complete(91) == RT_EOK && release_acks == 1);
}
static void record_load_close_failure(void)
{
    const char *path = "/sdcard/books/record.bin";
    uint32_t value = 12345, loaded = 0;
    CHECK(storage_record_save(path,0x1234,1,&value,sizeof(value)));
    fail_close = close_calls + 1;
    CHECK(!storage_record_load(path,0x1234,1,&loaded,sizeof(loaded)));
    CHECK(!open_handles());
    CHECK(storage_record_load(path,0x1234,1,&loaded,sizeof(loaded)) && loaded == value);
    CHECK(!open_handles());
}
typedef void (*test_fn)(void);
static uint8_t replacement_data[17001];
static void replace_segmented(void)
{
    for (unsigned i = 0; i < sizeof(replacement_data); ++i) replacement_data[i] = (uint8_t)i;
    short_write = 511;
    CHECK(storage_file_replace_session("/sdcard/books/dest.bin", replacement_data, sizeof(replacement_data), session));
    int i = lookup("/sdcard/books/dest.bin");
    CHECK(entries[i].size == sizeof(replacement_data) && !memcmp(entries[i].data, replacement_data, sizeof(replacement_data)));
    CHECK(max_writes <= 4096 && yields == 4 && !open_handles() && live_bytes == baseline_bytes);
}
static void replace_segmented_failure(unsigned kind)
{
    if (kind == 0) fail_write = 2;
    if (kind == 1) fail_sync = true;
    if (kind == 2) fail_rename = true;
    if (kind == 3) fail_close = 1;
    CHECK(!storage_file_replace_session("/sdcard/books/dest.bin", replacement_data, sizeof(replacement_data), session));
    int i = lookup("/sdcard/books/dest.bin");
    CHECK(entries[i].size == 7 && entries[i].data[0] == 66 && !open_handles());
}
static void replace_segmented_write_failure(void) { replace_segmented_failure(0); }
static void replace_segmented_sync_failure(void) { replace_segmented_failure(1); }
static void replace_segmented_rename_failure(void) { replace_segmented_failure(2); }
static void replace_segmented_close_failure(void) { replace_segmented_failure(3); }
static void replace_card_session(void) { ++session; yield_hook = NULL; }
static void replace_segmented_card_change(void)
{
    CHECK(!storage_file_replace_session("/sdcard/books/dest.bin", replacement_data, sizeof(replacement_data), session + 1));
    CHECK(!write_calls && !open_handles());
    yield_hook = replace_card_session;
    CHECK(!storage_file_replace_session("/sdcard/books/dest.bin", replacement_data, sizeof(replacement_data), session));
    CHECK(write_calls == 1 && !open_handles() && entries[lookup("/sdcard/books/dest.bin")].size == 7);
}
static void replace_competing_writer(void)
{
    yield_hook = NULL;
    CHECK(!storage_file_replace("/sdcard/books/dest.bin", "other", 5));
}
static void replace_segmented_collision(void)
{
    yield_hook = replace_competing_writer;
    CHECK(storage_file_replace_session("/sdcard/books/dest.bin", replacement_data, sizeof(replacement_data), session));
    CHECK(entries[lookup("/sdcard/books/dest.bin")].size == sizeof(replacement_data) && !open_handles());
}
static void replace_stop_writes(void) { allow_write = false; yield_hook = NULL; }
static void replace_segmented_reset(void)
{
    yield_hook = replace_stop_writes;
    CHECK(!storage_file_replace_session("/flash/books/dest.bin", replacement_data, sizeof(replacement_data), session));
    CHECK(write_calls == 1 && !open_handles() && entries[lookup("/flash/books/dest.bin")].size == 7);
}
static void owned_public_lifecycle(void)
{
    const char *path="/sdcard/books/staging.tmp";
    seed(path,7,33);
    int fd=storage_file_owned_open(path,O_RDWR,true);CHECK(fd>=0);
    CHECK(storage_file_owned_open("/sdcard/books/STAGING.TMP",O_TRUNC|O_RDWR,true)<0);
    CHECK(entries[lookup(path)].size==7);
    fail_close=1;CHECK(!storage_file_owned_close(fd));
    CHECK(!open_handles()&&lookup(path)<0);
    fd=storage_file_owned_open("/sdcard/books/source.bin",O_RDONLY,false);CHECK(fd>=0);
    fail_close_always=true;CHECK(!storage_file_owned_close(fd));
    unsigned calls=close_calls;
    for(unsigned i=0;i<100;++i)CHECK(storage_file_owned_open("/sdcard/books/source.bin",O_TRUNC|O_RDWR,false)<0);
    CHECK(close_calls==calls+1&&open_handles()==1&&entries[lookup("/sdcard/books/source.bin")].size==16507);
    calls=close_calls;fail_close_always=false;++session;
    int next=storage_file_owned_open("/sdcard/books/dest.bin",O_RDONLY,false);CHECK(next>=0);
    CHECK(close_calls==calls+1&&open_handles()==1&&!strcmp(owned_find(next)->path,"/sdcard/books/dest.bin"));
    CHECK(storage_file_owned_close(next)&&!open_handles());
}
static void owned_public_lfs_quarantine(void)
{
    int fd=storage_file_owned_open("/flash/books/source.bin",O_RDONLY,false);CHECK(fd>=0);
    lfs_consumes_failed_close=true;fail_close=1;
    CHECK(!storage_file_owned_close(fd)&&owned_find(fd)&&!owned_find(fd)->retryable);
    unsigned calls=close_calls;
    for(unsigned i=1;i<100;++i) {
        CHECK(!storage_file_release(STORAGE_FLASH,i));
        CHECK(storage_file_owned_open("/flash/books/source.bin",O_TRUNC|O_RDWR,false)<0);
    }
    CHECK(close_calls==calls&&open_handles()==1);
}
static const struct { const char *name; test_fn run; } cases[] = {
    {"owned_public_lifecycle",owned_public_lifecycle},
    {"owned_public_lfs_quarantine",owned_public_lfs_quarantine},
    {"replace_segmented",replace_segmented},
    {"replace_segmented_write_failure",replace_segmented_write_failure},
    {"replace_segmented_sync_failure",replace_segmented_sync_failure},
    {"replace_segmented_rename_failure",replace_segmented_rename_failure},
    {"replace_segmented_close_failure",replace_segmented_close_failure},
    {"replace_segmented_card_change",replace_segmented_card_change},
    {"replace_segmented_collision",replace_segmented_collision},
    {"replace_segmented_reset",replace_segmented_reset},
    {"signature_hit_and_rewrite",signature_hit_and_rewrite}, {"signature_cache_bound",signature_cache_bound},
    {"signature_delegation",signature_delegation},
    {"signature_allocation_failures",signature_allocation_failures}, {"signature_read_failure",signature_read_failure},
    {"signature_removal",signature_removal}, {"signature_mutation_during_read",signature_mutation_during_read},
    {"signature_case_alias",signature_case_alias}, {"signature_hit_stat_race",signature_hit_stat_race},
    {"import_segmented",import_segmented}, {"import_read_failure",import_read_failure},
    {"import_write_failure",import_write_failure}, {"import_sync_failure",import_sync_failure},
    {"import_rename_failure",import_rename_failure}, {"import_removal",import_removal},
    {"import_source_mutation",import_source_mutation}, {"mkdir_invalidates_directory",mkdir_invalidates_directory},
    {"signature_close_failure",signature_close_failure},
    {"import_same_volume_collision",import_same_volume_collision},
    {"unrelated_rename_with_writer",unrelated_rename_with_writer},
    {"import_close_failure",import_close_failure},
    {"watch_aliases_no_allocations",watch_aliases_no_allocations},
    {"watch_close_retry_keeps_pin",watch_close_retry_keeps_pin},
    {"watch_slot_exhaustion",watch_slot_exhaustion},
    {"import_expected_session",import_expected_session},
    {"watch_allocation_failure",watch_allocation_failure},
    {"retained_path_pins",retained_path_pins},
    {"unicode_path_tokens",unicode_path_tokens},
    {"signature_hit_stat_session",signature_hit_stat_session},
    {"signature_hit_stat_writer",signature_hit_stat_writer},
    {"signature_final_stat_revision",signature_final_stat_revision},
    {"signature_final_stat_session",signature_final_stat_session},
    {"signature_final_stat_writer",signature_final_stat_writer},
    {"close_recovered_by_operation",close_recovered_by_operation},
    {"close_release_poll_bound",close_release_poll_bound},
    {"close_release_waits_for_active",close_release_waits_for_active},
    {"close_capacity_bounded",close_capacity_bounded},
    {"import_output_close_transient",import_output_close_transient},
    {"import_output_close_pending",import_output_close_pending},
    {"import_close_retired_session",import_close_retired_session},
    {"replace_close_pending",replace_close_pending},
    {"replace_close_recovered_by_operation",replace_close_recovered_by_operation},
    {"lfs_close_quarantined",lfs_close_quarantined},
    {"record_load_close_failure",record_load_close_failure},
};
int main(int argc, char **argv)
{
    if(argc != 2) return 2;
    if(!strcmp(argv[1],"--list")) {
        for(unsigned i = 0; i < sizeof(cases)/sizeof(*cases); ++i) puts(cases[i].name);
        return 0;
    }
    for(unsigned i = 0; i < sizeof(cases)/sizeof(*cases); ++i) if(!strcmp(argv[1],cases[i].name)) {
        if(!strcmp(cases[i].name,"watch_allocation_failure")) fail_cache_alloc = true;
        setup(); cases[i].run(); CHECK(!depth); printf("PASS %s\n",cases[i].name); return 0;
    }
    return 2;
}
