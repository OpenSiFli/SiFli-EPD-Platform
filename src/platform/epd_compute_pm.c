/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_compute_pm.h"
#include <rtthread.h>

#if defined(EPD_COMPUTE_ACPU) && defined(RT_USING_PM) && defined(SF32LB57X)
#include <rtdevice.h>
#include <rthw.h>
#include "epd_compute_transport.h"

static struct rt_device compute_pm_device;
static rt_bool_t suspended;
static rt_uint8_t suspended_mode;
static rt_uint32_t deep_pauses, deep_resumes, reported;

static int compute_suspend(const struct rt_device *device, rt_uint8_t mode)
{
    (void)device;
    if (mode <= PM_SLEEP_MODE_IDLE || suspended) return RT_EOK;
    if (!epd_compute_transport_parked()) return -RT_EBUSY;
    suspended = RT_TRUE;
    suspended_mode = mode;
    if (mode == PM_SLEEP_MODE_DEEP) ++deep_pauses;
    return RT_EOK;
}

static void compute_resume(const struct rt_device *device, rt_uint8_t mode)
{
    (void)device; (void)mode;
    if (!suspended) return;
    suspended = RT_FALSE;
    if (suspended_mode == PM_SLEEP_MODE_DEEP) ++deep_resumes;
}

static const struct rt_device_pm_ops compute_pm_ops = {
    .suspend = compute_suspend,
    .resume = compute_resume
};

static int compute_pm_init(void)
{
    rt_pm_device_register(&compute_pm_device, &compute_pm_ops);
    return RT_EOK;
}
INIT_ENV_EXPORT(compute_pm_init);

void epd_compute_pm_report(void)
{
    rt_base_t key = rt_hw_interrupt_disable();
    rt_uint32_t pauses = deep_pauses, resumes = deep_resumes;
    rt_bool_t changed = reported != pauses;
    reported = pauses;
    rt_bool_t parked = epd_compute_transport_parked();
    rt_hw_interrupt_enable(key);
    if (changed)
        rt_kprintf("cmp power deep_pause=%u deep_resume=%u parked=%u\n",
                   (unsigned)pauses, (unsigned)resumes, (unsigned)parked);
}
#else
void epd_compute_pm_report(void) {}
#endif
