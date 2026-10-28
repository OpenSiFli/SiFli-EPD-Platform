/* SPDX-License-Identifier: LicenseRef-SiFli */
#ifndef SDCARD_SDIO_BRIDGE_H
#define SDCARD_SDIO_BRIDGE_H

#include <rtthread.h>

struct rt_mmcsd_host;
typedef enum
{
    SDCARD_SDIO_NONE,
    SDCARD_SDIO_PENDING,
    SDCARD_SDIO_DONE,
} sdcard_sdio_state_t;

struct rt_mmcsd_host *sdcard_sdio_find(const char *name);
sdcard_sdio_state_t sdcard_sdio_state(struct rt_mmcsd_host *host);
rt_err_t sdcard_sdio_request(struct rt_mmcsd_host *host);

/* GNU ld wrappers around the SDK detector's mailbox. Other mailboxes and
 * non-SD hosts retain the original RT-Thread behavior. */
rt_err_t __wrap_rt_mb_init(struct rt_mailbox *mb, const char *name, void *pool,
                          rt_size_t size, rt_uint8_t flag);
rt_err_t __wrap_rt_mb_send(struct rt_mailbox *mb, rt_uint32_t value);
rt_err_t __wrap_rt_mb_recv(struct rt_mailbox *mb, rt_uint32_t *value,
                          rt_int32_t timeout);

#endif
