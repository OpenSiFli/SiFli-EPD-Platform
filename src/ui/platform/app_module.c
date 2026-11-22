/* SPDX-License-Identifier: Apache-2.0 */
#include "storage_internal.h"
#include "app_module.h"
#include "app_memory.h"
#include "epd_input.h"
#include <dlmodule.h>
#include <dlfcn.h>
#include <rthw.h>
#include <string.h>
#include <stdio.h>
#include "lv_ext_resource_manager.h"
#include "platform/epd_operation_runtime.h"
#include "platform/epd_resource_runtime.h"
#include "platform/epd_cold_runtime.h"
#include "app_background.h"
#include "app_service_internal.h"
#include <lvgl.h>

typedef struct app_module
{
    struct app_module *next;
    void *module;
    char name[RT_NAME_MAX];
    app_package_revision_t revision;
    size_t load_peak;
    bool close_pending;
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
    epd_memory_owner_t owner;
    bool background_bound;
    bool closing;
    bool close_prepared;
    bool resources_closing;
    bool eviction_requested;
    bool termination_requested;
    bool aborting;
    epd_app_background_state_t terminal_state;
} app_instance_t;

static app_instance_t *instances;
static struct rt_mutex module_lifecycle;
static bool lifecycle_ready;
static bool retire_pending;
static void module_close_serialized(void *module, bool retiring);

epd_memory_owner_t app_module_owner_acquire(const char *id)
{
    if (!id) return EPD_MEMORY_OWNER_INVALID;
    epd_memory_owner_t owner = EPD_MEMORY_OWNER_INVALID;
    rt_base_t level = rt_hw_interrupt_disable();
    for (app_instance_t *instance = instances; instance; instance = instance->next)
        if (!strcmp(instance->id, id))
        {
            if (!instance->closing && epd_memory_owner_retain_trusted(instance->owner))
                owner = instance->owner;
            break;
        }
    rt_hw_interrupt_enable(level);
    return owner;
}

static rt_uint32_t unresolved_symbol(const char *name)
{
    (void)name;
    return 0;
}

static bool lifecycle_lock_wait(int timeout)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (!lifecycle_ready)
        lifecycle_ready = rt_mutex_init(&module_lifecycle, "app_module", RT_IPC_FLAG_PRIO) == RT_EOK;
    bool ready = lifecycle_ready;
    rt_hw_interrupt_enable(level);
    return ready && rt_mutex_take(&module_lifecycle, timeout) == RT_EOK;
}

static bool lifecycle_lock(void) { return lifecycle_lock_wait(RT_WAITING_FOREVER); }

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
    if (!id) return false;
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
    instance->closing = true;
    retire_pending = true;
    epd_cold_runtime_close_owner(instance->owner);
    if (epd_cold_runtime_owner_busy(instance->owner)) return;
    epd_operation_runtime_close_owner(instance->owner);
    epd_resource_runtime_close_owner(instance->owner);
    epd_memory_owner_close_trusted(instance->owner);
    if (!epd_operation_runtime_owner_drained(instance->owner) ||
        epd_resource_runtime_owner_busy(instance->owner)) return;
#if LV_USE_EXECUTION_CONTEXT
    lv_execution_context_cancel(instance->owner);
#endif
    if (instance->background_bound)
    {
        app_background_stopped(instance->owner);
        epd_app_background_snapshot_t state;
        if (app_background_snapshot(instance->owner, &state) != EPD_MEMORY_OK ||
            app_background_report(instance->owner, state.revision, EPD_APP_BG_RELEASED) != EPD_MEMORY_OK ||
            app_background_unbind(instance->owner) != EPD_MEMORY_OK)
        {
            instance->closing = true;
            retire_pending = true;
            return;
        }
        instance->background_bound = false;
    }
    epd_memory_owner_snapshot_t owner_state;
    if (!epd_memory_owner_get(instance->owner, &owner_state) || owner_state.references != 1) return;
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
    epd_memory_owner_close_trusted(instance->owner);
    epd_memory_owner_release_trusted(instance->owner);
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
    if (instance && (instance->closing || instance->termination_requested)) return NULL;
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
        instance->owner = epd_memory_owner_create(id);
        if (instance->owner == EPD_MEMORY_OWNER_INVALID) { rt_free(instance); return NULL; }
        strcpy(instance->id, id);
        strcpy(instance->root, root);
        instance->volume = volume;
        instance->session = session;
        if (!instance_pin(instance))
        {
            epd_memory_owner_close_trusted(instance->owner);
            epd_memory_owner_release_trusted(instance->owner);
            rt_free(instance);
            return NULL;
        }
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
        if (app_background_bind(id, instance->owner) != EPD_MEMORY_OK) goto failed;
        instance->background_bound = true;
        app_background_foreground(instance->owner, false);
    }
    /* Missing imports must fail the load instead of asserting in the SDK. */
    app_memory_load_watch_t *watch = app_memory_load_current();
    if (watch && peak > watch->peak) watch->peak = peak;
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(instance->owner);
    if (previous == EPD_MEMORY_OWNER_INVALID) goto failed;
    bool resolver_owned = dlmodule_register_ex_symbol_resolver(unresolved_symbol) == RT_EOK;
    struct rt_dlmodule *module = dlopen(path, 0);
    if (resolver_owned) dlmodule_unregister_ex_symbol_resolver();
    /* Init callbacks may already own work. Retire them before unloading code. */
    bool invalidated = module && (!storage_session_valid(volume, session) ||
        !app_package_revision(root, id, library, &current) || memcmp(&revision, &current, sizeof(revision)));
    epd_memory_owner_leave_trusted(previous);
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
    if (invalidated)
    {
        if (!library && new_record)
        {
            instance->aborting = instance->closing = true;
            record->close_pending = true;
            retire_pending = true;
        }
        else if (!new_record) { dlclose(module); }
        else { module_close_serialized(module, false); }
        return NULL;
    }
    if (!library && record->close_pending)
    {
        /* Transfer the loader's deferred reference to this successful caller. */
        record->close_pending = false;
        instance->eviction_requested = false;
        dlclose(module);
    }
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

static void module_close_serialized(void *module, bool retiring)
{
    if (!module) return;
    app_instance_t *instance = NULL;
    app_module_t *record = NULL;
    for (app_instance_t *item = instances; item && !record; item = item->next)
        for (app_module_t *m = item->modules; m; m = m->next)
            if (m->module == module)
            { instance = item; record = m; break; }
    if (record && record->close_pending && !retiring) return;
    if (record && ((struct rt_dlmodule *)module)->nref == 1 && !strcmp(record->name, instance->id))
    {
        if (!retiring)
        {
            record->close_pending = true;
            retire_pending = true;
            return;
        }
        record->close_pending = true;
        retire_pending = true;
        if (!instance->close_prepared)
        {
            epd_app_background_snapshot_t background;
            if (app_background_snapshot(instance->owner, &background) != EPD_MEMORY_OK) return;
            if (!instance->aborting && background.effective == EPD_APP_BG_STATE && !instance->eviction_requested)
            {
                app_background_report(instance->owner, background.revision, EPD_APP_BG_RETAINED);
                return;
            }
            /* A private library can still be borrowed by an in-flight caller. */
            if (instance->modules != record || record->next) return;
            if (instance->aborting && app_background_clean(instance->owner))
            {
                if (app_service_quiesce(instance->id) != EPD_MEMORY_OK) return;
                app_background_stopped(instance->owner);
                instance->terminal_state = EPD_APP_BG_RELEASED;
            }
            else if (app_background_prepare_close(instance->owner, &instance->terminal_state) != EPD_MEMORY_OK)
                return;
            instance->closing = true;
            instance->close_prepared = true;
            epd_cold_runtime_close_owner(instance->owner);
        }
        if (epd_cold_runtime_owner_busy(instance->owner)) return;
        if (!instance->resources_closing)
        {
            epd_operation_runtime_close_owner(instance->owner);
            epd_resource_runtime_close_owner(instance->owner);
            epd_memory_owner_close_trusted(instance->owner);
            instance->resources_closing = true;
        }
        epd_memory_owner_snapshot_t state;
        if (!epd_operation_runtime_owner_drained(instance->owner) ||
            epd_resource_runtime_owner_busy(instance->owner))
        {
            record->close_pending = true;
            retire_pending = true;
            return;
        }
        /* Cleanup may remove registered callbacks which still pin the module. */
        struct rt_dlmodule *loaded = module;
        if (loaded->cleanup_func)
        {
            epd_memory_owner_t cleanup_previous = epd_memory_owner_enter_trusted(instance->owner);
            if (cleanup_previous == EPD_MEMORY_OWNER_INVALID) return;
            void (*cleanup)(struct rt_dlmodule *) = loaded->cleanup_func;
            loaded->cleanup_func = NULL;
            cleanup(loaded);
            epd_memory_owner_leave_trusted(cleanup_previous);
        }
        epd_memory_owner_close_trusted(instance->owner);
#if LV_USE_EXECUTION_CONTEXT
        lv_execution_context_cancel(instance->owner);
#endif
        epd_input_cleanup_closed();
        if (!epd_operation_runtime_owner_drained(instance->owner) ||
            epd_resource_runtime_owner_busy(instance->owner)) return;
        if (instance->background_bound)
        {
            epd_app_background_snapshot_t background;
            if (app_background_snapshot(instance->owner, &background) == EPD_MEMORY_OK &&
                app_background_report(instance->owner, background.revision, instance->terminal_state) == EPD_MEMORY_OK &&
                app_background_unbind(instance->owner) == EPD_MEMORY_OK)
                instance->background_bound = false;
        }
        if (instance->background_bound || !epd_memory_owner_get(instance->owner, &state) ||
            state.references != 1)
        {
            record->close_pending = true;
            retire_pending = true;
            return;
        }
    }
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(instance ? instance->owner : EPD_MEMORY_OWNER_SYSTEM);
    if (previous == EPD_MEMORY_OWNER_INVALID) return;
    dlclose(module);
    epd_memory_owner_leave_trusted(previous);
    if (record) retire_pending = true;
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
    app_memory_load_watch_t *watch = app_memory_load_current();
    if (!lifecycle_lock_wait(watch ? 0 : RT_WAITING_FOREVER)) {
        if (watch) watch->busy = true;
        return NULL;
    }
    epd_memory_owner_t previous = epd_memory_owner_enter_trusted(EPD_MEMORY_OWNER_SYSTEM);
    if (previous == EPD_MEMORY_OWNER_INVALID) { rt_mutex_release(&module_lifecycle); return NULL; }
    void *module = module_open_serialized(path, id);
    epd_memory_owner_leave_trusted(previous);
    rt_mutex_release(&module_lifecycle);
    return module;
}

void app_module_close(void *module)
{
    if (!module || !lifecycle_lock()) return;
    module_close_serialized(module, false);
    rt_mutex_release(&module_lifecycle);
}

/* All remaining SDK references must be owned by the loader or its services. */
static bool instance_idle_references(app_instance_t *instance, app_module_t *main)
{
    return main && ((struct rt_dlmodule *)main->module)->nref ==
        (int)(app_service_module_pins(instance->id) + (main->close_pending ? 1u : 0u));
}

static bool state_reclaimable(const epd_app_background_snapshot_t *state)
{
    if (state->foreground || state->tasks || state->effective != EPD_APP_BG_STATE) return false;
    epd_cold_runtime_snapshot_t cold;
    if (epd_cold_runtime_get(state->owner, &cold) && cold.registered) return !cold.failed;
    return state->dirty_revision == state->saved_revision;
}

bool app_modules_reclaim(const char *foreground_id)
{
    if (!foreground_id || !lifecycle_ready || rt_mutex_take(&module_lifecycle, 0) != RT_EOK) return false;
    bool pending = false;
    for (app_instance_t *instance = instances; instance; instance = instance->next)
    {
        if (!strcmp(instance->id, foreground_id)) continue;
        if (instance->closing || instance->eviction_requested) { pending = true; break; }
        epd_app_background_snapshot_t state;
        app_module_t *main = instance->modules;
        while (main && strcmp(main->name, instance->id)) main = main->next;
        if (app_background_snapshot(instance->owner, &state) == EPD_MEMORY_OK &&
            state_reclaimable(&state) && instance_idle_references(instance, main))
        {
            instance->eviction_requested = true;
            retire_pending = pending = true;
            break;
        }
    }
    rt_mutex_release(&module_lifecycle);
    return pending;
}

bool app_modules_retiring(void)
{
    if (!lifecycle_ready) return false;
    if (rt_mutex_take(&module_lifecycle, 0) != RT_EOK) return true;
    bool pending = false;
    for (app_instance_t *instance = instances; instance; instance = instance->next)
        if (instance->closing || instance->eviction_requested || instance->termination_requested) {
            pending = true;
            break;
        }
    rt_mutex_release(&module_lifecycle);
    return pending;
}

static epd_memory_result_t instance_request_stop(app_instance_t *instance)
{
    epd_memory_result_t result = instance->termination_requested || !instance->background_bound ?
                                 EPD_MEMORY_OK : app_background_request_stop(instance->owner);
    if (result == EPD_MEMORY_OK)
    {
        instance->termination_requested = true;
        instance->eviction_requested = false;
        retire_pending = true;
    }
    return result;
}

epd_memory_result_t app_module_request_stop(const char *id)
{
    if (!id || !*id) return EPD_MEMORY_INVALID;
    if (!lifecycle_ready) return EPD_MEMORY_OK;
    if (rt_mutex_take(&module_lifecycle, 0) != RT_EOK) return EPD_MEMORY_WAITING;
    epd_memory_result_t result = EPD_MEMORY_OK;
    for (app_instance_t *instance = instances; instance; instance = instance->next) {
        if (strcmp(instance->id, id)) continue;
        result = instance_request_stop(instance);
        break;
    }
    rt_mutex_release(&module_lifecycle);
    return result;
}

void app_modules_request_stop(void)
{
    if (!lifecycle_ready || rt_mutex_take(&module_lifecycle, 0) != RT_EOK) return;
    for (app_instance_t *instance = instances; instance; instance = instance->next)
        if (!instance->closing && !instance->termination_requested)
            instance_request_stop(instance);
    rt_mutex_release(&module_lifecycle);
}

void app_modules_process(void)
{
    app_memory_context_process();
    if (!retire_pending || !lifecycle_ready || rt_mutex_take(&module_lifecycle, 0) != RT_EOK) return;
    retire_pending = false;
    for (app_instance_t *instance = instances, *next; instance; instance = next)
    {
        next = instance->next;
        if (!instance->modules && instance->closing)
        {
            instance_release_empty(instance);
            continue;
        }
        if (instance->eviction_requested && !instance->close_prepared)
        {
            epd_app_background_snapshot_t state;
            app_module_t *main = instance->modules;
            while (main && strcmp(main->name, instance->id)) main = main->next;
            if (app_background_snapshot(instance->owner, &state) != EPD_MEMORY_OK ||
                !state_reclaimable(&state) ||
                !instance_idle_references(instance, main)) instance->eviction_requested = false;
            else
            {
                /* Services may own the last SDK refs; stop them before capture. */
                app_service_quiesce(instance->id);
                retire_pending = true;
            }
        }
        if (!instance->closing && !instance->eviction_requested)
        {
            epd_app_background_snapshot_t state;
            app_module_t *main = instance->modules;
            while (main && strcmp(main->name, instance->id)) main = main->next;
            if (main && app_background_snapshot(instance->owner, &state) == EPD_MEMORY_OK &&
                !state.foreground && instance_idle_references(instance, main))
            {
                /* Idle service workers are not a reason to retain RELEASE apps. */
                if (!state.tasks && state.effective == EPD_APP_BG_RELEASE)
                    app_service_quiesce(instance->id);
                retire_pending = true;
            }
        }
        for (app_module_t *record = instance->modules; record; record = record->next)
            if (record->close_pending)
            {
                module_close_serialized(record->module, true);
                break;
            }
    }
    rt_mutex_release(&module_lifecycle);
}
