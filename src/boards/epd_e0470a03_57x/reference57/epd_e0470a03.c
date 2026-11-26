#include "platform/epd_render_profile.h"
/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <string.h>
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif
#include "board.h"
#include "drv_io.h"
#include "drv_lcd.h"
#include "mem_section.h"
#include "epd_memory.h"
#include "epd_e0470a03_timing.h"
#include "epd_tps.h"
#include "epd_waveform.h"
#include "gyd7n6/reference_waveform.h"
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
#define EPD_STATS_APP_LUT 17
#define EPD_STATS_APP_GC 18
#define APP_ERASE_POLARITY_FRAMES 8u
#define APP_ERASE_FIRST 0x0fu /* black to white */
#define APP_ERASE_LAST  0x8fu /* dark gray to white */
L2_RET_BSS_SECT_BEGIN(epd_app_lut)
static const uint8_t *app_lut L2_RET_BSS_SECT(epd_app_lut);
static uint8_t staged_lut_count L2_RET_BSS_SECT(epd_app_lut);
static epd_frame_transition_t frame_transition L2_RET_BSS_SECT(epd_app_lut);
static bool app_lut_active L2_RET_BSS_SECT(epd_app_lut);
L2_RET_BSS_SECT_END

void epd_display_stage_frame(const uint8_t *phases, uint8_t count, epd_frame_transition_t transition)
{
    app_lut = phases;
    staged_lut_count = count;
    frame_transition = transition;
}
static LCDC_InitTypeDef lcdc_config;
L2_NON_RET_BSS_SECT_BEGIN(reference57_pixels)
L2_NON_RET_BSS_SECT(reference57_pixels, ALIGN(64) static uint8_t mixed[PANEL_WIDTH * PANEL_HEIGHT]);
L2_NON_RET_BSS_SECT(reference57_pixels, static epd_gray_dirty_t frame_dirty);
L2_NON_RET_BSS_SECT(reference57_pixels, static epd_gray_dirty_t sync_dirty);
L2_NON_RET_BSS_SECT(reference57_pixels, static epd_gray_dirty_t prepared_dirty);
L2_NON_RET_BSS_SECT(reference57_pixels, static epd_gray_dirty_t coverage_dirty);
L2_NON_RET_BSS_SECT(reference57_pixels, static uint8_t frame_coverage[EPD_GRAY_COVERAGE_BYTES]);
L2_NON_RET_BSS_SECT_END
static uint32_t lut[HAL_LCDC_LOOKUP_TABLE_SIZE >> 2];
static struct rt_semaphore finished;
static rt_thread_t finish_thread;
static LCDC_HandleTypeDef *active_lcdc;
static void (*sdk_complete)(LCDC_HandleTypeDef *);
static void (*sdk_error)(LCDC_HandleTypeDef *);
static HAL_LCDC_PixelFormat input_format;
static uint32_t frame_count, next_frame;
static volatile bool drawing, powered, collecting, pending, finishing;
static volatile bool resume_pending;
static volatile bool frame_succeeded;
static bool initialized, failed;
/* Low nibbles retain the desired image even when a request fails. */
static bool shadow_known, scan_started, recovery_init, init_only;
static int32_t changed;
static uint8_t *prepared_pixels;
static rt_device_t prepared_lcd;
ALIGN(4) static uint16_t prepared_token;
static volatile bool prepared_busy, prepared_in_flight;
static EpdDrawMode prepared_mode;
static struct { const void *pixels; uint32_t stride; bool first, last; } chunk;
extern void BSP_EPD_BusIdle(void);

typedef struct {
    rt_tick_t prepare, poweron, scan, poweroff, commit, eof;
} request_timing_t;
typedef struct {
    int last_mode;
    uint32_t frame_count, elapsed_ms, input_pixels;
    uint32_t completed, input, gl, gc, init, other, errors;
    request_timing_t timing;
} panel_stats_t;
static panel_stats_t panel_stats = {.last_mode = -1};
static request_timing_t request_timing;
static rt_tick_t request_started, scan_started_at;
static uint32_t request_frames, request_input_pixels;
static bool scan_timing_active;

static uint32_t stats_ms(rt_tick_t ticks)
{
    return (uint32_t)((uint64_t)ticks * 1000u / RT_TICK_PER_SECOND);
}

/* Count committed requests, including all scans of INIT + GC recovery.
 * Unchanged requests do not drive the panel and do not enter these counters. */
static void stats_finish_request(bool success)
{
    int mode = app_lut_active ? EPD_STATS_APP_LUT : ref57_wave_get_selected_mode();
    if (frame_transition == EPD_FRAME_APP_SWITCH) mode = EPD_STATS_APP_GC;
    rt_tick_t elapsed = rt_tick_get() - request_started;
    uint32_t elapsed_ms = stats_ms(elapsed);
    rt_base_t level = rt_hw_interrupt_disable();
    if (success)
    {
        panel_stats.last_mode = mode;
        panel_stats.frame_count = request_frames;
        panel_stats.elapsed_ms = elapsed_ms;
        panel_stats.input_pixels = request_input_pixels;
        panel_stats.timing = request_timing;
        ++panel_stats.completed;
        if (mode == 1) ++panel_stats.input;
        else if (mode == EPD_WAVE_GL16_FAST || mode == 5) ++panel_stats.gl;
        else if (mode == 0 || mode == EPD_STATS_APP_GC) ++panel_stats.gc;
        else if (mode == 3) ++panel_stats.init;
        else ++panel_stats.other;
    }
    else ++panel_stats.errors;
    rt_hw_interrupt_enable(level);
}

#ifdef RT_USING_FINSH
static const char *stats_mode_name(int mode)
{
    switch (mode)
    {
    case -1: return "NONE";
    case 0: return "GC";
    case 1: return "INPUT_DU";
    case 3: return "INIT";
    case EPD_WAVE_GL16_FAST: return "GL_FAST";
    case 5: return "GL";
    case EPD_WAVE_TEXT: return "TEXT";
    case EPD_STATS_APP_LUT: return "APP_LUT";
    case EPD_STATS_APP_GC: return "APP_GC";
    default: return "OTHER";
    }
}

static void epd_stats(int argc, char **argv)
{
    bool reset = argc == 2 && !strcmp(argv[1], "reset");
    if (argc != 1 && !reset)
    {
        rt_kprintf("usage: epd_stats [reset]\n");
        return;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    if (reset)
    {
        /* In-flight requests count when they finish, even after a reset. */
        memset(&panel_stats, 0, sizeof(panel_stats));
        panel_stats.last_mode = -1;
    }
    panel_stats_t snapshot = panel_stats;
    rt_hw_interrupt_enable(level);
    rt_kprintf("epd_stats last_mode=%s mode=%d frame_count=%lu elapsed_ms=%lu\n",
               stats_mode_name(snapshot.last_mode), snapshot.last_mode,
               (unsigned long)snapshot.frame_count, (unsigned long)snapshot.elapsed_ms);
    rt_kprintf("epd_stats completed=%lu input=%lu gl=%lu gc=%lu init=%lu other=%lu errors=%lu\n",
               (unsigned long)snapshot.completed, (unsigned long)snapshot.input,
               (unsigned long)snapshot.gl, (unsigned long)snapshot.gc,
               (unsigned long)snapshot.init, (unsigned long)snapshot.other,
               (unsigned long)snapshot.errors);
    rt_kprintf("epd_stats prepare_ms=%lu poweron_ms=%lu scan_ms=%lu poweroff_ms=%lu commit_ms=%lu eof_ms=%lu input_pixels=%lu\n",
               (unsigned long)stats_ms(snapshot.timing.prepare),
               (unsigned long)stats_ms(snapshot.timing.poweron),
               (unsigned long)stats_ms(snapshot.timing.scan),
               (unsigned long)stats_ms(snapshot.timing.poweroff),
               (unsigned long)stats_ms(snapshot.timing.commit),
               (unsigned long)stats_ms(snapshot.timing.eof),
               (unsigned long)snapshot.input_pixels);
}
MSH_CMD_EXPORT(epd_stats, read or reset panel completion statistics);
#endif

static void panel_quiesce(void)
{
    epd_e0470a03_timing_stop();
    /* Stop panel drive even when the next scan reuses the powered rails. */
    HAL_PIN_Set(PAD_PA26, GPIO_A26, PIN_NOPULL, 1);
    BSP_GPIO_Set(26, 0, 1);
    BSP_LCD_GMODE_Set(0);
    HAL_Delay_us(20);
}

static void panel_enable_oe(void)
{
    GPIO_InitTypeDef oe = {.Pin = 26, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_NOPULL};
    HAL_GPIO_Init(hwp_gpio1, &oe);
    HAL_PIN_Set(PAD_PA26, LCDC1_TCON_SDOE, PIN_NOPULL, 1);
}

static void finish_power_off(rt_tick_t vcom_disabled_at)
{
    tps_finish_powerdown(vcom_disabled_at);
    BSP_EPD_BusIdle();
    epd_e0470a03_timing_release();
    powered = false;
#ifdef RT_USING_PM
    rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
}

static void power_off(void)
{
    if (!powered) return;
    panel_quiesce();
    finish_power_off(tps_disable_vcom());
}

static int start_power_on(LCDC_HandleTypeDef *lcdc)
{
    if (powered)
    {
        /* A successful INIT keeps the TCON claim and rails for GC16. */
        if (!tps_rails_good()) return -RT_ERROR;
        return RT_EOK;
    }
#ifdef RT_USING_PM
    rt_pm_request(PM_SLEEP_MODE_IDLE);
#endif
    BSP_LCD_PowerUp();
    /* SDK DEEP sleep stops the clocks without issuing LCD CTRL_RESUME.
     * Restore the TCON before enabling panel power. */
    if (resume_pending)
    {
        if (HAL_LCDC_Resume(lcdc) != HAL_OK) goto fail;
        HAL_LCDC_SetROIArea(lcdc, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
        resume_pending = false;
    }
    panel_enable_oe();
    if (!epd_e0470a03_timing_prepare(lcdc)) goto fail;
    if (tps_start_powerup() != RT_EOK)
    {
        panel_quiesce();
        tps_enter_sleep();
        goto fail;
    }
    powered = true; /* Also owns a started ramp until failure cleanup. */
    return RT_EOK;

fail:
    BSP_EPD_BusIdle();
    epd_e0470a03_timing_release();
#ifdef RT_USING_PM
    rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
    return -RT_ERROR;
}

static void launch_scan(LCDC_HandleTypeDef *lcdc, int frames);

static void commit_pixels(void)
{
    unsigned next = 0, offset, length;
    while (epd_gray_next_span(&frame_dirty, &next, &offset, &length))
        epd_gray_settle_span(mixed + offset, length);
    for (unsigned i = 0; i < EPD_GRAY_DIRTY_WORDS; ++i)
        sync_dirty.blocks[i] |= frame_dirty.blocks[i];
    memset(&frame_dirty, 0, sizeof(frame_dirty));
}

/* Finish the scan and power-down before releasing the SDK LCD request. */
static void finish_request(void)
{
    LCDC_HandleTypeDef *lcdc = active_lcdc;
    rt_tick_t started = rt_tick_get();
    if (!failed && recovery_init)
    {
        panel_quiesce();
        /* INIT establishes white independently of the old image. Retain every
         * desired target for GC16 without another TPS power cycle. */
        for (size_t i = 0; i < sizeof(mixed); ++i) mixed[i] |= 0xf0u;
        memset(&frame_dirty, 0xff, sizeof(frame_dirty));
        shadow_known = true;
        recovery_init = scan_started = false;
        finishing = false;
        int frames = ref57_wave_prepare_mode(0) == 0 ? (int)ref57_wave_get_frames() : -RT_ERROR;
        request_timing.prepare += rt_tick_get() - started;
        launch_scan(lcdc, frames);
        return;
    }
    rt_tick_t vcom_disabled_at = 0;
    if (powered)
    {
        panel_quiesce();
        vcom_disabled_at = tps_disable_vcom();
        request_timing.poweroff += vcom_disabled_at - started;
    }
    started = rt_tick_get();
    if (!failed)
    {
        if (init_only)
        {
            memset(mixed, 0xff, sizeof(mixed));
            memset(&frame_dirty, 0xff, sizeof(frame_dirty));
        }
        commit_pixels();
        shadow_known = true;
    }
    else if (scan_started) shadow_known = false;
    rt_tick_t settled = rt_tick_get();
    request_timing.commit += settled - started;
    if (powered)
    {
        finish_power_off(vcom_disabled_at);
        request_timing.poweroff += rt_tick_get() - settled;
    }
    started = rt_tick_get();
    epd_wave_finish_frame(mixed, sizeof(mixed), !failed);
    HAL_LCDC_LayerSetFormat(lcdc, HAL_LCDC_LAYER_DEFAULT, input_format);
    void (*callback)(LCDC_HandleTypeDef *) = sdk_complete;
    sdk_complete = RT_NULL;
    lcdc->XferCpltCallback = callback;
    lcdc->XferErrorCallback = sdk_error;
    frame_succeeded = !failed;
    request_timing.commit += rt_tick_get() - started;
    stats_finish_request(frame_succeeded);
    frame_transition = EPD_FRAME_NORMAL;
    drawing = finishing = false;
    if (callback) callback(lcdc);
#ifdef RT_USING_PM
    epd_power_notify();
#endif
}

static void finish_entry(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        rt_sem_take(&finished, RT_WAITING_FOREVER);
        finish_request();
    }
}

static void finish_scan(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool signal = !finishing;
    finishing = true;
    if (signal && scan_timing_active)
    {
        rt_tick_t now = rt_tick_get();
        request_timing.scan += now - scan_started_at;
        /* Offset from request start, not another additive phase. Recovery
         * replaces the INIT endpoint with the final successful GC endpoint. */
        if (!failed) request_timing.eof = now - request_started;
        scan_timing_active = false;
    }
    rt_hw_interrupt_enable(level);
    if (signal) rt_sem_release(&finished);
}

/* Extend the erase only over existing HOLD slots, preserving the net drive. */
static bool app_erase_fits(void)
{
    for (unsigned frame = 0; frame < 2u * APP_ERASE_POLARITY_FRAMES; ++frame)
    {
        epd_wave_table_fill_lut(lut, frame);
        for (unsigned i = APP_ERASE_FIRST; i <= APP_ERASE_LAST; i += 16u)
            if (lut[i] & 3u) return false;
    }
    return true;
}

static bool start_frame(LCDC_HandleTypeDef *lcdc, uint32_t index)
{
    if (index >= frame_count) return false;
    if (app_lut_active)
    {
        const uint8_t *phase = app_lut + index * EPD_LUT_PHASE_BYTES;
        for (unsigned i = 0; i < 256; ++i)
            lut[i] = 0xff000000u | ((phase[i / 4u] >> (2u * (i % 4u))) & 3u);
    }
    else
    {
        epd_wave_table_fill_lut(lut, index);
        if (frame_transition == EPD_FRAME_APP_SWITCH && index < 2u * APP_ERASE_POLARITY_FRAMES)
        {
            uint32_t drive = index < APP_ERASE_POLARITY_FRAMES ? 1u : 2u;
            for (unsigned i = APP_ERASE_FIRST; i <= APP_ERASE_LAST; i += 16u)
                lut[i] |= drive;
        }
    }
    BSP_LCD_GMODE_Set(0);
    HAL_Delay_us(1);
    BSP_LCD_GMODE_Set(1);
    if (!epd_e0470a03_timing_arm()) { failed = true; return false; }
    if (HAL_LCDC_SendLayerData_IT(lcdc) == HAL_OK) return true;
    failed = true;
    LOG_E("frame %u failed", index);
    return false;
}

static void frame_complete(LCDC_HandleTypeDef *lcdc)
{
    bool timing_ok = epd_e0470a03_timing_complete();
    /* The HAL records ICB overflow but still dispatches the EOF callback. */
    if (!timing_ok || (lcdc->ErrorCode & HAL_LCDC_ERROR_OVERFLOW))
    {
        failed = true;
        finish_scan();
        return;
    }
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
    bool idle = !drawing && !powered && !collecting && !chunk.pixels && !pending &&
                !prepared_in_flight;
    rt_hw_interrupt_enable(level);
    return idle;
}

bool epd_display_prepare_init(void)
{
    prepared_lcd = rt_device_find("lcd");
    if (!prepared_lcd) return false;
    prepared_pixels = epd_app_alloc(EPD_GRAY_PREPARE_BYTES, EPD_APP_PSRAM);
    if (!prepared_pixels) return false;
    memset(prepared_pixels, 0, EPD_GRAY_PREPARE_BYTES);
    return true;
}

bool epd_display_can_prepare(void) { return prepared_pixels && !prepared_busy; }
bool epd_display_prepared_in_flight(void) { return prepared_in_flight; }

EPD_GRAY_HOT void epd_display_prepare_chunk(const uint8_t *pixels, uint32_t stride,
                               uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    RT_ASSERT(epd_display_can_prepare() && x0 <= x1 && y0 <= y1 &&
              x1 < PANEL_HEIGHT && y1 < PANEL_WIDTH);
    for (unsigned x = x0; x <= x1; ++x)
    {
        unsigned offset = (PANEL_HEIGHT - 1u - x) * PANEL_WIDTH;
        unsigned last = (offset + y1) / EPD_GRAY_BLOCK_BYTES;
        for (unsigned block = (offset + y0) / EPD_GRAY_BLOCK_BYTES; block <= last; ++block)
            epd_gray_mark(&prepared_dirty, block * EPD_GRAY_BLOCK_BYTES);
    }
    if (!(y0 & 7u) && !((y1 - y0 + 1u) & 7u))
    {
        const epd_gray_area_t area = {x0, y0, x1, y1};
        epd_gray_prepare_rgb565_packed(prepared_pixels, pixels, stride, &area);
        return;
    }
    uint8_t tile[EPD_GRAY_TILE_W][EPD_GRAY_TILE_H];
    for (unsigned y = y0; y <= y1; y += EPD_GRAY_TILE_H)
    {
        unsigned height = y1 - y + 1u;
        if (height > EPD_GRAY_TILE_H) height = EPD_GRAY_TILE_H;
        for (unsigned x = x0; x <= x1; x += EPD_GRAY_TILE_W)
        {
            unsigned width = x1 - x + 1u;
            if (width > EPD_GRAY_TILE_W) width = EPD_GRAY_TILE_W;
            epd_gray_rgb565_tile(pixels + (y - y0) * stride + (x - x0) * 2u,
                                 stride, width, height, tile);
            for (unsigned col = 0; col < width; ++col)
            {
                unsigned offset = (PANEL_HEIGHT - 1u - x - col) * PANEL_WIDTH + y;
                epd_gray_prepare_span(prepared_pixels, offset, tile[col], height);
            }
        }
    }
}

static EPD_GRAY_HOT void consume_prepared(bool apply)
{
    unsigned next = 0, offset, length;
    if (apply) coverage_dirty = prepared_dirty;
    while (epd_gray_next_span(&prepared_dirty, &next, &offset, &length))
    {
        uint8_t *coverage = prepared_pixels + EPD_GRAY_BYTES / 2u + offset / 8u;
        if (apply)
        {
            changed += epd_gray_apply_prepared(mixed, &frame_dirty, prepared_pixels, offset, length);
            memcpy(frame_coverage + offset / 8u, coverage, length / 8u);
        }
        memset(coverage, 0, length / 8u);
    }
    memset(&prepared_dirty, 0, sizeof(prepared_dirty));
}

void epd_display_discard_prepared(void)
{
    RT_ASSERT(epd_display_can_prepare());
    consume_prepared(false);
}

static rt_err_t prepared_complete(rt_device_t lcd, void *pixels)
{
    (void)lcd;
    if (pixels == &prepared_token)
    {
        if (prepared_busy)
        {
            consume_prepared(false);
            prepared_busy = false;
        }
        prepared_in_flight = false;
    }
    return RT_EOK;
}

void epd_display_submit_prepared(EpdDrawMode mode)
{
    RT_ASSERT(epd_display_can_prepare() && !prepared_in_flight);
    prepared_mode = mode;
    prepared_busy = prepared_in_flight = true;
    frame_succeeded = false;
    rt_device_set_tx_complete(prepared_lcd, prepared_complete);
    const struct rt_device_graphic_ops *ops = rt_graphix_ops(prepared_lcd);
    /* The SDK serializes this request with its LCD controls. The board driver
     * recognizes the token and consumes the already prepared pixels. */
    ops->set_window(0, 0, 0, 0);
    ops->draw_rect_async((const char *)&prepared_token, 0, 0, 0, 0);
}

static void begin_pixels(void)
{
    frame_succeeded = false;
    uint32_t trace = epd_render_profile_begin(EPD_RENDER_SETTLE);
    /* A failed pre-scan request still has a trustworthy old image. Preserve
     * its targets and differences, including pixels outside the next chunk. */
    changed = 0;
    unsigned next = 0, offset, length;
    while (epd_gray_next_span(&frame_dirty, &next, &offset, &length))
        for (unsigned i = offset; i < offset + length; ++i)
            changed += (mixed[i] >> 4) != (mixed[i] & 15u);
    next = 0;
    while (epd_gray_next_span(&coverage_dirty, &next, &offset, &length))
        memset(frame_coverage + offset / 8u, 0, length / 8u);
    memset(&coverage_dirty, 0, sizeof(coverage_dirty));
    epd_render_profile_end(EPD_RENDER_SETTLE, trace);
}

static void sync_pixels(void)
{
    for (unsigned i = 0; i < EPD_GRAY_DIRTY_WORDS; ++i)
        sync_dirty.blocks[i] |= frame_dirty.blocks[i];
    unsigned next = 0, offset, length;
    while (epd_gray_next_span(&sync_dirty, &next, &offset, &length))
        mpu_dcache_clean(mixed + offset, length);
    memset(&sync_dirty, 0, sizeof(sync_dirty));
}

static void copy_pixels(const uint8_t *pixels, uint32_t stride,
                        const epd_gray_area_t *area, HAL_LCDC_PixelFormat format)
{
    for (unsigned x = area->x0; x <= area->x1; ++x)
        epd_gray_cover_span(frame_coverage, &coverage_dirty,
            (PANEL_HEIGHT - 1u - x) * PANEL_WIDTH + area->y0, area->y1 - area->y0 + 1u);
    if (format == LCDC_PIXEL_FORMAT_RGB565)
    {
        uint8_t tile[EPD_GRAY_TILE_W][EPD_GRAY_TILE_H];
        changed += epd_gray_copy_rgb565(mixed, &frame_dirty, pixels, stride, area, area, tile);
    }
    else
    {
        RT_ASSERT(format == LCDC_PIXEL_FORMAT_A4);
        for (uint32_t y = area->y0; y <= area->y1; ++y)
            for (uint32_t x = area->x0; x <= area->x1; ++x)
            {
                uint32_t sx = x - area->x0;
                uint8_t value = (pixels[(y - area->y0) * stride + sx / 2] >> ((sx & 1) * 4)) & 15;
                changed += epd_gray_store(mixed, &frame_dirty,
                                          (PANEL_HEIGHT - 1u - x) * PANEL_WIDTH + y, value);
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
    lcdc_config = epd_e0470a03_gyd7n6_config;
    lcdc->Init = lcdc_config;
    HAL_LCDC_Init(lcdc);
    HAL_LCDC_SetROIArea(lcdc, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
    resume_pending = false;
    memset(mixed, 0xff, sizeof(mixed));
    memset(&frame_dirty, 0, sizeof(frame_dirty));
    memset(&sync_dirty, 0xff, sizeof(sync_dirty));
    memset(&coverage_dirty, 0, sizeof(coverage_dirty));
    memset(frame_coverage, 0, sizeof(frame_coverage));
    memset(&chunk, 0, sizeof(chunk));
    collecting = drawing = pending = finishing = false;
    frame_succeeded = false;
    shadow_known = scan_started = recovery_init = init_only = false;
    app_lut = NULL;
    staged_lut_count = 0;
    frame_transition = EPD_FRAME_NORMAL;
    app_lut_active = false;
    epd_wave_table();
    initialized = tps_init(EPD_VCOM_MV) == RT_EOK;
    tps_enter_sleep();
    BSP_EPD_BusIdle();
}

static void LCD_WriteMultiplePixels(LCDC_HandleTypeDef *lcdc, const uint8_t *pixels,
                                    uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const epd_gray_area_t area = {x0, y0, x1, y1};
    bool is_prepared = pixels == (const uint8_t *)&prepared_token;
    bool is_chunk = chunk.pixels == pixels;
    bool first = !is_chunk || chunk.first;
    bool last = !is_chunk || chunk.last;
    uint32_t stride = is_chunk ? chunk.stride :
        (lcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_format == LCDC_PIXEL_FORMAT_A4 ?
         (x1 - x0 + 2u) / 2u : (x1 - x0 + 1u) * 2u);
    RT_ASSERT(!drawing && x0 <= x1 && y0 <= y1 && x1 < PANEL_HEIGHT && y1 < PANEL_WIDTH);
    if (first)
    {
        RT_ASSERT(!collecting);
        if (is_prepared) epd_wave_submit_frame(prepared_mode);
        epd_wave_begin_frame();
        begin_pixels();
        collecting = true;
    }
    RT_ASSERT(collecting);
    if (is_chunk) chunk.pixels = RT_NULL;
    uint32_t trace = epd_render_profile_begin(EPD_RENDER_COPY);
    if (is_prepared)
    {
        consume_prepared(true);
        prepared_busy = false;
    }
    else copy_pixels(pixels, stride, &area, lcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_format);
    epd_render_profile_end(EPD_RENDER_COPY, trace);
    if (!last)
    {
        if (lcdc->XferCpltCallback) lcdc->XferCpltCallback(lcdc);
        return;
    }
    if (!shadow_known && epd_wave_get_mode() != 3) epd_wave_request_full();
    request_started = rt_tick_get();
    request_frames = request_input_pixels = 0;
    memset(&request_timing, 0, sizeof(request_timing));
    scan_timing_active = false;
    if (epd_wave_binary_target())
    {
        /* frame_dirty also retains targets from failed requests. Quantization
         * must cover those spans without replacing their physical source. */
        changed = 0;
        unsigned next = 0, offset, length;
        while (epd_gray_next_span(&frame_dirty, &next, &offset, &length))
        {
            changed |= epd_wave_quantize_input(mixed + offset, length);
            request_input_pixels += length;
        }
    }
    int frames = epd_wave_prepare_frame(changed != 0);
    int mode = ref57_wave_get_selected_mode();
    /* Full refreshes, input waveforms and unknown-shadow recovery remain
     * driven by the calibrated system table. */
    app_lut_active = staged_lut_count && frames > 0 && shadow_known &&
        (mode == EPD_WAVE_GL16_FAST || mode == 5);
    if (app_lut_active) frames = (int)staged_lut_count;
    staged_lut_count = 0;
    bool clear_first = frame_transition == EPD_FRAME_CLEAR_FIRST;
    if (frame_transition != EPD_FRAME_APP_SWITCH || !shadow_known || mode != 0 || frames != 44 || !app_erase_fits())
        frame_transition = EPD_FRAME_NORMAL;
    if (frames == 0)
    {
        collecting = false;
        commit_pixels();
        frame_succeeded = true;
        if (lcdc->XferCpltCallback) lcdc->XferCpltCallback(lcdc);
        return;
    }
    active_lcdc = lcdc;
    sdk_complete = lcdc->XferCpltCallback;
    sdk_error = lcdc->XferErrorCallback;
    input_format = lcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_format;
    drawing = true;
    collecting = false;
    failed = scan_started = recovery_init = false;
    init_only = !clear_first && frames > 0 && mode == 3;
    if (frames > 0 && (clear_first || (!shadow_known && !init_only)))
    {
        recovery_init = true;
        frames = ref57_wave_prepare_mode(3) == 0 ? (int)ref57_wave_get_frames() : -RT_ERROR;
    }
    request_timing.prepare += rt_tick_get() - request_started;
    launch_scan(lcdc, frames);
}

static void launch_scan(LCDC_HandleTypeDef *lcdc, int frames)
{
    if (!initialized || frames <= 0)
    {
        failed = true;
        finish_scan();
        return;
    }
    bool cold = !powered;
    rt_tick_t started = rt_tick_get();
    int result = start_power_on(lcdc);
    request_timing.poweron += rt_tick_get() - started;
    if (result != RT_EOK) goto fail;

    const uint8_t *indices = app_lut_active ? NULL : epd_wave_gray_repaint_indices();
    if (!indices)
    {
        /* Write back ordinary transition bytes while PWRUP is ramping. */
        started = rt_tick_get();
        sync_pixels();
        request_timing.prepare += rt_tick_get() - started;
    }
    if (cold)
    {
        started = rt_tick_get();
        result = tps_wait_powerup();
        request_timing.poweron += rt_tick_get() - started;
        if (result != RT_EOK) goto fail;
    }

    started = rt_tick_get();
    frame_count = (uint32_t)frames;
    request_frames += frame_count;
    /* Repaint rewrites the source nibbles. Do not do it until power-good,
     * so a pre-scan power failure preserves the known physical image. */
    scan_started = true;
    if (indices)
    {
        unsigned next = 0, offset, length;
        while (epd_gray_next_span(&coverage_dirty, &next, &offset, &length))
            epd_gray_repaint_span(mixed, &frame_dirty, frame_coverage, indices, offset, length);
        sync_pixels();
    }
    if (!cold) panel_enable_oe();
    HAL_LCDC_LayerSetData(lcdc, HAL_LCDC_LAYER_DEFAULT, mixed, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
    HAL_LCDC_LayerSetFormat(lcdc, HAL_LCDC_LAYER_DEFAULT, LCDC_PIXEL_FORMAT_L8);
    HAL_LCDC_LayerSetLTab(lcdc, HAL_LCDC_LAYER_DEFAULT, (LCDC_AColorDef *)lut);
    lcdc->XferCpltCallback = frame_complete;
    lcdc->XferErrorCallback = frame_error;
    /* ErrorCode is sticky across transfers; this scan owns new overflow errors. */
    lcdc->ErrorCode &= ~HAL_LCDC_ERROR_OVERFLOW;
    next_frame = 1;
    scan_started_at = rt_tick_get();
    request_timing.prepare += scan_started_at - started;
    scan_timing_active = true;
    if (!start_frame(lcdc, 0)) finish_scan();
    return;

fail:
    failed = true;
    finish_scan();
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
