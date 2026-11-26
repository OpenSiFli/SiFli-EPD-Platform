#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include <stdbool.h>

#define UI_READING_FONT_SIZES 36, 40, 44, 48, 52, 56, 60, 64
#define UI_FULL_REFRESH_COUNTS 1, 5, 10, 15, 20, 25, 30, 35, 40

enum { UI_READING_LAYOUT_STEPS = 5, UI_READING_LINE_DEFAULT = 1, UI_READING_MARGIN_DEFAULT = 0 };

static inline unsigned ui_reading_line_percent(unsigned index)
{
    static const unsigned char percent[UI_READING_LAYOUT_STEPS] = {100, 110, 130, 150, 170};
    return percent[index < UI_READING_LAYOUT_STEPS ? index : UI_READING_LINE_DEFAULT];
}

static inline unsigned ui_reading_line_space(unsigned line_height, unsigned index)
{
    return line_height * (ui_reading_line_percent(index) - 100u) / 100u;
}

static inline unsigned ui_reading_margin_px(unsigned index)
{
    static const unsigned char margin[UI_READING_LAYOUT_STEPS] = {2, 6, 10, 14, 18};
    return margin[index < UI_READING_LAYOUT_STEPS ? index : UI_READING_MARGIN_DEFAULT];
}

enum { UI_READER_NEGATIVE = 1u, UI_READER_GUIDE_SOLID = 2u,
       UI_READER_GUIDE_DOTTED = 4u, UI_READER_GUIDE_MASK = 6u,
       UI_READER_ANTIALIAS = 8u, UI_READER_APPEARANCE_COUNT = 14u };

static inline bool ui_reader_appearance_valid(unsigned appearance)
{
    return appearance < UI_READER_APPEARANCE_COUNT &&
           (appearance & UI_READER_GUIDE_MASK) != UI_READER_GUIDE_MASK;
}

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
    UI_SETTING_READER_APPEARANCE,
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
