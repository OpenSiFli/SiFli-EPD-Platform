#include "storage_file.h"
#include "storage.h"
#include "epd_memory.h"
#include "board_store.h"
#include "platform/epd_app.h"
#include <dfs_posix.h>
#include <errno.h>
#include <string.h>

typedef struct { bool (*cancel)(void *); void *context; } load_context_t;
static bool stopped(const load_context_t *load)
{
    return load && load->cancel && load->cancel(load->context);
}
static void *allocate(size_t size, void *context)
{
    if (stopped(context)) return NULL;
    return epd_app_alloc(size, EPD_APP_PSRAM);
}
static void release(void *pointer, void *context)
{
    (void)context;
    epd_app_free(pointer);
}

static bool load_valid(const load_context_t *load, const char *path, storage_volume_t volume,
                       uint32_t session, uint32_t revision)
{
    return !stopped(load) && !storage_changing() && storage_app_available("board") &&
        storage_session_valid(volume, session) && storage_path_revision(path) == revision &&
        !storage_file_writers(path);
}

board_result_t board_store_load(board_model_t **out, board_message_t *message)
{
    return board_store_load_cancelable(out, message, NULL, NULL);
}

board_result_t board_store_load_cancelable(board_model_t **out, board_message_t *message,
                                          bool (*cancel)(void *), void *context)
{
    load_context_t load = {cancel, context};
    const board_allocator_t allocator = {allocate, release, &load};
    char path[STORAGE_PATH_MAX];
    char *data = NULL;
    int fd = -1;
    struct stat info;
    bool locked = true;
    uint32_t session = 0, revision = 0;
    storage_volume_t volume = STORAGE_FLASH;
    board_result_t result = BOARD_SCHEMA;
    *out = NULL;
    *message = BOARD_MESSAGE_CONFIG_UNAVAILABLE;
    storage_lock();
    if (stopped(&load) || storage_changing() || !storage_app_available("board") ||
        !storage_app_path(path, sizeof(path), "board", STORAGE_APP_DATA, "layout.json") ||
        !storage_file_recover(path)) goto finish;
    errno = 0;
    fd = storage_file_owned_open(path, O_RDONLY, false);
    if (fd < 0)
    {
        if (errno != ENOENT && errno != -ENOENT) goto finish;
        if (!storage_app_path(path, sizeof(path), "board", STORAGE_APP_CODE, "res/layout.json")) goto finish;
        fd = storage_file_owned_open(path, O_RDONLY, false);
    }
    if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) goto finish;
    if ((uint64_t)info.st_size >= (uint64_t)PTRDIFF_MAX)
    { result = BOARD_BOUNDS; goto finish; }
    volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    session = storage_card_session();
    revision = storage_path_revision(path);
    if (!revision || !load_valid(&load, path, volume, session, revision)) goto finish;
    size_t size = (size_t)info.st_size, done = 0;
    storage_unlock(); locked = false;
    data = allocate(size + 1, &load);
    if (!data) { result = BOARD_MEMORY; goto finish; }
    while (done < size)
    {
        size_t chunk = size - done < 4096 ? size - done : 4096;
        storage_lock(); locked = true;
        if (!load_valid(&load, path, volume, session, revision)) goto finish;
        int count = read(fd, data + done, chunk);
        storage_unlock(); locked = false;
        if (count <= 0 || (size_t)count > chunk) goto finish;
        done += (size_t)count;
    }
    char extra;
    storage_lock(); locked = true;
    if (!load_valid(&load, path, volume, session, revision) || read(fd, &extra, 1) != 0) goto finish;
    if (!storage_file_owned_close(fd)) { fd = -1; goto finish; }
    fd = -1;
    storage_unlock(); locked = false;
    result = board_model_parse(data, size, &allocator, out);
    release(data, NULL);
    /* Models outlive this call; their destructor must not retain the stack context. */
    if (*out) (*out)->allocator.context = NULL;
    if (stopped(&load)) { board_model_free(*out); *out = NULL; return BOARD_SCHEMA; }
    if (result == BOARD_OK) { *message = BOARD_MESSAGE_NONE; return result; }
    *message = result == BOARD_MEMORY ? BOARD_MESSAGE_LOAD_MEMORY : BOARD_MESSAGE_CONFIG_INVALID;
    return result;
finish:
    if (locked) storage_unlock();
    if (fd >= 0) storage_file_owned_close(fd);
    release(data, NULL);
    if (result == BOARD_MEMORY) *message = BOARD_MESSAGE_LOAD_MEMORY;
    else if (result == BOARD_BOUNDS) *message = BOARD_MESSAGE_CONFIG_BOUNDS;
    return result;
}
