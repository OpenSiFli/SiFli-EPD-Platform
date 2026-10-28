#include "storage_file.h"
#include "storage.h"
#include "platform/epd_app.h"

EPD_APP_DEFINE("cfgdemo");

#define CONFIG_MAGIC 0x43464731u
#define CONFIG_VERSION 1u

typedef struct
{
    uint32_t presses;
} saved_config_t;

typedef struct
{
    lv_obj_t *label;
    saved_config_t config;
} config_page_t;

static bool config_path(char *path, size_t size)
{
    return storage_app_path(path, size, "cfgdemo", STORAGE_APP_DATA, "settings.bin");
}

static void show_config(config_page_t *page, const char *message)
{
    char text[160];
    rt_snprintf(text, sizeof(text), "Saved button presses: %lu\n%s",
                (unsigned long)page->config.presses, message);
    epd_app_text(page->label, text);
}

static void save_clicked(lv_event_t *event)
{
    config_page_t *page = lv_event_get_user_data(event);
    char path[STORAGE_PATH_MAX];
    saved_config_t next = {page->config.presses + 1};
    if (storage_changing() || !config_path(path, sizeof(path)) ||
        !storage_record_save(path, CONFIG_MAGIC, CONFIG_VERSION, &next, sizeof(next)))
    {
        show_config(page, "Save failed. Check application storage.");
        return;
    }
    page->config = next;
    show_config(page, "Saved. Reopen this app to read the file.");
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    config_page_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        lv_obj_t *screen = lv_screen_active();
        char path[STORAGE_PATH_MAX];
        saved_config_t loaded_config;
        page->config.presses = 0;
        bool loaded = config_path(path, sizeof(path)) &&
                      storage_record_load(path, CONFIG_MAGIC, CONFIG_VERSION,
                                          &loaded_config, sizeof(loaded_config));
        if (loaded) page->config = loaded_config;
        gui_app_close_anim();
        epd_app_object_init(screen);
        lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(screen, lv_color_black(), 0);
        epd_app_input_group(screen);
        epd_app_header(screen);
        page->label = epd_app_label(screen, "", 32, 160, 580, epd_app_font());
        show_config(page, loaded ? "Configuration loaded." : "No valid saved configuration.");
        epd_app_button(screen, 32, 320, 400, 72, "Increment and save", save_clicked, page);
        epd_app_back_button(screen);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(lv_screen_active());
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("cfgdemo", page_message, NULL, sizeof(config_page_t));
}
