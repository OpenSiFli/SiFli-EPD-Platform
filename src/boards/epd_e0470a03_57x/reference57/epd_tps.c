/* SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 * TPS65185 registers: TI SLVSAQ8G, section 8.6.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "epd_tps.h"
#include "bsp_epd_pmic.h"

#define DBG_TAG "epd.tps"
#define DBG_LVL DBG_ERROR
#include <rtdbg.h>

#define TPS_ADDR 0x68
#define REG_VADJ 0x02
#define REG_VCOM1 0x03
#define REG_VCOM2 0x04
#define REG_PG 0x0f
#define REG_REVID 0x10
#define PG_ALL_RAILS 0xfa
#define TPS_WAKEUP_MS 10
#define TPS_VCOM_ON_MS 10
#define TPS_VCOM_OFF_MS 5
#define TPS_DISCHARGE_MS 150
#define TPS_POWERUP_TIMEOUT_MS 500
#define TPS_PG_POLL_MS 2

static struct rt_i2c_bus_device *bus;
static rt_bool_t configured;
static rt_tick_t powerup_started;
static uint16_t saved_vcom_mv;

static rt_err_t read_reg(rt_uint8_t reg, rt_uint8_t *value)
{
    struct rt_i2c_msg msg[2] = {
        {.addr=TPS_ADDR, .flags=RT_I2C_WR, .len=1, .buf=&reg},
        {.addr=TPS_ADDR, .flags=RT_I2C_RD, .len=1, .buf=value},
    };
    return bus && rt_i2c_transfer(bus, msg, 2) == 2 ? RT_EOK : -RT_ERROR;
}

static rt_err_t write_checked(rt_uint8_t reg, rt_uint8_t value)
{
    rt_uint8_t data[2] = {reg, value}, actual = 0;
    struct rt_i2c_msg msg = {.addr=TPS_ADDR, .flags=RT_I2C_WR, .len=2, .buf=data};
    if (!bus || rt_i2c_transfer(bus, &msg, 1) != 1 ||
        read_reg(reg, &actual) != RT_EOK || actual != value)
    {
        LOG_E("register 0x%02x verify failed: want=%02x got=%02x", reg, value, actual);
        return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_err_t restore_config(void)
{
    /* WAKEUP low resets volatile registers. Restore before PWRUP, never EEPROM. */
    if (write_checked(0x09, 0xb1) || write_checked(0x0a, 0x00) ||
        write_checked(0x0b, 0x4e) || write_checked(0x0c, 0x84) ||
        write_checked(REG_VADJ, 0x03) ||
        write_checked(REG_VCOM1, (saved_vcom_mv / 10) & 0xff) ||
        write_checked(REG_VCOM2, (saved_vcom_mv / 10) >> 8))
        return -RT_ERROR;
    configured = RT_TRUE;
    return RT_EOK;
}

rt_err_t tps_init(uint16_t vcom_mv)
{
    rt_uint8_t id = 0;
    struct rt_i2c_configuration cfg = {.mode=0, .addr=0, .timeout=500, .max_hz=400000};
    configured = RT_FALSE;
    saved_vcom_mv = vcom_mv;
    rt_pin_mode(EPD_TPS_WAKEUP_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(EPD_TPS_PWRCOM_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(EPD_TPS_PWRUP_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(EPD_TPS_PWRCOM_PIN, 0);
    rt_pin_write(EPD_TPS_PWRUP_PIN, 0);
    rt_thread_mdelay(TPS_DISCHARGE_MS);
    rt_pin_write(EPD_TPS_WAKEUP_PIN, 0);
    rt_thread_mdelay(TPS_WAKEUP_MS);
    rt_pin_write(EPD_TPS_WAKEUP_PIN, 1);
    rt_thread_mdelay(TPS_WAKEUP_MS);

    bus = (struct rt_i2c_bus_device *)rt_device_find(EPD_TPS_I2C_DEVICE_NAME);
    if (!bus || rt_device_open((rt_device_t)bus,
        RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_TX | RT_DEVICE_FLAG_INT_RX) != RT_EOK ||
        rt_i2c_configure(bus, &cfg) != RT_EOK || read_reg(REG_REVID, &id) != RT_EOK)
    {
        LOG_E("init failed on %s addr=0x68", EPD_TPS_I2C_DEVICE_NAME);
        return -RT_ERROR;
    }
    /* Preserve reference panel sequences: up VGL,VNEG,VGH,VPOS; reverse down.
     * Only volatile registers are written; never program the PMIC EEPROM. */
    if (!bsp_epd_pmic_supported(id))
    {
        LOG_E("unsupported PMIC REVID=0x%02x", id);
        return -RT_ERROR;
    }
    if (vcom_mv > 5110 || restore_config() != RT_EOK)
    {
        LOG_E("config failed, REVID=0x%02x", id);
        return -RT_ERROR;
    }
    configured = RT_TRUE;
    return RT_EOK;
}

rt_tick_t tps_disable_vcom(void)
{
    rt_pin_write(EPD_TPS_PWRCOM_PIN, 0);
    return rt_tick_get();
}

void tps_finish_powerdown(rt_tick_t vcom_disabled_at)
{
    /* Pixel settlement may already have used part of the VCOM-off delay. */
    rt_tick_t wait = rt_tick_from_millisecond(TPS_VCOM_OFF_MS);
    rt_tick_t elapsed = rt_tick_get() - vcom_disabled_at;
    if (elapsed < wait) rt_thread_delay(wait - elapsed);
    rt_pin_write(EPD_TPS_PWRUP_PIN, 0);
    /* WAKEUP must stay high throughout the configured rail-down sequence. */
    rt_thread_mdelay(TPS_DISCHARGE_MS);
    rt_pin_write(EPD_TPS_WAKEUP_PIN, 0);
    configured = RT_FALSE;
}

void tps_enter_sleep(void)
{
    tps_finish_powerdown(tps_disable_vcom());
}

rt_err_t tps_start_powerup(void)
{
    if (!bus) return -RT_ERROR;
    if (!configured)
    {
        rt_pin_write(EPD_TPS_WAKEUP_PIN, 1);
        rt_thread_mdelay(TPS_WAKEUP_MS);
        if (restore_config() != RT_EOK) return -RT_ERROR;
    }
    rt_pin_write(EPD_TPS_PWRUP_PIN, 1);
    powerup_started = rt_tick_get();
    return RT_EOK;
}

rt_err_t tps_wait_powerup(void)
{
    rt_uint8_t pg = 0;
    while (rt_tick_get() - powerup_started < rt_tick_from_millisecond(TPS_POWERUP_TIMEOUT_MS))
    {
        if (read_reg(REG_PG, &pg) != RT_EOK) break;
        if ((pg & PG_ALL_RAILS) == PG_ALL_RAILS)
        {
            rt_pin_write(EPD_TPS_PWRCOM_PIN, 1);
            rt_thread_mdelay(TPS_VCOM_ON_MS);
            return RT_EOK;
        }
        rt_thread_mdelay(TPS_PG_POLL_MS);
    }
    LOG_E("power failed PG=%02x", pg);
    return -RT_ERROR;
}

bool tps_rails_good(void)
{
    rt_uint8_t pg = 0;
    return read_reg(REG_PG, &pg) == RT_EOK && (pg & PG_ALL_RAILS) == PG_ALL_RAILS;
}
