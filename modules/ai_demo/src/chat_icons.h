#ifndef CHAT_ICONS_H
#define CHAT_ICONS_H
#include "lvgl.h"

typedef enum {
    CHAT_ICON_SESSIONS,
    CHAT_ICON_NEW,
    CHAT_ICON_SPEAKER,
    CHAT_ICON_GLOBE,
    CHAT_ICON_MICROPHONE,
    CHAT_ICON_SEND,
    CHAT_ICON_STOP,
    CHAT_ICON_CHEVRON,
    CHAT_ICON_WELCOME,
    CHAT_ICON_ASSISTANT,
    CHAT_ICON_RETRY,
    CHAT_ICON_COUNT
} chat_icon_t;

/* Pre-rendered sizes at 90, 95, 100, 110 and 120 percent, rounded to even pixels.
 * At 100 percent: 36 px, except globe 28, chevron 20, welcome 120, assistant 32.
 * Active icons are black and inactive icons gray; send and stop stay white.
 */
const lv_image_dsc_t *chat_icon(chat_icon_t icon, unsigned scale, bool active);
void chat_icons_release(void);
#endif
