#include "board_sdcard.h"
#include "board.h"
#include "bsp_tf_card.h"
#include <rtdevice.h>
#ifdef PKG_USING_EPD_SDCARD
#include <drivers/mmcsd_core.h>
#include "sdcard_sdio_bridge.h"
#endif

void board_sdcard_prepare(void)
{
#ifdef PKG_USING_EPD_SDCARD
    _Static_assert(EPD_SDCARD_DETECT_PIN == BSP_TF_DETECT_PIN, "Board TF card detect pin");
    bsp_tf_card_setup_detect();
    rt_pin_mode(EPD_SDCARD_DETECT_PIN, PIN_MODE_INPUT);
#endif
}

bool board_sdcard_poweroff_ready(void)
{
#if defined(BSP_USING_SDMMC2) && defined(PKG_USING_EPD_SDCARD)
    struct rt_mmcsd_host *host = sdcard_sdio_find(EPD_SDCARD_DEVICE_NAME);
    if (host && (sdcard_sdio_state(host) == SDCARD_SDIO_PENDING || host->bus_lock.owner ||
                 (hwp_sdmmc2->SR & (SD_SR_CMD_BUSY | SD_SR_DATA_BUSY)))) return false;
    if (host && host->card && rt_pin_read(EPD_SDCARD_DETECT_PIN) == PIN_LOW)
        return HAL_GPIO_ReadPin(hwp_gpio1, 49) == GPIO_PIN_SET;
#endif
    return true;
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
