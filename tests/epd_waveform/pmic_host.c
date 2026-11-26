#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define RT_EOK 0
#define RT_ERROR 1
#define RT_TRUE true
#define RT_FALSE false
#define RT_I2C_WR 0
#define RT_I2C_RD 1
#define RT_DEVICE_FLAG_RDWR 1
#define RT_DEVICE_FLAG_INT_TX 2
#define RT_DEVICE_FLAG_INT_RX 4
#define PIN_MODE_OUTPUT 1
#define EPD_TPS_I2C_DEVICE_NAME "tps"
#define EPD_TPS_WAKEUP_PIN 42
#define EPD_TPS_PWRCOM_PIN 43
#define EPD_TPS_PWRUP_PIN 44
#define LOG_E(...) ((void)0)

typedef int rt_err_t;
typedef bool rt_bool_t;
typedef uint8_t rt_uint8_t;
typedef uint32_t rt_tick_t;
typedef void *rt_device_t;
struct rt_i2c_bus_device { int unused; };
struct rt_i2c_msg { uint16_t addr, flags, len; uint8_t *buf; };
struct rt_i2c_configuration { unsigned mode, addr, timeout, max_hz; };

static struct rt_i2c_bus_device mock_bus;
static rt_tick_t tick, pg_at;
static bool fail_read;
static uint8_t regs[256], pins[45];
static rt_tick_t edges[45];
static unsigned pg_reads, config_writes, pin_writes;

static rt_tick_t rt_tick_get(void) { return tick; }
static rt_tick_t rt_tick_from_millisecond(int delay) { return (rt_tick_t)delay; }
static void rt_thread_delay(rt_tick_t delay) { tick += delay; }
static void rt_thread_mdelay(int delay) { tick += (rt_tick_t)delay; }
static void rt_pin_mode(int pin, int mode) { (void)pin; CHECK(mode == PIN_MODE_OUTPUT); }
static void rt_pin_write(int pin, uint8_t on)
{
    pins[pin] = on;
    edges[pin] = tick;
    ++pin_writes;
}
static rt_device_t rt_device_find(const char *name) { CHECK(name[0] == 't'); return &mock_bus; }
static int rt_device_open(rt_device_t dev, int flags) { CHECK(dev && flags); return RT_EOK; }
static int rt_i2c_configure(struct rt_i2c_bus_device *dev, const struct rt_i2c_configuration *cfg)
{ CHECK(dev == &mock_bus && cfg->max_hz == 400000); return RT_EOK; }
static bool bsp_epd_pmic_supported(uint8_t id) { return id == 5; }

static int rt_i2c_transfer(struct rt_i2c_bus_device *dev, struct rt_i2c_msg *msg, int count)
{
    CHECK(dev == &mock_bus && msg->flags == RT_I2C_WR && pins[EPD_TPS_WAKEUP_PIN]);
    uint8_t reg = msg->buf[0];
    if (count == 1)
    {
        CHECK(msg->len == 2 && !pins[EPD_TPS_PWRUP_PIN]);
        regs[reg] = msg->buf[1];
        ++config_writes;
    }
    else
    {
        CHECK(count == 2 && msg->len == 1 && msg[1].flags == RT_I2C_RD && msg[1].len == 1);
        if (reg == 0x0f)
        {
            ++pg_reads;
            if (fail_read) return 0;
            *msg[1].buf = pins[EPD_TPS_PWRUP_PIN] && tick >= pg_at ? 0xfa : 0;
        }
        else *msg[1].buf = regs[reg];
    }
    return count;
}

#include "tps.inc"

int main(void)
{
    regs[0x10] = 5;
    CHECK(tps_init(2100) == RT_EOK && config_writes == 7);
    tps_enter_sleep();
    CHECK(!pins[EPD_TPS_WAKEUP_PIN] && edges[EPD_TPS_WAKEUP_PIN] - edges[EPD_TPS_PWRUP_PIN] == 150);

    CHECK(tps_start_powerup() == RT_EOK && config_writes == 14 && !pg_reads);
    rt_tick_t raised_at = edges[EPD_TPS_PWRUP_PIN];
    CHECK(raised_at - edges[EPD_TPS_WAKEUP_PIN] >= 10 && !pins[EPD_TPS_PWRCOM_PIN]);
    pg_at = raised_at + 12;
    tick += 7; /* CPU cache maintenance while the PMIC is ramping. */
    CHECK(tps_wait_powerup() == RT_EOK);
    CHECK(pg_reads == 4 && edges[EPD_TPS_PWRCOM_PIN] == raised_at + 13);
    CHECK(tick - edges[EPD_TPS_PWRCOM_PIN] == 10);
    unsigned writes = pin_writes;
    CHECK(tps_rails_good() && pin_writes == writes);

    rt_tick_t vcom_off = tps_disable_vcom();
    tick += 8; /* Pixel settlement already waited longer than the VCOM guard. */
    tps_finish_powerdown(vcom_off);
    CHECK(edges[EPD_TPS_PWRUP_PIN] == vcom_off + 8);
    CHECK(tick == vcom_off + 158 && !pins[EPD_TPS_WAKEUP_PIN]);

    CHECK(tps_start_powerup() == RT_EOK);
    fail_read = true;
    CHECK(tps_wait_powerup() != RT_EOK && !pins[EPD_TPS_PWRCOM_PIN]);
    tps_enter_sleep();
    CHECK(edges[EPD_TPS_PWRUP_PIN] - edges[EPD_TPS_PWRCOM_PIN] == 5);
    fail_read = false;
    CHECK(tps_start_powerup() == RT_EOK);
    raised_at = tick;
    pg_at = raised_at + 501;
    CHECK(tps_wait_powerup() != RT_EOK && tick == raised_at + 500 && !pins[EPD_TPS_PWRCOM_PIN]);
    tps_enter_sleep();
    puts("PASS: TPS production ramp, power-good polling, VCOM ordering and discharge guards");
    return 0;
}
