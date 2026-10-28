/* SPDX-License-Identifier: LicenseRef-SiFli */
#include "sdcard_sdio_bridge.h"
#include <drivers/mmcsd_core.h>
#include <rthw.h>
#include <string.h>

/* SiFli SDIO has at most two controller instances. Hosts live until reboot. */
static struct
{
    struct rt_mmcsd_host *host;
    sdcard_sdio_state_t state;
} sd_hosts[2];
static struct rt_mailbox *detect_mailbox;
static int active_host = -1;

rt_err_t __real_rt_mb_init(struct rt_mailbox *mb, const char *name, void *pool,
                          rt_size_t size, rt_uint8_t flag);
rt_err_t __real_rt_mb_send(struct rt_mailbox *mb, rt_uint32_t value);
rt_err_t __real_rt_mb_recv(struct rt_mailbox *mb, rt_uint32_t *value,
                          rt_int32_t timeout);

static int host_index(struct rt_mmcsd_host *host, rt_bool_t create)
{
    unsigned i;
    int empty = -1;
    for (i = 0; i < sizeof(sd_hosts) / sizeof(sd_hosts[0]); ++i)
    {
        if (sd_hosts[i].host == host)
            return (int)i;
        if (!sd_hosts[i].host)
            empty = (int)i;
    }
    if (create && empty >= 0)
        sd_hosts[empty].host = host;
    return create ? empty : -1;
}

rt_err_t __wrap_rt_mb_init(struct rt_mailbox *mb, const char *name, void *pool,
                          rt_size_t size, rt_uint8_t flag)
{
    rt_err_t result = __real_rt_mb_init(mb, name, pool, size, flag);
    if (result == RT_EOK && strcmp(name, "mmcsdmb") == 0)
        detect_mailbox = mb;
    return result;
}

rt_err_t __wrap_rt_mb_send(struct rt_mailbox *mb, rt_uint32_t value)
{
    struct rt_mmcsd_host *host;
    sdcard_sdio_state_t previous;
    rt_base_t level;
    rt_err_t result;
    int index;

    if (mb != detect_mailbox || !mb)
        return __real_rt_mb_send(mb, value);
    host = (struct rt_mmcsd_host *)value;
    if (!host)
        return -RT_EINVAL;
    if ((host->flags & MMCSD_HOST_TYPE_MASK) != MMCSD_HOST_TYPE_SDCARD)
        return __real_rt_mb_send(mb, value);

    level = rt_hw_interrupt_disable();
    index = host_index(host, RT_TRUE);
    if (index < 0)
    {
        rt_hw_interrupt_enable(level);
        return -RT_ENOMEM;
    }
    previous = sd_hosts[index].state;
    if (previous == SDCARD_SDIO_PENDING)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    sd_hosts[index].state = SDCARD_SDIO_PENDING;
    rt_hw_interrupt_enable(level);

    /* The detector can finish before rt_mb_send returns. Do not overwrite
     * completion (or a subsequent request) after a successful submission. */
    result = __real_rt_mb_send(mb, value);
    if (result != RT_EOK)
    {
        level = rt_hw_interrupt_disable();
        sd_hosts[index].state = previous;
        rt_hw_interrupt_enable(level);
    }
    return result;
}

rt_err_t __wrap_rt_mb_recv(struct rt_mailbox *mb, rt_uint32_t *value,
                          rt_int32_t timeout)
{
    rt_err_t result;
    rt_base_t level;
    if (mb != detect_mailbox || !mb)
        return __real_rt_mb_recv(mb, value, timeout);

    /* The SDK detector returns to receive only after the previous probe or
     * removal has finished, including failure paths and host unlock. */
    level = rt_hw_interrupt_disable();
    if (active_host >= 0)
        sd_hosts[active_host].state = SDCARD_SDIO_DONE;
    active_host = -1;
    rt_hw_interrupt_enable(level);
    result = __real_rt_mb_recv(mb, value, timeout);
    if (result == RT_EOK)
    {
        level = rt_hw_interrupt_disable();
        active_host = host_index((struct rt_mmcsd_host *)*value, RT_FALSE);
        rt_hw_interrupt_enable(level);
    }
    return result;
}

struct rt_mmcsd_host *sdcard_sdio_find(const char *name)
{
    struct rt_mmcsd_host *host = RT_NULL;
    unsigned i;
    rt_base_t level = rt_hw_interrupt_disable();
    for (i = 0; i < sizeof(sd_hosts) / sizeof(sd_hosts[0]); ++i)
        if (sd_hosts[i].host && strcmp(sd_hosts[i].host->name, name) == 0)
            host = sd_hosts[i].host;
    rt_hw_interrupt_enable(level);
    return host;
}

sdcard_sdio_state_t sdcard_sdio_state(struct rt_mmcsd_host *host)
{
    rt_base_t level = rt_hw_interrupt_disable();
    int index = host ? host_index(host, RT_FALSE) : -1;
    sdcard_sdio_state_t state = index < 0 ? SDCARD_SDIO_NONE : sd_hosts[index].state;
    rt_hw_interrupt_enable(level);
    return state;
}

rt_err_t sdcard_sdio_request(struct rt_mmcsd_host *host)
{
    if (!host || (host->flags & MMCSD_HOST_TYPE_MASK) != MMCSD_HOST_TYPE_SDCARD)
        return -RT_EINVAL;
    if (!detect_mailbox)
        return -RT_ENOSYS;
    return __wrap_rt_mb_send(detect_mailbox, (rt_uint32_t)host);
}
