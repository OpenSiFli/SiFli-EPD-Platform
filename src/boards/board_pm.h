/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_BOARD_PM_H
#define EPD_BOARD_PM_H

#include <stdbool.h>
#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent. Call after SDK component initialization, before buttons_init.
 * available() stays false until the PA34 raw IRQ registration is chained;
 * initialization/registration failure leaves DEEP disabled. Unsupported
 * boards remain unavailable. No separate buttons hook is required. */
void board_pm_init(void);
bool board_pm_available(void);

/* UI thread, after stopping submissions and closing the frontlight.
 * true: commit only after the LCD queue, scan and rail discharge are idle and
 * PA34 is released. Failure restores the ordinary policy and returns false.
 * false: restore the ordinary policy and attempt touch recovery. True reports
 * policy restoration; board_pm_touch_resume_result() retains touch failures.
 * Call with interrupts and scheduling enabled; touch I2C/reset may block.
 * The caller owns the input/work synchronization boundary around this call.
 * This selects SDK policy; it does not put the CPU to sleep or release apps. */
bool board_pm_set_system_sleep(bool enabled);
/* UI thread: resume error clears only after verified active touch, including
 * framework close/reopen recovery. A failed touch does not block services. */
rt_err_t board_pm_touch_resume_result(void);

/* Nonblocking snapshots. Stop new draws before using display_idle as a barrier.
 * The key level is the shared physical line, independent of ADC identification. */
bool board_pm_display_idle(void);
/* Read the previous complete frame result only after stopping new submissions
 * and observing board_pm_display_idle() == true. */
bool board_pm_display_succeeded(void);
bool board_pm_key_pressed(void);

#ifdef __cplusplus
}
#endif
#endif
