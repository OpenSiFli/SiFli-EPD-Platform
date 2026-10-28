#include "system_reset_store.h"
#include "storage_file.h"
#include <dfs_posix.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define RESET_PATH STORAGE_FLASH_ROOT "/system/factory_reset.bin"
#define RESET_MAGIC 0x45505253u
#define RESET_VERSION 1u

static rt_err_t io_error(void)
{
    return errno ? (errno > 0 ? -errno : errno) : -RT_EIO;
}
static int exists_at(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == ENOENT || errno == -ENOENT ? 0 : -1;
}
static bool record_valid(const reset_record_t *r)
{
    return r->step <= RESET_TERMINAL && r->card_state <= RESET_CARD_CANCELLED &&
           (r->step != RESET_TERMINAL || r->card_state != RESET_CARD_PENDING);
}

rt_err_t reset_record_write(const reset_record_t *record)
{
    reset_record_t verified;
    if (!record || !record_valid(record)) return -RT_EINVAL;
    storage_lock();
    bool ok = storage_record_save(RESET_PATH, RESET_MAGIC, RESET_VERSION, record, sizeof(*record)) &&
              storage_record_load(RESET_PATH, RESET_MAGIC, RESET_VERSION, &verified, sizeof(verified)) &&
              !memcmp(record, &verified, sizeof(verified));
    storage_unlock();
    return ok ? RT_EOK : -RT_EIO;
}

rt_err_t reset_record_read(reset_record_t *record, bool *exists)
{
    *exists = true;
    storage_lock();
    rt_err_t result = -RT_EIO;
    if (!storage_available(STORAGE_FLASH)) goto done;
    int main_file = exists_at(RESET_PATH);
    if (main_file < 0) goto done;
    if (!main_file)
    {
        int old = exists_at(RESET_PATH ".old"), temporary = exists_at(RESET_PATH ".tmp");
        if (old < 0 || temporary < 0) goto done;
        if (!old && !temporary) { *exists = false; result = RT_EOK; goto done; }
        /* An uncommitted temporary alone is not authorization to clear data. */
        if (!old || !storage_record_load(RESET_PATH ".old", RESET_MAGIC, RESET_VERSION,
                                        record, sizeof(*record)) || !record_valid(record)) goto done;
        result = reset_record_write(record);
        goto done;
    }
    if (storage_record_load(RESET_PATH, RESET_MAGIC, RESET_VERSION, record, sizeof(*record)) &&
        record_valid(record)) result = RT_EOK;
done:
    storage_unlock();
    return result;
}

static rt_err_t remove_if_present(const char *path)
{
    int present = exists_at(path);
    if (present < 0) return io_error();
    return !present || unlink(path) == 0 ? RT_EOK : io_error();
}

rt_err_t reset_record_cleanup(void)
{
    reset_record_t record;
    bool exists;
    storage_lock();
    rt_err_t error = reset_record_read(&record, &exists);
    if (error == RT_EOK && exists && record.step != RESET_TERMINAL) error = -RT_EBUSY;
    /* LittleFS commits each remove. Keep the terminal record until neither
     * recovery file can resurrect an earlier destructive phase. */
    if (error == RT_EOK && exists) error = remove_if_present(RESET_PATH ".old");
    if (error == RT_EOK && exists) error = remove_if_present(RESET_PATH ".tmp");
    if (error == RT_EOK && exists) error = remove_if_present(RESET_PATH);
    storage_unlock();
    return error;
}

static bool journal_name(const char *directory, const char *name)
{
    return !strcmp(directory, STORAGE_FLASH_ROOT "/system") &&
           (!strcmp(name, "factory_reset.bin") || !strcmp(name, "factory_reset.bin.old") ||
            !strcmp(name, "factory_reset.bin.tmp"));
}

static rt_err_t card_matches(const sdcard_identity_t *target, uint32_t session)
{
    sdcard_identity_t actual;
    if (!storage_session_valid(STORAGE_SD, session)) return -RT_EBUSY;
    rt_err_t error = sdcard_get_identity(&actual);
    return error != RT_EOK ? error : sdcard_identity_equal(target, &actual) ? RT_EOK : -RT_EBUSY;
}

/* Iterative traversal uses one bounded path, not one stack frame per directory.
 * Reopen after each removal: DFS/FAT directory cursors need not survive mutation. */
static rt_err_t erase_tree(const char *root, bool preserve_system,
                           const sdcard_identity_t *card, uint32_t session)
{
    char *path = rt_malloc(STORAGE_PATH_MAX);
    if (!path) return -RT_ENOMEM;
    strcpy(path, root);
    const size_t root_length = strlen(root);
    rt_err_t error = RT_EOK;
    for (;;)
    {
        storage_lock();
        if (card) error = card_matches(card, session);
        else if (!storage_available(STORAGE_FLASH)) error = -RT_EIO;
        struct stat st;
        if (error == RT_EOK && stat(path, &st) != 0)
        {
            if (strlen(path) == root_length && (errno == ENOENT || errno == -ENOENT))
            { storage_unlock(); break; }
            error = io_error();
        }
        if (error != RT_EOK) { storage_unlock(); break; }
        bool descend = false;
        if (S_ISDIR(st.st_mode))
        {
            DIR *dir = opendir(path);
            if (!dir) error = io_error();
            else
            {
                errno = 0;
                struct dirent *entry;
                while ((entry = readdir(dir)) != NULL)
                {
                    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
                        (preserve_system && journal_name(path, entry->d_name))) continue;
                    size_t length = strlen(path), name_length = strlen(entry->d_name);
                    if (length + name_length + 2 > STORAGE_PATH_MAX) { error = -ENAMETOOLONG; break; }
                    path[length] = '/';
                    memcpy(path + length + 1, entry->d_name, name_length + 1);
                    descend = true;
                    break;
                }
                if (!entry && errno) error = io_error();
                if (closedir(dir) != 0 && error == RT_EOK) error = io_error();
            }
        }
        if (error != RT_EOK) { storage_unlock(); break; }
        if (!descend)
        {
            if (preserve_system && !strcmp(path, STORAGE_FLASH_ROOT "/system"))
            {
                /* Keep the directory containing the reset journal. */
                storage_unlock(); break;
            }
            /* Removal IRQ revokes availability even while this thread owns the
             * I/O mutex. Do not start another mutation after a revoked read. */
            if (card && !storage_session_valid(STORAGE_SD, session)) error = -RT_EBUSY;
            else if (!card && !storage_available(STORAGE_FLASH)) error = -RT_EIO;
            else if ((S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) != 0) error = io_error();
            if (strlen(path) == root_length) { storage_unlock(); break; }
            *strrchr(path, '/') = 0;
        }
        storage_unlock();
        if (error != RT_EOK) break;
        rt_thread_mdelay(1);
    }
    rt_free(path);
    return error;
}

rt_err_t reset_erase_internal(void)
{
    /* Clear each root child independently so the journal's system directory can
     * remain without repeatedly selecting it in the root traversal. */
    DIR *dir;
    rt_err_t error = RT_EOK;
    char path[STORAGE_PATH_MAX];
    storage_lock();
    dir = opendir(STORAGE_FLASH_ROOT);
    if (!dir) { storage_unlock(); return io_error(); }
    struct dirent *entry;
    /* Gather root names before deleting; no live directory cursor spans deletes. */
    struct name { struct name *next; char text[1]; } *names = NULL;
    errno = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size_t length = strlen(entry->d_name);
        struct name *node = rt_malloc(sizeof(*node) + length);
        if (!node) { error = -RT_ENOMEM; break; }
        memcpy(node->text, entry->d_name, length + 1); node->next = names; names = node;
    }
    if (!entry && errno) error = io_error();
    if (closedir(dir) != 0 && error == RT_EOK) error = io_error();
    storage_unlock();
    while (names)
    {
        struct name *node = names; names = node->next;
        int length = snprintf(path, sizeof(path), STORAGE_FLASH_ROOT "/%s", node->text);
        if (error == RT_EOK)
            error = length < 0 || (size_t)length >= sizeof(path) ? -ENAMETOOLONG :
                    erase_tree(path, !strcmp(node->text, "system"), NULL, 0);
        rt_free(node);
    }
    return error;
}

rt_err_t reset_erase_card(const sdcard_identity_t *identity, uint32_t session)
{
    return erase_tree(STORAGE_SD_ROOT "/.epd", false, identity, session);
}

rt_err_t reset_default_directories(void)
{
    static const char *const names[] = {"apps", "data", "cache", "system", "fonts", "update"};
    char path[64];
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    {
        snprintf(path, sizeof(path), STORAGE_FLASH_ROOT "/%s", names[i]);
        if (!storage_mkdirs(path)) return -RT_EIO;
    }
    /* Missing preference records are the existing defaults on the next boot. */
    return RT_EOK;
}
