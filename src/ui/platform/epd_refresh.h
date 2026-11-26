#ifndef EPD_REFRESH_H
#define EPD_REFRESH_H

#include "lvgl.h"

#define EPD_REFRESH_LOCK_INTERVAL_MS 60000u

/* Install after app_fwk and LVGL initialization, before starting applications. */
void epd_refresh_init(void);
/* Reconcile the view's orientation before LVGL draws system overlays. */
void epd_refresh_update_orientation(void);

/* UI thread only. Register a status-only region in display coordinates once per
 * screen. Screen deletion releases the registration. */
void epd_refresh_status_area(lv_obj_t *screen, const lv_area_t *area);

/* Scope automatic widget updates to a partial-refresh classification. */
void epd_refresh_status_begin(void);
void epd_refresh_status_end(void);

/* Keyboard and editor rectangles use monochrome DU without advancing the
 * ordinary full-refresh cycle. Unregister before deleting either owner. */
void epd_refresh_input_objects(lv_obj_t *keyboard, lv_obj_t *editor, bool enabled);

/* UI thread. Defer an active screen's first frame for at most 100 ms.
 * Repeated requests cannot extend the deadline. Release on content/error;
 * screen changes, deletion and interaction also release the deferral.
 * Dirty regions and normal input/UI timers remain active. */
void epd_refresh_defer_first_frame(lv_obj_t *screen, bool defer);

/* All functions run on the UI thread. Defaults: unlocked, maintenance off.
 * One prepared frame can be held while the preceding LCD frame completes.
 * Updates within an app use the existing waveform policy. Switching apps
 * selects FULL for the first submitted frame of the destination.
 * Lock mode additionally limits ordinary submissions to 60 s after completion.
 * Changing lock mode does not grant an immediate frame or select a waveform. */
void epd_refresh_set_locked(bool enabled);

/* Defer lock-screen status updates while an unlock press is in progress. */
void epd_refresh_set_lock_hold(bool enabled);

/* Suppress ALL new frames during wallpaper maintenance, including interaction
 * requests. Finish the wallpaper transaction before enabling suppression.
 * On user wake, disable suppression and request one interaction frame. */
void epd_refresh_set_maintenance(bool enabled);

/* Grant the next nonempty frame one immediate submission. This does not dirty
 * widgets or change STATUS/CONTENT/FULL classification. The grant survives an
 * empty refresh; a maintenance transition cancels an unused old grant. */
void epd_refresh_request_interaction(void);

/* Request a full redraw and FULL waveform. This does not bypass the lock gate;
 * pair with request_interaction for an immediate system/user full refresh. */
void epd_refresh_request_full(void);
void epd_refresh_request_clean_full(void);

/* Query before updating automatic lock-screen snapshots. After applying the
 * latest snapshot to widgets, call process to schedule pending dirty regions.
 * Both use the same deadline as the flush gate. A private LVGL timer also
 * services deferred invalidations; lv_refr_now cannot bypass the gate.
 * process schedules rendering, never waits for hardware or changes UI models. */
bool epd_refresh_update_due(void);
/* True when the display can accept the next UI frame for preparation. */
bool epd_refresh_can_prepare(void);
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
void epd_reader_timing_wait_page(lv_obj_t *screen);
#endif
/* True after a submitted frame has completed in the SDK display path. */
bool epd_refresh_completed_once(void);
/* Submission serial changes only after a nonempty frame is handed to the SDK.
 * idle also requires the framebuffer, LCD and GPU to finish that submission. */
uint32_t epd_refresh_submission(void);
bool epd_refresh_idle(void);
void epd_refresh_process(void);

#endif
