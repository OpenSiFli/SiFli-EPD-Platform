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
#include "ptm_ins.h"

#ifndef HAL_LCDC_EPD_START_HOOK_SUPPORTED
#error "Run python tools/prepare_build.py to apply the EPD SDK patch"
#endif
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
/* CKV low phase: 55 clocks / 24 MHz = 2.292 us. */
#define PANEL_CKV_LOW_CLOCKS 55u

static const LCDC_InitTypeDef panel_config = {
    .lcd_itf = LCDC_INTF_EPD_8BIT,
    .freq = 24 * 1000 * 1000,
    .color_mode = LCDC_PIXEL_FORMAT_F2_SWAP,
    .cfg = {.epd = {
        .SDMODE = 0, .SDCLK_polarity = 0, .GDSP_polarity = 0,
        .GDCLK_polarity = 1,
        .LSL = 20, .LBL = 48, .LEL = 36, .LDL = PANEL_WIDTH >> 2,
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

/* Reserve PTM1 Core0, TCM[0..15] and CH0 for EPD. Do not share with LCDC
 * DPI_AUX/SPI_AUX. PA41 selects CH0 through the 57x matrix; WAIT_PA40 samples
 * CKV without changing its TCON mux. Delay both SPV edges by 7.55 us from
 * the first two CKV rising edges. */
#define SPV_PHASE_NS 7550u
#define SPV_PTM_CH (1u << 0)
#define SPV_PTM_END 7u
#define SPV_PTM_FLAGS (PTM_ISR_EVT0 | PTM_ISR_ERR0 | PTM_ISR_TCM0)
static bool spv_ptm_ready, spv_ptm_armed, spv_ptm_clock_was_enabled;

static void configure_spv_core(uint32_t last)
{
    /* No PTM IRQ is needed: LCDC completion checks EVT0. Explicit END
     * addresses prevent execution into another core's TCM. PSC=0, REP=0. */
    hwp_ptm1->CER |= PTM_CER_RST0;
    hwp_ptm1->ISR = SPV_PTM_FLAGS;
    MODIFY_REG(hwp_ptm1->IER, PTM_IER_EVTIE0 | PTM_IER_ERRIE0 | PTM_IER_EVTAUTO0,
               PTM_IER_EVTAUTO0);
    hwp_ptm1->CCR0 = (last << PTM_CCR0_END0_Pos) | (last << PTM_CCR0_END1_Pos);
    hwp_ptm1->ICR0 = 0;
    hwp_ptm1->OCR0 = 0; /* CH0 = OUT0; no FIFO-induced stalls. */
}

static void release_spv_ptm(void)
{
    if (!spv_ptm_ready) return;
    hwp_ptm1->CER |= PTM_CER_RST0;
    hwp_ptm1->DOSR = SPV_PTM_CH;
    hwp_ptm1->DOECR = SPV_PTM_CH;
    CLEAR_BIT(hwp_ptm1->IER, PTM_IER_EVTIE0 | PTM_IER_ERRIE0 | PTM_IER_EVTAUTO0);
    hwp_ptm1->ISR = SPV_PTM_FLAGS;
    spv_ptm_ready = spv_ptm_armed = false;
    if (!spv_ptm_clock_was_enabled && !(hwp_ptm1->CER & 0xffu))
        HAL_RCC_DisableModule(RCC_MOD_PTM1);
}

static int prepare_spv_ptm(LCDC_HandleTypeDef *lcdc)
{
    uint32_t spv_ptm_hz = HAL_RCC_GetHCLKFreq(CORE_ID_DEFAULT);
    uint64_t cycles = ((uint64_t)spv_ptm_hz * SPV_PHASE_NS + 500000000u) / 1000000000u;
    if (!spv_ptm_hz || !cycles || cycles > 65535u) return -RT_ERROR;
    uint32_t spv_delay_cycles = (uint32_t)cycles;
    if (!HAL_DBG_DWT_IsInit()) HAL_DBG_DWT_Init();
    spv_ptm_clock_was_enabled = !!(hwp_hpsys_rcc->ENR1 & HPSYS_RCC_ENR1_PTM1);
    HAL_RCC_EnableModule(RCC_MOD_PTM1);
    /* This project has one LCDC interface. Refuse a competing PTM user. */
    if ((hwp_ptm1->CER & 0xffu) || (hwp_ptm1->DOER & SPV_PTM_CH)) return -RT_EBUSY;
    spv_ptm_ready = true;

    /* Check TCM execution before enabling panel rails. Interrupts remain
     * enabled; this bounded poll checks readiness, not output timing. */
    configure_spv_core(1);
    volatile uint32_t *code = PTM1_CORE0_TCM;
    code[0] = PTM_SET(PTM_X, PTM_ZERO, OP_OR, 0x5757);
    code[1] = PTM_SET(PTM_OS, PTM_ZERO, OP_OR, SPV_PTM_CH);
    __DSB();
    hwp_ptm1->CER |= PTM_CER_EN0;
    uint32_t start = DWT->CYCCNT;
    while (!(hwp_ptm1->ISR & SPV_PTM_FLAGS))
    {
        if ((uint32_t)(DWT->CYCCNT - start) > spv_ptm_hz / 10000u) goto fail;
    }
    if ((hwp_ptm1->ISR & SPV_PTM_FLAGS) != PTM_ISR_EVT0 ||
        hwp_ptm1->X0 != 0x5757 || !(hwp_ptm1->DOR & SPV_PTM_CH)) goto fail;

    configure_spv_core(SPV_PTM_END);
    const uint32_t program[] = {
        PTM_SET(PTM_X, PTM_ZERO, OP_OR, LCD_IF_COMMAND_START),
        PTM_STM(PTM_X, (uint32_t)&lcdc->Instance->COMMAND),
        PTM_WAIT(WAIT_PA40, 0, 0),
        PTM_WAIT(WAIT_PA40, 1, spv_delay_cycles),
        PTM_SET(PTM_OC, PTM_ZERO, OP_OR, SPV_PTM_CH),
        PTM_WAIT(WAIT_PA40, 0, 0),
        PTM_WAIT(WAIT_PA40, 1, spv_delay_cycles),
        PTM_SET(PTM_OS, PTM_ZERO, OP_OR, SPV_PTM_CH),
    };
    RT_ASSERT(sizeof(program) / sizeof(program[0]) == SPV_PTM_END + 1u);
    for (unsigned i = 0; i <= SPV_PTM_END; ++i) code[i] = program[i];
    __DSB();
    for (unsigned i = 0; i <= SPV_PTM_END; ++i)
        if (code[i] != program[i]) goto fail;
    hwp_ptm1->DOSR = SPV_PTM_CH;
    hwp_ptm1->DOESR = SPV_PTM_CH;
    if (HAL_PIN_Set(PAD_PA41, PTM1_CH0, PIN_NOPULL, 1) != 0) goto fail;
    HAL_PIN_SetMode(PAD_PA41, 1, PIN_DIGITAL_IO_NORMAL);
    return RT_EOK;
fail:
    release_spv_ptm();
    return -RT_ERROR;
}

static bool arm_spv_ptm(void)
{
    if (!spv_ptm_ready || (hwp_ptm1->CER & PTM_CER_EN0)) return false;
    configure_spv_core(SPV_PTM_END);
    hwp_ptm1->DOSR = SPV_PTM_CH;
    __DSB();
    spv_ptm_armed = true;
    return true;
}

bool HAL_LCDC_EPD_StartHook(LCDC_HandleTypeDef *lcdc)
{
    if (lcdc != active_lcdc || lcdc->Init.lcd_itf != LCDC_INTF_EPD_8BIT)
        return false;
    if (!spv_ptm_armed)
    {
        failed = true;
        return false;
    }
    spv_ptm_armed = false;
    /* PTM issues LCDC START and drives both SPV edges from TCM. No CPU edge
     * wait, GPIO toggle or interrupt masking is used for the pulse. */
    hwp_ptm1->CER |= PTM_CER_EN0;
    return true;
}

static bool complete_spv_ptm(void)
{
    uint32_t status = hwp_ptm1->ISR;
    if ((status & SPV_PTM_FLAGS) != PTM_ISR_EVT0 ||
        (hwp_ptm1->CER & PTM_CER_EN0) || !(hwp_ptm1->DOR & SPV_PTM_CH))
    {
        failed = true;
        hwp_ptm1->CER |= PTM_CER_RST0;
        hwp_ptm1->DOSR = SPV_PTM_CH;
        return false;
    }
    return true;
}

static void configure_gate_pulse(LCDC_HandleTypeDef *lcdc)
{
    /* HAL copies LDL into GDH. With GDCLK_POL=1, that long phase is low.
     * Decouple the gate pulse from the 304 data clocks: low about 2.3 us,
     * high for the rest of the line. Reapply after HAL restores the TCON. */
    MODIFY_REG(lcdc->Instance->TCON_IF_CONF3, LCD_IF_TCON_IF_CONF3_GDH_Msk,
               PANEL_CKV_LOW_CLOCKS << LCD_IF_TCON_IF_CONF3_GDH_Pos);
}

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
    release_spv_ptm();
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
        configure_gate_pulse(lcdc);
        HAL_LCDC_SetROIArea(lcdc, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
        resume_pending = false;
    }
    GPIO_InitTypeDef oe = {.Pin = 26, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_NOPULL};
    HAL_GPIO_Init(hwp_gpio1, &oe);
    HAL_PIN_Set(PAD_PA26, LCDC1_TCON_SDOE, PIN_NOPULL, 1);
    if (prepare_spv_ptm(lcdc) != RT_EOK) goto fail;
    if (tps_exit_sleep() != RT_EOK) goto fail;
    powered = true;
    return RT_EOK;

fail:
    BSP_EPD_BusIdle();
    release_spv_ptm();
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
    if (!arm_spv_ptm()) { failed = true; return false; }
    if (HAL_LCDC_SendLayerData_IT(lcdc) == HAL_OK) return true;
    failed = true;
    LOG_E("frame %u failed", index);
    return false;
}

static void frame_complete(LCDC_HandleTypeDef *lcdc)
{
    if (!complete_spv_ptm()) { finish_scan(); return; }
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
    configure_gate_pulse(lcdc);
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
