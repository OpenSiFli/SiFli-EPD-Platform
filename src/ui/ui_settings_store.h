#ifndef UI_SETTINGS_STORE_H
#define UI_SETTINGS_STORE_H
#include "ui_settings_internal.h"
#include "ui_settings.h"
#include <stdint.h>

/* Initialize on the UI owner before concurrent status/flush calls. */
bool ui_settings_store_start(void);
/* Synchronous reload drains accepted saves first. An unsuccessful pending save
 * returns IO instead of replacing unsaved runtime values with an old record.
 * Missing/invalid/unknown supplies unsaved defaults. Cache entries are bound to
 * path/volume revisions and are invalid while a file writer is active. */
ui_settings_load_result_t ui_settings_store_reload(uint8_t values[UI_SETTING_COUNT]);
/* Compatibility: true only for OK, MIGRATED or MIGRATION_PENDING. */
bool ui_settings_store_load(uint8_t values[UI_SETTING_COUNT]);
/* Accept the latest snapshot; never waits for storage. One worker serializes
 * writes and coalesces snapshots still queued. Failure is terminal until an
 * explicit new save. No caller retains access to the copied input buffer. */
bool ui_settings_store_save(const uint8_t values[UI_SETTING_COUNT]);
/* Reset barrier: reject new submissions until this snapshot is committed or
 * fails, then return its actual result. UI owner only, never the save worker. */
bool ui_settings_store_save_sync(const uint8_t values[UI_SETTING_COUNT]);
ui_settings_save_status_t ui_settings_store_status(uint32_t *revision);
#endif
