#ifndef EPD_KEYBOARD_H
#define EPD_KEYBOARD_H

#include "lvgl.h"
#include <stdbool.h>

typedef enum
{
    EPD_KEYBOARD_TEXT,
    EPD_KEYBOARD_ASCII,
    EPD_KEYBOARD_NUMBER,
    EPD_KEYBOARD_PASSWORD
} epd_keyboard_mode_t;

typedef struct
{
    epd_keyboard_mode_t mode;
    bool chinese;
    bool voice;
} epd_keyboard_config_t;

/* UI thread only. The returned composite widget owns its keyboard and input
 * method; deleting it releases both. The textarea remains owned by the caller.
 * Its accepted characters and maximum length remain authoritative. Deleting
 * the textarea detaches it automatically. Password mode disables Chinese and
 * voice input. NULL config selects text input with Chinese and voice enabled.
 * Attach READY/CANCEL handlers to the returned widget, not its children.
 * READY converts pending pinyin first; a subsequent confirmation submits when
 * no composition remains. CANCEL hides the keyboard and discards only
 * uncommitted composition; it does not cancel the editor or clear its text.
 * Clicking or focusing the textarea reopens a hidden keyboard.
 * The preedit tab occupies reserved space inside the widget; the toolbar and
 * candidates share one row, so composition never moves the keys or editor.
 * Event handlers may delete the widget. */
lv_obj_t *epd_keyboard_create(lv_obj_t *parent, lv_obj_t *textarea,
                              const epd_keyboard_config_t *config);
void epd_keyboard_set_textarea(lv_obj_t *keyboard, lv_obj_t *textarea);
lv_obj_t *epd_keyboard_get_textarea(lv_obj_t *keyboard);
/* UI thread. Open platform voice input for the bound editor, when allowed by
 * its mode and input policy. Pending pinyin must be committed first. */
void epd_keyboard_open_voice(lv_obj_t *keyboard);
/* Rebinding or changing mode discards uncommitted composition. */
void epd_keyboard_set_mode(lv_obj_t *keyboard, const epd_keyboard_config_t *config);
/* Refresh scaled fonts and internal layout after a platform scale change. */
void epd_keyboard_refresh(lv_obj_t *keyboard);
int32_t epd_keyboard_preferred_height(lv_obj_t *keyboard);

#endif
