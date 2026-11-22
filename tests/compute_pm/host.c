/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "rtdevice.h"
#include "rthw.h"
#include "bf0_hal.h"
#include "epd_compute_pm.h"
#include "epd_compute_transport.h"

int test_pm_init(void);
static const struct rt_device_pm_ops *ops;
static struct rt_device *registered_device;
static unsigned registrations, irq_depth, hardware_calls, checks, logs;
static rt_bool_t parked = RT_TRUE;
static uint32_t secu_acpu = 0x629e0011u;
static char last_log[256];

void rt_pm_device_register(struct rt_device *device, const struct rt_device_pm_ops *value)
{
    assert(device && value && value->suspend && value->resume);
    registered_device = device; ops = value; ++registrations;
}
rt_base_t rt_hw_interrupt_disable(void)
{
    rt_base_t previous = (rt_base_t)irq_depth;
    ++irq_depth; return previous;
}
void rt_hw_interrupt_enable(rt_base_t previous)
{
    assert(irq_depth == (unsigned)previous + 1);
    irq_depth = (unsigned)previous;
}
rt_bool_t epd_compute_transport_parked(void)
{
    assert(irq_depth); ++checks; return parked;
}
bool HAL_RCC_IsModuleEnabled(int module)
{ (void)module; ++hardware_calls; return (secu_acpu & 16u) != 0; }
void HAL_RCC_DisableModule(int module)
{ (void)module; ++hardware_calls; secu_acpu &= ~16u; }
void HAL_RCC_EnableModule(int module)
{ (void)module; ++hardware_calls; secu_acpu |= 16u; }
void test_dsb(void) { ++hardware_calls; }
void rt_kprintf(const char *format, ...)
{
    assert(!irq_depth);
    va_list args; va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args); ++logs;
}
static int suspend_core(unsigned mode)
{
    uint32_t before = secu_acpu;
    rt_base_t previous = rt_hw_interrupt_disable();
    int result = ops->suspend(registered_device, (rt_uint8_t)mode);
    rt_hw_interrupt_enable(previous);
    assert(secu_acpu == before && hardware_calls == 0);
    return result;
}
static void resume_core(unsigned mode)
{
    uint32_t before = secu_acpu;
    rt_base_t previous = rt_hw_interrupt_disable();
    ops->resume(registered_device, (rt_uint8_t)mode);
    rt_hw_interrupt_enable(previous);
    assert(secu_acpu == before && hardware_calls == 0);
}
static void cycle(unsigned suspend_mode, unsigned resume_mode)
{
    assert(suspend_core(suspend_mode) == RT_EOK);
    resume_core(resume_mode);
}
static void test_light(void)
{
    cycle(PM_SLEEP_MODE_LIGHT, PM_SLEEP_MODE_LIGHT);
    assert(checks == 1);
    epd_compute_pm_report(); assert(logs == 0);
}
static void test_deep(void)
{
    cycle(PM_SLEEP_MODE_DEEP, PM_SLEEP_MODE_DEEP);
    epd_compute_pm_report(); assert(logs == 1);
    assert(!strcmp(last_log, "cmp power deep_pause=1 deep_resume=1 parked=1\n"));
    epd_compute_pm_report(); assert(logs == 1);
}
static void test_abort_after_suspend(void)
{
    /* A later device rejects suspend; PM resumes all devices without sleeping. */
    assert(suspend_core(PM_SLEEP_MODE_DEEP) == RT_EOK);
    resume_core(PM_SLEEP_MODE_DEEP);
    resume_core(PM_SLEEP_MODE_DEEP);
    epd_compute_pm_report();
    assert(!strcmp(last_log, "cmp power deep_pause=1 deep_resume=1 parked=1\n"));
    cycle(PM_SLEEP_MODE_LIGHT, PM_SLEEP_MODE_LIGHT);
}
static void test_early_resume(void)
{
    resume_core(PM_SLEEP_MODE_DEEP); resume_core(PM_SLEEP_MODE_LIGHT);
    assert(checks == 0);
    epd_compute_pm_report(); assert(logs == 0);
}
static void test_mode_downgrade(void)
{
    cycle(PM_SLEEP_MODE_DEEP, PM_SLEEP_MODE_IDLE);
    cycle(PM_SLEEP_MODE_LIGHT, PM_SLEEP_MODE_IDLE);
    epd_compute_pm_report();
    assert(!strcmp(last_log, "cmp power deep_pause=1 deep_resume=1 parked=1\n"));
}
static void test_disabled_core(void)
{
    secu_acpu &= ~16u; parked = RT_FALSE;
    assert(suspend_core(PM_SLEEP_MODE_LIGHT) == -RT_EBUSY);
    resume_core(PM_SLEEP_MODE_LIGHT);
    parked = RT_TRUE;
    cycle(PM_SLEEP_MODE_DEEP, PM_SLEEP_MODE_IDLE);
    assert(!(secu_acpu & 16u));
}
static void test_unparked_veto(void)
{
    parked = RT_FALSE;
    assert(suspend_core(PM_SLEEP_MODE_LIGHT) == -RT_EBUSY);
    resume_core(PM_SLEEP_MODE_LIGHT);
    assert(suspend_core(PM_SLEEP_MODE_DEEP) == -RT_EBUSY);
    resume_core(PM_SLEEP_MODE_DEEP);
    epd_compute_pm_report(); assert(logs == 0);
    parked = RT_TRUE; cycle(PM_SLEEP_MODE_DEEP, PM_SLEEP_MODE_DEEP);
}
static void test_multiple_cycles(void)
{
    for (unsigned i = 0; i < 128; ++i)
    {
        unsigned mode = i % 2 ? PM_SLEEP_MODE_LIGHT : PM_SLEEP_MODE_DEEP;
        cycle(mode, i % 3 ? mode : PM_SLEEP_MODE_IDLE);
        resume_core(mode);
    }
    epd_compute_pm_report();
    assert(!strcmp(last_log, "cmp power deep_pause=64 deep_resume=64 parked=1\n"));
}
static void test_duplicate_suspend(void)
{
    assert(suspend_core(PM_SLEEP_MODE_DEEP) == RT_EOK);
    assert(suspend_core(PM_SLEEP_MODE_DEEP) == RT_EOK); assert(checks == 1);
    resume_core(PM_SLEEP_MODE_DEEP); resume_core(PM_SLEEP_MODE_DEEP);
    epd_compute_pm_report();
    assert(!strcmp(last_log, "cmp power deep_pause=1 deep_resume=1 parked=1\n"));
}
static void test_idle_noop(void)
{
    parked = RT_FALSE;
    cycle(PM_SLEEP_MODE_NONE, PM_SLEEP_MODE_NONE);
    cycle(PM_SLEEP_MODE_IDLE, PM_SLEEP_MODE_IDLE); assert(checks == 0);
}
static const struct { const char *name; void (*run)(void); } cases[] = {
    {"light_no_enr", test_light}, {"deep_no_enr", test_deep},
    {"abort_after_suspend", test_abort_after_suspend}, {"early_resume", test_early_resume},
    {"mode_downgrade", test_mode_downgrade}, {"disabled_core_no_enr", test_disabled_core},
    {"unparked_veto", test_unparked_veto}, {"multiple_cycles", test_multiple_cycles},
    {"duplicate_suspend", test_duplicate_suspend}, {"idle_noop", test_idle_noop}
};
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--list"))
    {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) puts(cases[i].name);
        return 0;
    }
    assert(argc == 2);
    assert(test_pm_init() == RT_EOK && registrations == 1);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        if (strcmp(argv[1], cases[i].name)) continue;
        cases[i].run(); assert(!irq_depth && !hardware_calls);
        printf("PASS %s\n", cases[i].name); return 0;
    }
    return 2;
}
