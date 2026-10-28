/*
 * Copyright (c) 2026, SiFli Technology
 * SPDX-License-Identifier: LicenseRef-SiFli
 * See LICENSE in the component root.
 */
#include "sdcard_port.h"
#include <rthw.h>
#include <rtdevice.h>
#include <drivers/mmcsd_core.h>
#include "sdcard_sdio_bridge.h"
#include <dfs_fs.h>
#include <string.h>

rt_err_t sdcard_port_bind(const char *device_name, struct rt_mmcsd_host **host)
{
    struct rt_mmcsd_host *candidate = sdcard_sdio_find(device_name);

    if (!candidate)
    {
        return -RT_ENOSYS;
    }
    if ((candidate->flags & MMCSD_HOST_TYPE_MASK) != MMCSD_HOST_TYPE_SDCARD ||
        rt_strcmp(candidate->name, device_name) != 0)
    {
        return -RT_EINVAL;
    }
    *host = candidate;
    return RT_EOK;
}

void sdcard_port_get_status(struct rt_mmcsd_host *host, sdcard_port_status_t *status)
{
    rt_base_t level;
    rt_device_t device;

    rt_memset(status, 0, sizeof(*status));
    level = rt_hw_interrupt_disable();
    switch (sdcard_sdio_state(host))
    {
    case SDCARD_SDIO_PENDING:
        status->change = SDCARD_CHANGE_PENDING;
        break;
    case SDCARD_SDIO_DONE:
        status->change = SDCARD_CHANGE_DONE;
        break;
    default:
        status->change = SDCARD_CHANGE_NONE;
        break;
    }
    /* The detector may be modifying card while PENDING. Read the result only
     * after completion, without waiting on its bus lock or consuming global IPC. */
    if (status->change != SDCARD_CHANGE_PENDING && host->card)
    {
        status->has_card = RT_TRUE;
        status->sd_memory = host->card->card_type == CARD_TYPE_SD &&
                            host->card->sdio_function_num == 0;
    }
    rt_hw_interrupt_enable(level);
    if (status->change != SDCARD_CHANGE_PENDING)
    {
        device = rt_device_find(host->name);
        status->block_device = device && device->type == RT_Device_Class_Block;
    }
}

rt_err_t sdcard_port_request(struct rt_mmcsd_host *host)
{
    return sdcard_sdio_request(host);
}

#include "sdcard_identity_impl.h"
