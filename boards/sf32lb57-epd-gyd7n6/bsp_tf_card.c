/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bsp_board.h"
#include "bsp_tf_card.h"

#if defined(BSP_USING_SDMMC2) || defined(EPD_TF_DFU)

static const struct
{
    uint8_t pin;
    pin_function function;
    int pull;
} tf_bus[] = {
    {45, SD2_DIO2, PIN_PULLUP},
    {46, SD2_DIO3, PIN_PULLUP},
    {47, SD2_CLK, PIN_NOPULL},
    {48, SD2_CMD, PIN_PULLUP},
    {49, SD2_DIO0, PIN_PULLUP},
    {50, SD2_DIO1, PIN_PULLUP},
};

void bsp_tf_card_set_power(bool enabled)
{
    if (!enabled)
    {
        /* Disconnect SDMMC and its pull-ups before removing card VDD. */
        for (unsigned i = 0; i < sizeof(tf_bus) / sizeof(tf_bus[0]); ++i)
        {
            unsigned pin = tf_bus[i].pin;
            GPIO_InitTypeDef input = {.Pin = pin, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_NOPULL};
            HAL_GPIO_Init(hwp_gpio1, &input);
            HAL_PIN_Set(PAD_PA00 + pin, GPIO_A0 + pin, PIN_NOPULL, 1);
            HAL_PIN_SetMode(PAD_PA00 + pin, 1, PIN_ANALOG_INPUT);
        }
    }
    /* MainBoard V1.1 U14 (LPW5210B5F) has an active-high enable. */
    GPIO_InitTypeDef output = {.Pin = EPD_TF_POWER_PIN, .Mode = GPIO_MODE_OUTPUT, .Pull = GPIO_NOPULL};
    HAL_GPIO_WritePin(hwp_gpio1, EPD_TF_POWER_PIN, enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_Init(hwp_gpio1, &output);
    HAL_PIN_Set(PAD_PA00 + EPD_TF_POWER_PIN, GPIO_A0 + EPD_TF_POWER_PIN, PIN_NOPULL, 1);
    HAL_PIN_SetMode(PAD_PA00 + EPD_TF_POWER_PIN, 1, PIN_DIGITAL_O_NORMAL);
}

void bsp_tf_card_connect_bus(void)
{
    for (unsigned i = 0; i < sizeof(tf_bus) / sizeof(tf_bus[0]); ++i)
    {
        unsigned pad = PAD_PA00 + tf_bus[i].pin;
        HAL_PIN_SetMode(pad, 1, PIN_DIGITAL_IO_NORMAL);
        HAL_PIN_Set(pad, tf_bus[i].function, tf_bus[i].pull, 1);
    }
}

void bsp_tf_card_setup_detect(void)
{
    /* R56 pulls card detect up to the board's unswitched 3.3 V rail. */
    const int pad = PAD_PA00 + BSP_TF_DETECT_PIN;
    HAL_PIN_Set(pad, GPIO_A0 + BSP_TF_DETECT_PIN, PIN_NOPULL, 1);
    HAL_PIN_SetMode(pad, 1, PIN_DIGITAL_IO_NORMAL);
}

#endif
