/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_internal.h"
#include "app_module.h"
#include <dlmodule.h>
#include <dlfcn.h>
#include <rthw.h>
#include <string.h>
#include <stdio.h>
#include "lv_ext_resource_manager.h"

typedef struct app_module
{
    struct app_module *next;
    void *module;
    char name[RT_NAME_MAX];
    app_package_revision_t revision;
    size_t load_peak;
} app_module_t;

typedef struct app_instance
{
    struct app_instance *next;
    char id[RT_NAME_MAX];
    char root[96];
    storage_volume_t volume;
    uint32_t session;
    app_package_revision_t installation;
    app_module_t *modules;
} app_instance_t;

static app_instance_t *instances;
static struct rt_mutex module_lifecycle;
static bool lifecycle_ready;

static bool lifecycle_lock(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!lifecycle_ready)
        lifecycle_ready = rt_mutex_init(&module_lifecycle, "app_module", RT_IPC_FLAG_PRIO) == RT_EOK;
    bool ready = lifecycle_ready;
    rt_hw_interrupt_enable(level);
    return ready && rt_mutex_take(&module_lifecycle, RT_WAITING_FOREVER) == RT_EOK;
}

static bool installation_matches(const app_package_revision_t *installation, const app_package_revision_t *revision)
{
    /* The first three paths are root, app.json and files.sha256 for every
     * module; the executable path at index 3 is deliberately not shared. */
    return revision->session == installation->session &&
           revision->state == installation->state &&
           !memcmp(revision->paths, installation->paths, 3 * sizeof(uint32_t));
}

int app_module_verified(const char *root, const char *id, const char *library, size_t *peak)
{
    bool found = false, matching_root = false, resident = false;
    app_package_revision_t installation, saved, current;
    size_t load_peak = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    for (app_instance_t *instance = instances; instance; instance = instance->next)
    {
        if (strcmp(instance->id, id)) continue;
        found = true;
        matching_root = !strcmp(instance->root, root);
        installation = instance->installation;
        for (app_module_t *record = instance->modules; record; record = record->next)
        {
            if (strcmp(record->name, library ? library : id)) continue;
            saved = record->revision;
            load_peak = record->load_peak;
            resident = true;
            break;
        }
        break;
    }
    rt_hw_interrupt_enable(level);
    if (!found) return 0;
    if (!matching_root || !app_package_revision(root, id, library, &current) ||
        !installation_matches(&installation, &current)) return -1;
    if (!resident) return 0;
    if (memcmp(&current, &saved, sizeof(current))) return -1;
    if (peak) *peak = load_peak;
    return 1;
}

void app_modules_set_locale(const char *locale)
{
    if (!lifecycle_lock()) return;
    for (app_instance_t *instance = instances; instance; instance = instance->next)
        for (app_module_t *item = instance->modules; item; item = item->next)
        {
            lv_ext_res_mng_t manager = dlmodule_get_res_mng(item->module);
            if (!manager) continue;
            if (lv_ext_set_locale(manager, locale) == LV_RES_OK) continue;
            const lv_i18n_lang_pack_t *pack = lv_ext_get_lang_pack(manager);
            if (pack && pack[0]) lv_ext_set_locale(manager, pack[0]->locale);
        }
    rt_mutex_release(&module_lifecycle);
}

bool app_module_storage(const char *id, storage_volume_t *volume, uint32_t *session)
{
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (app_instance_t *instance = instances; instance; instance = instance->next)
        if (!strcmp(instance->id, id))
        {
            *volume = instance->volume;
            *session = instance->session;
            found = true;
            break;
        }
    rt_hw_interrupt_enable(level);
    return found;
}

bool app_modules_present(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool present = instances != NULL;
    rt_hw_interrupt_enable(level);
    return present;
}

bool app_module_loaded(const char *id)
{
    bool found = false;
    rt_base_t level = rt_hw_interrupt_disable();
    for (app_instance_t *instance = instances; instance; instance = instance->next)
        if (!strcmp(instance->id, id))
        {
            found = true;
            break;
        }
    rt_hw_interrupt_enable(level);
    return found;
}

static void identity_path(const app_instance_t *instance, unsigned index, char path[160])
{
    static const char *suffix[] = {"", "/app.json", "/files.sha256", "/codecs", ""};
    snprintf(path, 160, "%s%s", instance->root, suffix[index]);
    if (index == 4) *strrchr(path, '/') = 0;
}

static bool instance_pin(app_instance_t *instance)
{
    char path[160];
    unsigned count = 0;
    for (; count < 5; ++count)
    {
        identity_path(instance, count, path);
        if (!storage_path_pin(path)) break;
    }
    if (count == 5) return true;
    while (count)
    {
        identity_path(instance, --count, path);
        storage_path_unpin(path);
    }
    return false;
}

static void record_unpin(const app_instance_t *instance, const app_module_t *record)
{
    char path[160];
    snprintf(path, sizeof(path), strcmp(instance->id, record->name) ? "%s/codecs/%s.so" : "%s/%s.so",
             instance->root, record->name);
    storage_path_unpin(path);
}

static void instance_release_empty(app_instance_t *instance)
{
    if (instance->modules) return;
    rt_base_t level = rt_hw_interrupt_disable();
    app_instance_t **link = &instances;
    while (*link && *link != instance) link = &(*link)->next;
    if (*link) *link = instance->next;
    rt_hw_interrupt_enable(level);
    char path[160];
    for (unsigned i = 0; i < 5; ++i)
    {
        identity_path(instance, i, path);
        storage_path_unpin(path);
    }
    rt_free(instance);
}

static void *module_open_serialized(const char *path, const char *id)
{
    if (!path || !id || !*id || strlen(id) >= RT_NAME_MAX) return NULL;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t length = strlen(name);
    if (length <= 3 || strcmp(name + length - 3, ".so") || length - 3 >= RT_NAME_MAX) return NULL;
    char module_name[RT_NAME_MAX];
    memcpy(module_name, name, length - 3);
    module_name[length - 3] = 0;
    const char *library = strcmp(module_name, id) ? module_name : NULL;
    app_instance_t *instance = instances;
    while (instance && strcmp(instance->id, id)) instance = instance->next;
    app_module_t *record = instance ? instance->modules : NULL;
    while (record && strcmp(record->name, module_name)) record = record->next;
    bool fresh = instance == NULL;
    char root[96], expected[160];
    if (instance) strcpy(root, instance->root);
    else if (!storage_app_path(root, sizeof(root), id, STORAGE_APP_CODE, "")) return NULL;
    int n = snprintf(expected, sizeof(expected), library ? "%s/codecs/%s.so" : "%s/%s.so",
                     root, module_name);
    if (n < 0 || (size_t)n >= sizeof(expected) || strcmp(path, expected)) return NULL;
    storage_volume_t volume = !strcmp(storage_path_root(path), STORAGE_SD_ROOT) ? STORAGE_SD : STORAGE_FLASH;
    uint32_t session = volume == STORAGE_SD ? storage_card_session() : 0;
    if (!storage_apps_enabled(volume) || (!fresh &&
        (instance->volume != volume || strcmp(instance->root, root) ||
         !storage_session_valid(volume, instance->session)))) return NULL;
    /* The SDK keys loaded modules by basename, not by installation directory. */
    struct rt_dlmodule *existing = dlmodule_find(module_name);
    if (existing && (!record || record->module != existing)) return NULL;
    if (fresh)
    {
        instance = rt_calloc(1, sizeof(*instance));
        if (!instance) return NULL;
        strcpy(instance->id, id);
        strcpy(instance->root, root);
        instance->volume = volume;
        instance->session = session;
        if (!instance_pin(instance)) { rt_free(instance); return NULL; }
    }
    bool new_record = record == NULL;
    if (new_record)
    {
        record = rt_calloc(1, sizeof(*record));
        if (!record || !storage_path_pin(path))
        {
            rt_free(record);
            instance_release_empty(instance);
            return NULL;
        }
        strcpy(record->name, module_name);
    }
    app_package_revision_t revision, current;
    size_t peak = 0;
    if (!app_package_revision(root, id, library, &revision) || revision.session != session ||
        (!fresh && !installation_matches(&instance->installation, &revision))) goto failed;
    if (!new_record)
    {
        if (existing != record->module || memcmp(&revision, &record->revision, sizeof(revision))) goto failed;
        peak = record->load_peak;
    }
    else
    {
        app_package_status_t status = library ? app_package_verify_library(root, id, library, &peak) :
                                               app_package_verify(root, id, false, &peak);
        if (status != APP_PACKAGE_OK || !app_package_revision(root, id, library, &current) ||
            memcmp(&revision, &current, sizeof(revision))) goto failed;
    }
    if (fresh)
    {
        instance->installation = revision;
        rt_base_t level = rt_hw_interrupt_disable();
        instance->next = instances;
        instances = instance;
        rt_hw_interrupt_enable(level);
    }
    struct rt_dlmodule *module = dlopen(path, 0);
    /* Removal may race a successful final read. Never run its entry afterward. */
    if (module && (!storage_session_valid(volume, session) ||
        !app_package_revision(root, id, library, &current) || memcmp(&revision, &current, sizeof(revision))))
    { dlclose(module); module = NULL; }
    rt_base_t level = rt_hw_interrupt_disable();
    if (module)
    {
        record->module = module;
        record->revision = revision;
        record->load_peak = peak;
        strcpy(record->name, module_name);
        if (new_record)
        {
            record->next = instance->modules;
            instance->modules = record;
        }
    }
    rt_hw_interrupt_enable(level);
    if (!module) goto failed;
    return module;
failed:
    if (new_record)
    {
        record_unpin(instance, record);
        rt_free(record);
    }
    instance_release_empty(instance);
    return NULL;
}

static void module_close_serialized(void *module)
{
    if (!module) return;
    app_instance_t *instance = NULL;
    app_module_t *record = NULL;
    for (app_instance_t *item = instances; item && !record; item = item->next)
        for (app_module_t *m = item->modules; m; m = m->next)
            if (m->module == module)
            { instance = item; record = m; break; }
    dlclose(module);
    /* Background services may still hold nref. Only retire a destroyed module. */
    if (record && dlmodule_find(record->name) != module)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        app_module_t **link = &instance->modules;
        while (*link != record) link = &(*link)->next;
        *link = record->next;
        rt_hw_interrupt_enable(level);
        record_unpin(instance, record);
        rt_free(record);
        instance_release_empty(instance);
    }
}

void *app_module_open(const char *path, const char *id)
{
    if (!lifecycle_lock()) return NULL;
    void *module = module_open_serialized(path, id);
    rt_mutex_release(&module_lifecycle);
    return module;
}

void app_module_close(void *module)
{
    if (!module || !lifecycle_lock()) return;
    module_close_serialized(module);
    rt_mutex_release(&module_lifecycle);
}
