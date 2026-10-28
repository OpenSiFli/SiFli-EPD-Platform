/**
 * @file buttons.h
 * @brief 按键服务接口（纯 C，57 板 ADC 按键）
 *
 * 三个物理键（KEY1/KEY2/KEY3）共用 PA34 中断脚，经 ADC 分压区分按键，
 * 向常驻输入层报告消抖后的物理按下与松开，由输入层判定单击和长按。
 *
 * 线程说明：动作回调运行在 button 库的软定时器线程上下文，
 *           回调仅投递动作，由 UI 主循环处理，不直接操作 LVGL。
 */
#ifndef __BUTTONS_H__
#define __BUTTONS_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    BUTTONS_KEY1 = 1,
    BUTTONS_KEY2 = 2,
    BUTTONS_KEY3 = 3
} buttons_key_t;

typedef void (*buttons_callback_t)(buttons_key_t key, bool pressed);
void buttons_init(buttons_callback_t callback);

/** 运行期注册/替换动作回调（UI 初始化完成后调用；传 NULL 注销） */
void buttons_set_callback(buttons_callback_t callback);

/** 按键服务是否初始化成功 */
bool buttons_ready(void);
/* Shared PA34 level; no ADC identity is required. */
bool buttons_pressed(void);

#ifdef __cplusplus
}
#endif

#endif /* __BUTTONS_H__ */
