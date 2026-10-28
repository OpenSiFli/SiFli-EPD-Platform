#include "network_internal.h"
#include "ui_navigation.h"
#include "platform/app_catalog.h"
#include "platform/epd_refresh.h"
#include "system_reset.h"

#define DBG_TAG "ui.nav"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static ui_page_lifecycle_t page_lifecycle;
static uint32_t page_memory_size;
static const struct
{
    const char *app;
    const char *page;
    bool root;
} routes[UI_PAGE_COUNT] = {
    [UI_PAGE_HOME] = {"Main", "root", true},
    [UI_PAGE_FILES] = {"files", "root", true},
    [UI_PAGE_PORTAL] = {"portal", "root", true},
    [UI_PAGE_PORTAL_WEB] = {"portal", "web", false},
    [UI_PAGE_SETTINGS] = {"settings", "root", true},
    [UI_PAGE_TEXT_SETTINGS] = {NULL, "text_settings", false},
    [UI_PAGE_READING_FONTS] = {NULL, "reading_fonts", false},
    [UI_PAGE_ABOUT] = {"settings", "about", false},
    [UI_PAGE_LOCK] = {NULL, "lock", false},
    [UI_PAGE_DISPLAY_SETTINGS] = {"settings", "display", false},
    [UI_PAGE_NETWORK_SETTINGS] = {"settings", "network", false},
    [UI_PAGE_WIFI_SETTINGS] = {"settings", "wifi", false},
    [UI_PAGE_STORAGE] = {"settings", "storage", false},
    [UI_PAGE_APP_MANAGEMENT] = {"settings", "applications", false},
    [UI_PAGE_APP_INSTALL] = {"settings", "install", false},
    [UI_PAGE_APP_UNINSTALL] = {"settings", "uninstall", false},
    [UI_PAGE_APP_SETTINGS] = {"settings", "app_settings", false},
    [UI_PAGE_MEMORY] = {"settings", "memory", false},
    [UI_PAGE_LANGUAGE_SETTINGS] = {"settings", "language", false},
    [UI_PAGE_LOCK_SETTINGS] = {"settings", "lockscreen", false},
    [UI_PAGE_MORE_SETTINGS] = {"settings", "more", false},
    [UI_PAGE_TIME_SETTINGS] = {"settings", "time", false},
    [UI_PAGE_TIMEZONE_SETTINGS] = {"settings", "timezone", false},
    [UI_PAGE_TIME_EDIT] = {"settings", "time_edit", false},
    [UI_PAGE_STATUS_INFO] = {"settings", "status", false},
    [UI_PAGE_LOCK_OPTIONS] = {"settings", "lock_options", false},
    [UI_PAGE_NETWORK_STATUS] = {"settings", "network_status", false},
    [UI_PAGE_BLUETOOTH_SETTINGS] = {"settings", "bluetooth", false},
    [UI_PAGE_USB_SETTINGS] = {"settings", "usb", false},
    [UI_PAGE_RESET_SETTINGS] = {"settings", "reset", false},
    [UI_PAGE_SETTING_OPTIONS] = {NULL, "setting_options", false},
    [UI_PAGE_FRONTLIGHT] = {"settings", "frontlight", false},
    [UI_PAGE_FRONTLIGHT_MODE] = {"settings", "frontlight_mode", false},
    [UI_PAGE_SOUND] = {"settings", "sound", false},
    [UI_PAGE_SOUND_VOLUME] = {"settings", "sound_volume", false},
    [UI_PAGE_SOUND_TONE] = {"settings", "sound_tone", false},
    [UI_PAGE_UI_SCALE] = {"settings", "ui_scale", false},
    [UI_PAGE_WALLPAPER] = {"settings", "wallpaper", false},
    [UI_PAGE_FILE_PICKER] = {NULL, "file_picker", false},
    [UI_PAGE_AI_SETTINGS] = {"settings", "ai_settings", false},
    [UI_PAGE_AI_SELECT] = {NULL, "ai_select", false},
    [UI_PAGE_SPEECH_SETTINGS] = {"settings", "speech_settings", false}
};

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    ui_page_id_t page = (ui_page_id_t)((uintptr_t)gui_app_this_page_userdata() - 1u);
    RT_ASSERT(page < UI_PAGE_COUNT);
    if (message == GUI_APP_MSG_ONSTART) gui_app_close_anim();
    page_lifecycle(page, message, gui_app_this_page_memory());
}

int ui_navigation_start(const char *app, ui_page_id_t root)
{
    return gui_app_regist_msg_handler_ext(app, page_message,
                                         (void *)(uintptr_t)(root + 1), page_memory_size);
}

static const char *active_app(void)
{
    intent_t intent = gui_app_get_intent();
    return intent ? intent_get_action(intent) : NULL;
}

static ui_page_id_t active_page(void)
{
    uintptr_t id = (uintptr_t)gui_app_this_page_userdata();
    if (!id || id > UI_PAGE_COUNT) return UI_PAGE_COUNT;
    ui_page_id_t page = (ui_page_id_t)(id - 1u);
    const char *app = active_app();
    if (!app || (routes[page].app && strcmp(app, routes[page].app))) return UI_PAGE_COUNT;
    return gui_page_is_actived(app, routes[page].page) ? page : UI_PAGE_COUNT;
}

bool ui_navigation_init(ui_page_lifecycle_t lifecycle, uint32_t memory_size)
{
    page_lifecycle = lifecycle;
    page_memory_size = memory_size;
    gui_app_init(1);
    epd_refresh_init();
    app_catalog_init();
    gui_app_run_now("Main");
    if (active_page() != UI_PAGE_HOME)
    {
        LOG_E("Main/root did not enter the foreground");
        return false;
    }
    return true;
}

static bool page_present(ui_page_id_t page)
{
    if (page < UI_PAGE_HOME || page >= UI_PAGE_COUNT) return false;
    const char *app = active_app();
    if (!app || (routes[page].app && strcmp(app, routes[page].app))) return false;
    return gui_app_is_page_present((char *)routes[page].page);
}

bool ui_navigation_run(const char *app)
{
    if (system_reset_blocks_apps() && (!app || strcmp(app, "settings"))) return false;
    if (!app || gui_app_run(app) != RT_EOK) return false;
    gui_app_exec_now();
    return gui_app_is_actived((char *)app);
}

bool ui_navigation_open(ui_page_id_t page)
{
    if (system_reset_blocks_apps() && page != UI_PAGE_RESET_SETTINGS) return false;
    if (page < UI_PAGE_HOME || page >= UI_PAGE_COUNT) return false;
    if (active_page() == page) return true;
    const char *app = active_app();
    if (!app) return false;
    if (routes[page].root) return ui_navigation_run(routes[page].app);
    if (routes[page].app && strcmp(app, routes[page].app)) return false;
    int result;
    if (page_present(page))
        result = gui_app_goback_to_page(routes[page].page);
    else
        result = gui_app_create_page_for_app_ext(app, routes[page].page, page_message,
                           (void *)(uintptr_t)(page + 1), page_memory_size);
    if (result != RT_EOK) return false;
    gui_app_exec_now();
    return active_page() == page;
}

bool ui_navigation_back(void)
{
    if (system_reset_blocks_apps()) return false;
    ui_page_id_t previous = active_page();
    if (previous == UI_PAGE_HOME || gui_app_goback() != RT_EOK) return false;
    gui_app_exec_now();
    return active_app() != NULL && active_page() != previous;
}
