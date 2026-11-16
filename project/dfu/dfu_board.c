/* SPDX-License-Identifier: Apache-2.0 */
#include "dfu_board.h"
#include "dfu_records.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "ptab.h"
#include <string.h>

extern const uint32_t dfu_vectors[];
extern uint8_t __flash_ops_start[], __flash_ops_end[];
#define FLASH_CODE __attribute__((section(".dfu_flash_ops"), noinline))
static volatile uint32_t milliseconds;
static FLASH_HandleTypeDef *nor;

/* Bound the C library's workspace independently of the stack. */
void *_sbrk(ptrdiff_t increment)
{
    static uint8_t arena[4096] __attribute__((aligned(8)));
    static ptrdiff_t used;
    if (increment < -used || increment > (ptrdiff_t)sizeof(arena) - used) return (void *)-1;
    void *start = arena + used;
    used += increment;
    return start;
}

FLASH_CODE void SysTick_Handler(void) { ++milliseconds; }
uint32_t HAL_GetTick(void) { return milliseconds; }

void dfu_log(const char *text)
{
    for (; *text; ++text)
    {
        uint32_t start = HAL_GetTick();
        while (!(hwp_usart1->ISR & USART_ISR_TXE))
            if (HAL_GetTick() - start > 20) return;
        hwp_usart1->TDR = (uint8_t)*text;
    }
}

void dfu_fault(void)
{
    for (;;) __WFI();
}

bool dfu_board_init(void)
{
    SysTick->CTRL = 0;
    for (unsigned i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); ++i)
    {
        NVIC->ICER[i] = UINT32_MAX;
        NVIC->ICPR[i] = UINT32_MAX;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    /* Startup copied the Flash routines and SysTick handler to SRAM. */
    SCB_CleanDCache_by_Addr((void *)__flash_ops_start, __flash_ops_end - __flash_ops_start);
    SCB_InvalidateICache();
    SCB->VTOR = (uint32_t)dfu_vectors;
    SCB->CPACR |= 0xfu << 20;
    __DSB();
    __ISB();
    uint32_t hz = HAL_RCC_GetHCLKFreq(CORE_ID_HCPU);
    if (hz < 1000 || SysTick_Config(hz / 1000)) return false;
    __enable_irq();
    dfu_log("\r\nTF DFU\r\n");

    HAL_RCC_ResetModule(RCC_MOD_MPI2);
    /* Match the SDK bootloader's 48 MHz MPI2 clock. */
    BSP_SetFlash2DIV(6);
    BSP_Flash_hw2_init();
    nor = BSP_Flash_get_handle(DFU_LOADER_START_ADDR);
    return nor && !nor->isNand && !nor->dualFlash && nor->size >= 0x01000000;
}

static bool flash_range(uint32_t address, size_t size)
{
    return nor && size && address >= nor->base && size <= nor->size &&
           address - nor->base <= nor->size - size;
}

bool dfu_flash_read(uint32_t address, void *data, size_t size)
{
    if (!flash_range(address, size)) return false;
    SCB_InvalidateDCache_by_Addr((void *)address, size);
    memcpy(data, (const void *)address, size);
    return true;
}

FLASH_CODE bool dfu_flash_write(uint32_t address, const void *data, size_t size)
{
    if (!flash_range(address, size)) return false;
    const uint8_t *bytes = data;
    SCB_CleanDCache_by_Addr((void *)data, size);
    while (size)
    {
        size_t chunk = QSPI_NOR_PAGE_SIZE - (address & (QSPI_NOR_PAGE_SIZE - 1));
        if (chunk > size) chunk = size;
        if (HAL_QSPIEX_WRITE_PAGE(nor, address - nor->base, bytes, chunk) != (int)chunk)
            return false;
        address += chunk;
        bytes += chunk;
        size -= chunk;
    }
    return true;
}

FLASH_CODE bool dfu_flash_erase(uint32_t address, size_t size)
{
    if (!flash_range(address, size) || (address | size) % QSPI_NOR_SECT_SIZE) return false;
    while (size)
    {
        uint32_t offset = address - nor->base;
        bool block = !(offset % QSPI_NOR_BLK64_SIZE) && size >= QSPI_NOR_BLK64_SIZE;
        int result = block ? HAL_QSPIEX_BLK64_ERASE(nor, offset) : HAL_QSPIEX_SECT_ERASE(nor, offset);
        if (result != 0) return false;
        size_t erased = block ? QSPI_NOR_BLK64_SIZE : QSPI_NOR_SECT_SIZE;
        address += erased;
        size -= erased;
    }
    return true;
}

void dfu_reboot(void)
{
    HAL_PMU_Reboot();
    for (;;) __WFI();
}
