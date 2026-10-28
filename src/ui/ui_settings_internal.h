#ifndef UI_SETTINGS_INTERNAL_H
#define UI_SETTINGS_INTERNAL_H

#include "ui_settings.h"

typedef struct
{
    const char *label;
    const char *const *options;
    unsigned option_count;
    bool is_switch;
} ui_setting_t;

typedef enum
{
    UI_SETTINGS_LOAD_OK,
    UI_SETTINGS_LOAD_MISSING,
    UI_SETTINGS_LOAD_INVALID,
    UI_SETTINGS_LOAD_UNKNOWN_VERSION,
    UI_SETTINGS_LOAD_IO_ERROR,
    UI_SETTINGS_LOAD_MIGRATED,
    UI_SETTINGS_LOAD_MIGRATION_PENDING,
    UI_SETTINGS_LOAD_APPLY_FAILED
} ui_settings_load_result_t;


const ui_setting_t *ui_settings_item(ui_setting_id_t id);
const char *ui_settings_label(ui_setting_id_t id);
/* Option text includes switches. Invalid id/index returns NULL. Formatted text
 * is borrowed until the next option/value call; copy it when retaining it. */
const char *ui_settings_option(ui_setting_id_t id, unsigned index);

void ui_settings_init(void);
/* Reload after volume recovery, before reopening ordinary configuration writes.
 * Missing/invalid/unknown uses unsaved defaults; IO leaves runtime unchanged.
 * MIGRATION_PENDING applies the migrated values but the v1 record remains.
 * APPLY_FAILED leaves the model unchanged if a wireless request fails. */
ui_settings_load_result_t ui_settings_reload(void);
/* Return the default option index (0 for an invalid id); does not save/apply. */
unsigned ui_settings_default(ui_setting_id_t id);
typedef enum {
    UI_SETTINGS_SAVE_OK,
    UI_SETTINGS_SAVE_PENDING,
    UI_SETTINGS_SAVE_FAILED
} ui_settings_save_status_t;
/* Apply an explicit option and queue persistence. False means the change was
 * not accepted; a later storage failure is reported through notifications.
 * UI_SCALE prepares fonts before applying and queueing the snapshot.
 * SYSTEM_SLEEP is the continuous eligible wait after locking.
 * An explicit save may replace an unsupported record with v2 through the file
 * transaction. Explicit same-value saves may retry a previous failure. */
bool ui_settings_set(ui_setting_id_t id, unsigned index);
/* Latest accepted snapshot, including all coalesced changes. No I/O or retry. */
ui_settings_save_status_t ui_settings_save_status(void);
/* Nonblocking I/O completion barrier. Pending saves return false; completed
 * failures return true and remain FAILED in save_status(). No retry is started.
 * Operations requiring durable success use the synchronous reset save. */
bool ui_settings_flush(void);
/* UI owner: publish completed save failures in the notification center. */
void ui_settings_process(void);

/* UI thread, after credential erasure. Preserve every non-network preference. */
bool ui_settings_reset_network(void);

unsigned ui_settings_refresh_count(void);
/* Zero disables automatic locking. Uses the saved screen-sleep preference. */
unsigned ui_settings_lock_timeout_ms(void);
/* Zero disables wallpaper system sleep. Stored values are minutes, not indices. */
unsigned ui_settings_sleep_timeout_ms(void);

#endif
