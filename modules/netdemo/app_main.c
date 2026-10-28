#include "app_service.h"
#include "network.h"
#include "platform/epd_app.h"

EPD_APP_DEFINE("netdemo");

typedef struct
{
    lv_obj_t *label;
    lv_timer_t *timer;
} network_page_t;

/* The platform owns the worker and its network event subscription. */
static void network_worker(epd_service_t *service)
{
    while (!epd_service_cancelled(service))
    {
        network_snapshot_t snapshot;
        network_snapshot(&snapshot);
        epd_service_publish_summary(service, snapshot.ready ? "Network ready" : "Waiting for network");
        epd_service_wait(service, RT_WAITING_FOREVER);
    }
}

const epd_background_t epd_app_background = {
    .run = network_worker,
    .stack_size = 2048,
    .priority = 24,
    .network = true,
};

static void show_snapshot(network_page_t *page)
{
    network_snapshot_t snapshot;
    char text[160];
    network_snapshot(&snapshot);
    rt_snprintf(text, sizeof(text), "%s\nSource: %s\nGeneration: %lu",
                snapshot.ready ? "Network ready" : "Waiting for network",
                snapshot.source == NETWORK_WIFI ? "Wi-Fi" :
                snapshot.source == NETWORK_PAN ? "Bluetooth PAN" : "Auto",
                (unsigned long)snapshot.generation);
    epd_app_text(page->label, text);
}

static void poll(lv_timer_t *timer)
{
    show_snapshot(lv_timer_get_user_data(timer));
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    network_page_t *page = gui_app_this_page_memory();
    switch (message)
    {
    case GUI_APP_MSG_ONSTART:
    {
        lv_obj_t *screen = lv_screen_active();
        gui_app_close_anim();
        epd_app_object_init(screen);
        lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(screen, lv_color_black(), 0);
        epd_app_input_group(screen);
        epd_app_header(screen);
        page->label = epd_app_label(screen, "Reading network snapshot...", 32, 160, 580,
                                    epd_app_font());
        epd_app_back_button(screen);
        page->timer = lv_timer_create(poll, 1000, page);
        if (page->timer) lv_timer_pause(page->timer);
        break;
    }
    case GUI_APP_MSG_ONRESUME:
        epd_app_header_refresh(lv_screen_active());
        show_snapshot(page);
        if (page->timer) lv_timer_resume(page->timer);
        break;
    case GUI_APP_MSG_ONPAUSE:
        if (page->timer) lv_timer_pause(page->timer);
        break;
    case GUI_APP_MSG_ONSTOP:
        if (page->timer) lv_timer_delete(page->timer);
        page->timer = NULL;
        break;
    default:
        break;
    }
}

int app_main(intent_t intent)
{
    (void)intent;
    return gui_app_regist_msg_handler_ext("netdemo", page_message, NULL, sizeof(network_page_t));
}
