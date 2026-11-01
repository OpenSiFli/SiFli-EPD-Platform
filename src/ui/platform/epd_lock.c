#include "platform/epd_lock_internal.h"
#include "epd_lock.h"
#include "epd_refresh.h"
#include "epd_power.h"
#include "app_catalog.h"
#include "storage_file.h"
#include "ui_navigation.h"
#include "launcher.h"
#include "ui_shell.h"
#include "frontlight.h"
#include <rtm.h>
#include <string.h>

#define LOCK_PATH STORAGE_FLASH_ROOT "/system/lockscreen.bin"
#define LOCK_MAGIC 0x4c4f434bu
#define LOCK_VERSION 1u

typedef struct { char id[GUI_APP_ID_MAX_LEN]; } lock_selection_t;
static lock_selection_t selected;
static char active_id[GUI_APP_ID_MAX_LEN];
static bool initialized, locked, unlock_requested;
static uint32_t catalog_revision, volume_revision;

static bool valid_selection(const char *id)
{
    if (!id || strlen(id) >= sizeof(selected.id) || strlen(id) >= RT_NAME_MAX) return false;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

void epd_lock_init(void)
{
    if (initialized) return;
    lock_selection_t saved;
    if (storage_record_load(LOCK_PATH, LOCK_MAGIC, LOCK_VERSION, &saved, sizeof(saved)) &&
        memchr(saved.id, 0, sizeof(saved.id)) && valid_selection(saved.id)) selected = saved;
    initialized = true;
}

const char *epd_lock_selected(void)
{
    epd_lock_init();
    return selected.id;
}
RTM_EXPORT(epd_lock_selected);

bool epd_lock_select(const char *id)
{
    epd_lock_init();
    if (!valid_selection(id) || (*id && !app_catalog_lockscreen_available(id))) return false;
    if (!strcmp(id, selected.id)) return true;
    lock_selection_t next = {{0}};
    strcpy(next.id, id);
    if (!storage_record_save(LOCK_PATH, LOCK_MAGIC, LOCK_VERSION, &next, sizeof(next))) return false;
    selected = next;
    return true;
}
RTM_EXPORT(epd_lock_select);

bool epd_lock_active(void) { return locked; }
RTM_EXPORT(epd_lock_active);

static bool builtin_enter(void)
{
    active_id[0] = 0;
    /* Reserve private state before navigation can hide the previous page. */
    if (!epd_lock_builtin_prepare()) return false;
    epd_refresh_request_interaction();
    bool ready = ui_navigation_open(UI_PAGE_LOCK) && epd_lock_builtin_ready();
    epd_lock_builtin_cancel();
    return ready;
}

bool epd_lock_enter(void)
{
    if (locked) return true;
    if (storage_changing()) return false;
    ui_navigation_cancel();
    epd_lock_init();
    unlock_requested = false;
    locked = true;
    ui_shell_prepare_lock();
    frontlight_set_locked(true);
    epd_refresh_set_locked(true);
    epd_refresh_request_interaction();
#if defined(GUI_MAX_RUNNING_APPS) && GUI_MAX_RUNNING_APPS >= 2
    /* A different intent restarts an already-running instance of the same ID.
     * Keep that instance's current page by using its built-in lock subpage. */
    if (selected.id[0] && !gui_app_is_actived(selected.id) &&
        app_catalog_lockscreen_available(selected.id))
    {
        intent_t intent = intent_init(selected.id);
        bool queued = intent && intent_set_string(intent, "lockscreen", "1") == RT_EOK &&
                      intent_runapp(intent) == RT_EOK;
        if (intent) intent_deinit(intent);
        if (queued)
        {
            strcpy(active_id, selected.id);
            gui_app_exec_now();
            if (gui_app_is_actived(active_id))
            {
                catalog_revision = app_catalog_revision();
                volume_revision = storage_revision();
                return true;
            }
        }
    }
#endif
    locked = builtin_enter();
    if (!locked)
    {
        epd_refresh_set_locked(false);
        frontlight_set_locked(false);
    }
    return locked;
}

bool epd_lock_unlock(void)
{
    if (!locked || epd_power_transitioning()) return false;
    unlock_requested = true;
    return true;
}
RTM_EXPORT(epd_lock_unlock);

void epd_lock_process(void)
{
    if (!locked || storage_changing()) return;
    if (unlock_requested)
    {
        epd_refresh_request_interaction();
        /* Teardown may already have removed the page that requested unlock. */
        int result = active_id[0] ? gui_app_exit(active_id) :
                     launcher_current_page() == UI_PAGE_LOCK ? gui_app_goback() : RT_EOK;
        if (result != RT_EOK) return;
        gui_app_exec_now();
        if (active_id[0] ? gui_app_is_actived(active_id) : launcher_current_page() == UI_PAGE_LOCK) return;
        active_id[0] = 0;
        locked = unlock_requested = false;
        epd_refresh_set_locked(false);
        frontlight_set_locked(false);
        lv_display_trigger_activity(NULL);
        return;
    }
    if (!active_id[0])
    {
        /* Storage teardown can also remove a built-in page owned by an app. */
        if (launcher_current_page() != UI_PAGE_LOCK) builtin_enter();
        return;
    }
    bool available = gui_app_is_actived(active_id);
    if (available && (catalog_revision != app_catalog_revision() || volume_revision != storage_revision()))
    {
        available = app_catalog_lockscreen_available(active_id);
        catalog_revision = app_catalog_revision();
        volume_revision = storage_revision();
    }
    if (available) return;
    /* app_fwk owns both module teardown and resuming the previous page. */
    if (gui_app_exit(active_id) != RT_EOK) return;
    gui_app_exec_now();
    builtin_enter();
}
