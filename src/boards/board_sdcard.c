#include "board_sdcard.h"
#include "board.h"
#include <rtdevice.h>

void board_sdcard_prepare(void)
{
#ifdef PKG_USING_EPD_SDCARD
    HAL_PIN_Set(PAD_PA00 + EPD_SDCARD_DETECT_PIN, GPIO_A0 + EPD_SDCARD_DETECT_PIN, PIN_PULLUP, 1);
    rt_pin_mode(EPD_SDCARD_DETECT_PIN, PIN_MODE_INPUT_PULLUP);
#endif
}

#ifdef PKG_USING_EPD_SDCARD
/* Establish the GPIO pull-up before the component takes over hotplug IRQs. */
static int board_sdcard_early_init(void)
{
    board_sdcard_prepare();
    return RT_EOK;
}
INIT_PREV_EXPORT(board_sdcard_early_init);
#endif
