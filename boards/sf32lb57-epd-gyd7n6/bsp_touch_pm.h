/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_BSP_TOUCH_PM_H
#define EPD_BSP_TOUCH_PM_H
#include <stdbool.h>
#include <rtthread.h>

/* Synchronous thread-only interface; may wait for the touch mutex, I2C and reset
 * delays (5 + 200 ms). Call with interrupts and scheduling enabled. Invalid
 * context or an unavailable driver lock returns -RT_EBUSY without changing state.
 *
 * true: mask delivery/IRQ and write FT5446 PMODE hibernate; no rail switching.
 * false: cancel sleep or resume by reset and verified interrupt-mode restoration.
 * Calls are serialized with probe/read_point/init/deinit. Successful repeated
 * requests are no-ops. Before init/after deinit, only the policy is recorded for
 * the next init; this interface does not open or power up a closed touch device.
 *
 * Failed sleep entry attempts active-state rollback and still returns the entry
 * error. Recovery allows three configuration attempts; exhaustion keeps delivery
 * gated and returns the error without further resets. Close/open or an external
 * supply cycle renews the budget. Independent services may resume on failure.
 * Framework init honors pending sleep without enabling IRQ.
 */
rt_err_t bsp_touch_set_sleep(bool sleeping);

/* Bracket an external touch supply cycle in thread context. begin attempts sleep;
 * RT_EOK means IRQ/delivery are gated and the driver mutex is STILL HELD, even
 * if IC configuration failed. Only after RT_EOK may the caller power down the
 * touch supply. On an aborted/returning terminal shutdown,
 * restore the supply first, then call end from the SAME thread on every path.
 * A failed begin releases the mutex and follows set_sleep's rollback contract.
 *
 * Other threads' probe/read_point/init/deinit/PM calls wait for end. The owning
 * thread must not call these interfaces during the hold (-RT_EBUSY). end keeps
 * delivery gated, renews recovery attempts and invalidates IC configuration.
 * set_sleep(false) must succeed before touch is usable. end does not perform I2C or
 * change supply pins. Calls from another thread/unmatched end return -RT_EBUSY.
 * Direct supply/reset calls elsewhere must follow the same exclusion contract.
 */
rt_err_t bsp_touch_power_hold_begin(void);
rt_err_t bsp_touch_power_hold_end(void);
#endif
