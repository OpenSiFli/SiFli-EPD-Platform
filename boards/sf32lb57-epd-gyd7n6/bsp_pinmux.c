/* SPDX-License-Identifier: Apache-2.0 */
#include "bsp_board.h"
#include "bf0_hal_pmu.h"
#include "bsp_tf_card.h"
#include "bsp_wifi_power.h"

extern void BSP_GPIO_Set(int pin, int val, int is_porta);

#ifdef SOC_BF0_HCPU
static void bsp_disconnect_pad(unsigned int pin)
{
    GPIO_InitTypeDef input = {.Pin = pin, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_NOPULL};
    HAL_GPIO_Init(hwp_gpio1, &input);
    HAL_PIN_Set(PAD_PA00 + pin, GPIO_A0 + pin, PIN_NOPULL, 1);
    HAL_PIN_SetMode(PAD_PA00 + pin, 1, PIN_ANALOG_INPUT);
}

static void bsp_disable_light_and_audio(void)
{
    BSP_GPIO_Set(53, 0, 1); /* Frontlight */
    BSP_GPIO_Set(54, 0, 1); /* Speaker amplifier */
    BSP_GPIO_Set(57, 0, 1); /* PDM microphone supply */
}
#endif

#if defined(SOC_BF0_HCPU) && defined(BSP_USING_SDMMC1)
static const pin_function wifi_bus[] = {SD1_DIO2, SD1_DIO3, SD1_CLK,
                                        SD1_CMD, SD1_DIO0, SD1_DIO1};

void bsp_wifi_connect_bus(void)
{
    for (unsigned i = 0; i < sizeof(wifi_bus) / sizeof(wifi_bus[0]); ++i)
    {
        HAL_PIN_SetMode(PAD_PA12 + i, 1, PIN_DIGITAL_IO_NORMAL);
        HAL_PIN_Set(PAD_PA12 + i, wifi_bus[i], wifi_bus[i] == SD1_CLK ? PIN_NOPULL : PIN_PULLUP, 1);
    }
}

void bsp_wifi_set_supply(bool enabled)
{
    if (!enabled)
    {
        /* U13 supplies the radio. Disconnect the host and its pull-ups first. */
        for (unsigned i = 0; i < sizeof(wifi_bus) / sizeof(wifi_bus[0]); ++i)
            bsp_disconnect_pad(12 + i);
    }
    BSP_GPIO_Set(8, enabled, 1);
}
#endif

#if defined(SOC_BF0_HCPU) && !defined(CFG_BOOTLOADER)
extern void BSP_EPD_BusIdle(void);
extern void BSP_EPD_SetBusRetention(bool retain);

static void bsp_set_poweroff_retention(bool retain)
{
    static const pin_pad enables[] = {PAD_PA08, PAD_PA09, PAD_PA53, PAD_PA54, PAD_PA57};
    for (unsigned i = 0; i < sizeof(enables) / sizeof(enables[0]); ++i)
    {
        if (retain)
            HAL_PMU_SetPadRetention(enables[i]);
        else
            HAL_PMU_ClearPadRetention(enables[i]);
    }
    BSP_EPD_SetBusRetention(retain);
}

void bsp_epd_poweroff_io(void)
{
    /* The touch driver has masked its IRQ and removed CTP_3.3V. */
    bsp_disconnect_pad(2); /* CTP_INT */
    bsp_disconnect_pad(3); /* CTP_SDA */
    bsp_disconnect_pad(4); /* CTP_SCL */
#ifdef BSP_USING_PDM1
    bsp_disconnect_pad(55); /* PDM1_CLK */
    bsp_disconnect_pad(56); /* PDM1_DATA */
#endif
    bsp_disable_light_and_audio();

    BSP_GPIO_Set(9, 0, 1); /* Wi-Fi PDN */
#ifdef BSP_USING_SDMMC1
    bsp_wifi_set_supply(false);
#else
    BSP_GPIO_Set(8, 0, 1);
#endif

    /* The panel's logic supply stays on after its high-voltage rails go off. */
    BSP_EPD_BusIdle();
    bsp_set_poweroff_retention(true);
}
#endif

void BSP_PIN_Init(void)
{
#ifdef SOC_BF0_HCPU
    HAL_PIN_Set(PAD_PA42, GPIO_A42, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA43, GPIO_A43, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA44, GPIO_A44, PIN_NOPULL, 1);
    BSP_GPIO_Set(42, 0, 1); /* TPS WAKEUP */
    BSP_GPIO_Set(43, 0, 1); /* TPS PWRCOM */
    BSP_GPIO_Set(44, 0, 1); /* TPS PWRUP */
    HAL_PIN_Set(PAD_PA39, GPIO_A39, PIN_NOPULL, 1);
    BSP_GPIO_Set(39, 0, 1); /* GMODE */

#ifdef BSP_USING_I2C1
    HAL_PIN_Set(PAD_PA20, I2C1_SCL, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA21, I2C1_SDA, PIN_PULLUP, 1);
#endif
#ifdef BSP_USING_I2C2
    /* Touch pull-ups are connected to the switched CTP_3.3V supply. */
    HAL_PIN_Set(PAD_PA04, I2C2_SCL, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA03, I2C2_SDA, PIN_NOPULL, 1);
#endif
#ifdef BSP_USING_SDMMC1
    HAL_PIN_Set(PAD_PA10, GPIO_A10, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA11, GPIO_A11, PIN_PULLDOWN, 1);
    BSP_GPIO_Set(10, 0, 1);
#endif
#ifdef BSP_USING_SDMMC2
    bsp_tf_card_set_power(false);
    bsp_tf_card_setup_detect();
#endif
    HAL_PIN_Set(PAD_PA06, GPIO_A6, PIN_NOPULL, 1);  /* USB_DET */
    HAL_PIN_Set(PAD_PA07, GPIO_A7, PIN_PULLUP, 1);  /* CHG_STATUS */

#if defined(BSP_USING_PDM1) && !defined(CFG_BOOTLOADER)
    HAL_PIN_Set(PAD_PA55, PDM1_CLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA56, PDM1_DATA, PIN_PULLDOWN, 1);
#endif

    /* Set peripheral supply and reset outputs before device initialization. */
    HAL_PIN_Set(PAD_PA08, GPIO_A8, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA09, GPIO_A9, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA53, GPIO_A53, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA54, GPIO_A54, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA57, GPIO_A57, PIN_NOPULL, 1);
    BSP_GPIO_Set(9, 0, 1);  /* Wi-Fi PDN */
#ifdef BSP_USING_SDMMC1
    bsp_wifi_set_supply(false);
#else
    BSP_GPIO_Set(8, 0, 1);
#endif
    bsp_disable_light_and_audio();
#ifndef CFG_BOOTLOADER
    /* Program the low GPIO levels before releasing the Hibernate latches. */
    BSP_EPD_BusIdle();
    bsp_set_poweroff_retention(false);
#endif
#endif
}
