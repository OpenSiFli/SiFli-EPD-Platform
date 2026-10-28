/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_PANEL_PM_H
#define EPD_PANEL_PM_H
#include <stdbool.h>

/* Nonblocking read-only snapshot; false while the power mutex is owned,
 * a refresh is active, power-up is pending, or panel rails remain powered.
 * The caller must also drain the SDK LCD queue and stop new submissions. */
bool epd_panel_idle(void);

/* Result of the last complete frame, including all chunks and rail discharge.
 * Read only after stopping submissions and draining the SDK and panel queues. */
bool epd_panel_last_frame_succeeded(void);

/* Mark the controller for restoration before the next scan. Safe in the PM
 * notification with interrupts masked; hardware access runs in lcd_task. */
void epd_panel_resume(void);
#endif
