#ifndef COMPUTE_PM_TEST_RTDEVICE_H
#define COMPUTE_PM_TEST_RTDEVICE_H
#include "rtthread.h"
enum { PM_SLEEP_MODE_NONE, PM_SLEEP_MODE_IDLE, PM_SLEEP_MODE_LIGHT,
       PM_SLEEP_MODE_DEEP, PM_SLEEP_MODE_STANDBY, PM_SLEEP_MODE_SHUTDOWN };
struct rt_device { unsigned unused; };
struct rt_device_pm_ops {
    int (*suspend)(const struct rt_device *, rt_uint8_t);
    void (*resume)(const struct rt_device *, rt_uint8_t);
};
void rt_pm_device_register(struct rt_device *, const struct rt_device_pm_ops *);
#endif
