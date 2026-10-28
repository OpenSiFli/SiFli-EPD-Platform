/* SPDX-License-Identifier: Apache-2.0 */
#include "board_pm.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include "board.h"

#if defined(BSP_USING_PM) && defined(PM_DEEP_ENABLE)
#include "bf0_pm.h"
#include "bf0_hal_aon.h"
#include <drivers/pin.h>
#include "drv_gpio.h"
#include "drv_lcd.h"
#include "epd_panel_pm.h"
#include "bsp_touch_pm.h"
#include "bsp_touch_ft5446.h"
#include "platform/epd_power.h"

/* low_power_init uses this weak SDK policy before board_pm_init runs.
 * Keep boot safe even when initialization fails. The SDK still owns WFE/WFI,
 * device suspend/resume, RAM retention and tick compensation. */
const pm_policy_t pm_policy[] = {{30, PM_SLEEP_MODE_IDLE}};
/* Policy thresholds are milliseconds. Fit inside the 8 ms pointer poll and
 * 20 ms UI wait cap; 2 ms matches the SDK's two-tick tickless floor at 1 kHz.
 * Shorter deadlines and NONE/IDLE requests still keep the CPU in IDLE. */
static const pm_policy_t normal_policy[] = {{2, PM_SLEEP_MODE_LIGHT}};
static const pm_policy_t sleep_policy[] = {{30, PM_SLEEP_MODE_DEEP}};
static bool initialized;
static bool system_sleep;
static rt_err_t touch_resume_result;
static uint32_t key_wsr_mask;
static const struct rt_pin_ops *sdk_pin_ops;
static struct rt_pin_ops board_pin_ops;
static void (*key_irq_handler)(void *);
static void *key_irq_args;
static rt_uint32_t key_irq_mode;

static void board_key_irq(void *args)
{
    (void)args;
    /* GPIO dispatch has already cleared WSR. Even an already-released short
     * pulse must reach the coordinator before SDK debounce/ADC sampling.
     * Keep both edges: the coordinator consumes the whole wake gesture, and
     * the original handler still needs the release to finish its state. */
    (void)epd_power_key_wakeup();
    if (key_irq_handler) key_irq_handler(key_irq_args);
}

static rt_err_t board_pin_attach_irq(struct rt_device *device, rt_int32_t pin,
                                    rt_uint32_t mode, void (*handler)(void *), void *args)
{
    if (pin != BSP_KEY1_PIN || !handler)
        return sdk_pin_ops->pin_attach_irq(device, pin, mode, handler, args);

    rt_base_t level = rt_hw_interrupt_disable();
    rt_err_t result;
    if (key_irq_handler && (key_irq_handler != handler || key_irq_args != args || key_irq_mode != mode))
        result = RT_EBUSY; /* Preserve this SDK's positive attach-error convention. */
    else
    {
        result = sdk_pin_ops->pin_attach_irq(device, pin, mode, board_key_irq, RT_NULL);
        if (result == RT_EOK)
        {
            key_irq_handler = handler;
            key_irq_args = args;
            key_irq_mode = mode;
        }
    }
    rt_hw_interrupt_enable(level);
    return result;
}

static rt_err_t board_pin_detach_irq(struct rt_device *device, rt_int32_t pin)
{
    if (pin != BSP_KEY1_PIN) return sdk_pin_ops->pin_detach_irq(device, pin);
    rt_base_t level = rt_hw_interrupt_disable();
    rt_err_t result = sdk_pin_ops->pin_detach_irq(device, pin);
    if (pin == BSP_KEY1_PIN && result == RT_EOK)
    {
        key_irq_handler = RT_NULL;
        key_irq_args = RT_NULL;
        if (initialized)
        {
            rt_pm_policy_register(1, normal_policy);
            system_sleep = false;
            /* Touch restoration can use I2C; defer it to the UI thread. */
            (void)epd_power_key_wakeup();
        }
    }
    rt_hw_interrupt_enable(level);
    return result;
}

static void capture_key_wakeup(void)
{
    if (initialized && (HAL_HPAON_GET_WSR() & key_wsr_mask))
        (void)epd_power_key_wakeup();
}

static void board_pm_notify(uint8_t event, uint8_t mode, void *data)
{
    (void)data;
    /* pm.c calls EXIT with interrupts masked, before GPIO1_IRQHandler clears
     * PMUC PIN WSR. SF57 HAL_HPAON_GET_WSR includes those PMUC bits. */
    if (event == RT_PM_EXIT_SLEEP)
    {
        if (mode == PM_SLEEP_MODE_DEEP) epd_panel_resume();
        capture_key_wakeup();
    }
}

void aon_irq_handler_hook(uint32_t status)
{
    /* The SDK removed PIN bits from status; read the still-latched register.
     * GPIO dispatch remains owned by the SDK and performs the eventual clear. */
    (void)status;
    capture_key_wakeup();
}

void board_pm_init(void)
{
    if (initialized || !rt_device_find("pm")) return;
    struct rt_device_pin *pins = (struct rt_device_pin *)rt_device_find("pin");
    if (!pins || !pins->ops || !pins->ops->pin_attach_irq || !pins->ops->pin_detach_irq) return;
    rt_base_t level = rt_hw_interrupt_disable();
    int8_t pin = HAL_HPAON_QueryWakeupPin(GET_GPIO_INSTANCE(BSP_KEY1_PIN),
                                        GET_GPIOx_PIN(BSP_KEY1_PIN));
    if (pin >= 0 && pm_enable_pin_wakeup((uint8_t)pin, AON_PIN_MODE_POS_EDGE) == RT_EOK)
    {
        key_wsr_mask = 1UL << (HPSYS_AON_WSR_PIN_FIRST_POS + pin);
        /* Install before buttons_init. Keep the SDK's pin operations intact;
         * only PA34 registration is chained, not the shared GPIO1 vector.
         * Missing/failed button registration keeps available() false. */
        sdk_pin_ops = pins->ops;
        board_pin_ops = *sdk_pin_ops;
        board_pin_ops.pin_attach_irq = board_pin_attach_irq;
        board_pin_ops.pin_detach_irq = board_pin_detach_irq;
        pins->ops = &board_pin_ops;
        rt_pm_notify_set(board_pm_notify, RT_NULL);
        /* This SDK's selector ignores LIGHT request counts. Use its policy
         * API to cap ordinary idle at LIGHT; NONE/IDLE device requests still
         * veto both policies in _pm_enter_sleep. */
        rt_pm_policy_register(1, normal_policy);
        initialized = true;
    }
    rt_hw_interrupt_enable(level);
}

static int board_pm_auto_init(void)
{
    board_pm_init();
    return RT_EOK;
}
INIT_ENV_EXPORT(board_pm_auto_init);

bool board_pm_available(void)
{
    return initialized && key_irq_handler != RT_NULL;
}

bool board_pm_display_idle(void)
{
    rt_device_t lcd = rt_device_find("lcd");
    bool busy = true;
    if (!lcd || rt_device_control(lcd, RTGRAPHIC_CTRL_GET_BUSY, &busy) != RT_EOK || busy)
        return false;
    return epd_panel_idle();
}

bool board_pm_display_succeeded(void)
{
    return epd_panel_last_frame_succeeded();
}

rt_err_t board_pm_touch_resume_result(void)
{
    if (touch_resume_result != RT_EOK && bsp_touch_ft5446_active())
        touch_resume_result = RT_EOK;
    return touch_resume_result;
}

static void board_touch_resume(void)
{
    rt_err_t result = bsp_touch_set_sleep(false);
    if (result != RT_EOK && result != touch_resume_result)
        rt_kprintf("sleep: touch resume failed (%d); input disabled\n", result);
    if (result != RT_EOK) touch_resume_result = result;
    else (void)board_pm_touch_resume_result();
}

bool board_pm_set_system_sleep(bool enabled)
{
    if (!initialized) return false;
    if (enabled && (!board_pm_available() || !board_pm_display_idle()))
    {
        (void)board_pm_set_system_sleep(false);
        return false;
    }

    rt_base_t level = rt_hw_interrupt_disable();
    if (!enabled)
    {
        rt_pm_policy_register(1, normal_policy);
        system_sleep = false;
        rt_hw_interrupt_enable(level);
        board_touch_resume();
        return true;
    }
    if (system_sleep)
    {
        rt_hw_interrupt_enable(level);
        return true;
    }
    rt_pm_policy_register(1, normal_policy);
    bool key = board_pm_key_pressed() || (HAL_HPAON_GET_WSR() & key_wsr_mask);
    bool ok = key_irq_handler != RT_NULL && !key;
    if (key) (void)epd_power_key_wakeup();
    rt_hw_interrupt_enable(level);

    /* Touch register access and reset delays require normal thread context. */
    if (ok) ok = bsp_touch_set_sleep(true) == RT_EOK;
    level = rt_hw_interrupt_disable();
    key = board_pm_key_pressed() || (HAL_HPAON_GET_WSR() & key_wsr_mask);
    if (key) (void)epd_power_key_wakeup();
    ok = ok && key_irq_handler != RT_NULL && !key;
    if (ok)
    {
        system_sleep = true;
        rt_pm_policy_register(1, sleep_policy);
    }
    rt_hw_interrupt_enable(level);
    if (!ok) board_touch_resume();
    return ok;
}

#else
void board_pm_init(void) {}
bool board_pm_available(void) { return false; }
bool board_pm_set_system_sleep(bool enabled) { return !enabled; }
rt_err_t board_pm_touch_resume_result(void) { return RT_EOK; }
bool board_pm_display_idle(void) { return false; }
bool board_pm_display_succeeded(void) { return false; }
#endif

bool board_pm_key_pressed(void)
{
#ifdef BSP_KEY1_PIN
    return rt_pin_read(BSP_KEY1_PIN) != 0;
#else
    return false;
#endif
}
