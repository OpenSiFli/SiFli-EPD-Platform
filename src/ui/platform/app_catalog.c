#include "app_service_internal.h"
#include "storage_internal.h"
#include "storage_file_internal.h"
#include "epd_memory.h"
#include "mem_section.h"
#include "app_memory_pressure.h"
#include "platform/epd_locale.h"
#include "app_catalog.h"
#include "app_package.h"
#include "app_installer.h"
#include "app_module.h"
#include "app_recovery.h"
#include "epd_app.h"
#include "epd_i18n.h"
#include "ui_reader_font_cache.h"
#include "storage.h"
#include "portal.h"
#include "ai_settings.h"
#include "speech/speech_internal.h"
#include "speech/speech_settings.h"
#include "icons/ui_icons.h"
#include "gui_app_int.h"
#include <dfs_posix.h>
#include <dlmodule.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define DBG_TAG "apps"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static epd_app_entry_t *entries;
static unsigned entry_count;
static uint32_t catalog_revision;
static uint32_t background_revision = UINT32_MAX;
typedef struct
{
    char id[RT_NAME_MAX], root[96];
    app_package_revision_t package_revision;
    uint32_t catalog_revision, storage_revision;
    size_t peak;
} app_load_estimate_t;
L2_RET_BSS_SECT_BEGIN(app_load_estimates)
static app_load_estimate_t load_estimates[4] L2_RET_BSS_SECT(app_load_estimates);
L2_RET_BSS_SECT_END
static unsigned load_estimate_next;

/* Retained PSRAM is outside the startup zero table. Clear on boot;
 * ordinary sleep/resume keeps estimates and their revision checks. */
static int app_load_estimates_init(void)
{
    memset(load_estimates, 0, sizeof(load_estimates));
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(app_load_estimates_init);

static void *array_grow(void *items, unsigned *capacity, unsigned count, size_t item_size)
{
    if (count < *capacity) return items;
    if (count == UINT_MAX || (size_t)count + 1 > SIZE_MAX / item_size) return NULL;
    unsigned needed = count + 1;
    unsigned size = *capacity && *capacity <= UINT_MAX / 2 ? *capacity * 2 : needed;
    if (size < needed || size > SIZE_MAX / item_size) size = needed;
    void *next = epd_app_realloc(items, size * item_size, EPD_APP_PSRAM);
    if (!next && size != needed)
    {
        size = needed;
        next = epd_app_realloc(items, size * item_size, EPD_APP_PSRAM);
    }
    if (next) *capacity = size;
    return next;
}

void app_catalog_changed(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    ++catalog_revision;
    rt_hw_interrupt_enable(level);
}

uint32_t app_catalog_revision(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t revision = catalog_revision;
    rt_hw_interrupt_enable(level);
    return revision;
}

static bool valid_id(const char *id)
{
    size_t n = strlen(id);
    if (!n || n >= GUI_APP_ID_MAX_LEN || n >= RT_NAME_MAX) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '_'))
            return false;
    return true;
}

static bool builtin_id(const char *id)
{
    const builtin_app_desc_t *item = gui_builtin_app_list_open();
    bool found = false;
    while (item)
    {
        if (!strcmp(item->id, id)) { found = true; break; }
        item = gui_builtin_app_list_get_next(item);
    }
    gui_builtin_app_list_close(item);
    return found;
}

static void remember_load_required(const char *root, const char *id, size_t peak,
                                  const app_package_revision_t *verified_revision)
{
    app_package_revision_t current;
    if (!peak || strlen(id) >= sizeof(load_estimates[0].id) ||
        strlen(root) >= sizeof(load_estimates[0].root) ||
        !app_package_revision(root, id, NULL, &current) ||
        memcmp(&current, verified_revision, sizeof(current))) return;
    app_load_estimate_t *entry = NULL;
    for (unsigned i = 0; i < sizeof(load_estimates) / sizeof(load_estimates[0]); ++i)
        if (!strcmp(load_estimates[i].id, id)) { entry = &load_estimates[i]; break; }
    if (!entry) entry = &load_estimates[load_estimate_next++ %
                                     (sizeof(load_estimates) / sizeof(load_estimates[0]))];
    strcpy(entry->root, root);
    strcpy(entry->id, id);
    entry->package_revision = current;
    entry->catalog_revision = app_catalog_revision();
    entry->storage_revision = storage_revision();
    entry->peak = peak;
}

size_t app_catalog_load_required(const char *id)
{
    if (!id || !valid_id(id) || builtin_id(id) || dlmodule_find(id)) return 0;
    for (unsigned i = 0; i < sizeof(load_estimates) / sizeof(load_estimates[0]); ++i)
    {
        const app_load_estimate_t *entry = &load_estimates[i];
        if (strcmp(entry->id, id)) continue;
        app_package_revision_t current;
        if (entry->catalog_revision != app_catalog_revision() ||
            entry->storage_revision != storage_revision() ||
            !app_package_revision(entry->root, id, NULL, &current) ||
            memcmp(&current, &entry->package_revision, sizeof(current))) return 0;
        return entry->peak;
    }
    return 0;
}

static bool package_read(const char *directory, const char *id, epd_app_entry_t *entry, const char *locale)
{
    if (!valid_id(id) || !storage_path_available(directory)) return false;
    char root[96];
    int n = snprintf(root, sizeof(root), "%s/%s", directory, id);
    if (n <= 0 || n >= sizeof(root) || !app_package_metadata_locale(root, id, entry, locale)) return false;
    entry->volume = !strcmp(directory, STORAGE_SD_APPS) ? STORAGE_SD : STORAGE_FLASH;
    /* Resource-only packages may supply a built-in app, never replace its code. */
    return entry->resources_only ? (builtin_id(id) || !strcmp(id, "weather")) : !builtin_id(id);
}

bool app_package_read(const char *directory, const char *id, epd_app_entry_t *entry)
{
    return package_read(directory, id, entry, NULL);
}

bool app_package_read_display(const char *directory, const char *id, epd_app_entry_t *entry)
{
    return package_read(directory, id, entry, epd_locale_get());
}

bool app_catalog_lockscreen_available(const char *id)
{
    if (!id || !valid_id(id) || app_installer_blocks(id)) return false;
    epd_app_entry_t entry;
    storage_volume_t volume;
    storage_lock();
    bool available = storage_app_locate(id, &volume) &&
                     app_installer_volume_ready(volume) && storage_apps_enabled(volume) &&
                     app_package_read(storage_app_directory(volume), id, &entry) &&
                     !entry.resources_only && entry.lockscreen;
    storage_unlock();
    return available;
}

static bool module_open(const char *id, app_entity_info *info)
{
    info->entry_func = info->user_data = 0;
    rt_err_t policy = app_recovery_check_dynamic();
    if (policy != RT_EOK)
    {
        /* dlopen executes module_init, so reject before even opening the ELF. */
        rt_set_errno(policy);
        LOG_W("app %s: recovery policy blocked load: %d", id, policy);
        return false;
    }
    if (app_installer_blocks(id)) return false;
    /* A cancelled worker must retire before pages can reuse its module state. */
    if (app_service_app_stopping(id)) return false;
    epd_app_entry_t entry;
    storage_lock();
    storage_volume_t volume;
    bool found = storage_app_locate(id, &volume) &&
                 app_package_read(storage_app_directory(volume), id, &entry);
    char root[96];
    if (found) found = storage_app_path(root, sizeof(root), id, STORAGE_APP_CODE, "");
    storage_unlock();
    size_t load_peak = 0;
    app_package_revision_t load_revision;
    bool load_revision_valid = found && app_package_revision(root, id, NULL, &load_revision);
    app_package_status_t status = found ? app_package_verify(root, id, false, &load_peak) : APP_PACKAGE_FORMAT_ERROR;
    if (status != APP_PACKAGE_OK)
    {
        LOG_W("app %s: preflight failed: %s", id, app_package_error(status));
        return false;
    }
    if (load_revision_valid) remember_load_required(root, id, load_peak, &load_revision);
    char path[96];
    int path_length = snprintf(path, sizeof(path), "%s/%s.so", root, id);
    if (path_length <= 0 || (size_t)path_length >= sizeof(path)) return false;
    /* Only a new ELF load needs headroom; reusing a module must not discard
     * its reader's warm cache on every page entry. */
    if (!dlmodule_find(id)) ui_reader_font_cache_reserve(load_peak);
    struct rt_dlmodule *module = app_module_open(path, id);
    if (!module)
    {
        LOG_W("app %s: ELF load failed", id);
        return false;
    }
    const epd_app_info_t *descriptor = dlsym(module, "epd_app_info");
    void *entry_point = dlsym(module, "app_main");
    if (!descriptor || descriptor->abi != EPD_APP_ABI || !descriptor->id ||
        strcmp(descriptor->id, id) || !entry_point)
    {
        LOG_W("app %s: descriptor/entry mismatch", id);
        app_module_close(module);
        return false;
    }
    if (entry.background && !app_service_running(id))
    {
        const epd_background_t *background = dlsym(module, "epd_app_background");
        if (!background || !app_service_start(id, background, module))
        {
            LOG_W("app %s: background start failed", id);
            app_module_close(module);
            return false;
        }
    }
    app_service_set_background(id, entry.background);
    info->entry_func = (uint32_t)entry_point;
    info->user_data = (uint32_t)module;
    return true;
}

static bool module_close(const char *id, const app_entity_info *info)
{
    /* SDK built-in descriptors do not initialize a dynamic module handle. */
    if (builtin_id(id)) return false;
    if (info->user_data)
    {
        app_module_close((void *)info->user_data);
    }
    return true;
}

void app_catalog_init(void)
{
    rt_err_t result = app_recovery_init();
    if (result != RT_EOK) LOG_W("recovery record unavailable: %d; dynamic apps disabled", result);
    gui_app_register_dl_open(module_open);
    gui_app_register_dl_close(module_close);
}

void app_catalog_background_resume(void)
{
    background_revision = UINT32_MAX;
}

void app_catalog_process(void)
{
    if (app_memory_pressure_background_blocked() || app_recovery_check_dynamic() != RT_EOK ||
        storage_changing() || app_installer_busy() || app_service_stopping() ||
        background_revision == app_catalog_revision()) return;
    background_revision = app_catalog_revision();
    /* Do not keep storage locked while starting code that may use the filesystem. */
    char (*ids)[RT_NAME_MAX] = NULL;
    unsigned count = 0, capacity = 0;
    bool ok = true;
    storage_lock();
    for (storage_volume_t v = STORAGE_FLASH; ok && v < STORAGE_COUNT; ++v)
    {
        if (!app_installer_volume_ready(v) || !storage_apps_enabled(v)) continue;
        const char *directory = storage_app_directory(v);
        DIR *dir = opendir(directory);
        if (!dir) continue;
        struct dirent *file;
        epd_app_entry_t entry;
        while ((file = readdir(dir)) != NULL)
        {
            storage_volume_t selected;
            if (app_package_read(directory, file->d_name, &entry) && entry.background && !entry.resources_only &&
                storage_app_locate(entry.id, &selected) && selected == v)
            {
                void *next = array_grow(ids, &capacity, count, sizeof(*ids));
                if (!next) { ok = false; break; }
                ids = next;
                size_t id_length = strlen(entry.id);
                if (id_length >= RT_NAME_MAX) continue;
                memcpy(ids[count++], entry.id, id_length + 1);
            }
        }
        closedir(dir);
    }
    storage_unlock();
    if (!ok) LOG_W("background discovery: allocation failed");
    for (unsigned i = 0; ok && i < count; ++i)
    {
        if (app_memory_pressure_background_blocked()) break;
        if (app_service_running(ids[i])) continue;
        app_entity_info info;
        if (module_open(ids[i], &info)) module_close(ids[i], &info);
    }
    epd_app_free(ids);
}

bool app_catalog_release_storage(void)
{
    bool portal_idle = portal_release_storage();
    ai_settings_stop();
    speech_settings_stop();
    speech_stop_all();
    speech_process();
    /* An installer waiting for UI preparation must be released before unmount. */
    app_installer_process();
    app_service_stop_modules();
    app_service_process();
    rt_list_t *cursor = NULL;
    gui_runing_app_t *app;
    bool exiting = false;
    /* Enqueue exits first; execution destroys nodes in the framework-owned list. */
    while ((app = gui_app_trav(&cursor)) != NULL)
        if (!builtin_id(app->id))
        {
            gui_app_exit(app->id);
            exiting = true;
        }
    if (exiting) gui_app_exec_now();
    cursor = NULL;
    while ((app = gui_app_trav(&cursor)) != NULL)
        if (!builtin_id(app->id)) return false;
    background_revision = UINT32_MAX;
    return portal_idle && ai_settings_idle() && speech_settings_idle() && speech_idle() &&
           !app_service_modules_running() && !app_service_running("reader") &&
           !app_modules_present() && !app_installer_busy();
}

static int entry_order(const epd_app_entry_t *entry)
{
    static const char *const order[] = {"books", "weather", "settings"};
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
        if (!strcmp(entry->id, order[i])) return i;
    return 100;
}

bool app_catalog_refresh(void)
{
    epd_app_entry_t *next = NULL;
    unsigned count = 0, capacity = 0;
    bool ok = true;
    const builtin_app_desc_t *item = gui_builtin_app_list_open();
    while (item)
    {
        if (item->icon)
        {
            void *grown = array_grow(next, &capacity, count, sizeof(*next));
            if (!grown) { ok = false; break; }
            next = grown;
            memset(&next[count], 0, sizeof(next[count]));
            snprintf(next[count].id, sizeof(next[count].id), "%s", item->id);
            const char *name = LV_EXT_STR_GET(item->name);
            if (!strcmp(item->id, "settings")) name = epd_get_str(platform_app_settings);
            else if (!strcmp(item->id, "portal")) name = epd_get_str(platform_app_portal);
            snprintf(next[count].name, sizeof(next[count].name), "%s", name);
            next[count++].icon = item->icon;
        }
        item = gui_builtin_app_list_get_next(item);
    }
    gui_builtin_app_list_close(item);
    storage_lock();
    for (storage_volume_t v = STORAGE_FLASH; ok && v < STORAGE_COUNT; ++v)
    {
        if (!app_installer_volume_ready(v) || !storage_apps_enabled(v)) continue;
        const char *directory = storage_app_directory(v);
        DIR *dir = opendir(directory);
        if (!dir) continue;
        struct dirent *file;
        while ((file = readdir(dir)) != NULL)
        {
            storage_volume_t selected;
            epd_app_entry_t entry;
            if (app_package_read_display(directory, file->d_name, &entry) && !entry.resources_only &&
                storage_app_locate(entry.id, &selected) && selected == v)
            {
                void *grown = array_grow(next, &capacity, count, sizeof(*next));
                if (!grown) { ok = false; break; }
                next = grown;
                next[count++] = entry;
            }
        }
        closedir(dir);
    }
    storage_unlock();
    if (!ok)
    {
        epd_app_free(next);
        LOG_W("catalog refresh: allocation failed; keeping previous snapshot");
        return false;
    }
    for (unsigned i = 1; i < count; ++i)
    {
        epd_app_entry_t value = next[i];
        unsigned j = i;
        while (j && (entry_order(&next[j - 1]) > entry_order(&value) ||
              (entry_order(&next[j - 1]) == entry_order(&value) &&
               strcmp(next[j - 1].id, value.id) > 0)))
        {
            next[j] = next[j - 1];
            --j;
        }
        next[j] = value;
    }
    epd_app_free(entries);
    entries = next;
    entry_count = count;
    return true;
}

unsigned app_catalog_count(void) { return entry_count; }

const epd_app_entry_t *app_catalog_get(unsigned index)
{
    return index < entry_count ? &entries[index] : NULL;
}
