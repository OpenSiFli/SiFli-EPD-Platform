/* SPDX-License-Identifier: Apache-2.0 */
#include "rtconfig.h"
#ifdef BSP_USING_RTTHREAD
#include <rtthread.h>
#include <rtdevice.h>
#include "board.h"
#include "../../src/boards/frontlight_device.h"

#define LOG_TAG "frontlight"
#define DBG_LEVEL DBG_INFO
#include "log.h"

#define AW99703_ADDR        0x36
#define AW99703_ENABLE_PIN  53
#define AW99703_MODE        0x02
#define AW99703_LEDCUR      0x03
#define AW99703_BSTCTR2     0x05
#define AW99703_LEDLSB      0x06
#define AW99703_LEDMSB      0x07
#define AW99703_LINEAR_I2C  0x14
#define AW99703_BACKLIGHT   0x01

static struct rt_device frontlight;
static struct rt_i2c_bus_device *frontlight_bus;
static uint8_t frontlight_percent;
static frontlight_mode_t frontlight_mode = FRONTLIGHT_MIXED;

static uint8_t channel_current(frontlight_mode_t mode)
{
    uint8_t channels = mode == FRONTLIGHT_COOL ? 0x04 :
                       mode == FRONTLIGHT_WARM ? 0x02 : 0x06;
    return (19 << 3) | channels;
}

static rt_err_t aw99703_write(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    struct rt_i2c_msg msg = {
        .addr = AW99703_ADDR,
        .flags = RT_I2C_WR,
        .buf = data,
        .len = sizeof(data),
    };

    return rt_i2c_transfer(frontlight_bus, &msg, 1) == 1 ? RT_EOK : -RT_EIO;
}

static rt_err_t frontlight_init(rt_device_t dev)
{
    frontlight_bus = rt_i2c_bus_device_find("i2c1");
    if (!frontlight_bus)
        return -RT_ENOSYS;

    rt_pin_mode(AW99703_ENABLE_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(AW99703_ENABLE_PIN, PIN_LOW);
    return rt_device_open(&frontlight_bus->parent,
                          RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_TX |
                          RT_DEVICE_FLAG_INT_RX);
}

static rt_size_t frontlight_read(rt_device_t dev, rt_off_t pos,
                                void *buffer, rt_size_t size)
{
    if (!buffer || size == 0)
        return 0;
    *(uint8_t *)buffer = frontlight_percent;
    return 1;
}

static rt_size_t frontlight_write(rt_device_t dev, rt_off_t pos,
                                 const void *buffer, rt_size_t size)
{
    uint8_t percent;
    uint16_t level;

    if (!buffer || size == 0)
        return 0;
    percent = *(const uint8_t *)buffer;
    if (percent > 100)
        percent = 100;
    if (percent == frontlight_percent)
        return 1;

    if (percent == 0)
    {
        rt_pin_write(AW99703_ENABLE_PIN, PIN_LOW);
        frontlight_percent = 0;
        return 1;
    }

    if (frontlight_percent == 0)
    {
        rt_pin_write(AW99703_ENABLE_PIN, PIN_HIGH);
        rt_thread_mdelay(1); /* HWEN to I2C ready: at least 250 us. */
        /* PWM is unconnected. Keep standby until brightness is programmed. */
        if (aw99703_write(AW99703_MODE, AW99703_LINEAR_I2C) != RT_EOK ||
            /* 20 mA full scale; HVLED2 is warm and HVLED3 is cool. */
            aw99703_write(AW99703_LEDCUR, channel_current(frontlight_mode)) != RT_EOK ||
            /* L3 is 10 uH. */
            aw99703_write(AW99703_BSTCTR2, 0x40) != RT_EOK)
            goto failed;
    }

    /* Logical 0-100% uses the lower half of the hardware brightness range. */
    level = ((uint32_t)percent * 2047 + 100) / 200;
    /* The chip latches the 11-bit brightness when LEDMSB is written. */
    if (aw99703_write(AW99703_LEDLSB, level & 0x07) != RT_EOK ||
        aw99703_write(AW99703_LEDMSB, level >> 3) != RT_EOK ||
        aw99703_write(AW99703_MODE,
                      AW99703_LINEAR_I2C | AW99703_BACKLIGHT) != RT_EOK)
        goto failed;

    frontlight_percent = percent;
    return 1;

failed:
    rt_pin_write(AW99703_ENABLE_PIN, PIN_LOW);
    frontlight_percent = 0;
    LOG_E("AW99703 I2C write failed");
    return 0;
}

static rt_err_t frontlight_control(rt_device_t dev, int command, void *argument)
{
    if (!argument) return -RT_EINVAL;
    frontlight_mode_t *mode = argument;
    if (command == FRONTLIGHT_CTRL_GET_MODE)
    {
        *mode = frontlight_mode;
        return RT_EOK;
    }
    if (command != FRONTLIGHT_CTRL_SET_MODE) return -RT_ENOSYS;
    if ((unsigned)*mode >= FRONTLIGHT_MODE_COUNT) return -RT_EINVAL;
    if (*mode == frontlight_mode) return RT_EOK;
    if (frontlight_percent && aw99703_write(AW99703_LEDCUR, channel_current(*mode)) != RT_EOK)
    {
        rt_pin_write(AW99703_ENABLE_PIN, PIN_LOW);
        frontlight_percent = 0;
        return -RT_EIO;
    }
    frontlight_mode = *mode;
    return RT_EOK;
}

static int frontlight_register(void)
{
    frontlight.type = RT_Device_Class_Char;
    frontlight.init = frontlight_init;
    frontlight.read = frontlight_read;
    frontlight.write = frontlight_write;
    frontlight.control = frontlight_control;
    return rt_device_register(&frontlight, "lcdlight",
                              RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_STANDALONE);
}
INIT_COMPONENT_EXPORT(frontlight_register);
#endif
