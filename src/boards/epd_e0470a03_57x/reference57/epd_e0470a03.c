#include "platform/epd_render_profile.h"
/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <string.h>
#include "board.h"
#include "drv_io.h"
#include "drv_lcd.h"
#include "mem_section.h"
#include "system_bf0_ap.h"
#include "epd_tps.h"
#include "../epd_waveform.h"
#include "../epd_display_chunks.h"
#include "../epd_gray_pixels.h"
#include "../epd_panel_pm.h"
#ifdef RT_USING_PM
#include "platform/epd_power.h"
#endif

#define DBG_LEVEL DBG_ERROR
#define LOG_TAG "epd.reference57"
#include "log.h"
#define THE_LCD_ID 0x19357
#define PANEL_WIDTH 1216
#define PANEL_HEIGHT 684

static const LCDC_InitTypeDef panel_config = {
    .lcd_itf = LCDC_INTF_EPD_8BIT,
    .freq = 48 * 1000 * 1000,
    .color_mode = LCDC_PIXEL_FORMAT_F2_SWAP,
    .cfg = {.epd = {
        .SDMODE = 0, .SDCLK_polarity = 0, .GDSP_polarity = 0,
        .GDCLK_polarity = 1,
        .LSL = 20, .LBL = 48, .LEL = 24, .LDL = PANEL_WIDTH >> 2,
        .GSTA = 3, .FSL = 1, .FBL = 3, .FDL = PANEL_HEIGHT, .FEL = 1,
    }},
};
static LCDC_InitTypeDef lcdc_config;
L2_NON_RET_BSS_SECT_BEGIN(reference57_pixels)
L2_NON_RET_BSS_SECT(reference57_pixels, ALIGN(64) static uint8_t mixed[PANEL_WIDTH * PANEL_HEIGHT]);
L2_NON_RET_BSS_SECT_END
static uint32_t lut[HAL_LCDC_LOOKUP_TABLE_SIZE >> 2];
static struct rt_semaphore finished;
static rt_thread_t finish_thread;
static LCDC_HandleTypeDef *active_lcdc;
static void (*sdk_complete)(LCDC_HandleTypeDef *);
static HAL_LCDC_PixelFormat input_format;
static uint32_t frame_count, next_frame;
static volatile bool drawing, powered, collecting, pending, finishing;
static volatile bool resume_pending;
static volatile bool frame_succeeded;
static bool initialized, failed;
static int32_t changed;
static struct { const void *pixels; uint32_t stride; bool first, last; } chunk;
extern void BSP_EPD_BusIdle(void);

static void power_off(void)
{
    if (!powered) return;
    /* Reference panel sequence: OE low before the TPS starts discharging. */
    HAL_PIN_Set(PAD_PA26, GPIO_A26, PIN_NOPULL, 1);
    BSP_GPIO_Set(26, 0, 1);
    BSP_LCD_GMODE_Set(0);
    HAL_Delay_us(20);
    tps_enter_sleep();
    BSP_EPD_BusIdle();
    powered = false;
#ifdef RT_USING_PM
    rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
}

static int power_on(LCDC_HandleTypeDef *lcdc)
{
    if (powered) return RT_EOK;
#ifdef RT_USING_PM
    rt_pm_request(PM_SLEEP_MODE_IDLE);
#endif
    BSP_LCD_PowerUp();
    /* SDK DEEP sleep stops the clocks without issuing LCD CTRL_RESUME.
     * Restore the TCON after the board selects its active clock and before
     * enabling panel power. Keep the framebuffer and SDK callback intact. */
    if (resume_pending)
    {
        if (HAL_LCDC_Resume(lcdc) != HAL_OK) goto fail;
        HAL_LCDC_SetROIArea(lcdc, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
        resume_pending = false;
    }
    GPIO_InitTypeDef oe = {.Pin = 26, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_NOPULL};
    HAL_GPIO_Init(hwp_gpio1, &oe);
    HAL_PIN_Set(PAD_PA26, LCDC1_TCON_SDOE, PIN_NOPULL, 1);
    if (tps_exit_sleep() != RT_EOK) goto fail;
    powered = true;
    return RT_EOK;

fail:
    BSP_EPD_BusIdle();
#ifdef RT_USING_PM
    rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
    return -RT_ERROR;
}

/* SDK LCD requests are serialized. The last RGB chunk remains owned until the
 * complete scan and reference power-down sequence finish in thread context. */
static void finish_entry(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        rt_sem_take(&finished, RT_WAITING_FOREVER);
        LCDC_HandleTypeDef *lcdc = active_lcdc;
        power_off();
        HAL_LCDC_LayerSetFormat(lcdc, HAL_LCDC_LAYER_DEFAULT, input_format);
        if (failed) epd_wave_request_full();
        void (*callback)(LCDC_HandleTypeDef *) = sdk_complete;
        sdk_complete = RT_NULL;
        lcdc->XferCpltCallback = callback;
        frame_succeeded = !failed;
        drawing = finishing = false;
        if (callback) callback(lcdc);
#ifdef RT_USING_PM
        epd_power_notify();
#endif
    }
}

static void finish_scan(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool signal = !finishing;
    finishing = true;
    rt_hw_interrupt_enable(level);
    if (signal) rt_sem_release(&finished);
}

static bool start_frame(LCDC_HandleTypeDef *lcdc, uint32_t index)
{
    if (index >= frame_count) return false;
    epd_wave_table_fill_lut(lut, index);
    BSP_LCD_GMODE_Set(0);
    HAL_Delay_us(1);
    BSP_LCD_GMODE_Set(1);
    if (HAL_LCDC_SendLayerData_IT(lcdc) == HAL_OK) return true;
    failed = true;
    LOG_E("frame %u failed", index);
    return false;
}

static void frame_complete(LCDC_HandleTypeDef *lcdc)
{
    if (!start_frame(lcdc, next_frame++)) finish_scan();
}

static void frame_error(LCDC_HandleTypeDef *lcdc)
{
    (void)lcdc;
    failed = true;
    finish_scan();
}

void epd_display_queue_chunk(const void *pixels, uint32_t stride, bool first, bool last)
{
    rt_base_t level = rt_hw_interrupt_disable();
    RT_ASSERT(pixels && !chunk.pixels);
    chunk.stride = stride;
    chunk.first = first;
    chunk.last = last;
    chunk.pixels = pixels;
    rt_hw_interrupt_enable(level);
}

void epd_display_set_pending(bool value) { pending = value; }
void epd_panel_resume(void) { resume_pending = true; }
bool epd_panel_last_frame_succeeded(void) { return frame_succeeded; }
bool epd_panel_idle(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool idle = !drawing && !powered && !collecting && !chunk.pixels && !pending;
    rt_hw_interrupt_enable(level);
    return idle;
}

static void begin_pixels(void)
{
    frame_succeeded = false;
    const epd_gray_area_t whole = {0, 0, PANEL_HEIGHT - 1, PANEL_WIDTH - 1};
    uint32_t trace = epd_render_profile_begin(EPD_RENDER_SETTLE);
    epd_gray_settle(mixed, &whole);
    epd_render_profile_end(EPD_RENDER_SETTLE, trace);
    changed = 0;
}

static void copy_pixels(const uint8_t *pixels, uint32_t stride,
                        const epd_gray_area_t *area, HAL_LCDC_PixelFormat format)
{
    if (format == LCDC_PIXEL_FORMAT_RGB565)
    {
        uint8_t tile[EPD_GRAY_TILE_W][EPD_GRAY_TILE_H];
        changed += epd_gray_copy_rgb565(mixed, pixels, stride, area, area, tile);
    }
    else
    {
        RT_ASSERT(format == LCDC_PIXEL_FORMAT_A4);
        for (uint32_t y = area->y0; y <= area->y1; ++y)
            for (uint32_t x = area->x0; x <= area->x1; ++x)
            {
                uint32_t sx = x - area->x0;
                uint8_t value = (pixels[(y - area->y0) * stride + sx / 2] >> ((sx & 1) * 4)) & 15;
                uint8_t *dst = mixed + (PANEL_HEIGHT - 1 - x) * PANEL_WIDTH + y;
                changed += (value != (*dst >> 4)) - ((*dst & 15) != (*dst >> 4));
                *dst = (*dst & 0xf0) | value;
            }
    }
}

static void LCD_Init(LCDC_HandleTypeDef *lcdc)
{
    if (!finish_thread)
    {
        rt_err_t result = rt_sem_init(&finished, "epd_end", 0, RT_IPC_FLAG_FIFO);
        RT_ASSERT(result == RT_EOK);
        finish_thread = rt_thread_create("epd_pwr", finish_entry, RT_NULL,
                                         1536, RT_THREAD_PRIORITY_HIGH - 1, 10);
        RT_ASSERT(finish_thread);
        result = rt_thread_startup(finish_thread);
        RT_ASSERT(result == RT_EOK);
        (void)result;
    }
    power_off();
    lcdc_config = panel_config;
    lcdc->Init = lcdc_config;
    HAL_LCDC_Init(lcdc);
    HAL_LCDC_SetROIArea(lcdc, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
    resume_pending = false;
    memset(mixed, 0xff, sizeof(mixed));
    memset(&chunk, 0, sizeof(chunk));
    collecting = drawing = pending = finishing = false;
    frame_succeeded = false;
    epd_wave_table();
    initialized = tps_init(EPD_VCOM_MV) == RT_EOK;
    tps_enter_sleep();
    BSP_EPD_BusIdle();
}

static void LCD_WriteMultiplePixels(LCDC_HandleTypeDef *lcdc, const uint8_t *pixels,
                                    uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const epd_gray_area_t area = {x0, y0, x1, y1};
    bool is_chunk = chunk.pixels == pixels;
    bool first = !is_chunk || chunk.first;
    bool last = !is_chunk || chunk.last;
    uint32_t stride = is_chunk ? chunk.stride :
        (lcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_format == LCDC_PIXEL_FORMAT_A4 ?
         (x1 - x0 + 2u) / 2u : (x1 - x0 + 1u) * 2u);
    RT_ASSERT(!drawing && x0 <= x1 && y0 <= y1 && x1 < PANEL_HEIGHT && y1 < PANEL_WIDTH);
    if (first) { RT_ASSERT(!collecting); begin_pixels(); collecting = true; }
    RT_ASSERT(collecting);
    if (is_chunk) chunk.pixels = RT_NULL;
    uint32_t trace = epd_render_profile_begin(EPD_RENDER_COPY);
    copy_pixels(pixels, stride, &area, lcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_format);
    epd_render_profile_end(EPD_RENDER_COPY, trace);
    if (!last)
    {
        if (lcdc->XferCpltCallback) lcdc->XferCpltCallback(lcdc);
        return;
    }
    collecting = false;
    if (!changed && epd_wave_skip_unchanged_frame())
    {
        frame_succeeded = true;
        if (lcdc->XferCpltCallback) lcdc->XferCpltCallback(lcdc);
        return;
    }
    active_lcdc = lcdc;
    sdk_complete = lcdc->XferCpltCallback;
    input_format = lcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_format;
    drawing = true;
    failed = false;
    frame_count = epd_wave_table_get_frames(28, EPD_DRAW_MODE_AUTO);
    if (!initialized || !frame_count || power_on(lcdc) != RT_EOK)
    {
        failed = true;
        finish_scan();
        return;
    }
    mpu_dcache_clean(mixed, sizeof(mixed));
    HAL_LCDC_LayerSetData(lcdc, HAL_LCDC_LAYER_DEFAULT, mixed, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
    HAL_LCDC_LayerSetFormat(lcdc, HAL_LCDC_LAYER_DEFAULT, LCDC_PIXEL_FORMAT_L8);
    HAL_LCDC_LayerSetLTab(lcdc, HAL_LCDC_LAYER_DEFAULT, (LCDC_AColorDef *)lut);
    lcdc->XferCpltCallback = frame_complete;
    lcdc->XferErrorCallback = frame_error;
    next_frame = 1;
    if (!start_frame(lcdc, 0)) finish_scan();
}

static uint32_t LCD_ReadID(LCDC_HandleTypeDef *lcdc) { (void)lcdc; return THE_LCD_ID; }
static void LCD_DisplayOn(LCDC_HandleTypeDef *lcdc) { (void)lcdc; }
static void LCD_DisplayOff(LCDC_HandleTypeDef *lcdc) { (void)lcdc; }
static void LCD_SetRegion(LCDC_HandleTypeDef *lcdc, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{ (void)lcdc; (void)x0; (void)y0; (void)x1; (void)y1; }
static void LCD_WritePixel(LCDC_HandleTypeDef *lcdc, uint16_t x, uint16_t y, const uint8_t *pixel)
{ (void)lcdc; (void)x; (void)y; (void)pixel; }
static void LCD_SetColorMode(LCDC_HandleTypeDef *lcdc, uint16_t mode) { (void)lcdc; (void)mode; }
static void LCD_SetBrightness(LCDC_HandleTypeDef *lcdc, uint8_t brightness)
{
    (void)lcdc;
    rt_device_t light = rt_device_find("lcdlight");
    if (light && rt_device_open(light, RT_DEVICE_OFLAG_RDWR) == RT_EOK)
    {
        rt_device_write(light, 0, &brightness, 1);
        rt_device_close(light);
    }
}
static void IdleModeOn(LCDC_HandleTypeDef *lcdc) { (void)lcdc; power_off(); }
static void IdleModeOff(LCDC_HandleTypeDef *lcdc) { (void)lcdc; }
static const LCD_DrvOpsDef operations = {
    LCD_Init, LCD_ReadID, LCD_DisplayOn, LCD_DisplayOff,
    LCD_SetRegion, LCD_WritePixel, LCD_WriteMultiplePixels, NULL,
    LCD_SetColorMode, LCD_SetBrightness, IdleModeOn, IdleModeOff,
    NULL, NULL, NULL, NULL,
};
LCD_DRIVER_EXPORT2(epd_e0470a03_57, THE_LCD_ID, &lcdc_config, &operations, 1);
