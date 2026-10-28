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
#include "bsp_touch_pm.h"
#include "bsp_touch_ft5446.h"

/* Define -------------------------------------------------------------------*/

#define DBG_LEVEL          DBG_ERROR
#define LOG_TAG              "drv.ft5446u"
#include <drv_log.h>

#define FT_DEV_ADDR             (0x38)
#define FT_TD_STATUS            (0x02)
#define FT_P1_XH                (0x05)
#define FT_P1_XL                (0x06)
#define FT_P1_YH                (0x03)
#define FT_P1_YL                (0x04)
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
    return (mode == 1) ? RT_EOK : -RT_ERROR;
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

rt_err_t bsp_touch_set_sleep(bool sleeping)
{
    rt_err_t err = touch_lock_take();
    if (err != RT_EOK) return err;
    err = touch_set_sleep_locked(sleeping);
    rt_mutex_release(&touch_lock);
    return err;
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
    if (touch_state != TOUCH_OFF) touch_state = TOUCH_FAULT;
    touch_recovery_attempts = 0;
    touch_recovery_error = -RT_ERROR;
    touch_power_owner = RT_NULL;
    rt_mutex_release(&touch_lock);
    return RT_EOK;
}

static rt_err_t read_point(touch_msg_t p_msg)
{
    unsigned char out_val[2];
    rt_uint16_t x, y;
    uint8_t tp_num;
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

    if (ft_bus)
    {
        err = read_regs(FT_TD_STATUS, 1, &tp_num);
        if (RT_EOK != err)
        {
            goto ERROR_HANDLE;
        }

        /* TD_STATUS uses its low nibble. An unready/all-ones report must
         * finish this sample, not keep the framework's read loop running. */
        if (tp_num & 0xF0)
        {
            err = RT_EEMPTY;
            goto DONE;
        }

        if (tp_num > 0)
        {
            // get x positon
            err = read_regs(FT_P1_XL, 1, &out_val[0]);
            if (RT_EOK != err)
            {
                LOG_I("get xL fail\n");
                goto ERROR_HANDLE;
            }
            err = read_regs(FT_P1_XH, 1, &out_val[1]);
            if (RT_EOK != err)
            {
                LOG_I("get xH fail\n");
                goto ERROR_HANDLE;
            }
            /* Register 0x05 contains the contact ID; 0xF is unused. */
            if ((out_val[1] >> 4) == 0x0F)
            {
                err = RT_EEMPTY;
                goto DONE;
            }
            y = ((out_val[1] & 0x7) << 8) | out_val[0];

            // get y position
            err = read_regs(FT_P1_YL, 1, &out_val[0]);
            if (RT_EOK != err)
            {
                LOG_I("get yL fail\n");
                goto ERROR_HANDLE;
            }
            err = read_regs(FT_P1_YH, 1, &out_val[1]);
            if (RT_EOK != err)
            {
                LOG_I("get yH fail\n");
                goto ERROR_HANDLE;
            }
            /* Register 0x03 carries DOWN (0), UP (1), CONTACT (2), or
             * no event (3). A released/unused slot is not a new press. */
            if ((out_val[1] >> 6) != 0 && (out_val[1] >> 6) != 2)
            {
                err = RT_EEMPTY;
                goto DONE;
            }
            x = ((out_val[1] & 0x7) << 8) | out_val[0];

            last_x = x;
            last_y = y;
            p_msg->x = x;
            p_msg->y = y;
            p_msg->event = TOUCH_EVENT_DOWN;

            err = (tp_num > 1) ? RT_EOK : RT_EEMPTY;
            goto DONE;
        }
        else
        {
            err = RT_EEMPTY;
            goto DONE;
        }
    }

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

    rt_touch_irq_pin_enable(0);

    ret = rt_sem_release(driver.isr_sem);
    RT_ASSERT(RT_EOK == ret);
}

static rt_err_t init(void)
{
    rt_err_t err = touch_lock_take();
    if (err != RT_EOK) return err;

    /* The framework may have reset/powered up the IC while sleep is requested. */
    touch_state = TOUCH_FAULT;
    err = touch_sleeping ? touch_sleep_locked() : touch_resume_locked();
    rt_mutex_release(&touch_lock);
    return err;

}

static rt_err_t deinit(void)
{
    rt_err_t err = touch_lock_take();
    if (err != RT_EOK) return err;

    err = touch_mask_irq_locked();
    /* The framework owns subsequent power-down and the next init. */
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

#endif /* BSP_USING_RTTHREAD && BSP_USING_TOUCHD */

