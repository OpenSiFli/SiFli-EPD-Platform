/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file bookshelf.c
 * @brief 扫描内部存储和 TF 卡中的电子书文件与分卷 EPUB
 */
#include "epd_memory.h"
#include "bookshelf.h"
#include "storage.h"
#include "storage_file.h"
#include "document/document.h"
#include "file_version.h"

#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

/* Independent volumes keep their own book files and reading records. */
#define BOOKSHELF_DIR_DEFAULT STORAGE_SD_ROOT ", " STORAGE_FLASH_ROOT

static const char *g_dir = BOOKSHELF_DIR_DEFAULT;
static bookshelf_item_t g_items[BOOKSHELF_MAX_ITEMS];
static int g_count;
static rt_mutex_t g_lock;
static bookshelf_event_cb_t g_event_cb;
static rt_bool_t g_inited;
static struct {
    char path[BOOKSHELF_PATH_MAX];
    uint32_t size, signature;
    book_file_version_t version;
} signatures[BOOKSHELF_MAX_ITEMS + 1];
static unsigned signature_next;

bool bookshelf_file_signature(const char *path, uint32_t size, uint32_t *signature)
{
    if (!g_inited || !path || strlen(path) >= BOOKSHELF_PATH_MAX || !signature) return false;
    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    for (unsigned i = 0; i < BOOKSHELF_MAX_ITEMS + 1; ++i)
        if (signatures[i].size == size && !strcmp(signatures[i].path, path) &&
            book_file_version_valid(path, &signatures[i].version))
        {
            *signature = signatures[i].signature;
            rt_mutex_release(g_lock);
            return true;
        }
    rt_mutex_release(g_lock);
    book_file_version_t version = book_file_version(path);
    if (!book_file_version_valid(path, &version)) return false;
    if (!storage_file_signature(path, signature) || !book_file_version_valid(path, &version)) return false;
    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    unsigned slot = signature_next;
    for (unsigned i = 0; i < BOOKSHELF_MAX_ITEMS + 1; ++i)
        if (!strcmp(signatures[i].path, path)) { slot = i; break; }
    if (slot == signature_next) signature_next = (signature_next + 1) % (BOOKSHELF_MAX_ITEMS + 1);
    strcpy(signatures[slot].path, path);
    signatures[slot].size = size;
    signatures[slot].signature = *signature;
    signatures[slot].version = version;
    rt_mutex_release(g_lock);
    return true;
}

static void bookshelf_notify(void)
{
    if (g_event_cb != RT_NULL)
        g_event_cb(g_count);
}

rt_err_t bookshelf_service_init(void)
{
    if (g_inited)
        return RT_EOK;

    g_lock = rt_mutex_create("bs_lock", RT_IPC_FLAG_PRIO);
    if (g_lock == RT_NULL)
        return -RT_ENOMEM;

    g_count = 0;
    memset(signatures, 0, sizeof(signatures));
    signature_next = 0;
    g_inited = RT_TRUE;
    return RT_EOK;
}

int bookshelf_scan(bookshelf_item_t *items, bool (*cancelled)(void *), void *user)
{
    if (!g_inited || !items) return -RT_ERROR;
    bool available = false;
    int count = 0, error = 0;
    book_file_version_t versions[BOOKSHELF_MAX_ITEMS];
    uint32_t split_versions[BOOKSHELF_MAX_ITEMS] = {0};
    uint32_t directories[STORAGE_COUNT] = {0};
    uint32_t session = storage_card_session(), revision = storage_revision();
    for (unsigned volume = 0; volume < STORAGE_COUNT; ++volume)
    {
        if (storage_changing() || (cancelled && cancelled(user))) { error = -RT_EIO; break; }
        storage_lock();
        if (revision != storage_revision() || session != storage_card_session())
        { storage_unlock(); error = -RT_EIO; break; }
        if (!storage_available(volume)) { storage_unlock(); continue; }
        const char *base = storage_root(volume);
        directories[volume] = storage_directory_revision(base);
        DIR *dir = opendir(base);
        storage_unlock();
        if (!dir) { error = -RT_EIO; break; }
        available = true;
        struct dirent *entry;
        while (count < BOOKSHELF_MAX_ITEMS)
        {
            if (storage_changing() || revision != storage_revision() || session != storage_card_session() ||
                (cancelled && cancelled(user))) { error = -RT_EIO; break; }
            storage_lock();
            if (!storage_session_valid(volume, session) || revision != storage_revision())
            { storage_unlock(); error = -RT_EIO; break; }
            errno = 0;
            entry = readdir(dir);
            if (!entry) { if (errno) error = -RT_EIO; storage_unlock(); break; }
            if (entry->d_name[0] == '.') { storage_unlock(); continue; }
            char full[BOOKSHELF_PATH_MAX];
            struct stat st;
            if (strlen(base) + 1 + strlen(entry->d_name) >= sizeof(full)) { storage_unlock(); continue; }
            rt_snprintf(full, sizeof(full), "%s/%s", base, entry->d_name);
            uint32_t before_stat = storage_volume_revision(volume);
            if (stat(full, &st) != 0) { storage_unlock(); error = -RT_EIO; break; }
            storage_unlock();
            document_format_t format = document_format(full, S_ISDIR(st.st_mode));
            if (format == DOCUMENT_UNKNOWN || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) continue;
            book_file_version_t version = book_file_version(full);
            if (version.storage != before_stat) { error = -RT_EIO; break; }
            if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) { error = -RT_EIO; break; }
            uint32_t modified = (uint32_t)st.st_mtime;
            uint32_t size = (uint32_t)st.st_size;
            split_versions[count] = 0;
            if (S_ISDIR(st.st_mode))
            {
                split_versions[count] = storage_volume_revision(volume);
                if (!document_split_identity(full, &size, &modified)) continue;
            }
            else if (volume == STORAGE_FLASH && !bookshelf_file_signature(full, size, &modified))
            { error = -RT_EIO; break; }
            if (!book_file_version_valid(full, &version)) { error = -RT_EIO; break; }
            versions[count] = version;
            bookshelf_item_t *item = &items[count++];
            memset(item, 0, sizeof(*item));
            rt_snprintf(item->path, sizeof(item->path), "%s", full);
            const char *dot = S_ISDIR(st.st_mode) ? NULL : strrchr(entry->d_name, '.');
            size_t length = dot ? (size_t)(dot - entry->d_name) : strlen(entry->d_name);
            if (length >= sizeof(item->name))
            {
                length = sizeof(item->name) - 1;
                while (length && (((unsigned char)entry->d_name[length] & 0xC0) == 0x80)) --length;
            }
            memcpy(item->name, entry->d_name, length);
            item->name[length] = '\0';
            item->size = size;
            item->modified = modified;
            item->format = format;
        }
        storage_lock();
        if (closedir(dir) != 0 || revision != storage_revision() || session != storage_card_session()) error = -RT_EIO;
        storage_unlock();
        if (error) break;
    }
    if (error || !available || (cancelled && cancelled(user))) return -RT_EIO;
    for (unsigned volume = 0; volume < STORAGE_COUNT; ++volume)
        if (directories[volume] && directories[volume] != storage_directory_revision(storage_root(volume))) return -RT_EIO;
    for (int i = 0; i < count; ++i)
    {
        storage_volume_t volume = !strcmp(storage_path_root(items[i].path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
        if (!book_file_version_valid(items[i].path, &versions[i]) ||
            (split_versions[i] && split_versions[i] != storage_volume_revision(volume))) return -RT_EIO;
    }
    for (int i = 1; i < count; ++i)
    {
        bookshelf_item_t item = items[i];
        int j = i - 1;
        while (j >= 0 && strcmp(items[j].name, item.name) > 0)
        {
            items[j + 1] = items[j];
            --j;
        }
        items[j + 1] = item;
    }
    return count;
}

int bookshelf_refresh(void)
{
    bookshelf_item_t *items = epd_app_alloc(sizeof(g_items), EPD_APP_PSRAM);
    if (!items) return -RT_ENOMEM;
    int count = bookshelf_scan(items, NULL, NULL);
    if (count >= 0)
    {
        rt_mutex_take(g_lock, RT_WAITING_FOREVER);
        memcpy(g_items, items, count * sizeof(*items));
        g_count = count;
        rt_mutex_release(g_lock);
    }
    epd_app_free(items);
    if (count < 0) return count;
    bookshelf_notify();
    return count;
}

int bookshelf_count(void)
{
    int count;

    if (!g_inited)
        return 0;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    count = g_count;
    rt_mutex_release(g_lock);
    return count;
}

int bookshelf_list(bookshelf_item_t *items, int max_items)
{
    int count;

    if (items == RT_NULL || max_items <= 0 || !g_inited)
        return 0;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    count = (g_count < max_items) ? g_count : max_items;
    if (count > 0)
        memcpy(items, g_items, (rt_size_t)count * sizeof(bookshelf_item_t));
    rt_mutex_release(g_lock);

    return count;
}

bool bookshelf_get(int index, bookshelf_item_t *out)
{
    bool ret = false;

    if (out == RT_NULL || !g_inited)
        return false;

    rt_mutex_take(g_lock, RT_WAITING_FOREVER);
    if (index >= 0 && index < g_count)
    {
        *out = g_items[index];
        ret = true;
    }
    rt_mutex_release(g_lock);

    return ret;
}

const char *bookshelf_dir(void)
{
    return g_dir;
}

void bookshelf_set_event_cb(bookshelf_event_cb_t cb)
{
    g_event_cb = cb;
}

void bookshelf_service_deinit(void)
{
    if (!g_inited) return;
    g_event_cb = NULL;
    g_count = 0;
    rt_mutex_delete(g_lock);
    g_lock = NULL;
    g_inited = RT_FALSE;
}
