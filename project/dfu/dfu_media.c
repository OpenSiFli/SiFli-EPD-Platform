/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dfu_media.h"
#include "dfu_board.h"
#include "diskio.h"
#include "bf0_hal.h"
#include "bsp_tf_card.h"
#include "dma_config.h"
#include "mem_map.h"

#include <string.h>
#include <stdio.h>

#define SD_HOST hwp_sdmmc2
#define SD_DETECT_PIN BSP_TF_DETECT_PIN
#define SD_DETECT_BANK (hwp_gpio1 + SD_DETECT_PIN / 32)
#define SD_DETECT_MASK (1u << (SD_DETECT_PIN % 32))
#define SD_SECTOR_SIZE 512u
#define SD_IDENT_HZ 400000u
#define SD_TRANSFER_HZ 24000000u
#define SD_CMD_MS 100u
#define SD_DATA_MS 600u
#define SD_POWERUP_MS 1500u
#define SD_PROGRAM_MS 1000u

#define SD_OCR_VOLTAGE 0x00ff8000u
#define SD_OCR_READY (1u << 31)
#define SD_OCR_CCS (1u << 30)
#define SD_R1_ERRORS 0xfff98008u
#define SD_R1_ILLEGAL (1u << 22)
#define SD_R1_READY (1u << 8)
#define SD_R1_TRAN (4u << 9)
#define SD_R1_STATE (15u << 9)
#define SD_R1_APP_CMD (1u << 5)
#define SD_DATA_ERRORS (SD_SR_DATA_CRC | SD_SR_DATA_TIMEOUT | SD_SR_STARTBIT_ERROR | \
                        SD_SR_FIFO_UNDERRUN | SD_SR_FIFO_OVERRUN)

#define KEY_ADC_PIN 34
#define KEY_ADC_CHANNEL 6
#define KEY_POLL_MS 4u
#define KEY_DEBOUNCE_MS 16u
#define KEY_RANGE_MV 190

typedef enum
{
    SD_RESPONSE_NONE,
    SD_RESPONSE_SHORT,
    SD_RESPONSE_OCR,
    SD_RESPONSE_LONG,
} sd_response_t;

static FATFS volume;
static DSTATUS media_status = STA_NOINIT;
static bool detect_ready;
static bool sd_powered;
static bool card_removed;
static bool block_addressed;
static uint32_t relative_address;
static uint32_t bus_width;
static uint32_t sd_clock;
static LBA_t sector_count;
static DMA_HandleTypeDef sd_dma = {
    .Instance = SDMMC2_DMA_INSTANCE,
    .Init = {
        .Request = SDMMC2_DMA_REQUEST,
        .PeriphInc = DMA_PINC_DISABLE,
        .MemInc = DMA_MINC_ENABLE,
        .PeriphDataAlignment = DMA_PDATAALIGN_WORD,
        .MemDataAlignment = DMA_MDATAALIGN_WORD,
        .Mode = DMA_NORMAL,
        .Priority = DMA_PRIORITY_MEDIUM,
        .BurstSize = 1,
    },
};

static ADC_HandleTypeDef key_adc = {
    .Instance = hwp_gpadc1,
    .Init = {
        .adc_se = 1,
        .en_slot = 1,
        .data_samp_delay = 3,
        .conv_width = 100,
        .sample_width = 95,
    },
};
static HAL_ADC_CalibContextTypeDef key_calibration;
static bool keys_ready;
static bool key_armed;
static dfu_key_t key_candidate;
static dfu_key_t key_stable;
static uint32_t key_candidate_since;
static uint32_t key_sample_at;

static bool expired(uint32_t start, uint32_t ms)
{
    return HAL_GetTick() - start >= ms;
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = HAL_GetTick();

    while (!expired(start, ms))
    {
    }
}

bool dfu_media_present(void)
{
    if (!detect_ready)
    {
        GPIO_InitTypeDef input = {
            .Pin = SD_DETECT_PIN,
            .Mode = GPIO_MODE_IT_RISING,
            .Pull = GPIO_NOPULL,
        };

        HAL_RCC_EnableModule(RCC_MOD_GPIO1);
        bsp_tf_card_set_power(false);
        bsp_tf_card_setup_detect();
        /* Latch the removal edge while GPIO1_IRQn stays masked at the CPU. */
        HAL_GPIO_Init(hwp_gpio1, &input);
        detect_ready = true;
    }

    bool present = HAL_GPIO_ReadPin(hwp_gpio1, SD_DETECT_PIN) == GPIO_PIN_RESET;
    if (!present || (SD_DETECT_BANK->ISR & SD_DETECT_MASK))
    {
        SD_DETECT_BANK->ISR = SD_DETECT_MASK;
        card_removed = true;
        media_status = STA_NOINIT | (present ? 0 : STA_NODISK);
    }
    else
        media_status &= (DSTATUS)~STA_NODISK;
    return present;
}

static bool sd_card_available(void)
{
    return dfu_media_present() && !card_removed;
}

static void sd_set_clock(uint32_t target)
{
    uint32_t source = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
    uint32_t divider = (source + target - 1u) / target;

    if (divider < 2)
        divider = 2;
    sd_clock = source / divider;
    HAL_SDMMC_CLK_SET(SD_HOST, divider, 1);
}

static HAL_StatusTypeDef sd_wait(uint32_t done, uint32_t errors, uint32_t busy, uint32_t ms)
{
    uint32_t start = HAL_GetTick();

    do
    {
        if (!sd_card_available())
            return HAL_ERROR;
        uint32_t status = HAL_SDMMC_GET_STA(SD_HOST);
        if (status & SD_SR_CMD_TIMEOUT)
            return HAL_TIMEOUT;
        if (status & errors)
            return HAL_ERROR;
        if ((status & done) == done && !(status & busy))
            return HAL_OK;
    } while (!expired(start, ms));

    return HAL_TIMEOUT;
}

static HAL_StatusTypeDef sd_command(uint8_t index, uint32_t argument,
                                    sd_response_t type, uint32_t response[4])
{
    if (!sd_card_available())
        return HAL_ERROR;

    uint32_t format = type == SD_RESPONSE_OCR ? SD_RESPONSE_SHORT : type;
    uint32_t done = type == SD_RESPONSE_NONE ? SD_SR_CMD_SENT : SD_SR_CMD_DONE;
    uint32_t errors = type == SD_RESPONSE_OCR ? 0u : SD_SR_CMD_RSP_CRC;

    HAL_SDMMC_SET_CMD(SD_HOST, index, format, argument);
    HAL_StatusTypeDef result = sd_wait(done, errors, 0, SD_CMD_MS);
    if (result != HAL_OK)
        return result;
    if (type == SD_RESPONSE_SHORT && HAL_SDMMC_GET_RCI(SD_HOST) != index)
        return HAL_ERROR;
    if (response != NULL)
        HAL_SDMMC_GET_RESP(SD_HOST, response);
    return HAL_OK;
}

static bool sd_r1(uint8_t index, uint32_t argument)
{
    uint32_t response[4];

    return sd_command(index, argument, SD_RESPONSE_SHORT, response) == HAL_OK &&
           !(response[0] & SD_R1_ERRORS);
}

static bool sd_app_command(uint32_t rca)
{
    uint32_t response[4];

    return sd_command(55, rca, SD_RESPONSE_SHORT, response) == HAL_OK &&
           !(response[0] & SD_R1_ERRORS) &&
           (response[0] & SD_R1_APP_CMD);
}

static bool sd_wait_ready(void)
{
    uint32_t start = HAL_GetTick();
    uint32_t response[4];

    do
    {
        if (sd_command(13, relative_address, SD_RESPONSE_SHORT, response) != HAL_OK ||
            (response[0] & SD_R1_ERRORS))
            return false;
        if ((response[0] & (SD_R1_READY | SD_R1_STATE)) == (SD_R1_READY | SD_R1_TRAN))
            return true;
        delay_ms(1);
    } while (!expired(start, SD_PROGRAM_MS));

    return false;
}

static bool sd_read_csd(bool *write_protected)
{
    uint32_t raw[4];

    if (sd_command(9, relative_address, SD_RESPONSE_LONG, raw) != HAL_OK)
        return false;

    /* The controller omits the low eight R2 bits and puts the low word first. */
    uint32_t csd_high = (raw[3] << 8) | (raw[2] >> 24);
    uint32_t csd_mid = (raw[2] << 8) | (raw[1] >> 24);
    uint32_t csd_low = (raw[1] << 8) | (raw[0] >> 24);
    uint32_t csd_tail = raw[0] << 8;
    uint32_t version = csd_high >> 30;

    if (version == 1 && block_addressed)
    {
        uint32_t size = ((csd_mid & 0x3fu) << 16) | (csd_low >> 16);
        sector_count = ((LBA_t)size + 1u) << 10;
    }
    else if (version == 0 && !block_addressed)
    {
        uint32_t read_bl_len = (csd_mid >> 16) & 15u;
        uint32_t size = ((csd_mid & 0x3ffu) << 2) | (csd_low >> 30);
        uint32_t size_mult = (csd_low >> 15) & 7u;
        if (read_bl_len < 9 || read_bl_len > 11)
            return false;
        sector_count = ((LBA_t)size + 1u) << (read_bl_len + size_mult + 2u - 9u);
    }
    else
        return false;

    *write_protected = (csd_tail & (3u << 12)) != 0;
    return true;
}

static bool sd_identify(bool *write_protected)
{
    uint32_t response[4];
    uint32_t argument = SD_OCR_VOLTAGE;
    uint32_t started;

    if (sd_command(0, 0, SD_RESPONSE_NONE, NULL) != HAL_OK)
        return false;

    HAL_StatusTypeDef result = sd_command(8, 0x1aau, SD_RESPONSE_SHORT, response);
    if (result == HAL_OK && response[0] == 0x1aau)
        argument |= SD_OCR_CCS; /* HCS in the ACMD41 request. */
    else if (result != HAL_TIMEOUT &&
             (result != HAL_OK || !(response[0] & SD_R1_ILLEGAL)))
        return false;

    started = HAL_GetTick();
    do
    {
        if (!sd_app_command(0) ||
            sd_command(41, argument, SD_RESPONSE_OCR, response) != HAL_OK ||
            !(response[0] & SD_OCR_VOLTAGE))
            return false;
        if (response[0] & SD_OCR_READY)
            break;
        delay_ms(10);
    } while (!expired(started, SD_POWERUP_MS));
    if (!(response[0] & SD_OCR_READY))
        return false;
    block_addressed = (response[0] & argument & SD_OCR_CCS) != 0;

    if (sd_command(2, 0, SD_RESPONSE_LONG, NULL) != HAL_OK ||
        sd_command(3, 0, SD_RESPONSE_SHORT, response) != HAL_OK ||
        (response[0] & 0xe000u) != 0)
        return false;
    relative_address = response[0] & 0xffff0000u;
    if (!relative_address || !sd_read_csd(write_protected) ||
        !sd_r1(7, relative_address) || !sd_wait_ready())
        return false;
    if (!block_addressed && !sd_r1(16, SD_SECTOR_SIZE))
        return false;

    /* All four data lines are routed. Use default-speed SD timing. */
    if (!sd_app_command(relative_address) || !sd_r1(6, 2))
        return false;
    bus_width = HAL_SDMMC_WIRE_QLINE;
    sd_set_clock(SD_TRANSFER_HZ);
    return true;
}

static void sd_host_start(void)
{
    bsp_tf_card_set_power(true);
    sd_powered = true;
    delay_ms(BSP_TF_POWER_SETTLE_MS);
    bsp_tf_card_connect_bus();
    HAL_RCC_EnableModule(RCC_MOD_SDMMC2);
    HAL_RCC_EnableModule(RCC_MOD_DMAC1);
    HAL_RCC_ResetModule(RCC_MOD_SDMMC2);
    HAL_SDMMC_INIT(SD_HOST);
    HAL_SDMMC_CLK_SET(SD_HOST, 2, 0);
    HAL_SDMMC_POWER_SET(SD_HOST, 2); /* Controller power-up. */
    delay_ms(10);
    sd_set_clock(SD_IDENT_HZ);
    HAL_SDMMC_POWER_SET(SD_HOST, 3); /* Controller power-on. */
    delay_ms(10);
}

static void sd_host_stop(void)
{
    if (!sd_powered)
        return;
    HAL_SDMMC_CLK_SET(SD_HOST, 1, 0);
    HAL_SDMMC_POWER_SET(SD_HOST, 0);
    bsp_tf_card_set_power(false);
    HAL_RCC_DisableModule(RCC_MOD_SDMMC2);
    sd_powered = false;
}

DSTATUS disk_status(BYTE drive)
{
    if (drive != 0)
        return STA_NOINIT;
    dfu_media_present();
    return media_status;
}

DSTATUS disk_initialize(BYTE drive)
{
    bool write_protected;

    if (drive != 0)
        return STA_NOINIT;
    if (!(disk_status(drive) & STA_NOINIT) || (media_status & STA_NODISK))
        return media_status;

    card_removed = false;
    sd_host_stop();
    sd_host_start();
    /* Let a legacy card's CMD8 time out in hardware before issuing CMD55. */
    HAL_SDMMC_SET_TIMEOUT(SD_HOST, sd_clock / 20u);
    if (sd_identify(&write_protected) && sd_card_available())
        media_status = write_protected ? STA_PROTECT : 0;
    else
        sd_host_stop();
    return disk_status(drive);
}

static bool sd_transfer(LBA_t sector, uint32_t words[SD_SECTOR_SIZE / 4], bool write)
{
    uint32_t address = block_addressed ? (uint32_t)sector : (uint32_t)(sector * SD_SECTOR_SIZE);
    uint32_t direction = write ? HAL_SDMMC_DATA_HOST2CARD : HAL_SDMMC_DATA_CARD2HOST;

    HAL_SDMMC_CLR_DATA_CTRL(SD_HOST);
    HAL_SDMMC_SET_TIMEOUT(SD_HOST, sd_clock / 2u);
    HAL_SDMMC_SET_DATALEN(SD_HOST, SD_SECTOR_SIZE);
    HAL_SDMMC_SET_DATA_CTRL(SD_HOST, SD_SECTOR_SIZE, direction, bus_width, HAL_SDMMC_DATA_BLOCK_MODE);
    uint32_t fifo = (uint32_t)&SD_HOST->FIFO;
    uint32_t memory = (uint32_t)words;
    sd_dma.Init.Direction = write ? DMA_MEMORY_TO_PERIPH : DMA_PERIPH_TO_MEMORY;
    if (!write) SCB_InvalidateDCache_by_Addr(words, SD_SECTOR_SIZE);
    if (HAL_DMA_Init(&sd_dma) != HAL_OK ||
        HAL_DMA_Start(&sd_dma, write ? memory : fifo, write ? fifo : memory,
                      SD_SECTOR_SIZE / sizeof(uint32_t)) != HAL_OK)
    {
        HAL_SDMMC_CLR_DATA_CTRL(SD_HOST);
        dfu_log("DFU SD: DMA setup failed\r\n");
        return false;
    }
    if (!write)
        HAL_SDMMC_SET_DATA_START(SD_HOST, 1);

    bool success = sd_r1(write ? 24 : 17, address);
    if (success && write)
        HAL_SDMMC_SET_DATA_START(SD_HOST, 1);
    if (success)
        success = sd_wait(SD_SR_DATA_DONE, SD_DATA_ERRORS, 0, SD_DATA_MS) == HAL_OK;
    if (success)
        success = HAL_DMA_PollForTransfer(&sd_dma, HAL_DMA_FULL_TRANSFER, SD_CMD_MS) == HAL_OK;
    if (!success) (void)HAL_DMA_Abort(&sd_dma);
    if (!write) SCB_InvalidateDCache_by_Addr(words, SD_SECTOR_SIZE);
    if (success)
        success = sd_wait(0, SD_DATA_ERRORS, SD_SR_DATA_BUSY, SD_CMD_MS) == HAL_OK;
    HAL_SDMMC_CLR_DATA_CTRL(SD_HOST);

    if (success && write)
    {
        HAL_SDMMC_SET_TIMEOUT(SD_HOST, sd_clock / 20u);
        success = sd_wait_ready();
    }
    if (!success)
    {
        char message[80];
        snprintf(message, sizeof(message), "DFU SD: CMD%u sector=%lu status=%08lx\r\n",
                 write ? 24u : 17u, (unsigned long)sector, (unsigned long)SD_HOST->SR);
        dfu_log(message);
    }
    return success && sd_card_available();
}

static DRESULT sd_transfer_error(void)
{
    media_status |= STA_NOINIT;
    return sd_card_available() ? RES_ERROR : RES_NOTRDY;
}

static DRESULT sd_check_range(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count)
{
    if (drive != 0 || buffer == NULL || count == 0)
        return RES_PARERR;
    if (disk_status(drive) & STA_NOINIT)
        return RES_NOTRDY;
    if (sector >= sector_count || count > sector_count - sector)
        return RES_PARERR;
    return RES_OK;
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count)
{
    _Alignas(32) uint32_t words[SD_SECTOR_SIZE / 4];
    DRESULT result = sd_check_range(drive, buffer, sector, count);

    if (result != RES_OK)
        return result;
    for (UINT i = 0; i < count; ++i)
    {
        if (!sd_transfer(sector + i, words, false))
            return sd_transfer_error();
        memcpy(buffer, words, SD_SECTOR_SIZE);
        buffer += SD_SECTOR_SIZE;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count)
{
    _Alignas(32) uint32_t words[SD_SECTOR_SIZE / 4];
    DRESULT result = sd_check_range(drive, buffer, sector, count);

    if (result != RES_OK)
        return result;
    if (media_status & STA_PROTECT)
        return RES_WRPRT;
    for (UINT i = 0; i < count; ++i)
    {
        memcpy(words, buffer, SD_SECTOR_SIZE);
        if (!sd_transfer(sector + i, words, true))
            return sd_transfer_error();
        buffer += SD_SECTOR_SIZE;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
    if (drive != 0)
        return RES_PARERR;
    if (disk_status(drive) & STA_NOINIT)
        return RES_NOTRDY;

    switch (command)
    {
    case CTRL_SYNC:
        HAL_SDMMC_SET_TIMEOUT(SD_HOST, sd_clock / 20u);
        return sd_wait_ready() ? RES_OK : sd_transfer_error();
    case GET_SECTOR_COUNT:
        if (buffer == NULL)
            return RES_PARERR;
        *(LBA_t *)buffer = sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (buffer == NULL)
            return RES_PARERR;
        *(WORD *)buffer = SD_SECTOR_SIZE;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

FRESULT dfu_media_mount(void)
{
    FRESULT result = f_mount(&volume, DFU_MEDIA_ROOT, 1);
    if (result != FR_OK)
        dfu_media_unmount();
    return result;
}

bool dfu_media_mounted(void)
{
    return sd_card_available() && !(media_status & STA_NOINIT);
}

void dfu_media_unmount(void)
{
    f_mount(NULL, DFU_MEDIA_ROOT, 0);
    sd_host_stop();
    media_status |= STA_NOINIT;
}

static void keys_load_calibration(void)
{
    uint8_t ate[12];

    /* SF57 factory ADC calibration uses the two samples in ATE bank 1. */
    HAL_RCC_EnableModule(RCC_MOD_EFUSEC);
    if (HAL_EFUSE_Init() != HAL_OK ||
        HAL_EFUSE_Read(EFUSE_ATE_DATA_OFFSET, ate, sizeof(ate)) != sizeof(ate) ||
        ate[0] == 0)
        return;

    HAL_ADC_CalibConfigTypeDef factory = {
        .reg_value1 = ate[4] | ((uint32_t)(ate[5] & 0x0fu) << 8),
        .reg_value2 = (ate[6] >> 1) | ((uint32_t)(ate[7] & 0x1fu) << 7),
        .voltage1_mv = 100u * ((ate[5] >> 4) | ((uint32_t)(ate[6] & 1u) << 4)),
        .voltage2_mv = 100u * ((ate[7] >> 5) | ((uint32_t)(ate[8] & 3u) << 3)),
        .range_mode = 1,
    };

    if (factory.reg_value1 != 0 && factory.voltage1_mv != 0 &&
        factory.reg_value2 > factory.reg_value1 && factory.voltage2_mv > factory.voltage1_mv)
        HAL_ADC_CalibSetCustom(&key_calibration, &factory);
}

bool dfu_keys_init(void)
{
    ADC_ChannelConfTypeDef channel = {
        .Channel = KEY_ADC_CHANNEL,
        .pchnl_sel = KEY_ADC_CHANNEL,
        .slot_en = 1,
    };

    if (keys_ready)
        return true;

    GPIO_InitTypeDef input = {.Pin = KEY_ADC_PIN, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_NOPULL};
    HAL_GPIO_Init(hwp_gpio1, &input);
    if (HAL_PIN_Set_Analog(PAD_PA34, 1) != 0)
        return false;
    HAL_RCC_EnableModule(RCC_MOD_GPADC);
    HAL_Delay_us(0);
    if (HAL_ADC_Init(&key_adc) != HAL_OK ||
        HAL_ADC_ConfigChannel(&key_adc, &channel) != HAL_OK ||
        HAL_ADC_CalibInit(&key_calibration) != HAL_OK)
        return false;
    keys_load_calibration();
    key_candidate_since = HAL_GetTick();
    key_sample_at = key_candidate_since - KEY_POLL_MS;
    keys_ready = true;
    return true;
}

static bool key_sample(dfu_key_t *key)
{
    static const int levels_mv[] = {2973, 2578, 2185};

    __HAL_ADC_CLEAR_FLAG(&key_adc, GPADC_GPADC_IRQ_GPADC_ICR);
    if (HAL_ADC_Start(&key_adc) != HAL_OK)
        return false;
    HAL_StatusTypeDef result = HAL_ADC_PollForConversion(&key_adc, 5);
    uint32_t raw = result == HAL_OK ? HAL_ADC_GetValue(&key_adc, KEY_ADC_CHANNEL) : 0;
    HAL_ADC_Stop(&key_adc);
    if (result != HAL_OK)
        return false;

    *key = DFU_KEY_NONE;
    int voltage_mv = (int)HAL_ADC_RegToVoltageFloat((float)raw, &key_calibration);
    for (unsigned int i = 0; i < sizeof(levels_mv) / sizeof(levels_mv[0]); ++i)
    {
        if (voltage_mv >= levels_mv[i] - KEY_RANGE_MV &&
            voltage_mv <= levels_mv[i] + KEY_RANGE_MV)
        {
            *key = (dfu_key_t)(DFU_KEY1 + i);
            break;
        }
    }
    return true;
}

dfu_key_t dfu_key_poll(void)
{
    dfu_key_t sample;
    uint32_t now = HAL_GetTick();

    if (!keys_ready || now - key_sample_at < KEY_POLL_MS)
        return DFU_KEY_NONE;
    key_sample_at = now;
    if (!key_sample(&sample))
    {
        key_candidate_since = now;
        return DFU_KEY_NONE;
    }
    if (sample != key_candidate)
    {
        key_candidate = sample;
        key_candidate_since = now;
        return DFU_KEY_NONE;
    }
    if (now - key_candidate_since < KEY_DEBOUNCE_MS)
        return DFU_KEY_NONE;
    if (sample == DFU_KEY_NONE)
        key_armed = true;
    if (sample == key_stable)
        return DFU_KEY_NONE;

    key_stable = sample;
    if (sample != DFU_KEY_NONE && key_armed)
    {
        key_armed = false;
        char message[24];
        snprintf(message, sizeof(message), "DFU: KEY%u pressed\r\n", (unsigned)sample);
        dfu_log(message);
        return sample;
    }
    return DFU_KEY_NONE;
}
