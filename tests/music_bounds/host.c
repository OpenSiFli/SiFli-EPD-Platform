#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include "music_library.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define STORAGE_PATH_MAX 512
#define STORAGE_SD_ROOT "/sd"
#define STORAGE_SD 1
#define S_ISDIR(mode) ((mode) == 1)
#define S_ISREG(mode) ((mode) == 2)
typedef struct { unsigned cursor; } DIR;
struct dirent { char d_name[256]; };
struct stat { unsigned st_mode; };
static unsigned total, reads, opens, closes;
static bool long_names, unsupported, duplicates, directories, changed, fail_stat;
typedef union { uint64_t align[2]; size_t size; } allocation_t;
static size_t live_bytes, live_blocks;
static int fail_after = -1;
static void *allocate(size_t size)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    allocation_t *p = malloc(sizeof(*p) + size); CHECK(p);
    p->size = size; live_bytes += size; ++live_blocks;
    return p + 1;
}
static void release(void *data)
{
    if (!data) return;
    allocation_t *p = (allocation_t *)data - 1;
    live_bytes -= p->size; --live_blocks; free(p);
}
static void *resize(void *data, size_t size)
{
    void *copy = allocate(size);
    if (!copy) return NULL;
    if (data) { size_t old = ((allocation_t *)data - 1)->size; memcpy(copy, data, old < size ? old : size); }
    release(data); return copy;
}
static const char *storage_path_root(const char *path) { (void)path; return STORAGE_SD_ROOT; }
static bool storage_path_available(const char *path) { (void)path; return true; }
static uint32_t storage_card_session(void) { return 1; }
static bool storage_session_valid(int volume, uint32_t session)
{ CHECK(volume == STORAGE_SD && session == 1); return !changed; }
static DIR *opendir(const char *path)
{ CHECK(!strcmp(path, "/sd")); DIR *d = allocate(sizeof(*d)); if (d) { d->cursor = 0; ++opens; } return d; }
static struct dirent *readdir(DIR *d)
{
    static struct dirent e;
    ++reads;
    if (d->cursor == total) return NULL;
    unsigned id = duplicates ? 1 : total - d->cursor;
    ++d->cursor;
    int n = snprintf(e.d_name, sizeof(e.d_name), "%06u", id);
    if (long_names) { memset(e.d_name + n, 'a', 240 - (size_t)n); n = 240; }
    strcpy(e.d_name + n, unsupported ? ".txt" : ".mp3");
    return &e;
}
static int closedir(DIR *d) { ++closes; release(d); return 0; }
static int stat(const char *path, struct stat *out)
{ (void)path; if (fail_stat) return -1; out->st_mode = directories ? 1 : 2; return 0; }
#include "actual.inc"

static void reset(unsigned count)
{
    CHECK(!live_blocks && !live_bytes && opens == closes);
    total = count; reads = 0;
    long_names = unsupported = duplicates = directories = changed = fail_stat = false;
    fail_after = -1;
}
static music_model_result_t scan_all(music_library_t *list)
{
    music_library_scan_t *scan = NULL;
    music_model_result_t rc = music_library_scan_begin("/sd", &scan);
    if (rc != MUSIC_MODEL_OK) return rc;
    do {
        unsigned before = reads;
        rc = music_library_scan_step(scan, list, 8);
        CHECK(reads - before <= 8);
        CHECK(list->count <= MUSIC_LIBRARY_ENTRY_LIMIT);
        CHECK(list->capacity * sizeof(*list->entries) + list->string_bytes <= MUSIC_LIBRARY_BYTE_LIMIT);
    } while (rc == MUSIC_MODEL_OK);
    music_library_scan_close(scan); return rc;
}
int main(void)
{
    music_model_allocator_t allocator = {allocate, resize, release};
    music_model_set_allocator(&allocator);
    music_library_t list = {0};
    reset(MUSIC_LIBRARY_ENTRY_LIMIT);
    CHECK(scan_all(&list) == MUSIC_MODEL_END && list.count == MUSIC_LIBRARY_ENTRY_LIMIT);
    CHECK(!strcmp(list.entries[0].name, "000001.mp3") && !strcmp(list.entries[list.count - 1].name, "001024.mp3"));
    CHECK(live_bytes == list.capacity * sizeof(*list.entries) + list.string_bytes);
    music_library_free(&list);
    reset(100000);
    CHECK(scan_all(&list) == MUSIC_MODEL_LIMIT && reads == MUSIC_LIBRARY_ENTRY_LIMIT + 1);
    CHECK(list.count == MUSIC_LIBRARY_ENTRY_LIMIT); music_library_free(&list);
    reset(100000); long_names = true;
    CHECK(scan_all(&list) == MUSIC_MODEL_LIMIT && list.count < MUSIC_LIBRARY_ENTRY_LIMIT);
    printf("PASS directory budgets: exact 1024 entries sorted; 100000-file input stops at 1025; long names stop at %zu entries / %zu retained bytes\n",
           list.count, live_bytes);
    music_library_free(&list);
    reset(100000); unsupported = true;
    CHECK(scan_all(&list) == MUSIC_MODEL_END && !list.count && reads == 100001);
    music_library_free(&list);
    reset(5000); duplicates = true;
    CHECK(scan_all(&list) == MUSIC_MODEL_END && list.count == 1); music_library_free(&list);
    reset(1025); directories = true;
    CHECK(scan_all(&list) == MUSIC_MODEL_LIMIT && list.count == 1024); music_library_free(&list);
    puts("PASS enumeration: bounded 8-entry steps, 100000 unsupported files skipped, duplicates ignored, directories count toward cap");
    reset(1); CHECK(music_library_read("/sd", &list) == MUSIC_MODEL_OK);
    music_library_entry_t *old = list.entries;
    size_t bytes = live_bytes, blocks = live_blocks;
    total = 1025;
    CHECK(music_library_read("/sd", &list) == MUSIC_MODEL_LIMIT);
    CHECK(list.entries == old && list.count == 1 && live_bytes == bytes && live_blocks == blocks);
    for (int fail = 0; fail < 40; ++fail) {
        total = 20; fail_after = fail;
        music_model_result_t rc = music_library_read("/sd", &list);
        CHECK(rc == MUSIC_MODEL_MEMORY || rc == MUSIC_MODEL_IO);
        CHECK(list.entries == old && live_bytes == bytes && live_blocks == blocks && opens == closes);
    }
    fail_after = -1; changed = true;
    CHECK(music_library_read("/sd", &list) == MUSIC_MODEL_CHANGED && live_bytes == bytes);
    changed = false; fail_stat = true;
    CHECK(music_library_read("/sd", &list) == MUSIC_MODEL_IO && live_bytes == bytes);
    fail_stat = false; total = 2;
    CHECK(music_library_read("/sd", &list) == MUSIC_MODEL_OK && list.count == 2);
    music_library_free(&list); reset(0);
    puts("PASS failure ownership: cap/40 allocation failures/card change/I/O preserve old complete output, zero leaked blocks, subsequent read succeeds");
    puts("music directory bounds: all 3 suites passed");
    return 0;
}
