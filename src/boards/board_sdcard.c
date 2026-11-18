#include "board_sdcard.h"
#include "board.h"
#include "bsp_tf_card.h"
#include <rtdevice.h>

void board_sdcard_prepare(void)
{
#ifdef PKG_USING_EPD_SDCARD
    _Static_assert(EPD_SDCARD_DETECT_PIN == BSP_TF_DETECT_PIN, "Board TF card detect pin");
    bsp_tf_card_setup_detect();
    rt_pin_mode(EPD_SDCARD_DETECT_PIN, PIN_MODE_INPUT);
#endif
}

#ifdef PKG_USING_EPD_SDCARD
void board_sdcard_set_power(rt_bool_t enabled)
{
    bsp_tf_card_set_power(enabled);
    if (enabled)
    {
        rt_thread_mdelay(BSP_TF_POWER_SETTLE_MS);
        bsp_tf_card_connect_bus();
    }
}

/* Prepare the card slot before the SDK starts its first probe. */
static int board_sdcard_early_init(void)
{
    board_sdcard_prepare();
    /* The SDK submits its first probe during device initialization. */
    board_sdcard_set_power(RT_TRUE);
    return RT_EOK;
}
INIT_PREV_EXPORT(board_sdcard_early_init);
#endif
