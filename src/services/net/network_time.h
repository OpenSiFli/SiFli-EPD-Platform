/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NETWORK_TIME_H
#define NETWORK_TIME_H

#include <rtthread.h>
#include <stdbool.h>

rt_err_t network_time_init(void);
/* Wake the worker after an automatic-mode change or settings reload. */
void network_time_settings_changed(void);

/* Thread context. RT_EOK accepts the asynchronous stop; poll suspend_result()
 * before suspending the network. Active work holds the "network_time" sleep
 * guard, so request this stop before waiting for holders to drain.
 * Repeated suspend calls keep the same deadline.
 * A service that has not started is already idle; initialization in progress
 * must drain. Initialization cannot start while suspended. */
#define NETWORK_TIME_SUSPEND_TIMEOUT_MS 20000
rt_err_t network_time_suspend(void);
/* RT_EOK: worker parked, DNS returned, UDP socket closed, RTC commit drained.
 * -RT_EBUSY: still draining, for at most NETWORK_TIME_SUSPEND_TIMEOUT_MS.
 * -RT_ETIMEOUT: this stop failed; remains failed until resume(), even if the
 * worker later drains. A synchronous DNS call cannot safely be force-deleted:
 * its sleep guard stays held until it returns. Abort sleep on any final error.
 * -RT_ERROR: no suspend request. Polling never blocks or changes time settings. */
rt_err_t network_time_suspend_result(void);
/* Idempotent. Call after sleep_guard_cancel() on wakeup or aborted preparation.
 * Always wakes the worker, even if no suspend was requested: PREPARING alone
 * may have parked it. Without a preceding suspend, the active transaction,
 * sync result and retry budget are preserved. After suspend, the cancelled
 * transaction is discarded and a fresh retry budget starts once a route is
 * ready, only if automatic time is enabled. Does not persist any settings. */
void network_time_resume(void);

#endif
