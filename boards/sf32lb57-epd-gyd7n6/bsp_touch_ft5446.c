/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rtconfig.h"
#if defined(BSP_USING_RTTHREAD) && defined(BSP_USING_TOUCHD)
#include <rtthread.h>
#include "board.h"
#include "drv_touch.h"
#include "drv_io.h"
#include "bsp_touch_pm.h"
#include "bsp_touch_ft5446.h"

/* Define -------------------------------------------------------------------*/

#define DBG_LEVEL          DBG_ERROR
#define LOG_TAG              "drv.ft5446u"
#include <drv_log.h>

#define FT_DEV_ADDR             (0x38)
#define FT_TD_STATUS            (0x02)
#define FT_CONTACT_BYTES        (5) /* TD_STATUS through P1_XL in board coordinates. */
#define FT_ID_G_CTRL            (0x86)
#define FT_CTRL_AUTO_MONITOR    (0x01)
#define FT_ID_G_PERIODMONITOR   (0x89)
#define FT_ID_G_MODE            (0xA4)
#define FT_ID_G_PMODE           (0xA5)
#define FT_PMODE_HIBERNATE       (0x03)
#define FT_RESET_LOW_MS         (5)
#define FT_RESET_READY_MS       (200)
#define FT_RECOVERY_ATTEMPTS     (3)

/* function and value-----------------------------------------------------------*/

static rt_err_t write_reg(uint8_t reg, rt_uint8_t data);
static rt_err_t read_regs(rt_uint8_t reg, rt_uint8_t len, rt_uint8_t *buf);
static void irq_handler(void *arg);

static struct rt_i2c_bus_device *ft_bus = NULL;
static rt_uint16_t last_x;
static rt_uint16_t last_y;

static struct touch_drivers driver;
static struct rt_mutex touch_lock;
static bool touch_lock_ready;
static bool touch_sleeping;
static bool touch_system_suspended;
static bool touch_enabled = true;
static bool touch_supply_suspended;
static volatile bool touch_irq_allowed;
static rt_thread_t volatile touch_power_owner;

enum touch_state
{
    TOUCH_OFF,
    TOUCH_ACTIVE,
    TOUCH_ASLEEP,
    TOUCH_FAULT
};
static enum touch_state touch_state;
static unsigned touch_recovery_attempts;
static rt_err_t touch_recovery_error = -RT_ERROR;

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <string.h>
static unsigned probe_policy = FT_CTRL_AUTO_MONITOR;
static uint8_t probe_period, probe_period_readback;
static uint8_t probe_default[4], probe_selected;
static rt_err_t probe_result;
static volatile unsigned probe_irqs, probe_contacts;
#endif

bool bsp_touch_ft5446_active(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool active = touch_state == TOUCH_ACTIVE && !touch_sleeping &&
                  touch_irq_allowed && !touch_power_owner;
    rt_hw_interrupt_enable(level);
    return active;
}

static rt_err_t touch_check_context(void)
{
    /* Cortex-M33 interrupt state is PRIMASK. Restore it before any IPC/I2C. */
    rt_base_t level = rt_hw_interrupt_disable();
    rt_hw_interrupt_enable(level);
    if (level || rt_interrupt_get_nest() || rt_critical_level() ||
        !rt_thread_self() || !touch_lock_ready)
    {
        return -RT_EBUSY;
    }
    return RT_EOK;
}

static rt_err_t touch_lock_take(void)
{
    rt_err_t err = touch_check_context();
    if (err != RT_EOK) return err;
    /* Do not let recursive mutex ownership bypass a supply-transition hold. */
    if (touch_power_owner == rt_thread_self()) return -RT_EBUSY;
    return rt_mutex_take(&touch_lock, RT_WAITING_FOREVER);
}

static rt_err_t touch_mask_irq_locked(void)
{
    touch_irq_allowed = false;
    return rt_touch_irq_pin_enable(0);
}

static rt_err_t touch_enable_irq_locked(void)
{
    rt_err_t err;

    touch_irq_allowed = true;
    err = rt_touch_irq_pin_enable(1);
    if (err != RT_EOK)
    {
        touch_state = TOUCH_FAULT;
        (void)touch_mask_irq_locked();
    }
    return err;
}

static rt_err_t write_reg(uint8_t reg, rt_uint8_t data)
{
    rt_int8_t res = 0;
    struct rt_i2c_msg msgs;
    rt_uint8_t buf[2] = {reg, data};

    msgs.addr  = FT_DEV_ADDR;    /* slave address */
    msgs.flags = RT_I2C_WR;        /* write flag */
    msgs.buf   = buf;              /* Send data pointer */
    msgs.len   = 2;

    if (rt_i2c_transfer(ft_bus, &msgs, 1) == 1)
    {
        res = RT_EOK;
    }
    else
    {
        res = -RT_ERROR;
    }
    return res;
}

static rt_err_t read_regs(rt_uint8_t reg, rt_uint8_t len, rt_uint8_t *buf)
{
    rt_int8_t res = 0;
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = FT_DEV_ADDR;    /* Slave address */
    msgs[0].flags = RT_I2C_WR;        /* Write flag */
    msgs[0].buf   = &reg;             /* Slave register address */
    msgs[0].len   = 1;                /* Number of bytes sent */

    msgs[1].addr  = FT_DEV_ADDR;    /* Slave address */
    msgs[1].flags = RT_I2C_RD;        /* Read flag */
    msgs[1].buf   = buf;              /* Read data pointer */
    msgs[1].len   = len;              /* Number of bytes read */

    if (rt_i2c_transfer(ft_bus, msgs, 2) == 2)
    {
        res = RT_EOK;
    }
    else
    {
        res = -RT_ERROR;
    }
    return res;
}

/* All helpers below run with touch_lock held, with scheduling/IRQs enabled. */
static rt_err_t touch_reset_config_once_locked(void)
{
    rt_uint8_t mode = 0;
    rt_err_t err;

    touch_state = TOUCH_FAULT;
    err = touch_mask_irq_locked();
    if (err != RT_EOK) return err;
    if (!ft_bus) return -RT_ERROR;
    err = rt_touch_irq_pin_attach(PIN_IRQ_MODE_FALLING, irq_handler, NULL);
    if (err != RT_EOK) return err;

    /* FT5x46: reset low >= 1 ms, report-ready <= 200 ms after reset. */
    BSP_TP_Reset(0);
    rt_thread_mdelay(FT_RESET_LOW_MS);
    BSP_TP_Reset(1);
    rt_thread_mdelay(FT_RESET_READY_MS);

    err = write_reg(FT_ID_G_MODE, 1);
    if (err != RT_EOK) return err;
    err = read_regs(FT_ID_G_MODE, 1, &mode);
    if (err != RT_EOK) return err;
    if (mode != 1) return -RT_ERROR;
    rt_uint8_t ctrl = 0;
    unsigned monitor = FT_CTRL_AUTO_MONITOR;
#ifdef RT_USING_FINSH
    err = read_regs(FT_ID_G_CTRL, sizeof(probe_default), probe_default);
    ctrl = probe_default[0];
    monitor = probe_policy;
#else
    err = read_regs(FT_ID_G_CTRL, 1, &ctrl);
#endif
    if (err == RT_EOK && (ctrl & FT_CTRL_AUTO_MONITOR) != monitor)
        err = write_reg(FT_ID_G_CTRL, (ctrl & ~FT_CTRL_AUTO_MONITOR) | monitor);
#ifdef RT_USING_FINSH
    if (err == RT_EOK && probe_period)
        err = write_reg(FT_ID_G_PERIODMONITOR, probe_period);
    if (err == RT_EOK) err = read_regs(FT_ID_G_CTRL, 1, &probe_selected);
    if (err == RT_EOK && (probe_selected & FT_CTRL_AUTO_MONITOR) != monitor) err = -RT_EIO;
    if (err == RT_EOK) err = read_regs(FT_ID_G_PERIODMONITOR, 1, &probe_period_readback);
    probe_result = err;
#endif
    /* Optional monitor configuration must not disable otherwise usable input. */
    return RT_EOK;
}

static rt_err_t touch_reset_config_locked(void)
{
    if (touch_recovery_attempts >= FT_RECOVERY_ATTEMPTS)
        return touch_recovery_error;
    ++touch_recovery_attempts;
    touch_recovery_error = touch_reset_config_once_locked();
    return touch_recovery_error;
}

static rt_err_t touch_resume_locked(void)
{
    /* Only a successful recovery or a new power lifecycle renews the budget. */
    if (touch_recovery_attempts >= FT_RECOVERY_ATTEMPTS)
        return touch_recovery_error;
    rt_err_t err;
    do
    {
        err = touch_reset_config_locked();
        if (err == RT_EOK) err = touch_enable_irq_locked();
        touch_recovery_error = err;
    } while (err != RT_EOK && touch_recovery_attempts < FT_RECOVERY_ATTEMPTS);
    if (err == RT_EOK)
    {
        touch_state = TOUCH_ACTIVE;
        touch_recovery_attempts = 0;
        /* Sample a contact whose falling edge occurred during reset. */
        rt_sem_release(driver.isr_sem);
    }
    else
    {
        touch_state = TOUCH_FAULT;
        LOG_E("touch recovery failed (%d); input disabled\n", err);
    }
    return err;
}

static rt_err_t touch_sleep_locked(void)
{
    rt_err_t err;
    bool was_active = (touch_state == TOUCH_ACTIVE);

    touch_state = TOUCH_FAULT;
    err = was_active ? touch_mask_irq_locked() : touch_reset_config_locked();
    if (err != RT_EOK) return err;

    /* FocalTech PMODE hibernate stops scanning without changing the rail. */
    err = write_reg(FT_ID_G_PMODE, FT_PMODE_HIBERNATE);
    touch_recovery_error = err;
    if (err == RT_EOK)
    {
        touch_state = TOUCH_ASLEEP;
        touch_recovery_attempts = 0;
        /* Release any contact already delivered before sleep. */
        rt_sem_release(driver.isr_sem);
    }
    return err;
}

static rt_err_t touch_set_sleep_locked(bool sleeping)
{
    rt_err_t err;

    touch_sleeping = sleeping;
    if (touch_state == TOUCH_OFF ||
        (sleeping && touch_state == TOUCH_ASLEEP) ||
        (!sleeping && touch_state == TOUCH_ACTIVE))
    {
        /* OFF records the policy for the next framework init/power-on. */
        err = RT_EOK;
    }
    else if (sleeping)
    {
        err = touch_sleep_locked();
        if (err != RT_EOK)
        {
            /* A failed transfer may still have put the IC to sleep. */
            touch_sleeping = false;
            if (touch_resume_locked() != RT_EOK)
                LOG_E("touch sleep rollback failed; input disabled\n");
        }
    }
    else
    {
        err = touch_resume_locked();
    }

    return err;
}

static void touch_cut_sleep_supply_locked(void)
{
    if (touch_state == TOUCH_ASLEEP && !touch_supply_suspended)
    {
        BSP_TP_PowerDown();
        touch_supply_suspended = true;
    }
}

static rt_err_t touch_system_sleep_locked(bool sleeping)
{
    if (!sleeping && touch_supply_suspended)
    {
        bsp_touch_power_on_in_reset();
        touch_supply_suspended = false;
    }
    rt_err_t err = touch_set_sleep_locked(sleeping);
    if (err == RT_EOK && sleeping) touch_cut_sleep_supply_locked();
    return err;
}

static rt_err_t touch_apply_policy_locked(void)
{
    if (!touch_enabled && touch_state != TOUCH_OFF)
    {
        if (touch_supply_suspended) return RT_EOK;
        /* The user explicitly disabled input. Reset and power it off with
         * delivery masked, even if the controller no longer answers I2C. */
        rt_err_t err = touch_mask_irq_locked();
        if (err != RT_EOK) return err;
        touch_sleeping = true;
        touch_state = TOUCH_ASLEEP;
        touch_cut_sleep_supply_locked();
        touch_recovery_attempts = 0;
        rt_sem_release(driver.isr_sem);
        return RT_EOK;
    }
    return touch_system_sleep_locked(touch_system_suspended || !touch_enabled);
}

rt_err_t bsp_touch_set_sleep(bool sleeping)
{
    rt_err_t err = touch_lock_take();
    if (err != RT_EOK) return err;
    touch_system_suspended = sleeping;
    err = touch_apply_policy_locked();
    if (err != RT_EOK) touch_system_suspended = false;
    rt_mutex_release(&touch_lock);
    return err;
}

rt_err_t bsp_touch_set_enabled(bool enabled)
{
    rt_err_t err = touch_lock_take();
    if (err != RT_EOK) return err;
    touch_enabled = enabled;
    err = touch_apply_policy_locked();
    rt_mutex_release(&touch_lock);
    return err;
}

bool bsp_touch_disabled(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool disabled = !touch_enabled && touch_supply_suspended && !touch_power_owner;
    rt_hw_interrupt_enable(level);
    return disabled;
}

rt_err_t bsp_touch_power_hold_begin(void)
{
    rt_err_t err = touch_lock_take();
    if (err != RT_EOK) return err;
    err = touch_set_sleep_locked(true);
    if (err != RT_EOK)
    {
        /* Exclusive, masked I/O is sufficient for an external power cut. */
        err = touch_mask_irq_locked();
        if (err != RT_EOK)
        {
            rt_mutex_release(&touch_lock);
            return err;
        }
    }
    /* The caller owns the supply sequence until hold_end on this thread. */
    touch_power_owner = rt_thread_self();
    return RT_EOK;
}

rt_err_t bsp_touch_power_hold_end(void)
{
    rt_err_t err = touch_check_context();
    if (err != RT_EOK) return err;
    if (touch_power_owner != rt_thread_self()) return -RT_EBUSY;

    /* Power-up resets PMODE/G_MODE; a later resume must configure the IC. */
    touch_supply_suspended = false;
    if (touch_state != TOUCH_OFF) touch_state = TOUCH_FAULT;
    touch_recovery_attempts = 0;
    touch_recovery_error = -RT_ERROR;
    touch_power_owner = RT_NULL;
    rt_mutex_release(&touch_lock);
    return RT_EOK;
}

static rt_err_t read_point(touch_msg_t p_msg)
{
    uint8_t report[FT_CONTACT_BYTES];
    rt_err_t err;

    if (!p_msg)
    {
        return RT_ERROR;
    }

    p_msg->x = 0;
    p_msg->y = 0;
    p_msg->event = TOUCH_EVENT_UP;
    err = touch_lock_take();
    if (err != RT_EOK) return err;

    /* Releases retain the last completely read contact position. */
    p_msg->x = last_x;
    p_msg->y = last_y;
    p_msg->event = TOUCH_EVENT_UP;

    if (touch_sleeping || touch_state != TOUCH_ACTIVE)
    {
        err = RT_EEMPTY;
        goto DONE;
    }
    err = touch_enable_irq_locked();
    if (err != RT_EOK) goto ERROR_HANDLE;

    if (!ft_bus || read_regs(FT_TD_STATUS, sizeof(report), report) != RT_EOK)
        goto ERROR_HANDLE;

    uint8_t contacts = report[0];
    unsigned event = report[1] >> 6;
    /* A reserved TD_STATUS bit, unused contact ID or release ends the read. */
    err = RT_EEMPTY;
    if (!contacts || (contacts & 0xf0u) || (report[3] >> 4) == 0x0fu ||
        (event != 0 && event != 2)) goto DONE;

    last_x = ((report[1] & 7u) << 8) | report[2];
    last_y = ((report[3] & 7u) << 8) | report[4];
    p_msg->x = last_x;
    p_msg->y = last_y;
    p_msg->event = TOUCH_EVENT_DOWN;
#ifdef RT_USING_FINSH
    ++probe_contacts;
#endif
    if (contacts > 1) err = RT_EOK;
    goto DONE;

ERROR_HANDLE:
    LOG_E("Error, return Up event, x = %d, y = %d\n", p_msg->x, p_msg->y);

    err = RT_ERROR;
DONE:
    rt_mutex_release(&touch_lock);
    return err;
}

static void irq_handler(void *arg)
{
    rt_err_t ret = RT_ERROR;

    (void)arg;
    if (!touch_irq_allowed) return;
#ifdef RT_USING_FINSH
    ++probe_irqs;
#endif

    rt_touch_irq_pin_enable(0);

    ret = rt_sem_release(driver.isr_sem);
    RT_ASSERT(RT_EOK == ret);
}

static rt_err_t init(void)
{
    rt_err_t err = touch_lock_take();
    if (err != RT_EOK) return err;

    /* The framework may have reset/powered up the IC while sleep is requested. */
    touch_supply_suspended = false;
    touch_state = TOUCH_FAULT;
    err = touch_apply_policy_locked();
    rt_mutex_release(&touch_lock);
    return err;

}

static rt_err_t deinit(void)
{
    rt_err_t err = touch_lock_take();
    if (err != RT_EOK) return err;

    err = touch_mask_irq_locked();
    /* The framework owns subsequent power-down and the next init. */
    touch_supply_suspended = false;
    touch_state = TOUCH_OFF;
    touch_recovery_attempts = 0;
    touch_recovery_error = -RT_ERROR;
    rt_mutex_release(&touch_lock);
    return err;

}

static rt_bool_t probe(void)
{
    struct rt_i2c_bus_device *bus;
    rt_bool_t found = RT_FALSE;
    rt_err_t err = touch_lock_take();
    if (err != RT_EOK) return RT_FALSE;

    if (ft_bus)
    {
        found = RT_TRUE;
        goto DONE;
    }
    bus = (struct rt_i2c_bus_device *)rt_device_find(TOUCH_DEVICE_NAME);
    if (!bus || RT_Device_Class_I2CBUS != bus->parent.type)
    {
        LOG_I("bus not find\n");
        goto DONE;
    }
    err = rt_device_open((rt_device_t)bus,
                        RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_TX | RT_DEVICE_FLAG_INT_RX);
    if (err != RT_EOK) goto DONE;

    {
        struct rt_i2c_configuration configuration =
        {
            .mode = 0,
            .addr = 0,
            .timeout = 500,
            .max_hz  = 400000,
        };

        err = rt_i2c_configure(bus, &configuration);
    }
    if (err == RT_EOK)
    {
        ft_bus = bus;
        found = RT_TRUE;
    }
    else
    {
        rt_device_close((rt_device_t)bus);
    }
DONE:
    rt_mutex_release(&touch_lock);
    return found;
}

static struct touch_ops ops =
{
    read_point,
    init,
    deinit
};

static int rt_tp_device_init(void)
{
    rt_err_t err = rt_mutex_init(&touch_lock, "ft5446", RT_IPC_FLAG_PRIO);
    if (err != RT_EOK) return err;

    driver.probe = probe;
    driver.ops = &ops;
    driver.user_data = RT_NULL;
    driver.isr_sem = rt_sem_create("ft5446u", 0, RT_IPC_FLAG_FIFO);
    if (!driver.isr_sem)
    {
        rt_mutex_detach(&touch_lock);
        return -RT_ENOMEM;
    }

    touch_lock_ready = true;
    rt_touch_drivers_register(&driver);

    return 0;

}
INIT_COMPONENT_EXPORT(rt_tp_device_init);

#ifdef RT_USING_FINSH
static void touch_probe(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "sample") &&
                      strcmp(argv[1], "active") &&
                      strcmp(argv[1], "monitor") &&
                      strcmp(argv[1], "period45") &&
                      strcmp(argv[1], "period60")))
    {
        rt_kprintf("usage: touch_probe sample|active|monitor|period45|period60\n");
        return;
    }
    if (!bsp_touch_ft5446_active())
    {
        rt_kprintf("touch_probe: touch is unavailable\n");
        return;
    }
    if (strcmp(argv[1], "sample"))
    {
        rt_err_t err = touch_lock_take();
        if (err != RT_EOK) return;
        if (touch_state == TOUCH_ACTIVE && !touch_sleeping)
        {
            probe_policy = strcmp(argv[1], "active") != 0;
            probe_period = !strcmp(argv[1], "period45") ? 45 :
                           !strcmp(argv[1], "period60") ? 60 : 0;
            err = touch_resume_locked();
        }
        rt_mutex_release(&touch_lock);
        if (err != RT_EOK)
        {
            rt_kprintf("touch_probe: reset failed %d\n", err);
            return;
        }
    }
    rt_kprintf("touch_probe begin %s ctrl_reset=%u enter=%u active=%u monitor=%u ctrl=%u period=%u result=%d\n",
               argv[1], probe_default[0], probe_default[1], probe_default[2],
               probe_default[3], probe_selected, probe_period_readback, probe_result);
    const unsigned irqs = probe_irqs;
    const unsigned contacts = probe_contacts;
    rt_thread_mdelay(10000);
    rt_kprintf("touch_probe end %s irqs=%u contacts=%u\n",
               argv[1], probe_irqs - irqs, probe_contacts - contacts);
}
MSH_CMD_EXPORT(touch_probe, Check FT5446U automatic monitor and interrupt activity);
#endif

#endif /* BSP_USING_RTTHREAD && BSP_USING_TOUCHD */

