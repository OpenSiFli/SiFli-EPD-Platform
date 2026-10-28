#include "ui_internal.h"
#include "app_wallpaper.h"
#include "ui_speech_settings.h"
#include "reading_fonts.h"

static bool home_dispatch(int *target)
{
    return home_action(*target);
}


static const ui_page_ops_t pages[UI_PAGE_COUNT] = {
    [UI_PAGE_HOME] = {home_create, home_resume, home_pause, home_stop, home_dispatch},
    [UI_PAGE_SETTINGS] = {settings_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_TEXT_SETTINGS] = {text_settings_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_LOCK] = {lock_create, lock_resume, lock_pause, lock_stop, NULL},
    [UI_PAGE_FILES] = {files_create, files_resume, files_pause, files_stop, files_action},
    [UI_PAGE_PORTAL] = {portal_page_create, portal_page_resume, NULL, portal_page_stop, portal_page_action},
    [UI_PAGE_PORTAL_WEB] = {portal_page_create, portal_page_resume, NULL, portal_page_stop, portal_page_action},
    [UI_PAGE_ABOUT] = {about_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_DISPLAY_SETTINGS] = {display_settings_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_LANGUAGE_SETTINGS] = {language_settings_create, language_settings_resume, NULL, NULL, settings_action},
    [UI_PAGE_NETWORK_SETTINGS] = {network_settings_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_WIFI_SETTINGS] = {wifi_settings_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_STORAGE] = {storage_page_create, storage_page_resume, files_pause, files_stop, files_action},
    [UI_PAGE_APP_MANAGEMENT] = {app_management_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_APP_INSTALL] = {app_list_create, app_list_resume, NULL, app_list_stop, app_list_action},
    [UI_PAGE_APP_UNINSTALL] = {app_list_create, app_list_resume, NULL, app_list_stop, app_list_action},
    [UI_PAGE_APP_SETTINGS] = {app_list_create, app_list_resume, NULL, app_list_stop, app_list_action},
    [UI_PAGE_MEMORY] = {memory_page_create, memory_page_resume, NULL, memory_page_stop, memory_page_action},
    [UI_PAGE_LOCK_SETTINGS] = {lock_settings_create, lock_settings_resume, NULL, lock_settings_stop, lock_settings_action},
    [UI_PAGE_MORE_SETTINGS] = {settings_more_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_TIME_SETTINGS] = {settings_time_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_TIMEZONE_SETTINGS] = {settings_timezone_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_TIME_EDIT] = {settings_time_edit_create, settings_time_edit_process, NULL, settings_time_edit_stop, settings_time_edit_action},
    [UI_PAGE_STATUS_INFO] = {settings_status_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_LOCK_OPTIONS] = {settings_lock_options_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_NETWORK_STATUS] = {settings_network_status_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_BLUETOOTH_SETTINGS] = {settings_bluetooth_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_USB_SETTINGS] = {settings_usb_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_RESET_SETTINGS] = {settings_reset_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_SETTING_OPTIONS] = {settings_options_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_FRONTLIGHT] = {frontlight_page_create, frontlight_page_process, frontlight_page_pause, frontlight_page_stop, NULL},
    [UI_PAGE_FRONTLIGHT_MODE] = {frontlight_mode_create, settings_resume, NULL, settings_stop, settings_action},
    [UI_PAGE_SOUND] = {sound_page_create, sound_page_resume, sound_page_pause, sound_page_stop, sound_page_action},
    [UI_PAGE_SOUND_VOLUME] = {sound_page_create, sound_page_resume, sound_page_pause, sound_page_stop, NULL},
    [UI_PAGE_SOUND_TONE] = {sound_page_create, sound_page_resume, sound_page_pause, sound_page_stop, sound_page_action},
    [UI_PAGE_UI_SCALE] = {scale_page_create, scale_page_process, NULL, scale_page_stop, scale_page_action},
    [UI_PAGE_WALLPAPER] = {wallpaper_page_create, wallpaper_page_resume, wallpaper_page_pause, wallpaper_page_stop, wallpaper_page_action},
    [UI_PAGE_FILE_PICKER] = {files_create, files_resume, files_pause, files_stop, files_action},
    [UI_PAGE_AI_SETTINGS] = {ui_ai_settings_create, ui_ai_settings_resume, ui_ai_settings_pause, ui_ai_settings_stop, ui_ai_settings_action},
    [UI_PAGE_AI_SELECT] = {ui_ai_settings_create, ui_ai_settings_resume, ui_ai_settings_pause, ui_ai_settings_stop, ui_ai_settings_action},
    [UI_PAGE_SPEECH_SETTINGS] = {ui_speech_settings_create, ui_speech_settings_resume, ui_speech_settings_pause, ui_speech_settings_stop, ui_speech_settings_action},
    [UI_PAGE_READING_FONTS] = {reading_fonts_create, reading_fonts_resume, reading_fonts_pause, reading_fonts_stop, reading_fonts_action}
};

const ui_page_ops_t *ui_page_operations(ui_page_id_t page)
{
    RT_ASSERT(page < UI_PAGE_COUNT);
    return &pages[page];
}
