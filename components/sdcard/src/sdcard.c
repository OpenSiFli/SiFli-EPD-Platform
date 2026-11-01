/*
 * Derived from Solution2.0 solution/components/tf/tf_init.c.
 * Copyright (c) 2019 - 2024,  Sifli Technology
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Sifli integrated circuit
 *    in a product or a software update for such product, must reproduce the above
 *    copyright notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Sifli nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Sifli integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY SIFLI TECHNOLOGY "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SIFLI TECHNOLOGY OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "sdcard.h"
#include "sdcard_port.h"
#include <rthw.h>
#include <rtdevice.h>
#include <dfs_fs.h>
#include <dfs_file.h>
#include <string.h>

#define DBG_TAG "sdcard"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* Sampling and probe budgets follow Solution's TF worker. */
#define SDCARD_SETTLE_MS       15
#define SDCARD_CONFIRM_MS      3
#define SDCARD_POLL_MS         30
#define SDCARD_PROBE_POLLS     1000
#define SDCARD_REMOVE_POLLS    100
/* Platform baseline: report slow release, then keep waiting cooperatively. */
#define SDCARD_RELEASE_MS      35000
#define SDCARD_STACK_SIZE      3072
#define SDCARD_PRIORITY        22

static struct
{
    sdcard_config_t config;
    sdcard_snapshot_t view;
    struct rt_mmcsd_host *host;
    struct rt_semaphore event;
    struct rt_semaphore released;
    rt_thread_t worker;
    uint32_t edges;
    uint32_t removals;
    uint32_t fault_removals;
    uint32_t observed_removals;
    uint32_t next_release;
    rt_bool_t invalidated;
    rt_bool_t wake_pending;
    rt_bool_t acknowledged;
    rt_bool_t export_draining;
    rt_bool_t release_posted;
    rt_device_t export_device; /* Lifetime protected by config.io_lock. */
    struct rt_device_blk_geometry export_geometry;
    rt_err_t export_write_error;
    rt_bool_t fatal;
    rt_bool_t boot_checked;
    rt_bool_t operation_busy;
    rt_bool_t worker_busy;
    rt_bool_t users_released;
    rt_bool_t reconcile_deferred;
    unsigned lifecycle; /* 0: stopped, 1: starting, 2: resident. */
} s_card;

static uint32_t _next_id(uint32_t value)
{
    ++value;
    return value ? value : 1;
}

static void _publish_changed(void)
{
    ++s_card.view.revision;
    if (s_card.config.notify) s_card.config.notify();
}

static rt_bool_t _pin_present(void)
{
    return rt_pin_read(s_card.config.detect_pin) == s_card.config.present_level;
}

static void _revoke_local_locked(void)
{
    if (s_card.view.available)
    {
        s_card.view.available = RT_FALSE;
        s_card.view.session = _next_id(s_card.view.session);
        _publish_changed();
    }
}

static void _invalidate_locked(void)
{
    s_card.invalidated = RT_TRUE;
    _revoke_local_locked();
    if (s_card.view.export_requested)
    {
        s_card.view.export_requested = RT_FALSE;
        _publish_changed();
    }
}

/* Called with interrupts disabled; acknowledgment and cancellation share one
 * queued token. Predicates, rather than the token, decide why draining ended. */
static rt_err_t _notify_release_locked(void)
{
    rt_err_t error = RT_EOK;
    if (!s_card.release_posted)
    {
        s_card.release_posted = RT_TRUE;
        error = rt_sem_release(&s_card.released);
        if (error != RT_EOK)
        {
            s_card.release_posted = RT_FALSE;
        }
    }
    return error;
}

static void _wake_worker(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_bool_t post = !s_card.wake_pending;
    s_card.wake_pending = RT_TRUE;
    rt_hw_interrupt_enable(level);
    if (post)
    {
        rt_sem_release(&s_card.event);
    }
}

static void _pin_irq(void *argument)
{
    rt_base_t level;
    (void)argument;
    level = rt_hw_interrupt_disable();
    ++s_card.edges;
    if (!_pin_present())
    {
        ++s_card.removals;
        _invalidate_locked();
    }
    if (s_card.view.state == SDCARD_DRAINING) _notify_release_locked();
    rt_hw_interrupt_enable(level);
    _wake_worker();
}

static void _set_state(sdcard_state_t state, sdcard_error_stage_t stage, rt_err_t error)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (s_card.view.state != state || s_card.view.error_stage != stage ||
        s_card.view.error != error)
    {
        s_card.view.state = state;
        s_card.view.error_stage = stage;
        s_card.view.error = error;
        _publish_changed();
    }
    rt_hw_interrupt_enable(level);
}

static void _fault(sdcard_error_stage_t stage, rt_err_t error, rt_bool_t fatal)
{
    rt_base_t level = rt_hw_interrupt_disable();
    _invalidate_locked();
    s_card.fatal = fatal;
    s_card.fault_removals = s_card.observed_removals;
    rt_hw_interrupt_enable(level);
    _set_state(SDCARD_FAULT, stage, error);
    LOG_W("stage=%d error=%d isolated=%d", stage, error, fatal);
}

static uint32_t _sample(rt_bool_t *present)
{
    uint32_t edges;
    rt_bool_t first, second;
    rt_base_t level;

    for (;;)
    {
        level = rt_hw_interrupt_disable();
        edges = s_card.edges;
        rt_hw_interrupt_enable(level);
        rt_thread_mdelay(SDCARD_SETTLE_MS);
        first = _pin_present();
        rt_thread_mdelay(SDCARD_CONFIRM_MS);
        second = _pin_present();
        level = rt_hw_interrupt_disable();
        if (first == second && edges == s_card.edges)
        {
            if (s_card.view.present != second)
            {
                s_card.view.present = second;
                _publish_changed();
            }
            if (!second)
            {
                _invalidate_locked();
            }
            *present = second;
            s_card.observed_removals = s_card.removals;
            rt_hw_interrupt_enable(level);
            return edges;
        }
        rt_hw_interrupt_enable(level);
    }
}

static rt_bool_t _current(uint32_t edges)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_bool_t current = edges == s_card.edges && _pin_present();
    rt_hw_interrupt_enable(level);
    return current;
}

static rt_err_t _wait_change(unsigned polls, sdcard_error_stage_t stage,
                            sdcard_port_status_t *status)
{
    sdcard_port_get_status(s_card.host, status);
    while (status->change == SDCARD_CHANGE_PENDING && polls)
    {
        --polls;
        rt_thread_mdelay(SDCARD_POLL_MS);
        sdcard_port_get_status(s_card.host, status);
    }
    if (status->change == SDCARD_CHANGE_PENDING)
    {
        /* A timeout does not cancel the SDK operation. No further requests. */
        _fault(stage, -RT_ETIMEOUT, RT_TRUE);
        return -RT_ETIMEOUT;
    }
    return RT_EOK;
}

static rt_bool_t _has_open_files(void)
{
    rt_bool_t busy = RT_FALSE;
    struct dfs_fdtable *table;
    unsigned i;

    dfs_lock();
    table = dfs_fdtable_get();
    for (i = 0; i < table->maxfd; ++i)
    {
        struct dfs_fd *fd = table->fds[i];
        if (fd && fd->ref_count && fd->fs && fd->fs->path &&
            strcmp(fd->fs->path, s_card.config.mount_point) == 0)
        {
            busy = RT_TRUE;
            break;
        }
    }
    dfs_unlock();
    return busy;
}

static rt_err_t _dfs_error(void)
{
    rt_err_t error = rt_get_errno();
    return error ? (error < 0 ? error : -error) : -RT_ERROR;
}

static void _set_mounted(rt_bool_t mounted)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (s_card.view.mounted != mounted)
    {
        s_card.view.mounted = mounted;
        _publish_changed();
    }
    rt_hw_interrupt_enable(level);
}

static rt_bool_t _drain(rt_bool_t exporting)
{
    rt_err_t error;
    rt_base_t level;
    rt_tick_t timeout = rt_tick_from_millisecond(SDCARD_RELEASE_MS);
    uint32_t observed_edges;

    level = rt_hw_interrupt_disable();
    if (exporting && !s_card.view.export_requested)
    {
        rt_hw_interrupt_enable(level);
        return RT_FALSE;
    }
    s_card.export_draining = exporting;
    s_card.next_release = _next_id(s_card.next_release);
    s_card.view.release_id = s_card.next_release;
    s_card.acknowledged = RT_FALSE;
    s_card.view.state = SDCARD_DRAINING;
    s_card.view.error_stage = SDCARD_ERROR_NONE;
    s_card.view.error = RT_EOK;
    observed_edges = s_card.edges;
    _publish_changed();
    rt_hw_interrupt_enable(level);

    for (;;)
    {
        /* No I/O or host lock is held while the platform saves/releases users. */
        error = rt_sem_take(&s_card.released, timeout);
        level = rt_hw_interrupt_disable();
        if (error == RT_EOK)
        {
            s_card.release_posted = RT_FALSE;
        }
        if (error == RT_EOK && s_card.acknowledged)
        {
            s_card.export_draining = RT_FALSE;
            rt_hw_interrupt_enable(level);
            return RT_TRUE;
        }
        if (error == RT_EOK && exporting && !s_card.view.export_requested &&
            !s_card.invalidated && _pin_present())
        {
            s_card.export_draining = RT_FALSE;
            s_card.view.release_id = 0;
            _publish_changed();
            rt_hw_interrupt_enable(level);
            _set_state(SDCARD_READY, SDCARD_ERROR_NONE, RT_EOK);
            return RT_FALSE;
        }
        rt_bool_t changed = observed_edges != s_card.edges;
        rt_hw_interrupt_enable(level);
        if (changed)
        {
            rt_bool_t present;
            observed_edges = _sample(&present);
            level = rt_hw_interrupt_disable();
            if (s_card.acknowledged)
            {
                s_card.export_draining = RT_FALSE;
                rt_hw_interrupt_enable(level);
                return RT_TRUE;
            }
            if (present)
            {
                /* A stable reinsert permits one new release attempt. */
                s_card.next_release = _next_id(s_card.next_release);
                s_card.view.release_id = s_card.next_release;
                s_card.view.error_stage = SDCARD_ERROR_NONE;
                s_card.view.error = RT_EOK;
                timeout = rt_tick_from_millisecond(SDCARD_RELEASE_MS);
                _publish_changed();
            }
            rt_hw_interrupt_enable(level);
            if (present) continue;
        }
        if (error == -RT_ETIMEOUT)
        {
            _set_state(SDCARD_DRAINING, SDCARD_ERROR_RELEASE_WAIT, error);
            timeout = RT_WAITING_FOREVER;
        }
        else if (error != RT_EOK)
        {
            level = rt_hw_interrupt_disable();
            s_card.export_draining = RT_FALSE;
            rt_hw_interrupt_enable(level);
            _fault(SDCARD_ERROR_RELEASE_WAIT, error, RT_TRUE);
            return RT_FALSE;
        }
    }
}

/* io_lock is held by the caller. A close failure retains the owned handle. */
static rt_err_t _close_export_locked(void)
{
    rt_err_t error;
    if (!s_card.export_device)
    {
        return RT_EOK;
    }
    error = rt_device_close(s_card.export_device);
    if (error == RT_EOK)
    {
        s_card.export_device = RT_NULL;
        rt_memset(&s_card.export_geometry, 0, sizeof(s_card.export_geometry));
        s_card.export_write_error = RT_EOK;
    }
    return error;
}

static rt_bool_t _close_export(void)
{
    rt_err_t error = rt_mutex_take(s_card.config.io_lock, RT_WAITING_FOREVER);
    if (error == RT_EOK)
    {
        error = _close_export_locked();
        rt_mutex_release(s_card.config.io_lock);
    }
    if (error != RT_EOK)
    {
        _fault(SDCARD_ERROR_EXPORT_CLOSE, error, RT_TRUE);
        return RT_FALSE;
    }
    return RT_TRUE;
}

/* io_lock is held across the last file check, revocation and DFS unmount. */
static rt_bool_t _unmount_locked(void)
{
    rt_base_t level;
    if (_has_open_files())
    {
        _fault(SDCARD_ERROR_OPEN_FILES, -RT_EBUSY, RT_TRUE);
        return RT_FALSE;
    }
    level = rt_hw_interrupt_disable();
    _revoke_local_locked();
    rt_hw_interrupt_enable(level);
    if (s_card.view.mounted)
    {
        rt_set_errno(0);
        if (dfs_unmount(s_card.config.mount_point) != 0)
        {
            _fault(SDCARD_ERROR_UNMOUNT, _dfs_error(), RT_TRUE);
            return RT_FALSE;
        }
        _set_mounted(RT_FALSE);
    }
    return RT_TRUE;
}

static void _retire(void)
{
    rt_err_t error;
    rt_base_t level;
    rt_bool_t unmounted;
    sdcard_port_status_t status;

    level = rt_hw_interrupt_disable();
    _invalidate_locked();
    rt_hw_interrupt_enable(level);
    /* Wait for in-flight raw I/O and close before the SDK may free the card. */
    if (s_card.export_device && !_close_export())
    {
        return;
    }
    if (!s_card.users_released &&
        (!s_card.view.release_id || !s_card.acknowledged) && !_drain(RT_FALSE))
    {
        return;
    }
    _set_state(SDCARD_UNMOUNTING, SDCARD_ERROR_NONE, RT_EOK);
    error = rt_mutex_take(s_card.config.io_lock, RT_WAITING_FOREVER);
    if (error != RT_EOK)
    {
        _fault(SDCARD_ERROR_UNMOUNT, error, RT_TRUE);
        return;
    }
    unmounted = _unmount_locked();
    rt_mutex_release(s_card.config.io_lock);
    if (!unmounted)
    {
        return;
    }

    sdcard_port_get_status(s_card.host, &status);
    if (status.change == SDCARD_CHANGE_PENDING)
    {
        _fault(SDCARD_ERROR_REMOVE_SUBMIT, -RT_EBUSY, RT_TRUE);
        return;
    }
    if (status.has_card)
    {
        error = sdcard_port_request(s_card.host);
        if (error != RT_EOK)
        {
            _fault(SDCARD_ERROR_REMOVE_SUBMIT, error, RT_TRUE);
            return;
        }
        if (_wait_change(SDCARD_REMOVE_POLLS, SDCARD_ERROR_REMOVE_WAIT, &status) != RT_EOK)
        {
            return;
        }
    }
    if (status.has_card || status.block_device)
    {
        _fault(SDCARD_ERROR_REMOVE_RESULT, -RT_ERROR, RT_TRUE);
        return;
    }
    level = rt_hw_interrupt_disable();
    s_card.view.release_id = 0;
    s_card.invalidated = RT_FALSE;
    _publish_changed();
    rt_hw_interrupt_enable(level);
    _set_state(SDCARD_ABSENT, SDCARD_ERROR_NONE, RT_EOK);
    _wake_worker(); /* Reconcile an insertion received while releasing the old card. */
}

static rt_bool_t _export_wanted(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_bool_t wanted = s_card.view.export_requested && !s_card.invalidated &&
                       s_card.view.present && _pin_present();
    rt_hw_interrupt_enable(level);
    return wanted;
}

static void _begin_export(uint32_t edges)
{
    rt_err_t error;
    rt_base_t level;
    rt_device_t device;
    sdcard_error_stage_t stage = SDCARD_ERROR_EXPORT_OPEN;
    struct rt_device_blk_geometry geometry = {0};

    if (!_drain(RT_TRUE))
    {
        return;
    }
    error = rt_mutex_take(s_card.config.io_lock, RT_WAITING_FOREVER);
    if (error != RT_EOK)
    {
        _fault(SDCARD_ERROR_UNMOUNT, error, RT_TRUE);
        return;
    }
    _set_state(SDCARD_UNMOUNTING, SDCARD_ERROR_NONE, RT_EOK);
    if (!_unmount_locked())
    {
        rt_mutex_release(s_card.config.io_lock);
        return;
    }
    if (!_export_wanted() || !_current(edges))
    {
        rt_mutex_release(s_card.config.io_lock);
        _wake_worker();
        return;
    }
    device = rt_device_find(s_card.config.device_name);
    error = device && device->type == RT_Device_Class_Block ?
            rt_device_open(device, RT_DEVICE_OFLAG_RDWR) : -RT_ERROR;
    if (error == RT_EOK)
    {
        s_card.export_device = device;
        stage = SDCARD_ERROR_EXPORT_GEOMETRY;
        error = rt_device_control(device, RT_DEVICE_CTRL_BLK_GETGEOME, &geometry);
        /* The native SD block driver transfers 512-byte sectors. */
        if (error == RT_EOK && (!geometry.sector_count || geometry.bytes_per_sector != 512))
        {
            error = -RT_EINVAL;
        }
    }
    if (error != RT_EOK)
    {
        rt_err_t close_error = _close_export_locked();
        rt_mutex_release(s_card.config.io_lock);
        _fault(close_error == RT_EOK ? stage : SDCARD_ERROR_EXPORT_CLOSE,
               close_error == RT_EOK ? error : close_error, close_error != RT_EOK);
        return;
    }
    level = rt_hw_interrupt_disable();
    if (s_card.view.export_requested && !s_card.invalidated &&
        edges == s_card.edges && _pin_present())
    {
        s_card.export_geometry = geometry;
        s_card.export_write_error = RT_EOK;
        s_card.view.release_id = 0;
        s_card.view.state = SDCARD_EXPORTED;
        _publish_changed();
    }
    rt_hw_interrupt_enable(level);
    rt_mutex_release(s_card.config.io_lock);
    /* Reconcile disable/removal racing open or geometry before publication. */
    _wake_worker();
}

static void _reconcile(void)
{
    rt_bool_t present;
    rt_bool_t adopted;
    uint32_t edges;
    rt_base_t level;
    rt_err_t error;
    sdcard_port_status_t status;

    if (s_card.fatal)
    {
        return;
    }
    edges = _sample(&present);
    level = rt_hw_interrupt_disable();
    if (s_card.view.state == SDCARD_FAULT && present &&
        s_card.fault_removals == s_card.removals)
    {
        rt_hw_interrupt_enable(level);
        return; /* Probe/mount failures require removal before another attempt. */
    }
    rt_hw_interrupt_enable(level);

    sdcard_port_get_status(s_card.host, &status);
    adopted = !s_card.boot_checked && status.change != SDCARD_CHANGE_NONE;
    s_card.boot_checked = RT_TRUE;
    if (status.change == SDCARD_CHANGE_PENDING)
    {
        adopted = RT_TRUE;
        _set_state(SDCARD_PROBING, SDCARD_ERROR_NONE, RT_EOK);
        if (_wait_change(SDCARD_PROBE_POLLS, SDCARD_ERROR_PROBE_WAIT, &status) != RT_EOK)
        {
            return;
        }
    }
    level = rt_hw_interrupt_disable();
    if (edges != s_card.edges)
    {
        _invalidate_locked();
    }
    present = _pin_present();
    if ((s_card.invalidated || !present) && (status.has_card || s_card.view.mounted))
    {
        rt_hw_interrupt_enable(level);
        _retire();
        return;
    }
    if (!present)
    {
        s_card.invalidated = RT_FALSE;
        rt_hw_interrupt_enable(level);
        _set_state(SDCARD_ABSENT, SDCARD_ERROR_NONE, RT_EOK);
        return;
    }
    if (edges != s_card.edges)
    {
        rt_hw_interrupt_enable(level);
        _wake_worker();
        return;
    }
    if (s_card.export_device)
    {
        rt_bool_t keep = s_card.view.export_requested;
        rt_hw_interrupt_enable(level);
        if (!keep && _close_export())
        {
            _set_state(SDCARD_PROBING, SDCARD_ERROR_NONE, RT_EOK);
            _wake_worker();
        }
        return;
    }
    if (s_card.view.available)
    {
        rt_bool_t exporting = s_card.view.export_requested;
        rt_hw_interrupt_enable(level);
        if (exporting)
        {
            _begin_export(edges);
        }
        return;
    }
    s_card.invalidated = RT_FALSE;
    rt_hw_interrupt_enable(level);

    _set_state(SDCARD_PROBING, SDCARD_ERROR_NONE, RT_EOK);
    if (!status.has_card && !adopted)
    {
        error = sdcard_port_request(s_card.host);
        if (error != RT_EOK)
        {
            _fault(SDCARD_ERROR_PROBE_SUBMIT, error, RT_FALSE);
            return;
        }
        if (_wait_change(SDCARD_PROBE_POLLS, SDCARD_ERROR_PROBE_WAIT, &status) != RT_EOK)
        {
            return;
        }
    }
    if (!_current(edges))
    {
        level = rt_hw_interrupt_disable();
        _invalidate_locked();
        rt_hw_interrupt_enable(level);
        if (status.has_card)
        {
            _retire();
        }
        else
        {
            _wake_worker();
        }
        return;
    }
    if (!status.has_card || !status.sd_memory || !status.block_device)
    {
        _fault(SDCARD_ERROR_PROBE_RESULT, -RT_ERROR, RT_FALSE);
        return;
    }

    error = rt_mutex_take(s_card.config.io_lock, RT_WAITING_FOREVER);
    if (error != RT_EOK)
    {
        _fault(SDCARD_ERROR_MOUNT, error, RT_FALSE);
        return;
    }
    if (!_current(edges))
    {
        rt_mutex_release(s_card.config.io_lock);
        _retire();
        return;
    }
    rt_set_errno(0);
    error = dfs_mount(s_card.config.device_name, s_card.config.mount_point, "elm", 0, RT_NULL);
    if (error != 0)
    {
        error = _dfs_error();
    }
    else
    {
        if (s_card.config.mounted) s_card.config.mounted();
        _set_mounted(RT_TRUE);
    }
    rt_mutex_release(s_card.config.io_lock);

    level = rt_hw_interrupt_disable();
    if (edges != s_card.edges || !_pin_present())
    {
        _invalidate_locked();
        rt_hw_interrupt_enable(level);
        _retire();
        return;
    }
    if (error != RT_EOK)
    {
        /* A readable boot sector may still identify an unmountable volume. */
        s_card.view.session = _next_id(s_card.view.session);
        rt_hw_interrupt_enable(level);
        _fault(SDCARD_ERROR_MOUNT, error, RT_FALSE);
        return;
    }
    s_card.view.available = RT_TRUE;
    s_card.users_released = RT_FALSE;
    s_card.view.session = _next_id(s_card.view.session);
    s_card.view.state = SDCARD_READY;
    s_card.view.release_id = 0;
    s_card.view.error_stage = SDCARD_ERROR_NONE;
    s_card.view.error = RT_EOK;
    _publish_changed();
    rt_hw_interrupt_enable(level);
}

static void _worker(void *argument)
{
    rt_base_t level;
    (void)argument;
    for (;;)
    {
        rt_sem_take(&s_card.event, RT_WAITING_FOREVER);
        level = rt_hw_interrupt_disable();
        s_card.wake_pending = RT_FALSE;
        if (s_card.operation_busy)
        {
            s_card.reconcile_deferred = RT_TRUE;
            rt_hw_interrupt_enable(level);
            continue;
        }
        s_card.worker_busy = RT_TRUE;
        rt_hw_interrupt_enable(level);
        _reconcile();
        level = rt_hw_interrupt_disable();
        s_card.worker_busy = RT_FALSE;
        rt_hw_interrupt_enable(level);
    }
}

void sdcard_get_snapshot(sdcard_snapshot_t *snapshot)
{
    rt_base_t level;
    RT_ASSERT(snapshot);
    level = rt_hw_interrupt_disable();
    *snapshot = s_card.view;
    rt_hw_interrupt_enable(level);
}

rt_err_t sdcard_release_complete(uint32_t release_id)
{
    rt_err_t error;
    rt_base_t level = rt_hw_interrupt_disable();
    if (s_card.lifecycle != 2 || s_card.view.state != SDCARD_DRAINING ||
        !release_id || release_id != s_card.view.release_id ||
        (s_card.export_draining && !s_card.view.export_requested && !s_card.invalidated))
    {
        rt_hw_interrupt_enable(level);
        return -RT_EINVAL;
    }
    if (s_card.acknowledged)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    s_card.acknowledged = RT_TRUE;
    error = _notify_release_locked();
    if (error != RT_EOK)
    {
        s_card.acknowledged = RT_FALSE;
    }
    rt_hw_interrupt_enable(level);
    return error;
}

rt_err_t sdcard_set_export(rt_bool_t enabled)
{
    rt_err_t error = RT_EOK;
    rt_base_t level = rt_hw_interrupt_disable();
    if (s_card.lifecycle != 2)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EINVAL;
    }
    if (enabled && (s_card.operation_busy || s_card.view.state != SDCARD_READY || !s_card.view.available ||
                    !s_card.view.mounted || s_card.invalidated ||
                    !s_card.view.present || !_pin_present()))
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    enabled = enabled ? RT_TRUE : RT_FALSE;
    if (!enabled && !s_card.view.export_requested && !s_card.export_draining)
    {
        rt_hw_interrupt_enable(level);
        return RT_EOK;
    }
    if (s_card.view.export_requested != enabled)
    {
        s_card.view.export_requested = enabled;
        _publish_changed();
    }
    if (!enabled && s_card.export_draining)
    {
        error = _notify_release_locked();
    }
    rt_hw_interrupt_enable(level);
    _wake_worker();
    return error;
}

/* Callers hold io_lock; IRQ protection makes permission checks coherent with
 * disable and physical removal, neither of which waits for in-flight I/O. */
static rt_bool_t _export_ready(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_bool_t ready = s_card.lifecycle == 2 &&
                      s_card.view.state == SDCARD_EXPORTED &&
                      s_card.view.export_requested && !s_card.invalidated &&
                      s_card.view.present && _pin_present() &&
                      !s_card.view.mounted && s_card.export_device != RT_NULL;
    rt_hw_interrupt_enable(level);
    return ready;
}

rt_err_t sdcard_export_info(sdcard_export_info_t *info)
{
    rt_base_t level;
    if (!info)
    {
        return -RT_EINVAL;
    }
    rt_memset(info, 0, sizeof(*info));
    level = rt_hw_interrupt_disable();
    if (!_export_ready())
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    info->ready = RT_TRUE;
    info->block_count = s_card.export_geometry.sector_count;
    info->block_size = s_card.export_geometry.bytes_per_sector;
    info->session = s_card.view.session;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

static rt_err_t _export_range(uint32_t sector, const void *buffer,
                              uint32_t length, rt_size_t *blocks)
{
    uint32_t count;
    if (!_export_ready())
    {
        return -RT_EBUSY;
    }
    if (!buffer || !length || length % s_card.export_geometry.bytes_per_sector)
    {
        return -RT_EINVAL;
    }
    count = length / s_card.export_geometry.bytes_per_sector;
    /* Subtraction avoids overflow at the end of a large medium. */
    if (sector >= s_card.export_geometry.sector_count ||
        count > s_card.export_geometry.sector_count - sector)
    {
        return -RT_EINVAL;
    }
    *blocks = count;
    return RT_EOK;
}

rt_err_t sdcard_export_read(uint32_t sector, void *buffer, uint32_t length)
{
    rt_size_t blocks, transferred;
    rt_err_t error = _export_range(sector, buffer, length, &blocks);
    if (error != RT_EOK)
    {
        return error;
    }
    rt_set_errno(0);
    transferred = rt_device_read(s_card.export_device, (rt_off_t)sector, buffer, blocks);
    error = transferred == blocks ? RT_EOK : _dfs_error();
    return _export_ready() ? error : -RT_EBUSY;
}

rt_err_t sdcard_export_write(uint32_t sector, const void *buffer, uint32_t length)
{
    rt_size_t blocks, transferred;
    rt_err_t error = _export_range(sector, buffer, length, &blocks);
    if (error != RT_EOK)
    {
        return error;
    }
    rt_set_errno(0);
    transferred = rt_device_write(s_card.export_device, (rt_off_t)sector, buffer, blocks);
    error = transferred == blocks ? RT_EOK : _dfs_error();
    if (error != RT_EOK)
    {
        s_card.export_write_error = error;
    }
    return _export_ready() ? error : -RT_EBUSY;
}

rt_err_t sdcard_export_sync(void)
{
    /* SDK block_dev.c: rt_mmcsd_write -> rt_mmcsd_req_blk waits for SEND_STATUS
     * ready/not-programming before returning. The caller's io_lock also waits
     * for all in-flight writes. BLK_SYNC is an unimplemented successful no-op.
     * Preserve failed/short writes until this exported device is closed. */
    return _export_ready() ? s_card.export_write_error : -RT_EBUSY;
}

rt_bool_t sdcard_identity_equal(const sdcard_identity_t *a, const sdcard_identity_t *b)
{
    return a && b && a->version == SDCARD_IDENTITY_VERSION &&
           b->version == SDCARD_IDENTITY_VERSION && a->sector_count &&
           a->sector_size == 512 && a->volume_sector_count &&
           (a->fat_bits == 12 || a->fat_bits == 16 || a->fat_bits == 32) &&
           memcmp(a, b, sizeof(*a)) == 0;
}

/* io_lock is acquired before claiming the lifecycle exclusion. The worker
 * never waits for a caller while it owns this exclusion, so a caller already
 * holding the recursive io_lock receives EBUSY instead of waiting for worker. */
static rt_err_t _operation_begin(uint32_t *edges)
{
    rt_err_t error;
    rt_base_t level;
    if (s_card.lifecycle != 2)
    {
        return -RT_EBUSY;
    }
    error = rt_mutex_take(s_card.config.io_lock, RT_WAITING_FOREVER);
    if (error != RT_EOK)
    {
        return error;
    }
    level = rt_hw_interrupt_disable();
    if (s_card.operation_busy || s_card.worker_busy || s_card.fatal ||
        s_card.export_device || s_card.view.export_requested ||
        s_card.view.release_id || !_pin_present() || !s_card.view.present ||
        !((s_card.view.state == SDCARD_READY && !s_card.invalidated) ||
          (s_card.view.state == SDCARD_FAULT && !s_card.view.mounted &&
           s_card.fault_removals == s_card.removals &&
           (s_card.view.error_stage == SDCARD_ERROR_MOUNT ||
            s_card.view.error_stage == SDCARD_ERROR_FORMAT ||
            s_card.view.error_stage == SDCARD_ERROR_REMOUNT))))
    {
        rt_hw_interrupt_enable(level);
        rt_mutex_release(s_card.config.io_lock);
        return -RT_EBUSY;
    }
    s_card.operation_busy = RT_TRUE;
    *edges = s_card.edges;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

static rt_bool_t _operation_current(void *context)
{
    return s_card.operation_busy && _current(*(uint32_t *)context);
}

static void _operation_end(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_bool_t wake = s_card.reconcile_deferred;
    s_card.reconcile_deferred = RT_FALSE;
    s_card.operation_busy = RT_FALSE;
    rt_hw_interrupt_enable(level);
    rt_mutex_release(s_card.config.io_lock);
    if (wake) _wake_worker();
}

rt_err_t sdcard_get_identity(sdcard_identity_t *out)
{
    uint32_t edges;
    rt_err_t error;
    if (!out)
    {
        return -RT_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    error = _operation_begin(&edges);
    if (error != RT_EOK)
    {
        return error;
    }
    error = sdcard_port_guard_begin(s_card.host, _operation_current, &edges);
    if (error == RT_EOK)
    {
        error = sdcard_port_identity(s_card.host, s_card.config.device_name, out);
        if (!_current(edges))
        {
            error = -RT_EBUSY;
        }
        sdcard_port_guard_end(s_card.host);
    }
    if (error != RT_EOK)
    {
        memset(out, 0, sizeof(*out));
    }
    _operation_end();
    return error;
}

rt_err_t sdcard_format(const sdcard_identity_t *identity, uint32_t session)
{
    sdcard_identity_t current;
    uint32_t edges;
    rt_base_t level;
    rt_err_t error;
    sdcard_error_stage_t stage = SDCARD_ERROR_IDENTITY;
    rt_bool_t destructive = RT_FALSE;
    if (!identity || !session || !sdcard_identity_equal(identity, identity))
    {
        return -RT_EINVAL;
    }
    error = _operation_begin(&edges);
    if (error != RT_EOK)
    {
        return error;
    }
    if (s_card.view.session != session)
    {
        _operation_end();
        return -RT_EINVAL;
    }
    error = sdcard_port_guard_begin(s_card.host, _operation_current, &edges);
    if (error != RT_EOK)
    {
        _operation_end();
        return error;
    }
    error = sdcard_port_identity(s_card.host, s_card.config.device_name, &current);
    if (error != RT_EOK) goto done;
    if (!sdcard_identity_equal(identity, &current) || !_current(edges) ||
        s_card.view.session != session)
    {
        error = -RT_EINVAL;
        goto done;
    }
    if (_has_open_files())
    {
        error = -RT_EBUSY;
        goto done;
    }
    level = rt_hw_interrupt_disable();
    s_card.users_released = RT_TRUE;
    if (!s_card.view.available)
    {
        s_card.view.session = _next_id(s_card.view.session);
    }
    _revoke_local_locked();
    s_card.view.state = SDCARD_FORMATTING;
    s_card.view.error_stage = SDCARD_ERROR_NONE;
    s_card.view.error = RT_EOK;
    _publish_changed();
    rt_hw_interrupt_enable(level);
    destructive = RT_TRUE;
    stage = SDCARD_ERROR_UNMOUNT;
    if (!_unmount_locked())
    {
        error = s_card.view.error;
        goto done;
    }
    /* Unmount may flush metadata or race a removal. Confirm again before mkfs. */
    stage = SDCARD_ERROR_IDENTITY;
    error = sdcard_port_identity(s_card.host, s_card.config.device_name, &current);
    if (error != RT_EOK) goto done;
    if (!sdcard_identity_equal(identity, &current) || !_current(edges))
    {
        error = -RT_EINVAL;
        goto done;
    }
    stage = SDCARD_ERROR_FORMAT;
    error = sdcard_port_format(s_card.host, s_card.config.device_name);
    if (error != RT_EOK) goto done;
    if (!_current(edges))
    {
        error = -RT_EBUSY;
        goto done;
    }
    stage = SDCARD_ERROR_REMOUNT;
    rt_set_errno(0);
    if (dfs_mount(s_card.config.device_name, s_card.config.mount_point, "elm", 0, RT_NULL))
    {
        error = _dfs_error();
        goto done;
    }
    if (s_card.config.mounted) s_card.config.mounted();
    _set_mounted(RT_TRUE);
    level = rt_hw_interrupt_disable();
    if (edges != s_card.edges || !_pin_present())
    {
        rt_hw_interrupt_enable(level);
        error = -RT_EBUSY;
        goto done;
    }
    s_card.invalidated = RT_FALSE;
    s_card.users_released = RT_FALSE;
    s_card.view.available = RT_TRUE;
    s_card.view.session = _next_id(s_card.view.session);
    s_card.view.state = SDCARD_READY;
    s_card.view.error_stage = SDCARD_ERROR_NONE;
    s_card.view.error = RT_EOK;
    _publish_changed();
    rt_hw_interrupt_enable(level);
done:
    sdcard_port_guard_end(s_card.host);
    if (destructive && error != RT_EOK && !s_card.fatal)
    {
        _fault(stage, error, RT_FALSE);
    }
    _operation_end();
    return error;
}

static rt_err_t _check_mount_owner(void)
{
    struct dfs_filesystem *fs;
    rt_device_t device;
    char *path;
    rt_err_t error;

    path = dfs_normalize_path(RT_NULL, s_card.config.mount_point);
    if (!path)
    {
        return -RT_ENOMEM;
    }
    error = strcmp(path, s_card.config.mount_point) == 0 ? RT_EOK : -RT_EINVAL;
    rt_free(path);
    if (error != RT_EOK)
    {
        return error;
    }
    error = rt_mutex_take(s_card.config.io_lock, RT_WAITING_FOREVER);
    if (error != RT_EOK)
    {
        return error;
    }
    device = rt_device_find(s_card.config.device_name);
    if (device && dfs_filesystem_get_mounted_path(device))
    {
        error = -RT_EBUSY;
    }
    fs = dfs_filesystem_lookup(s_card.config.mount_point);
    if (fs && strcmp(fs->path, s_card.config.mount_point) == 0)
    {
        error = -RT_EBUSY;
    }
    rt_mutex_release(s_card.config.io_lock);
    return error;
}

rt_err_t sdcard_start(const sdcard_config_t *config)
{
    rt_err_t error;
    rt_base_t level;

    if (!config || !config->device_name || !config->device_name[0] ||
        !config->mount_point || config->mount_point[0] != '/' || !config->mount_point[1] ||
        !config->io_lock || config->detect_pin != EPD_SDCARD_DETECT_PIN ||
        config->present_level != PIN_LOW)
    {
        return -RT_EINVAL;
    }
    level = rt_hw_interrupt_disable();
    if (s_card.lifecycle)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    rt_memset(&s_card, 0, sizeof(s_card));
    s_card.lifecycle = 1;
    s_card.config = *config;
    rt_hw_interrupt_enable(level);

    error = sdcard_port_bind(config->device_name, &s_card.host);
    if (error != RT_EOK)
    {
        goto fail;
    }
    error = _check_mount_owner();
    if (error != RT_EOK)
    {
        goto fail;
    }
    error = rt_sem_init(&s_card.event, "sd_event", 0, RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        goto fail;
    }
    error = rt_sem_init(&s_card.released, "sd_release", 0, RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        goto fail_event;
    }
    s_card.worker = rt_thread_create("sd_card", _worker, RT_NULL,
                                      SDCARD_STACK_SIZE, SDCARD_PRIORITY, 10);
    if (!s_card.worker)
    {
        error = -RT_ENOMEM;
        goto fail_release;
    }
    error = rt_pin_attach_irq(config->detect_pin, PIN_IRQ_MODE_RISING_FALLING, _pin_irq, RT_NULL);
    if (error != RT_EOK)
    {
        goto fail_thread;
    }
    rt_pin_mode(config->detect_pin, PIN_MODE_INPUT);
    error = rt_pin_irq_enable(config->detect_pin, PIN_IRQ_ENABLE);
    if (error != RT_EOK)
    {
        goto fail_irq;
    }
    level = rt_hw_interrupt_disable();
    s_card.lifecycle = 2;
    rt_hw_interrupt_enable(level);
    error = rt_thread_startup(s_card.worker);
    if (error != RT_EOK)
    {
        goto fail_irq;
    }
    _wake_worker();
    return RT_EOK;

fail_irq:
    rt_pin_irq_enable(config->detect_pin, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(config->detect_pin);
fail_thread:
    rt_thread_delete(s_card.worker);
    s_card.worker = RT_NULL;
fail_release:
    rt_sem_detach(&s_card.released);
fail_event:
    rt_sem_detach(&s_card.event);
fail:
    _set_state(SDCARD_FAULT, SDCARD_ERROR_START, error);
    level = rt_hw_interrupt_disable();
    s_card.lifecycle = 0;
    rt_hw_interrupt_enable(level);
    return error;
}
