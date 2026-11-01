#ifndef EPD_APP_H
#define EPD_APP_H

#include "gui_app_fwk.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "epd_ui_scale.h"

#define EPD_APP_ABI 1u

#ifdef BUILD_DLMODULE
#define EPD_APP_DEFINE(app_id) \
    const epd_app_info_t epd_app_info = {EPD_APP_ABI, app_id}
#endif

/* A module exports this descriptor as epd_app_info and its SDK entry as app_main. */
typedef struct
{
    uint32_t abi;
    const char *id;
} epd_app_info_t;

typedef struct
{
    int battery_percent;
    char clock[16];
} epd_app_status_t;
/* UI-thread platform services; no application-private objects are returned. */
void epd_app_status(epd_app_status_t *status);
void epd_app_set_recent_reading(const char *title, int percent);
void epd_app_request_full_refresh(void);
bool epd_app_open_text_settings(void);
bool epd_app_open_font_install(void);
bool epd_app_open_reading_fonts(void);
const lv_font_t *epd_app_font(void);
const lv_font_t *epd_app_font_role(epd_font_role_t role);
#define EPD_FONT_STYLE_BOLD 1u
#define EPD_FONT_STYLE_ITALIC 2u
/* UI thread. Synthetic MiSans styles; Emoji remains unchanged. Combine flags
 * with |; zero returns the ordinary role font. Unknown flags or allocation
 * failure return the ordinary font. The platform owns the returned font for
 * the system-font lifetime, including across global scale changes. If glyph
 * pixels cannot be allocated, drawing skips that glyph without caching the
 * failure; its measured layout remains stable and later drawing can retry. */
const lv_font_t *epd_app_font_style(epd_font_role_t role, unsigned flags);
void epd_app_object_init(lv_obj_t *obj);
lv_obj_t *epd_app_label(lv_obj_t *parent, const char *text, int x, int y, int width, const lv_font_t *font);
lv_obj_t *epd_app_centered_label(lv_obj_t *parent, const char *text, int x, int y, int width, const lv_font_t *font);
lv_obj_t *epd_app_panel(lv_obj_t *parent, int x, int y, int width, int height);
lv_obj_t *epd_app_icon(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y);
/* UI thread only. Copies the original text for change detection; use this
 * function for all subsequent text updates to the same label. Returns false
 * if the text copy could not be allocated; retry instead of consuming a new revision. */
bool epd_app_text(lv_obj_t *label, const char *text);
/* UI thread only. Create once for a screen or modal root. Its DELETE event
 * releases the group; callers must not delete that group themselves.
 * Physical key listeners select the topmost visible scope, with global
 * overlays preceding the active screen. Groups alone do not map keys. */
lv_group_t *epd_app_input_group(lv_obj_t *owner);
/* Physical application keys. KEY1 belongs exclusively to the system.
 * Callbacks run on the UI thread, only for the topmost visible input scope.
 * Deleting owner unregisters its callback. A hold emits LONG_PRESSED once at
 * three seconds; release emits RELEASED, then CLICKED only for a short press. */
typedef enum { EPD_APP_KEY2 = 2, EPD_APP_KEY3 = 3 } epd_app_key_t;
typedef enum
{
    EPD_APP_KEY_PRESSED,
    EPD_APP_KEY_RELEASED,
    EPD_APP_KEY_CLICKED,
    EPD_APP_KEY_LONG_PRESSED
} epd_app_key_event_t;
typedef void (*epd_app_key_callback_t)(epd_app_key_t key, epd_app_key_event_t event, void *data);
bool epd_app_input_listen(lv_obj_t *owner, epd_app_key_callback_t callback, void *data);
void epd_app_input_unlisten(lv_obj_t *owner);
/* Back activates this button; a hidden/disabled button blocks back. */
void epd_app_input_back(lv_obj_t *owner, lv_obj_t *button);
/* Standard buttons join the nearest managed input group automatically. */
lv_obj_t *epd_app_button(lv_obj_t *parent, int x, int y, int width, int height,
                         const char *text, lv_event_cb_t callback, void *data);
/* UI thread only. Declare a normal page using the scaled system header.
 * Its page declaration is released by the screen's DELETE event. */
void epd_app_header(lv_obj_t *screen);
void epd_app_header_refresh(lv_obj_t *screen);
/* UI thread. Normal pages declare a header; immersive pages draw beneath it.
 * The first top-edge pull temporarily reveals the system status bar. */
void epd_app_set_immersive(lv_obj_t *screen, bool immersive);
bool epd_app_refresh_done(void);
/* UI thread. Hold the active screen's first frame for at most 1000 ms without
 * blocking input or UI timers. Repeated requests do not extend the deadline.
 * Release after preparing content/error feedback, or when leaving the page. */
void epd_app_defer_first_frame(lv_obj_t *screen, bool defer);
/* UI thread. Available bytes in the LVGL private heap; zero if unavailable.
 * This is a snapshot, not a reservation or a largest-free-block guarantee. */
size_t epd_app_ui_available(void);
/* UI thread. Submission sequence; compare for change, then wait for completion. */
uint32_t epd_app_refresh_submission(void);
void epd_app_clean_draw_buffer(lv_draw_buf_t *buffer);
lv_obj_t *epd_app_back_button(lv_obj_t *screen);
lv_obj_t *epd_app_back_button_cb(lv_obj_t *parent, lv_event_cb_t callback, void *data);
/* UI thread. Align the standard back button with a page heading's center. */
void epd_app_back_button_layout(lv_obj_t *button, int32_t x, int32_t center_y);

#endif
