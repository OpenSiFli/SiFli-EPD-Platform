#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "ui_settings_internal.h"
#include "launcher.h"
#include "ui_font.h"
#include "platform/epd_ui_scale.h"
#include "icons/ui_icons.h"
#include "bf0_hal.h"
#include "ui_settings.h"
#include "ui_app.h"
#include "ui_navigation.h"
#include "apps/ui_ai_settings.h"
#include "apps/ui_speech_settings.h"
#include "platform/app_image.h"
#include "platform/epd_i18n.h"
#include "epd_waveform.h"   /* 屏驱波形接口（SDK: customer/peripherals/display/epd_e0470a03）*/
#include <stdio.h>
#include <string.h>

#define WIDTH 684
#define HEIGHT 1216
#define MARGIN epd_ui_px(32)
#define CONTENT_WIDTH (WIDTH - 2 * MARGIN)
#define PAGE_TITLE_X (MARGIN + epd_ui_px(72))
#define PAGE_TITLE_Y epd_ui_px(81)
#define PAGE_CONTENT_Y epd_ui_px(174)
#define ICON_BUTTON_SIZE epd_ui_touch(56)
#define BACK_BUTTON_Y epd_ui_px(72)
#define SETTINGS_TITLE_X epd_ui_px(91)
#define SETTINGS_TITLE_Y epd_ui_px(76)
#define SETTINGS_CONTENT_Y epd_ui_px(195)
#define FOCUS_CAPACITY 32
/* Fixed-label pages and install-location popups; settings lists own their text. */
#define TEXT_PAGE_BINDING_CAPACITY 7
#define TEXT_POPUP_BINDING_CAPACITY 4
#define TEXT_BINDING_CAPACITY (TEXT_PAGE_BINDING_CAPACITY + TEXT_POPUP_BINDING_CAPACITY)
#define HOME_PAGE_SIZE 16
#define NAV_BACK (-2)
#define NAV_IDLE (-1)
#define POPUP_CLOSE (-5)
#define SETTING_CHANGE_BASE (-192)

extern bool lv_refreshing_done(void);

typedef struct
{
    lv_obj_t *label;
    lv_ext_str_id_t id;
} text_binding_t;

typedef struct
{
    ui_page_id_t id;
    bool foreground;
    bool initial_focus_pending;
    unsigned saved_focus;
    void *feature;
    lv_obj_t *screen;
    lv_group_t *group;
    lv_obj_t *focus_items[FOCUS_CAPACITY];
    unsigned focus_count;
    lv_obj_t *home_tiles[HOME_PAGE_SIZE], *home_icons[HOME_PAGE_SIZE], *home_labels[HOME_PAGE_SIZE];
    lv_obj_t *home_pager;
    lv_obj_t *popup;
    lv_obj_t *popup_text, *popup_button, *popup_cancel;
    lv_obj_t *popup_detail;
    uint32_t popup_serial;
    unsigned popup_binding_start;
    lv_obj_t *popup_previous_focus;
    unsigned popup_focus_start;
    text_binding_t text_bindings[TEXT_BINDING_CAPACITY];
    unsigned text_binding_count;
    uint32_t locale_revision;
    uint32_t scale_revision;
    lv_obj_t *page_title, *back_button;
    bool locale_retry;
    lv_obj_t *language_options[2];
} page_context_t;

extern page_context_t *current;
typedef struct
{
    void (*create)(lv_obj_t *screen);
    void (*resume)(void);
    void (*pause)(void);
    void (*stop)(void);
    bool (*action)(int *target);
} ui_page_ops_t;

const ui_page_ops_t *ui_page_operations(ui_page_id_t page);
bool settings_action(int *target);
void settings_resume(void);
void settings_process(void);
void settings_stop(void);
void settings_restore_focus(void);
void settings_more_create(lv_obj_t *screen);
void settings_time_create(lv_obj_t *screen);
void settings_timezone_create(lv_obj_t *screen);
void settings_time_edit_create(lv_obj_t *screen);
void settings_time_edit_process(void);
void settings_time_edit_stop(void);
bool settings_time_edit_action(int *target);
void settings_status_create(lv_obj_t *screen);
void settings_lock_options_create(lv_obj_t *screen);
void settings_network_status_create(lv_obj_t *screen);
void settings_bluetooth_create(lv_obj_t *screen);
void settings_usb_create(lv_obj_t *screen);
void settings_reset_create(lv_obj_t *screen);
void settings_options_create(lv_obj_t *screen);
void frontlight_page_create(lv_obj_t *screen);
void scale_page_create(lv_obj_t *screen);
void scale_page_process(void);
void scale_page_stop(void);
bool scale_page_action(int *target);
void frontlight_page_process(void);
void frontlight_page_pause(void);
void frontlight_page_stop(void);
void sound_page_create(lv_obj_t *screen);
void sound_page_resume(void);
void sound_page_process(void);
void sound_page_pause(void);
void sound_page_stop(void);
void sound_page_restore_focus(void);
bool sound_page_action(int *target);
lv_ext_str_id_t sound_volume_title_id(void);
lv_ext_str_id_t sound_tone_title_id(void);
extern launcher_status_t status;
extern int pending_page;
lv_ext_str_id_t page_title_id(ui_page_id_t page);

void epd_obj_init(lv_obj_t *obj);
bool text_update(lv_obj_t *label, const char *text);
lv_obj_t *text_bind(lv_obj_t *label, lv_ext_str_id_t id);
void page_retranslate(void);
lv_obj_t *label_create_i18n(lv_obj_t *parent, lv_ext_str_id_t id, int x, int y,
                          int width, const lv_font_t *font);
lv_obj_t *button_create_i18n(lv_obj_t *parent, int x, int y, int width, int height,
                           lv_ext_str_id_t id, int target);
lv_obj_t *centered_label_i18n(lv_obj_t *parent, lv_ext_str_id_t id, int x, int y,
                            int width, const lv_font_t *font);
lv_obj_t *label_create(lv_obj_t *parent, const char *text, int x, int y,
                              int width, const lv_font_t *font);
lv_obj_t *icon_create(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y);
lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int width, int height);
lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int width, int height,
                               const char *text, int target);
lv_obj_t *icon_button_create(lv_obj_t *parent, int x, int y,
                                    const lv_image_dsc_t *src, int target);
lv_obj_t *page_title_create(lv_obj_t *screen, lv_ext_str_id_t id, const lv_font_t *font);
lv_obj_t *centered_label(lv_obj_t *parent, const char *text, int x, int y,
                                int width, const lv_font_t *font);
void object_visible(lv_obj_t *object, bool visible);
bool focus_eligible(lv_obj_t *object);
void focus_restore(unsigned index);
void popup_close(void);
void popup_open(const char *text, const char *detail);
void popup_confirm(const char *text, const char *detail, int target);
void popup_open_i18n(lv_ext_str_id_t id, const char *detail);
void popup_confirm_i18n(lv_ext_str_id_t id, const char *detail, int target);
void image_update(lv_obj_t *icon, const lv_image_dsc_t *src);
void home_create(lv_obj_t *screen);
void home_resume(void);
void home_pause(void);
void home_stop(void);
void home_process(void);
bool home_action(int target);
void lock_create(lv_obj_t *screen);
void lock_resume(void);
void lock_pause(void);
void lock_stop(void);
void lock_process(void);
void lock_settings_create(lv_obj_t *screen);
void lock_settings_resume(void);
void lock_settings_stop(void);
void lock_settings_process(void);
void lock_settings_restore_focus(void);
bool lock_settings_action(int *target);
void settings_create(lv_obj_t *screen);
void text_settings_create(lv_obj_t *screen);
void display_settings_create(lv_obj_t *screen);
void frontlight_mode_create(lv_obj_t *screen);
void language_settings_create(lv_obj_t *screen);
void language_settings_resume(void);
void network_settings_create(lv_obj_t *screen);
void wifi_settings_create(lv_obj_t *screen);
void about_create(lv_obj_t *screen);
void storage_page_create(lv_obj_t *screen);
void storage_page_resume(void);
void files_create(lv_obj_t *screen);
void portal_page_create(lv_obj_t *screen);
void portal_page_resume(void);
void portal_page_stop(void);
void portal_page_process(void);
bool portal_page_action(int *target);
void files_resume(void);
void files_pause(void);
void files_release_storage(void);
void files_retranslate(void);
void files_storage_changed(void);
void files_stop(void);
bool files_action(int *target);
void app_management_create(lv_obj_t *screen);
void app_list_create(lv_obj_t *screen);
void app_list_resume(void);
void app_list_stop(void);
bool app_list_action(int *target);
void app_list_process(void);
void app_list_restore_focus(void);
void app_list_retranslate(void);
void app_list_storage_changed(void);
void memory_page_create(lv_obj_t *screen);
void memory_page_resume(void);
void memory_page_stop(void);
void memory_page_process(void);
void memory_page_restore_focus(void);
void files_restore_focus(void);
bool memory_page_action(int *target);

#endif
