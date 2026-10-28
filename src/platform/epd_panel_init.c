/* SPDX-License-Identifier: Apache-2.0 */
#include "epd_panel_init.h"

#include <stdbool.h>
#include <string.h>
#include <rtdevice.h>
#include "drv_lcd.h"
#include "epd_waveform.h"
#include "epd_memory.h"

/* Called before the UI loop starts. Clear the panel once with INIT; the first
 * application frame then uses GC16 through the normal LVGL refresh policy. */
rt_err_t epd_panel_boot(void)
{
    rt_device_t lcd = rt_device_find("lcd");
    if (!lcd) return -RT_ENOSYS;
    /* LVGL has opened the LCD. This synchronous command keeps the INIT
     * refresh dark until the UI loads the saved frontlight preference. */
    uint8_t brightness = 0;
    rt_err_t result = rt_device_control(lcd, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &brightness);
    if (result != RT_EOK) return result;

    const size_t bytes = LCD_HOR_RES_MAX * LCD_VER_RES_MAX / 2;
    uint8_t *pixels = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (!pixels) return -RT_ENOMEM;
    memset(pixels, 0xff, bytes);

    int saved_mode = epd_wave_get_mode();
    bool busy;
    do
    {
        result = rt_device_control(lcd, RTGRAPHIC_CTRL_GET_BUSY, &busy);
        if (result != RT_EOK) goto done;
        if (busy) rt_thread_mdelay(1);
    } while (busy);

    uint16_t format = RTGRAPHIC_PIXEL_FORMAT_GRAY4;
    result = rt_device_control(lcd, RTGRAPHIC_CTRL_SET_BUF_FORMAT, &format);
    if (result == RT_EOK)
    {
        epd_wave_set_mode(3);
        rt_graphix_ops(lcd)->set_window(0, 0, LCD_HOR_RES_MAX - 1, LCD_VER_RES_MAX - 1);
        rt_graphix_ops(lcd)->draw_rect((const char *)pixels, 0, 0,
                                      LCD_HOR_RES_MAX - 1, LCD_VER_RES_MAX - 1);
    }

    /* Synchronous mode changes finish rail discharge before returning to LVGL. */
    uint8_t idle = 1;
    rt_err_t cleanup = rt_device_control(lcd, RTGRAPHIC_CTRL_SET_MODE, &idle);
    if (result == RT_EOK) result = cleanup;
    idle = 0;
    cleanup = rt_device_control(lcd, RTGRAPHIC_CTRL_SET_MODE, &idle);
    if (result == RT_EOK) result = cleanup;
    format = RTGRAPHIC_PIXEL_FORMAT_RGB565;
    cleanup = rt_device_control(lcd, RTGRAPHIC_CTRL_SET_BUF_FORMAT, &format);
    if (result == RT_EOK) result = cleanup;
done:
    epd_wave_set_mode(saved_mode);
    epd_wave_request_full();
    epd_app_free(pixels);
    return result;
}
