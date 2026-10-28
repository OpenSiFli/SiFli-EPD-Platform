#include "storage_internal.h"
#include "storage_file_internal.h"
#include "app_content.h"
#include "epd_i18n.h"
#include "app_catalog.h"
#include "app_package.h"
#include "app_installer.h"
#include "app_recovery.h"
#include "storage.h"
#include <dfs_posix.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static uint32_t request_serial;

static bool valid_path(const char *path)
{
    if (!path || (strncmp(path, STORAGE_FLASH_ROOT "/", sizeof(STORAGE_FLASH_ROOT)) &&
                 strncmp(path, STORAGE_SD_ROOT "/", sizeof(STORAGE_SD_ROOT)))) return false;
    const char *part = path + 1;
    for (const char *p = part; ; ++p)
    {
        if (*p == '/' || !*p)
        {
            size_t length = (size_t)(p - part);
            if (!length || (length == 1 && part[0] == '.') ||
                (length == 2 && part[0] == '.' && part[1] == '.')) return false;
            if (!*p) return true;
            part = p + 1;
        }
    }
}

static rt_err_t file_ready(const char *path)
{
    if (!valid_path(path)) return -RT_EINVAL;
    if (storage_changing() || !storage_path_available(path)) return -RT_EIO;
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? RT_EOK : -RT_EIO;
}

static bool handler_ready(const epd_app_entry_t *entry, const char *path)
{
    if (!entry || entry->resources_only || !entry->handles_content ||
        app_installer_blocks(entry->id)) return false;
    storage_volume_t volume;
    if (!storage_app_locate(entry->id, &volume) || volume != entry->volume ||
        !app_installer_volume_ready(volume) || !storage_apps_enabled(volume)) return false;
    char root[96];
    return storage_app_path(root, sizeof(root), entry->id, STORAGE_APP_CODE, "") &&
           app_package_handles_path(root, path);
}

rt_err_t app_content_query(const char *path, unsigned first,
                           app_content_handler_t *items, unsigned capacity, unsigned *count)
{
    if (!count || (capacity && !items)) return -RT_EINVAL;
    *count = 0;
    storage_lock();
    rt_err_t result = file_ready(path);
    if (result == RT_EOK)
    {
        unsigned total = app_catalog_count();
        for (unsigned i = 0; i < total; ++i)
        {
            const epd_app_entry_t *entry = app_catalog_get(i);
            if (!handler_ready(entry, path)) continue;
            unsigned index = (*count)++;
            if (index < first || index - first >= capacity) continue;
            app_content_handler_t *out = &items[index - first];
            memcpy(out->id, entry->id, sizeof(out->id));
            memcpy(out->name, entry->name, sizeof(out->name));
        }
    }
    storage_unlock();
    return result;
}

static rt_err_t intent_string(intent_t intent, const char *key, const char *value)
{
    int result = intent_set_string(intent, key, value);
    if (result == RT_EOK) return RT_EOK;
    return result == RT_ENOMEM ? -RT_ENOMEM : -ENAMETOOLONG;
}

rt_err_t app_content_open(const char *id, const char *path)
{
    if (!id || !*id || !valid_path(path)) return -RT_EINVAL;
    rt_err_t result = app_recovery_check_dynamic();
    if (result != RT_EOK) return result;
    storage_lock();
    result = file_ready(path);
    bool found = false;
    if (result == RT_EOK)
    {
        unsigned total = app_catalog_count();
        for (unsigned i = 0; i < total; ++i)
        {
            const epd_app_entry_t *entry = app_catalog_get(i);
            if (entry && !strcmp(entry->id, id) && handler_ready(entry, path))
            { found = true; break; }
        }
        if (!found) result = -ENOENT;
    }
    intent_t intent = NULL;
    if (result == RT_EOK)
    {
        intent = intent_init(id);
        if (!intent) result = -RT_ENOMEM;
    }
    if (result == RT_EOK) result = intent_string(intent, "path", path);
    char number[9];
    if (result == RT_EOK && !strcmp(storage_path_root(path), STORAGE_SD_ROOT))
    {
        snprintf(number, sizeof(number), "%lx", (unsigned long)storage_card_session());
        result = intent_string(intent, "s", number);
    }
    if (result == RT_EOK)
    {
        snprintf(number, sizeof(number), "%lx", (unsigned long)++request_serial);
        result = intent_string(intent, "r", number);
    }
    /* The SDK copies the intent into its queue. Caller buffers and catalog/UI
     * pointers are never retained by the queued content request. */
    if (result == RT_EOK) result = intent_runapp(intent);
    intent_deinit(intent);
    storage_unlock();
    return result;
}

rt_err_t app_content_receive(intent_t intent, char *path, size_t size)
{
    if (!path || !size) return -RT_EINVAL;
    path[0] = 0;
    if (!intent) return -ENOENT;
    const char *value = intent_get_string(intent, "path");
    if (!value) return -ENOENT;
    if (!valid_path(value)) return -RT_EINVAL;
    size_t length = strlen(value);
    if (length >= size) return -ENAMETOOLONG;
    storage_lock();
    rt_err_t result = file_ready(value);
    if (result == RT_EOK && !strcmp(storage_path_root(value), STORAGE_SD_ROOT))
    {
        char session[9];
        snprintf(session, sizeof(session), "%lx", (unsigned long)storage_card_session());
        const char *provided = intent_get_string(intent, "s");
        if (!provided || strcmp(provided, session)) result = -RT_EIO;
    }
    if (result == RT_EOK) memcpy(path, value, length + 1);
    storage_unlock();
    return result;
}

const char *app_content_error(rt_err_t error)
{
    if (error == -ENAMETOOLONG) return epd_get_str(platform_content_long);
    if (error == APP_RECOVERY_BLOCKED) return epd_get_str(platform_content_recovery);
    if (error == -RT_ENOMEM) return epd_get_str(platform_content_memory);
    if (error == -ENOENT) return epd_get_str(platform_content_handler);
    if (error == -RT_EIO) return epd_get_str(platform_content_storage);
    if (error == -RT_EINVAL) return epd_get_str(platform_content_path);
    return epd_get_str(platform_content_failed);
}
