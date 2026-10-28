/* SPDX-License-Identifier: Apache-2.0 */
#include "rtconfig.h"

#if defined(BSP_USING_CHARGER) && defined(BSP_USING_RTTHREAD) && defined(BF0_HCPU) && !defined(BF0_ACPU) && !defined(CFG_BOOTLOADER)

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include "charge.h"
#include "bsp_epd_charge.h"

#define EPD_USB_DETECT_PIN 6
#define EPD_CHARGE_STATUS_PIN 7

static rt_charge_device_t device;
static bool registered, framework_ready;
static uint32_t query_errors;

static rt_err_t query_failed(rt_err_t error)
{
    rt_base_t level = rt_hw_interrupt_disable();
    ++query_errors;
    rt_hw_interrupt_enable(level);
    return error;
}

uint32_t epd_charge_error_sequence(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t sequence = query_errors;
    rt_hw_interrupt_enable(level);
    return sequence;
}

static rt_err_t epd_charge_control(rt_charge_device_t *charge, int cmd, void *args)
{
    (void)charge;
    if (cmd != RT_CHARGE_GET_DETECT_STATUS && cmd != RT_CHARGE_GET_STATUS)
        return RT_CHARGE_ERROR_UNSUPPORTED;
    if (!args) return query_failed(-RT_EINVAL);

    rt_base_t detected = rt_pin_read(EPD_USB_DETECT_PIN);
    if (detected != PIN_LOW && detected != PIN_HIGH) return query_failed(-RT_EIO);
    uint8_t result = detected == PIN_HIGH;
    if (cmd == RT_CHARGE_GET_STATUS)
    {
        rt_base_t status = rt_pin_read(EPD_CHARGE_STATUS_PIN);
        if (status != PIN_LOW && status != PIN_HIGH) return query_failed(-RT_EIO);
        result = result && status == PIN_LOW;
    }
    *(uint8_t *)args = result;
    return RT_CHARGE_EOK;
}

static const struct rt_charge_ops ops = {.control = epd_charge_control};

bool epd_charge_ready(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = framework_ready;
    rt_hw_interrupt_enable(level);
    return ready && rt_device_find("charge") == &device.parent;
}

static int epd_charge_init(void)
{
    /* STAT1 is externally pulled up; STAT2 and EN have no MCU connection. */
    rt_pin_mode(EPD_USB_DETECT_PIN, PIN_MODE_INPUT);
    rt_pin_mode(EPD_CHARGE_STATUS_PIN, PIN_MODE_INPUT);
    rt_err_t result = rt_charge_register(&device, &ops, RT_NULL);
    registered = result == RT_EOK;
    return result;
}
INIT_PREV_EXPORT(epd_charge_init);

static int epd_charge_framework_ready(void)
{
    /* ENV runs after the SDK's DEVICE-stage charge_init binds g_charge. */
    bool ready = registered && rt_device_find("charge") == &device.parent;
    rt_base_t level = rt_hw_interrupt_disable();
    framework_ready = ready;
    rt_hw_interrupt_enable(level);
    return ready ? RT_EOK : -RT_ERROR;
}
INIT_ENV_EXPORT(epd_charge_framework_ready);

#endif
