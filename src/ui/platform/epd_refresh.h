#ifndef EPD_REFRESH_H
#define EPD_REFRESH_H

#include "lvgl.h"

#define EPD_REFRESH_LOCK_INTERVAL_MS 60000u

/* Install after app_fwk and LVGL initialization, before starting applications. */
void epd_refresh_init(void);

/* UI thread only. Register a status-only region in display coordinates once per
 * screen. Screen deletion releases the registration. */
void epd_refresh_status_area(lv_obj_t *screen, const lv_area_t *area);

/* UI thread. Defer an active screen's first frame for at most 1000 ms.
 * Repeated requests cannot extend the deadline. Release on content/error;
 * screen changes, deletion and interaction also release the deferral.
 * Dirty regions and normal input/UI timers remain active. */
void epd_refresh_defer_first_frame(lv_obj_t *screen, bool defer);

/* All functions run on the UI thread. Defaults: unlocked, maintenance off.
 * All pages retain dirty regions until the preceding LCD frame has completed.
 * Same-page updates use LVGL's region merging; page changes redraw the screen
 * using AUTO, so the waveform layer continues the existing refresh period.
 * Lock mode additionally limits ordinary submissions to 60 s after completion.
 * Changing lock mode does not grant an immediate frame or select a waveform. */
void epd_refresh_set_locked(bool enabled);

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

/* Query before updating automatic lock-screen snapshots. After applying the
 * latest snapshot to widgets, call process to schedule pending dirty regions.
 * Both use the same deadline as the flush gate. A private LVGL timer also
 * services deferred invalidations; lv_refr_now cannot bypass the gate.
 * process schedules rendering, never waits for hardware or changes UI models. */
bool epd_refresh_update_due(void);
/* True after a submitted frame has completed in the SDK display path. */
bool epd_refresh_completed_once(void);
/* Submission serial changes only after a nonempty frame is handed to the SDK.
 * idle also requires the framebuffer, LCD and GPU to finish that submission. */
uint32_t epd_refresh_submission(void);
bool epd_refresh_idle(void);
void epd_refresh_process(void);

#endif
