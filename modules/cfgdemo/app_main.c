#include "config_io.h"
#include "platform/epd_app.h"
#include <string.h>

EPD_APP_DEFINE("cfgdemo");

typedef struct
{
    uint32_t presses;
} saved_config_t;

typedef struct
{
    lv_obj_t *label, *button;
    saved_config_t config;
    config_io_t *io;
    bool loaded;
} config_page_t;

static void show_config(config_page_t *page, const char *message)
{
    char text[160];
    rt_snprintf(text, sizeof(text), "Saved button presses: %lu\n%s",
                (unsigned long)page->config.presses, message);
    epd_app_text(page->label, text);
}

static void completed(bool save, bool ok, uint32_t presses, void *context)
{
    config_page_t *page = context;
    page->io = NULL; page->loaded = true;
    if (ok) page->config.presses = presses;
    lv_obj_remove_state(page->button, LV_STATE_DISABLED);
    show_config(page, save ? (ok ? "Saved. Reopen this app to read the file." :
                                  "Save failed. Check application storage.") :
                            (ok ? "Configuration loaded." : "No valid saved configuration."));
}

static void save_clicked(lv_event_t *event)
{
    config_page_t *page = lv_event_get_user_data(event);
    if (!page->loaded || page->io) return;
    page->io = config_io_start(true, page->config.presses + 1u, completed, page);
    if (!page->io) { show_config(page, "Save failed. Check application storage."); return; }
    lv_obj_add_state(page->button, LV_STATE_DISABLED);
    show_config(page, "Saving...");
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    config_page_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        lv_obj_t *screen = lv_screen_active();
        memset(page, 0, sizeof(*page));
        gui_app_close_anim();
        epd_app_object_init(screen);
        lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(screen, lv_color_black(), 0);
        epd_app_input_group(screen);
        epd_app_header(screen);
        page->label = epd_app_label(screen, "", 32, 160, 580, epd_app_font());
        show_config(page, "Loading...");
        page->button = epd_app_button(screen, 32, 320, 400, 72, "Increment and save", save_clicked, page);
        lv_obj_add_state(page->button, LV_STATE_DISABLED);
        epd_app_back_button(screen);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(lv_screen_active());
        if (!page->loaded && !page->io) {
            page->io = config_io_start(false, 0, completed, page);
            if (!page->io) completed(false, false, 0, page);
        }
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        config_io_close(page->io); page->io = NULL;
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("cfgdemo", page_message, NULL, sizeof(config_page_t));
}
