/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_FRONTLIGHT_H
#define EPD_FRONTLIGHT_H

#include <stdbool.h>
#include <stdint.h>
#include "../boards/frontlight_device.h"

#define FRONTLIGHT_SETTINGS_PATH "/flash/system/frontlight.bin"

typedef enum {
    FRONTLIGHT_STORE_UNINITIALIZED,
    FRONTLIGHT_STORE_READY,
    FRONTLIGHT_STORE_MISSING,
    FRONTLIGHT_STORE_INVALID,
    FRONTLIGHT_STORE_IO_ERROR,
    FRONTLIGHT_STORE_SAVE_ERROR
} frontlight_store_status_t;

typedef enum {
    FRONTLIGHT_HW_UNINITIALIZED,
    FRONTLIGHT_HW_READY,
    FRONTLIGHT_HW_NO_DEVICE,
    FRONTLIGHT_HW_OPEN_ERROR,
    FRONTLIGHT_HW_READ_ERROR,
    FRONTLIGHT_HW_WRITE_ERROR,
    FRONTLIGHT_HW_MISMATCH,
    FRONTLIGHT_HW_CLOSE_ERROR
} frontlight_hardware_status_t;

typedef struct {
    /* Initialization ran after the first frame; individual operations can fail. */
    bool initialized;
    /* A loaded preference, first-use default, or startup baseline is known. Check
     * store_status separately for durability. Explicit recovery sets this only
     * after saving; committed_percent is otherwise just a rollback value. */
    bool preference_valid;
    /* The device was found. This does not imply a successful open/read/write. */
    bool available;
    bool previewing;
    bool locked;
    bool suspended;
    bool output_valid;
    bool output_pending;
    uint8_t committed_percent;
    uint8_t preview_percent;
    uint8_t desired_percent;
    uint8_t output_percent;
    frontlight_store_status_t store_status;
    /* May retain the failed preview/commit result after successful rollback. */
    frontlight_hardware_status_t hardware_status;
    bool mode_available;
    frontlight_mode_t mode;
} frontlight_snapshot_t;

/* All calls belong to the UI thread. Call init after storage initialization and
 * the first completed frame. The E0470A03/AW99703 board keeps cold-boot output
 * off until then. Init is idempotent; get/process never initialize the service
 * implicitly. */
void frontlight_init(void);
/* Reload after storage returns from USB. A confirmed missing record without a
 * runtime preference selects 15%, quantized to the driver's levels. Existing
 * preferences, including zero, are retained. Failed/invalid reads do not create
 * a record. A default is saved only after successful hardware access. */
frontlight_store_status_t frontlight_reload(void);
/* Pure snapshot copy. Percentages describe driver-reported brightness, not an
 * optical measurement. output_percent is meaningful only when output_valid. */
void frontlight_get(frontlight_snapshot_t *out);
/* Values are clamped to 100 and quantized to the configured driver's levels.
 * Preview never saves; failure cancels it and restores the committed value.
 * Preview requires a known preference and fails before initialization, while
 * locked, or while suspended. */
bool frontlight_preview(uint8_t percent);
/* Saves after verifying the output permitted by the current lock state.
 * Locked commits save the new preference without opening or extending the
 * temporary light window; a dark lock screen stays dark. Both commits and
 * mode changes fail before initialization or while suspended.
 * Failure cancels the preview and restores the committed preference; a failed
 * restore remains pending.
 * If the old preference is unknown, an explicit commit uses the current
 * readable output as a temporary rollback value. Only a successful save
 * establishes preference_valid; polling never saves this temporary baseline. */
bool frontlight_commit(uint8_t percent);
/* Select channels without changing the saved brightness. A failed hardware or
 * storage operation restores the previous mode. Off remains off, including
 * when changing the saved mode on a dark lock screen. */
bool frontlight_set_mode(frontlight_mode_t mode);
void frontlight_cancel_preview(void);
/* Cancels preview. Lock keeps an already-lit output for three seconds;
 * an already-dark output stays dark. Repeated calls do not extend the time. */
void frontlight_set_locked(bool locked);
/* UI thread: suspend cancels temporary light and forces zero. Resume restores
 * normal output when unlocked; a locked screen stays dark until wake(). */
void frontlight_set_suspended(bool suspended);
/* UI thread: while locked and resumed, use the nonzero committed preference
 * for three seconds from successful output. Never writes preferences. */
void frontlight_wake(void);
/* Milliseconds until the next deadline/retry, capped by default_ms. */
uint32_t frontlight_next_delay(uint32_t default_ms);
/* Checks external brightness changes and retries at 100 ms intervals. A due
 * shutdown deadline bypasses that retry throttle. No flash writes or worker. */
void frontlight_process(void);

#endif
