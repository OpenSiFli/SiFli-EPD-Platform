#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include <stdbool.h>

typedef enum
{
    UI_SETTING_TOUCH,
    UI_SETTING_TIMEOUT,
    UI_SETTING_FULL_REFRESH,
    UI_SETTING_BLUETOOTH,
    UI_SETTING_WIFI,
    UI_SETTING_LOW_POWER = 6,
    UI_SETTING_FONT,
    UI_SETTING_FONT_SIZE,
    UI_SETTING_FONT_WEIGHT,
    UI_SETTING_LINE_SPACING,
    UI_SETTING_MARGIN,
    UI_SETTING_ENCODING,
    UI_SETTING_UI_SCALE,
    UI_SETTING_SYSTEM_SLEEP,
    UI_SETTING_NAVIGATION,
    UI_SETTING_REFRESH_METHOD,
    UI_SETTING_COUNT
} ui_setting_id_t;

/* UI thread. Formatted text is borrowed until the next settings text query.
 * Copy it if the application needs to retain it. */
const char *ui_settings_value(ui_setting_id_t id);
bool ui_settings_enabled(ui_setting_id_t id);
/* Advance one option through the system settings owner; returns false on failure.
 * FONT is managed by the reading-font catalog and cannot be cycled here. */
bool ui_settings_cycle(ui_setting_id_t id);
bool ui_settings_set(ui_setting_id_t id, unsigned index);
/* FONT returns the actual stable ID, not an option index. Reading layout must
 * also compare epd_app_reading_font_revision() to detect content replacement. */
unsigned ui_settings_index(ui_setting_id_t id);

#endif
