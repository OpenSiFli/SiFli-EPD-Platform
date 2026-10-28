/*
 * Copyright (c) 2026, SiFli Technology
 * SPDX-License-Identifier: LicenseRef-SiFli
 * Minimal single-threaded consumer, without Launcher or LVGL.
 */
#include "sdcard_demo.h"
#include "sdcard.h"
#include <rtdevice.h>
#include <stdio.h>

static struct rt_mutex demo_io;
static rt_bool_t started;
static uint32_t shown_revision;
static uint32_t opened_session;
static uint32_t acknowledged_release;

rt_err_t sdcard_demo_start(void)
{
    const sdcard_config_t config =
    {
        .device_name = "sd1",
        .mount_point = "/sdcard",
        .detect_pin = EPD_SDCARD_DETECT_PIN,
        .present_level = PIN_LOW,
        .io_lock = &demo_io,
    };
    rt_err_t error;

    if (started)
    {
        return -RT_EBUSY;
    }
    error = rt_mutex_init(&demo_io, "demo_sd_io", RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        return error;
    }
    error = sdcard_start(&config);
    if (error != RT_EOK)
    {
        rt_mutex_detach(&demo_io);
        return error;
    }
    started = RT_TRUE;
    return RT_EOK;
}

/* Call from the same application thread that called sdcard_demo_start().
 * This example is the only SD file user; it closes each file before returning.
 * A real platform must first stop and release ALL its SD users before ACK. */
void sdcard_demo_step(void)
{
    sdcard_snapshot_t view, checked;
    rt_err_t error;

    if (!started)
    {
        return;
    }
    sdcard_get_snapshot(&view);
    if (view.revision != shown_revision)
    {
        shown_revision = view.revision;
        rt_kprintf("sd: state=%d present=%d mounted=%d available=%d session=%lu stage=%d error=%d\n",
                   view.state, view.present, view.mounted, view.available,
                   (unsigned long)view.session, view.error_stage, view.error);
    }
    if (view.state == SDCARD_DRAINING && view.release_id != acknowledged_release)
    {
        error = sdcard_release_complete(view.release_id);
        if (error == RT_EOK)
        {
            acknowledged_release = view.release_id;
        }
    }
    if (!view.available || view.session == opened_session)
    {
        return;
    }
    error = rt_mutex_take(&demo_io, RT_WAITING_FOREVER);
    if (error != RT_EOK)
    {
        return;
    }
    sdcard_get_snapshot(&checked);
    if (checked.available && checked.session == view.session)
    {
        char line[128];
        FILE *file = fopen("/sdcard/readme.txt", "rb");
        opened_session = checked.session;
        if (file)
        {
            size_t count = fread(line, 1, sizeof(line) - 1, file);
            line[count] = '\0';
            rt_kprintf("readme.txt: %s\n", line);
            fclose(file);
        }
    }
    rt_mutex_release(&demo_io);
}
