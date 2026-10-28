/**
 * @file buttons.c
 * @brief 按键服务实现（纯 C，57 板 ADC 按键）
 *
 * 57 板（sf32lb57-epd-gyd7n6）按键方案：
 *   - 三个物理键（KEY1/KEY2/KEY3）共用 1 个 GPIO（BSP_KEY1_PIN=PA34）作为中断脚，
 *     按下后经 ADC（"bat1" 设备、通道 6）采样电压区分具体按键（USING_ADC_BUTTON，
 *     电压表见板卡配置 CONFIG_ADC_BUTTON_GROUP1_BUTTONx_VOLT）；
 *   - 回调仅报告 KEY1/KEY2/KEY3 的消抖按下和松开；
 *   - 单击、长按及页面分发由常驻输入层在 UI 线程处理。
 */
#include "buttons.h"

#include "board.h"
#include "button.h"
#include "drv_gpio.h"   /* GET_GPIO_INSTANCE / GET_GPIOx_PIN */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>

#define DBG_TAG "buttons"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static buttons_callback_t s_event_cb = RT_NULL;

static volatile bool s_ready = false;


static void dummy_button_event_handler(int32_t pin, button_action_t action)
{
    /* 57 板按键全部走 ADC 按键路径，此 GPIO 回调仅作占位 */
    (void)pin;
    (void)action;
}

#ifdef USING_ADC_BUTTON

static int32_t s_button_id = -1;

static void adc_button_handler(uint8_t group_idx, int32_t btn_idx, button_action_t button_action)
{
    (void)group_idx;
    if (btn_idx < 0 || btn_idx > 2 || !s_event_cb) return;
    if (button_action == BUTTON_PRESSED || button_action == BUTTON_RELEASED)
        s_event_cb((buttons_key_t)(BUTTONS_KEY1 + btn_idx), button_action == BUTTON_PRESSED);
}

static void fix_key_pin_pad(void)
{
    int pad = PAD_PA00 + BSP_KEY1_PIN;
    HAL_PIN_Set(pad, GPIO_A0 + BSP_KEY1_PIN, PIN_NOPULL, 1);
    HAL_PIN_SetMode(pad, 1, PIN_DIGITAL_IO_NORMAL);
}
#endif /* USING_ADC_BUTTON */

void buttons_init(buttons_callback_t callback)
{
    s_event_cb = callback;
    if (s_ready) return;
    s_ready = false;

#ifdef USING_ADC_BUTTON
    int32_t id;
    button_cfg_t cfg;

    /* 前置防呆：ADC 设备不存在时安全退出（button 库绑定失败会 assert 死机） */
    if (RT_NULL == rt_device_find(ADC_BUTTON_ADC_DEV_NAME))
    {
        LOG_E("adc device \"%s\" not found, buttons disabled", ADC_BUTTON_ADC_DEV_NAME);
        return;
    }

    fix_key_pin_pad();

    memset(&cfg, 0, sizeof(cfg));
    cfg.pin = BSP_KEY1_PIN;

    cfg.active_state = BUTTON_ACTIVE_HIGH;

    cfg.mode = PIN_MODE_INPUT;
    cfg.button_handler = dummy_button_event_handler;

    id = button_init(&cfg);
    if (id < 0)
    {
        LOG_E("button_init failed: %d", (int)id);
        return;
    }

    {
        adc_button_handler_t handlers[ADC_BUTTON_GROUP1_MAX_NUM];
        uint8_t i;
        sf_err_t err;

        for (i = 0; i < ADC_BUTTON_GROUP1_MAX_NUM; i++)
        {
            handlers[i] = adc_button_handler;
        }

        err = button_bind_adc_button(id, 0, ADC_BUTTON_GROUP1_MAX_NUM, handlers);
        if (SF_EOK != err)
        {
            LOG_E("button_bind_adc_button failed: %d", (int)err);
            return;
        }
    }

    s_button_id = id;
    if (SF_EOK != button_enable(id))
    {
        LOG_E("button_enable failed");
        return;
    }

    s_ready = true;
#else
    LOG_W("USING_ADC_BUTTON not enabled, buttons disabled");
#endif /* USING_ADC_BUTTON */
}


/*---------------------------------------------------------------------------*/
/* 公共接口（与平台无关） */
/*---------------------------------------------------------------------------*/

void buttons_set_callback(buttons_callback_t callback)
{
    s_event_cb = callback;
}

bool buttons_ready(void)
{
    return s_ready;
}

bool buttons_pressed(void)
{
#if defined(USING_ADC_BUTTON)
    return s_ready && button_is_pressed(s_button_id);
#else
    return false;
#endif
}
