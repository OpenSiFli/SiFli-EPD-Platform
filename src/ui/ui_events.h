/**
 * @file ui_events.h
 * @brief UI 动作与内部消息定义（纯 C）
 *
 * 原 controls/Actions.h 的 C 版本：
 *  - UIAction：按键/触摸产生的用户动作 + 内部消息队列消息
 *  - ActionCallback_t：动作回调（由输入层调用，向应用分发）
 */
#ifndef __UI_EVENTS_H__
#define __UI_EVENTS_H__

typedef enum
{
    NONE = 0,
    UP,                     /* 上移 / 上一项 / 向前翻页          */
    DOWN,                   /* 下移 / 下一项 / 向后翻页          */
    SELECT,                 /* 确认 / 进入                       */
    UPGLIDE,                /* 长按：呼出阅读操作层 / 全局操作    */
    PREV_OPTION,            /* 参数值 -1                         */
    NEXT_OPTION,            /* 参数值 +1                         */
    SELECT_BOX,             /* 触控选中                          */
    ENTER_READING_SETTINGS, /* 进入阅读设置                      */
    LAST_INTERACTION,       /* 最后一次交互时间戳                */

    /* ---- 以下为内部消息（经消息队列投递给 UI 主循环） ---- */
    MSG_DRAW_LOW_POWER_PAGE,    /* 电量过低，绘制低电量页        */
    MSG_DRAW_CHARGE_PAGE,       /* 充电中（低电恢复），绘制充电页 */
    MSG_DRAW_WELCOME_PAGE,      /* 电量恢复，绘制欢迎页          */

    POWER_MENU,                /* KEY1 长按三秒，呼出电源菜单   */
} UIAction;

/* 动作回调：输入源（按键等）向应用分发动作 */
typedef void (*ActionCallback_t)(UIAction action);

#endif /* __UI_EVENTS_H__ */
