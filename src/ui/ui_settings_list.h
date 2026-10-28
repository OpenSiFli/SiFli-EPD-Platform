#ifndef UI_SETTINGS_LIST_H
#define UI_SETTINGS_LIST_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_settings_list ui_settings_list_t;

typedef enum {
    UI_SETTINGS_ROW_LINK,
    UI_SETTINGS_ROW_INFO,
    UI_SETTINGS_ROW_SWITCH,
    UI_SETTINGS_ROW_CHOICE,
    UI_SETTINGS_ROW_INLINE_INFO,
    UI_SETTINGS_ROW_SECTION
} ui_settings_row_kind_t;

typedef struct {
    int key;
    ui_settings_row_kind_t kind;
    const char *title;
    const char *value;
    const void *icon;
    lv_coord_t icon_size; /* Zero uses the default 60 px. */
    const lv_image_dsc_t *trailing_icon;
    lv_coord_t control_size; /* Zero uses the standard control geometry. */
    bool muted_title;
    bool black_value;
    bool joined_value;
    bool enabled;
    bool checked;
    lv_coord_t min_height;
    bool no_chevron;
    bool no_separator;
    lv_coord_t bottom_gap;
} ui_settings_list_row_t;

typedef void (*ui_settings_list_cb_t)(int key, void *user_data);

/* The parent supplies the available content area. Fonts and image descriptors
 * remain caller-owned. Deleting the parent also releases the list. */
ui_settings_list_t *ui_settings_list_create(
    lv_obj_t *parent, const lv_font_t *body, const lv_font_t *small_font,
    ui_settings_list_cb_t callback, void *user_data);

/* Copies rows and all text (including file/symbol image sources). Keys must be
 * unique. Unchanged visible geometry preserves objects and focus; changes to
 * other pages alone do not invalidate this page. Failure leaves the previous
 * rows and page intact. NULL/0 clears. */
bool ui_settings_list_set_rows(ui_settings_list_t *list,
    const ui_settings_list_row_t *rows, size_t count);

bool ui_settings_list_update_layout(ui_settings_list_t *list);
bool ui_settings_list_refresh_fonts(ui_settings_list_t *list,
    const lv_font_t *body, const lv_font_t *small_font);

/* Only enabled, visible non-INFO rows are added, in visible row order. Calling
 * again with the same group restores externally removed members. Detach before
 * deleting group. */
void ui_settings_list_set_group(ui_settings_list_t *list, lv_group_t *group);
bool ui_settings_list_show_key(ui_settings_list_t *list, int key);
bool ui_settings_list_focus_key(ui_settings_list_t *list, int key);
/* Shared horizontal margin for list rows and adjacent page controls. */
lv_coord_t ui_settings_list_content_margin(lv_coord_t width);
bool ui_settings_list_set_insets(ui_settings_list_t *list,
    lv_coord_t left, lv_coord_t top, lv_coord_t right, lv_coord_t bottom);

bool ui_settings_list_next_page(ui_settings_list_t *list);
bool ui_settings_list_prev_page(ui_settings_list_t *list);
/* Page indexes are zero-based; an empty list has one empty page. */
size_t ui_settings_list_get_page(const ui_settings_list_t *list);
size_t ui_settings_list_get_page_count(const ui_settings_list_t *list);
lv_obj_t *ui_settings_list_get_obj(ui_settings_list_t *list);
/* Optional early release. The handle becomes invalid after object deletion. */
void ui_settings_list_destroy(ui_settings_list_t *list);

#ifdef __cplusplus
}
#endif
#endif
