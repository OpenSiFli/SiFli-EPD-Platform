/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dfu_display.h"
#include "dfu_board.h"
#include "dfu_font.h"
#include "dfu_waveform_data.h"

#include "register.h"
#include "drv_io.h"
#include "system_bf0_ap.h"
#include <stdio.h>
#include <string.h>

extern void BSP_EPD_BusIdle(void);

enum
{
    PANEL_WIDTH = DFU_DISPLAY_HEIGHT,
    PANEL_HEIGHT = DFU_DISPLAY_WIDTH,
    PANEL_HZ = 48000000,
    LCDC_PALETTE_OFFSET = 0x400,
    PANEL_OE = 26,
    TPS_WAKEUP = 42,
    TPS_PWRCOM = 43,
    TPS_PWRUP = 44,
    TPS_PGOOD = 52,
    TPS_ADDR = 0x68,
    TPS_VADJ = 0x02,
    TPS_VCOM1 = 0x03,
    TPS_VCOM2 = 0x04,
    TPS_UPSEQ0 = 0x09,
    TPS_UPSEQ1 = 0x0a,
    TPS_DWNSEQ0 = 0x0b,
    TPS_DWNSEQ1 = 0x0c,
    TPS_TMST_VALUE = 0x00,
    TPS_TMST1 = 0x0d,
    TPS_READ_THERM = 0x80,
    TPS_CONV_END = 0x20,
    TPS_PG = 0x0f,
    TPS_REVID = 0x10,
    TPS_ALL_RAILS = 0xfa,
    TPS_IO_TIMEOUT_MS = 20,
    TPS_POWER_TIMEOUT_MS = 500,
    FRAME_TIMEOUT_MS = 100,
    FULL_REFRESH_INTERVAL = 24,
    FONT_SCALE = DFU_DISPLAY_CHAR_WIDTH / 8,
    PIXEL_LOW_BITS = 0x01010101u,
};

#ifndef EPD_VCOM_MV
#define EPD_VCOM_MV 2100
#endif

/* Two bits encode old and new monochrome pixels in the LCDC L8 index. */
_Static_assert(PANEL_WIDTH % sizeof(uint32_t) == 0, "Panel rows must be word aligned");
_Alignas(32) static uint32_t pixels[PANEL_HEIGHT][PANEL_WIDTH / sizeof(uint32_t)];
static uint32_t lut[HAL_LCDC_LOOKUP_TABLE_SIZE / sizeof(uint32_t)];
static LCDC_HandleTypeDef lcdc;
static I2C_HandleTypeDef i2c;
static bool initialized;
static bool init_needed;
static unsigned partial_refreshes;
static uint32_t pending_pixels;
static struct
{
    unsigned row_start, row_end;
    unsigned col_start, col_end;
} dirty = {PANEL_HEIGHT, 0, PANEL_WIDTH, 0};
static int temperature = 28;

extern void *__real_memcpy(void *destination, const void *source, size_t size);

void *__wrap_memcpy(void *destination, const void *source, size_t size)
{
    if ((uintptr_t)destination != LCDC1_BASE + LCDC_PALETTE_OFFSET ||
        size != HAL_LCDC_LOOKUP_TABLE_SIZE)
        return __real_memcpy(destination, source, size);

    /* The LCDC palette requires aligned 32-bit writes. */
    volatile uint32_t *palette = destination;
    const uint32_t *entries = source;
    for (unsigned i = 0; i < HAL_LCDC_LOOKUP_TABLE_SIZE / sizeof(*entries); ++i)
        palette[i] = entries[i];
    return destination;
}

static void gpio_write(uint8_t pin, bool high)
{
    HAL_GPIO_WritePin(hwp_gpio1, pin, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void gpio_low(uint8_t pin)
{
    GPIO_InitTypeDef output = {.Pin = pin, .Mode = GPIO_MODE_OUTPUT, .Pull = GPIO_NOPULL};
    HAL_GPIO_Init(hwp_gpio1, &output);
    gpio_write(pin, false);
    HAL_PIN_Set(PAD_PA00 + pin, GPIO_A0 + pin, PIN_NOPULL, 1);
    HAL_PIN_SetMode(PAD_PA00 + pin, 1, PIN_DIGITAL_O_NORMAL);
}

static void panel_delay(uint32_t ms, void (*service)(void))
{
    if (!service)
    {
        HAL_Delay(ms);
        return;
    }
    uint32_t start = HAL_GetTick();
    while (HAL_GetTick() - start < ms)
    {
        service();
        __WFI();
    }
}

static void panel_power_off(bool reset_controller, void (*service)(void))
{
    gpio_low(PANEL_OE);
    HAL_PIN_Set(PAD_PA39, GPIO_A39, PIN_NOPULL, 1);
    BSP_LCD_GMODE_Set(0);
    if (reset_controller)
        HAL_RCC_ResetModule(RCC_MOD_LCDC1);
    HAL_Delay_us(20);
    gpio_low(TPS_PWRCOM);
    panel_delay(10, service);
    gpio_low(TPS_PWRUP);
    panel_delay(150, service);
    gpio_low(TPS_WAKEUP);
    HAL_PIN_Set(PAD_PA00 + TPS_PGOOD, GPIO_A0 + TPS_PGOOD, PIN_NOPULL, 1);
    HAL_PIN_SetMode(PAD_PA00 + TPS_PGOOD, 1, PIN_ANALOG_INPUT);
    BSP_EPD_BusIdle();
}

static bool tps_read(uint8_t reg, uint8_t *value)
{
    return HAL_I2C_Master_Transmit(&i2c, TPS_ADDR, &reg, 1, TPS_IO_TIMEOUT_MS) == HAL_OK &&
           HAL_I2C_Master_Receive(&i2c, TPS_ADDR, value, 1, TPS_IO_TIMEOUT_MS) == HAL_OK;
}

static bool tps_write(uint8_t reg, uint8_t value)
{
    uint8_t pair[] = {reg, value};
    return HAL_I2C_Master_Transmit(&i2c, TPS_ADDR, pair, sizeof(pair), TPS_IO_TIMEOUT_MS) == HAL_OK;
}

static void tps_sample_temperature(void)
{
    if (!tps_write(TPS_TMST1, TPS_READ_THERM))
        return;
    uint32_t start = HAL_GetTick();
    do
    {
        uint8_t status, value;
        if (!tps_read(TPS_TMST1, &status))
            return;
        if (status & TPS_CONV_END)
        {
            if (tps_read(TPS_TMST_VALUE, &value))
                temperature = (int8_t)value;
            return;
        }
        HAL_Delay(1);
    } while (HAL_GetTick() - start < 30);
}

static bool panel_power_on(void (*service)(void))
{
    static const uint8_t settings[][2] = {
        {TPS_UPSEQ0, 0xb1}, {TPS_UPSEQ1, 0x00},
        {TPS_DWNSEQ0, 0x4e}, {TPS_DWNSEQ1, 0x84},
        {TPS_VADJ, 0x03},
        {TPS_VCOM1, (EPD_VCOM_MV / 10) & 0xff},
        {TPS_VCOM2, (EPD_VCOM_MV / 10) >> 8},
    };
    BSP_LCD_PowerUp();
    GPIO_InitTypeDef pgood = {.Pin = TPS_PGOOD, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_NOPULL};
    HAL_GPIO_Init(hwp_gpio1, &pgood);
    HAL_PIN_Set(PAD_PA00 + TPS_PGOOD, GPIO_A0 + TPS_PGOOD, PIN_PULLUP, 1);
    HAL_PIN_SetMode(PAD_PA00 + TPS_PGOOD, 1, PIN_DIGITAL_IO_PULLUP);
    gpio_write(TPS_WAKEUP, true);
    panel_delay(10, service);

    uint8_t id;
    if (!tps_read(TPS_REVID, &id) || (id & 0x0f) != 5)
    {
        dfu_log("DFU display: TPS identification failed\r\n");
        return false;
    }
    /* WAKEUP resets these volatile settings. */
    for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i)
    {
        if (service)
            service();
        uint8_t actual;
        if (!tps_write(settings[i][0], settings[i][1]) ||
            !tps_read(settings[i][0], &actual) || actual != settings[i][1])
        {
            dfu_log("DFU display: TPS configuration failed\r\n");
            return false;
        }
    }
    tps_sample_temperature();

    gpio_write(TPS_PWRUP, true);
    uint32_t start = HAL_GetTick();
    do
    {
        uint8_t rails;
        if (!tps_read(TPS_PG, &rails))
        {
            dfu_log("DFU display: TPS power status failed\r\n");
            return false;
        }
        if ((rails & TPS_ALL_RAILS) == TPS_ALL_RAILS &&
            HAL_GPIO_ReadPin(hwp_gpio1, TPS_PGOOD) == GPIO_PIN_SET)
        {
            gpio_write(TPS_PWRCOM, true);
            panel_delay(10, service);
            return true;
        }
        panel_delay(5, service);
    } while (HAL_GetTick() - start < TPS_POWER_TIMEOUT_MS);
    dfu_log("DFU display: panel power timeout\r\n");
    return false;
}

static bool lcdc_init(void)
{
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_LCDC, RCC_CLK_LCDC_SYSCLK);
    uint32_t clock = HAL_RCC_GetModuleFreq(RCC_MOD_LCDC1);
    if (clock < 2 * PANEL_HZ || clock % PANEL_HZ != 0)
    {
        dfu_log("DFU display: panel clock unavailable\r\n");
        return false;
    }

    lcdc.Instance = hwp_lcdc1;
    lcdc.Init = (LCDC_InitTypeDef){
        .lcd_itf = LCDC_INTF_EPD_8BIT,
        .freq = PANEL_HZ,
        .color_mode = LCDC_PIXEL_FORMAT_F2_SWAP,
        .cfg = {.epd = {
            .SDMODE = 0, .SDCLK_polarity = 0, .GDSP_polarity = 0, .GDCLK_polarity = 1,
            .LSL = 20, .LBL = 48, .LEL = 24, .LDL = PANEL_WIDTH / 4,
            .GSTA = 3, .FSL = 1, .FBL = 3, .FDL = PANEL_HEIGHT, .FEL = 1,
        }},
    };
    NVIC_DisableIRQ(LCDC1_IRQn);
    NVIC_ClearPendingIRQ(LCDC1_IRQn);
    const HAL_LCDC_LayerDef layer = HAL_LCDC_LAYER_DEFAULT;
    bool ok = HAL_LCDC_Init(&lcdc) == HAL_OK &&
           HAL_LCDC_SetROIArea(&lcdc, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1) == HAL_OK &&
           HAL_LCDC_LayerReset(&lcdc, layer) == HAL_OK &&
           HAL_LCDC_LayerSetData(&lcdc, layer, (const uint8_t *)pixels, 0, 0,
                                 PANEL_WIDTH - 1, PANEL_HEIGHT - 1) == HAL_OK &&
           HAL_LCDC_LayerSetFormat(&lcdc, layer, LCDC_PIXEL_FORMAT_L8) == HAL_OK &&
           HAL_LCDC_LayerSetLTab(&lcdc, layer, (const LCDC_AColorDef *)lut) == HAL_OK;
    if (!ok)
    {
        dfu_log("DFU display: LCDC setup failed\r\n");
        return false;
    }
    uint32_t config = lcdc.Instance->LCD_CONF;
    uint32_t intf = (config & LCD_IF_LCD_CONF_LCD_INTF_SEL) >> LCD_IF_LCD_CONF_LCD_INTF_SEL_Pos;
    uint32_t divider = (config & LCD_IF_LCD_CONF_INTF_CLK_DIV) >> LCD_IF_LCD_CONF_INTF_CLK_DIV_Pos;
    uint32_t tcon = lcdc.Instance->TCON_IF_CONF1 & LCD_IF_TCON_IF_CONF1_CLK_DIV;
    if (intf != 8 || tcon < 2 || !(lcdc.Instance->GREY_CONV & LCD_IF_GREY_CONV_ORDER) ||
        (lcdc.Instance->CONV_CFG & LCD_IF_CONV_CFG_FORMAT) != 6 ||
        ((config & LCD_IF_LCD_CONF_INTF_CLK_DIV2P5_SEL) ?
         2 * clock != 5 * tcon * PANEL_HZ : clock != divider * tcon * PANEL_HZ))
    {
        dfu_log("DFU display: invalid TCON configuration\r\n");
        return false;
    }
    return true;
}

static bool scan_frame(bool check_palette, void (*service)(void))
{
    if (HAL_GPIO_ReadPin(hwp_gpio1, TPS_PGOOD) != GPIO_PIN_SET)
    {
        dfu_log("DFU display: panel power lost\r\n");
        return false;
    }
    lcdc.Instance->IRQ = LCD_IF_IRQ_EOF_RAW_STAT | LCD_IF_IRQ_EOF_STAT |
                         LCD_IF_IRQ_ICB_OF_RAW_STAT | LCD_IF_IRQ_ICB_OF_STAT;
    if (lcdc.Instance->IRQ & LCD_IF_IRQ_EOF_STAT)
    {
        dfu_log("DFU display: stale frame status\r\n");
        return false;
    }
    BSP_LCD_GMODE_Set(0);
    HAL_Delay_us(1);
    BSP_LCD_GMODE_Set(1);
    if (HAL_LCDC_SendLayerData_IT(&lcdc) != HAL_OK)
    {
        dfu_log("DFU display: frame start failed\r\n");
        return false;
    }

    uint32_t start = HAL_GetTick();
    if (service)
        service();
    for (;;)
    {
        uint32_t irq = lcdc.Instance->IRQ;
        if (irq & LCD_IF_IRQ_ICB_OF_RAW_STAT)
        {
            dfu_log("DFU display: LCDC FIFO overflow\r\n");
            return false;
        }
        if (irq & LCD_IF_IRQ_EOF_STAT)
            break;
        if (HAL_GetTick() - start >= FRAME_TIMEOUT_MS)
        {
            dfu_log("DFU display: frame timeout\r\n");
            return false;
        }
    }
    HAL_LCDC_IRQHandler(&lcdc);
    NVIC_ClearPendingIRQ(LCDC1_IRQn);
    if (check_palette)
    {
        const volatile uint32_t *table =
            (const volatile uint32_t *)(LCDC1_BASE + LCDC_PALETTE_OFFSET);
        for (unsigned i = 0; i < 4; ++i)
            if (table[i] != lut[i])
            {
                dfu_log("DFU display: LCDC palette mismatch\r\n");
                return false;
            }
    }
    return lcdc.ErrorCode == HAL_LCDC_ERROR_NONE && lcdc.State == HAL_LCDC_STATE_READY;
}

static bool scan_wave(const dfu_wave_zone_t *zones, unsigned count, void (*service)(void))
{
    const dfu_wave_zone_t *zone = zones;
    for (unsigned i = 1; i < count && temperature >= zones[i].minimum; ++i)
        zone = &zones[i];

    uint32_t start = HAL_GetTick();
    unsigned frames = 0;
    mpu_dcache_clean(pixels, sizeof(pixels));
    for (unsigned i = 0; i < zone->count; ++i)
    {
        const dfu_wave_run_t run = dfu_wave_runs[zone->offset + i];
        for (unsigned slot = 0; slot < 4; ++slot)
            lut[slot] = 0xff000000u | ((run.codes >> (slot * 2)) & 3u);
        for (unsigned frame = 0; frame < run.frames; ++frame)
            if (!scan_frame(i == 0 && frame == 0, service))
                return false;
        frames += run.frames;
    }
    char message[80];
    snprintf(message, sizeof(message), "DFU display: %s %u frames in %lu ms\r\n",
             zones == dfu_wave_init ? "INIT" : "DU", frames, (unsigned long)(HAL_GetTick() - start));
    dfu_log(message);
    return true;
}

static void reset_dirty(void)
{
    dirty.row_start = PANEL_HEIGHT;
    dirty.row_end = 0;
    dirty.col_start = PANEL_WIDTH;
    dirty.col_end = 0;
}

static void mark_full_canvas_dirty(void)
{
    dirty.row_start = 0;
    dirty.row_end = PANEL_HEIGHT;
    dirty.col_start = 0;
    dirty.col_end = PANEL_WIDTH;
}

/* Each byte contains at most one flag, so multiplication sums four flags. */
static uint32_t count_pixel_flags(uint32_t flags)
{
    return (flags * PIXEL_LOW_BITS) >> 24;
}

static void update_old_pixels(bool white, void (*service)(void))
{
    if (white)
        mark_full_canvas_dirty();

    unsigned first_word = dirty.col_start / sizeof(uint32_t);
    unsigned end_word = (dirty.col_end + sizeof(uint32_t) - 1u) / sizeof(uint32_t);
    uint32_t pending = 0;
    for (unsigned row = dirty.row_start; row < dirty.row_end; ++row)
    {
        for (unsigned col = first_word; col < end_word; ++col)
        {
            uint32_t next = pixels[row][col] & PIXEL_LOW_BITS;
            uint32_t old = white ? PIXEL_LOW_BITS : next;
            pixels[row][col] = (old << 1) | next;
            pending += count_pixel_flags(old ^ next);
        }
        if (service)
            service();
    }
    pending_pixels = pending;
    if (!pending)
        reset_dirty();
}

int dfu_display_init(void)
{
    if (initialized)
        return 0;

    HAL_RCC_EnableModule(RCC_MOD_PINMUX1);
    HAL_RCC_EnableModule(RCC_MOD_GPIO1);
    panel_power_off(false, NULL);
    HAL_PIN_Set(PAD_PA20, I2C1_SCL, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA21, I2C1_SDA, PIN_PULLUP, 1);
    HAL_RCC_EnableModule(RCC_MOD_I2C1);
    HAL_RCC_ResetModule(RCC_MOD_I2C1);
    i2c.Instance = hwp_i2c1;
    i2c.Mode = HAL_I2C_MODE_MASTER;
    i2c.Init.ClockSpeed = 400000;
    i2c.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    if (HAL_I2C_Init(&i2c) != HAL_OK)
    {
        dfu_log("DFU display: I2C setup failed\r\n");
        return -1;
    }
    __HAL_I2C_ENABLE(&i2c);
    if (!lcdc_init())
        return -1;

    memset(pixels, 3, sizeof(pixels));
    initialized = true;
    init_needed = true;
    return 0;
}

void dfu_display_clear(void)
{
    if (!initialized)
        return;

    bool changed = false;
    for (unsigned row = 0; row < PANEL_HEIGHT; ++row)
    {
        for (unsigned col = 0; col < sizeof(pixels[row]) / sizeof(pixels[row][0]); ++col)
        {
            uint32_t pixel_word = pixels[row][col];
            uint32_t was_black = ~pixel_word & PIXEL_LOW_BITS;
            if (!was_black)
                continue;

            uint32_t erased_pending = count_pixel_flags(was_black & (pixel_word >> 1));
            pending_pixels += count_pixel_flags(was_black);
            pending_pixels -= 2 * erased_pending;
            pixels[row][col] = pixel_word | PIXEL_LOW_BITS;
            changed = true;
        }
    }
    if (changed)
        mark_full_canvas_dirty();
}

void dfu_display_rect(int x, int y, int w, int h, bool black)
{
    if (!initialized || w <= 0 || h <= 0 || x >= DFU_DISPLAY_WIDTH || y >= DFU_DISPLAY_HEIGHT)
        return;
    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0)
        return;
    if (w > DFU_DISPLAY_WIDTH - x)
        w = DFU_DISPLAY_WIDTH - x;
    if (h > DFU_DISPLAY_HEIGHT - y)
        h = DFU_DISPLAY_HEIGHT - y;

    /* Portrait (x, y) maps to panel (y, PANEL_HEIGHT - 1 - x). */
    bool changed = false;
    for (int i = x; i < x + w; ++i)
    {
        uint8_t *row = (uint8_t *)pixels[PANEL_HEIGHT - 1 - i] + y;
        for (int j = 0; j < h; ++j)
        {
            uint8_t updated = (row[j] & 2u) | !black;
            if (row[j] == updated)
                continue;
            changed = true;
            if ((updated >> 1) == (updated & 1u))
                --pending_pixels;
            else
                ++pending_pixels;
            row[j] = updated;
        }
    }
    if (changed)
    {
        unsigned row_start = PANEL_HEIGHT - (unsigned)(x + w);
        unsigned row_end = PANEL_HEIGHT - (unsigned)x;
        unsigned col_end = (unsigned)(y + h);
        if (row_start < dirty.row_start) dirty.row_start = row_start;
        if (row_end > dirty.row_end) dirty.row_end = row_end;
        if ((unsigned)y < dirty.col_start) dirty.col_start = (unsigned)y;
        if (col_end > dirty.col_end) dirty.col_end = col_end;
    }
}

void dfu_display_text(int x, int y, const char *text)
{
    if (!text)
        return;
    int start_x = x;
    while (*text && y < DFU_DISPLAY_HEIGHT)
    {
        uint8_t character = (uint8_t)*text++;
        if (character == '\n')
        {
            x = start_x;
            y += DFU_DISPLAY_LINE_HEIGHT;
            continue;
        }
        if (character >= 0x80)
        {
            while (((uint8_t)*text & 0xc0) == 0x80)
                ++text;
            character = '?';
        }
        if (character < ' ' || character > '~' || x >= DFU_DISPLAY_WIDTH)
            continue;
        const uint8_t *glyph = dfu_font[character - ' '];
        for (unsigned row = 0; row < 8; ++row)
        {
            for (unsigned col = 0; col < 8; ++col)
                if (glyph[row] & (1u << col))
                    dfu_display_rect(x + (int)col * FONT_SCALE, y + (int)row * FONT_SCALE,
                                     FONT_SCALE, FONT_SCALE, true);
        }
        x += DFU_DISPLAY_CHAR_WIDTH;
    }
}

int dfu_display_refresh(void (*service)(void))
{
    if (!initialized)
        return -1;
    if (!init_needed && !pending_pixels)
    {
        reset_dirty();
        return 0;
    }
    if (partial_refreshes >= FULL_REFRESH_INTERVAL)
        init_needed = true;
    bool full = init_needed;

    bool powered = panel_power_on(service);
    bool ok = powered;
    if (ok && init_needed)
    {
        ok = scan_wave(dfu_wave_init, sizeof(dfu_wave_init) / sizeof(dfu_wave_init[0]), service);
        if (ok)
            update_old_pixels(true, service);
    }
    if (ok && pending_pixels)
        ok = scan_wave(dfu_wave_du, sizeof(dfu_wave_du) / sizeof(dfu_wave_du[0]), service);
    panel_power_off(powered && !ok, service);
    if (!ok)
    {
        init_needed = true;
        if (powered && !lcdc_init())
            initialized = false;
        return -1;
    }

    if (pending_pixels)
        update_old_pixels(false, service);
    partial_refreshes = full ? 0 : partial_refreshes + 1;
    init_needed = false;
    return 0;
}
