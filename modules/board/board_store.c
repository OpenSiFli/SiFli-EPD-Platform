#include "storage_file.h"
#include "storage.h"
#include "epd_memory.h"
#include "board_store.h"
#include "platform/epd_app.h"
#include <dfs_posix.h>
#include <errno.h>
#include <string.h>

static void *allocate(size_t size, void *context)
{
    (void)context;
    return epd_app_alloc(size, EPD_APP_PSRAM);
}
static void release(void *pointer, void *context)
{
    (void)context;
    epd_app_free(pointer);
}

board_result_t board_store_load(board_model_t **out, board_message_t *message)
{
    const board_allocator_t allocator = {allocate, release, NULL};
    char path[STORAGE_PATH_MAX];
    char *data = NULL;
    int fd = -1;
    struct stat info;
    board_result_t result = BOARD_SCHEMA;
    *out = NULL;
    *message = BOARD_MESSAGE_CONFIG_UNAVAILABLE;
    storage_lock();
    if (storage_changing() || !storage_app_available("board") ||
        !storage_app_path(path, sizeof(path), "board", STORAGE_APP_DATA, "layout.json") ||
        !storage_file_recover(path)) goto finish;
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        if (errno != ENOENT && errno != -ENOENT) goto finish;
        if (!storage_app_path(path, sizeof(path), "board", STORAGE_APP_CODE, "res/layout.json")) goto finish;
        fd = open(path, O_RDONLY);
    }
    if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) goto finish;
    if ((uint64_t)info.st_size >= (uint64_t)PTRDIFF_MAX)
    { result = BOARD_BOUNDS; goto finish; }
    size_t size = (size_t)info.st_size, done = 0;
    data = allocate(size + 1, NULL);
    if (!data) { result = BOARD_MEMORY; goto finish; }
    while (done < size)
    {
        if (storage_changing() || !storage_app_available("board")) goto finish;
        size_t chunk = size - done < 4096 ? size - done : 4096;
        int count = read(fd, data + done, chunk);
        if (count <= 0 || (size_t)count > chunk) goto finish;
        done += (size_t)count;
    }
    char extra;
    if (read(fd, &extra, 1) != 0 || storage_changing() || !storage_app_available("board")) goto finish;
    if (close(fd) != 0) { fd = -1; goto finish; }
    fd = -1;
    storage_unlock();
    result = board_model_parse(data, size, &allocator, out);
    release(data, NULL);
    if (result == BOARD_OK) { *message = BOARD_MESSAGE_NONE; return result; }
    *message = result == BOARD_MEMORY ? BOARD_MESSAGE_LOAD_MEMORY : BOARD_MESSAGE_CONFIG_INVALID;
    return result;
finish:
    if (fd >= 0) close(fd);
    storage_unlock();
    release(data, NULL);
    if (result == BOARD_MEMORY) *message = BOARD_MESSAGE_LOAD_MEMORY;
    else if (result == BOARD_BOUNDS) *message = BOARD_MESSAGE_CONFIG_BOUNDS;
    return result;
}
