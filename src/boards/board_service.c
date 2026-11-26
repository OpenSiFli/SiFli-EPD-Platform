/**
 * @file board_service.c
 * @brief 板级服务实现（纯 C）——从 SF32Paper.cpp 抽取
 *
 * 覆盖：开机唤醒处理、文件系统挂载、SD 卡上下电、关机前处理。
 * 排除了原 C++ 版本中的渲染器/字体/UI 装配逻辑（LVGL 路线由 SDK lv_lcd 接管）。
 */
#include "app_service_internal.h"
#include "storage_internal.h"
#include "board_service.h"
#include "board_pm.h"
#include "epd_compute_transport.h"
#include "bsp_touch_pm.h"
#include "frontlight.h"
#include "frontlight_device.h"
#include "platform/epd_power.h"
#include "system_reset_guard.h"
#include "board_sdcard.h"
#include "storage.h"
#include "usb_card_reader.h"
#include "app_service.h"
#include "platform/app_catalog.h"
#include "platform/app_installer.h"
#include "portal/portal.h"

#include <string.h>

#include "board.h"
#include "dfs_fs.h"
#include "dfs_file.h"
#include "dfs_posix.h"
#include "mem_map.h"
#include "rtdevice.h"
#include "rthw.h"
#include "drv_gpio.h"
#include "bf0_hal_aon.h"
#include "bf0_hal_pmu.h"
#include "bf0_pm.h"
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif


#ifndef _WIN32
#include "drv_flash.h"
#ifdef BSP_USING_NOR_FLASH2
extern void rt_flash_wait_idle(uint32_t addr);
#endif
#endif

#define DBG_TAG "board"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 板级/SDK 提供的电源与触摸控制接口 */
extern void BSP_TP_PowerUp(void);
extern void BSP_TP_PowerDown(void);
#ifdef EPD_BOARD_SF32LB57_EPD
extern void bsp_epd_poweroff_io(void);
#endif

void board_power_up(void)
{
    switch (SystemPowerOnModeGet())
    {
    case PM_REBOOT_BOOT:
    case PM_COLD_BOOT:
    {
        /* 正常开机 */
        break;
    }
    case PM_HIBERNATE_BOOT:
    case PM_SHUTDOWN_BOOT:
    {
        if (PMUC_WSR_RTC & pm_get_wakeup_src())
        {
            /* RTC 唤醒 */
            NVIC_EnableIRQ(RTC_IRQn);
        }
        break;
    }
    default:
    {
        RT_ASSERT(0);
    }
    }
}

void board_start_filesystem(void)
{
    LOG_I("power-on mode=%u wake=0x%lx", (unsigned)SystemPowerOnModeGet(),
          (unsigned long)pm_get_wakeup_src());
    /* Configure the PA51 card-detect input before starting storage services. */
    board_sdcard_prepare();
    storage_init();
}

typedef enum { POWER_IDLE, POWER_REQUESTED, POWER_SAVE, POWER_STOP, POWER_FINALIZE, POWER_RECOVER } power_phase_t;
#define POWER_SAVE_TIMEOUT_MS 35000
#define POWER_DRAIN_TIMEOUT_MS 35000
#define POWER_FRONTLIGHT_TIMEOUT_MS 2000
#define POWER_IO_LOCK_TIMEOUT_MS 2000
static power_phase_t power_phase;
static board_power_error_t power_error;
static bool power_restart;
static bool power_light_suspended;
static bool power_wallpaper_started;
static bool power_touch_suspended;
static bool power_portal_paused;
static rt_tick_t power_phase_started;

static power_phase_t power_phase_get(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    power_phase_t phase = power_phase;
    rt_hw_interrupt_enable(level);
    return phase;
}

bool board_power_pending(void)
{
    return power_phase_get() != POWER_IDLE;
}

bool board_power_shutdown_pending(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool pending = power_phase != POWER_IDLE && !power_restart;
    rt_hw_interrupt_enable(level);
    return pending;
}

board_power_error_t board_power_error(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    board_power_error_t error = power_error;
    rt_hw_interrupt_enable(level);
    return error;
}

static void power_fail(board_power_error_t error)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (power_phase != POWER_IDLE && !power_restart && !system_reset_factory_busy())
    {
        power_phase = POWER_REQUESTED;
        rt_hw_interrupt_enable(level);
        epd_power_notify();
        return;
    }
    power_error = error;
    /* Device output is restored by the UI, never by the terminal worker. */
    power_phase = (power_light_suspended || power_wallpaper_started) ? POWER_RECOVER : POWER_IDLE;
    rt_hw_interrupt_enable(level);
    LOG_W("power request stopped: error=%d", (int)error);
    epd_power_notify();
}

static void power_wait(board_power_error_t reason)
{
    if (power_error == reason) return;
    rt_base_t level = rt_hw_interrupt_disable();
    power_error = reason;
    rt_hw_interrupt_enable(level);
    const char *detail;
    switch (reason)
    {
    case BOARD_POWER_ERROR_STORAGE: detail = "storage ownership, write or card shutdown"; break;
    case BOARD_POWER_ERROR_SAVE: detail = "saving application state"; break;
    case BOARD_POWER_ERROR_TOUCH: detail = "touch shutdown"; break;
    case BOARD_POWER_ERROR_PERIPHERALS: detail = "network or peripheral shutdown"; break;
    case BOARD_POWER_ERROR_WAKEUP: detail = "PMU wakeup configuration"; break;
    case BOARD_POWER_ERROR_RETURNED: detail = "PMU poweroff did not complete"; break;
    case BOARD_POWER_ERROR_START: detail = "poweroff worker allocation"; break;
    case BOARD_POWER_ERROR_FRONTLIGHT: detail = "frontlight disabled by hardware"; break;
    case BOARD_POWER_ERROR_DISPLAY: detail = "waiting for the display transfer to end"; break;
    case BOARD_POWER_WAIT_INSTALLER: detail = "waiting for installer cleanup"; break;
    case BOARD_POWER_WAIT_CARD_READER: detail = "waiting for USB card reader to return the card"; break;
    case BOARD_POWER_WAIT_RESET: detail = "waiting for format or network reset to finish"; break;
    case BOARD_POWER_WAIT_KEY: detail = "waiting for the power key to be released"; break;
    case BOARD_POWER_WAIT_COMPUTE: detail = "waiting for ACPU to park"; break;
    case BOARD_POWER_WAIT_DMA: detail = "waiting for DMA channels to stop"; break;
    case BOARD_POWER_WAIT_LCPU: detail = "LPSYS did not stay active with LCPU held in reset"; break;
    default: detail = "waiting for application services to close"; break;
    }
    LOG_W("shutdown stage=%d: %s", (int)power_phase_get(), detail);
}

bool board_power_request(bool restart)
{
    if (restart && (app_installer_busy() || storage_changing() || usb_card_reader_active()))
    {
        if (board_power_pending()) return false;
        power_fail(BOARD_POWER_ERROR_BUSY);
        return false;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    if (restart ? system_reset_busy() : system_reset_factory_busy())
    {
        power_error = BOARD_POWER_ERROR_BUSY;
        rt_hw_interrupt_enable(level);
        return false;
    }
    if (power_phase != POWER_IDLE)
    {
        if (!restart) power_restart = false;
        rt_hw_interrupt_enable(level);
        if (!restart) epd_power_notify();
        return !restart;
    }
    power_restart = restart;
    power_error = BOARD_POWER_ERROR_NONE;
    power_phase = POWER_REQUESTED;
    rt_hw_interrupt_enable(level);
    epd_power_notify();
    return true;
}

static bool storage_has_writer(void)
{
    bool busy = false;
    dfs_lock();
    struct dfs_fdtable *table = dfs_fdtable_get();
    for (unsigned i = 0; i < table->maxfd; ++i)
    {
        const struct dfs_fd *fd = table->fds[i];
        if (fd && fd->ref_count && fd->type == FT_REGULAR && fd->fs &&
            fd->fs->path && (fd->flags & (O_WRONLY | O_RDWR)) &&
            (!strcmp(fd->fs->path, STORAGE_FLASH_ROOT) ||
             !strcmp(fd->fs->path, STORAGE_SD_ROOT)))
        {
            busy = true;
            break;
        }
    }
    dfs_unlock();
    return busy;
}

static bool power_wakeup_configure(void)
{
    /* 57's PMU and HPAON use the same wake-pin numbering. The PAD-based
     * HAL_PMU_EnablePinWakeup2 entry is not implemented in this SDK. */
    int8_t pin = HAL_HPAON_QueryWakeupPin(GET_GPIO_INSTANCE(BSP_KEY1_PIN),
                                        GET_GPIOx_PIN(BSP_KEY1_PIN));
    if (pin < 0) return false;
    for (uint8_t i = 0; i < PMUC_WSR_PIN_NUM; ++i)
        if (i != (uint8_t)pin && HAL_PMU_DisablePinWakeup(i) != HAL_OK) return false;
    if (HAL_PMU_DisableRtcWakeup() != HAL_OK) return false;
    return HAL_PMU_EnablePinWakeup((uint8_t)pin, AON_PIN_MODE_HIGH) == HAL_OK;
}

#ifdef EPD_BOARD_SF32LB57_EPD
static bool hibernate_mpi_address(uint32_t address)
{
    return HCPU_IS_MPI_CBUS_ADDR(address) ||
           HCPU_IS_MPI_CBUS_ADDR(HCPU_MPI_SBUS_ADDR_2_CBUS_ADDR(address));
}

static bool hibernate_dma_targets_mpi(uint32_t source, uint32_t destination)
{
    return hibernate_mpi_address(source) || hibernate_mpi_address(destination);
}

static bool hibernate_hcpu_dma_idle(void)
{
    if (!(hwp_hpsys_rcc->ENR1 & HPSYS_RCC_ENR1_DMAC1)) return true;
    GPDMA_Channel_TypeDef *const channels[] = {
        (GPDMA_Channel_TypeDef *)DMA1_Channel1, (GPDMA_Channel_TypeDef *)DMA1_Channel2,
        (GPDMA_Channel_TypeDef *)DMA1_Channel3, (GPDMA_Channel_TypeDef *)DMA1_Channel4,
        (GPDMA_Channel_TypeDef *)DMA1_Channel5, (GPDMA_Channel_TypeDef *)DMA1_Channel6,
        (GPDMA_Channel_TypeDef *)DMA1_Channel7, (GPDMA_Channel_TypeDef *)DMA1_Channel8,
    };
    if ((hwp_gpdma1->LCR1 | hwp_gpdma1->LCR2 | hwp_gpdma1->LCR3 | hwp_gpdma1->LCR4 |
         hwp_gpdma1->LCR5 | hwp_gpdma1->LCR6 | hwp_gpdma1->LCR7 | hwp_gpdma1->LCR8) &
        (GPDMA_LCR1_START | GPDMA_LCR1_BUSY))
        return false;

    /* Peripheral-to-SRAM transfers, including console RX, remain safe. */
    for (unsigned i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i)
    {
        GPDMA_Channel_TypeDef *channel = channels[i];
        uint32_t control = channel->CCR;
        if (!(control & GPDMA_CCR1_EN) ||
            !hibernate_dma_targets_mpi(channel->CPAR, channel->CM0AR))
            continue;
        if ((control & GPDMA_CCR1_CIRC) ||
            (channel->CNDTR & GPDMA_CNDTR1_NDT) ||
            (channel->CSR & GPDMA_CSR1_DSTNDT))
            return false;
        /* A completed polled SD transfer can leave EN set with both counts zero. */
        channel->CCR = control & ~GPDMA_CCR1_EN;
        if (channel->CCR & GPDMA_CCR1_EN) return false;
    }
    return true;
}

static bool hibernate_lcpu_dma_idle(void)
{
    if (!(hwp_lpsys_rcc->ENR1 & LPSYS_RCC_ENR1_DMAC2)) return true;
    const DMA_Channel_TypeDef *const channels[] = {
        DMA2_Channel1, DMA2_Channel2, DMA2_Channel3, DMA2_Channel4,
        DMA2_Channel5, DMA2_Channel6, DMA2_Channel7, DMA2_Channel8,
    };
    for (unsigned i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i)
    {
        const DMA_Channel_TypeDef *channel = channels[i];
        if ((channel->CCR & DMAC_CCR1_EN) &&
            hibernate_dma_targets_mpi(channel->CPAR, channel->CM0AR))
            return false;
    }
    return true;
}

HAL_RAM_RET_CODE_SECT(epd_hibernate,
                     static __attribute__((noinline)) board_power_error_t epd_hibernate(FLASH_HandleTypeDef *flash))
{
    /* Keep LPSYS active throughout the handoff to HAL_PMU_EnterHibernate. */
    HAL_HPAON_WakeCore(CORE_ID_LCPU);
    if (!hibernate_lcpu_dma_idle())
    {
        HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
        return BOARD_POWER_WAIT_DMA;
    }
    HAL_RCC_Reset_and_Halt_LCPU(1);
    __DSB();
    if (!HAL_HPAON_IS_LP_ACTIVE())
    {
        HAL_RCC_ReleaseLCPU();
        HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
        return BOARD_POWER_WAIT_LCPU;
    }

    SCB_CleanInvalidateDCache();
    __DSB();
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO_1V8, false, true);

    /* MPI2 uses hardware CS. The boot ROM releases DPD at the next boot. */
    HAL_FLASH_DEEP_PWRDOWN(flash);
    HAL_Delay_us(50);
    HAL_PMU_EnterHibernate();
    for (;;) __NOP();
}
#endif

/* Called with interrupts masked, after all reversible preparation is complete. */
static board_power_error_t power_enter_hibernate(void)
{
    uint32_t wake_mode = hwp_pmuc->WKUP_MODE;
    uint32_t wake_enable = hwp_pmuc->WER;
    board_power_error_t error = BOARD_POWER_ERROR_WAKEUP;
    if (power_wakeup_configure())
    {
#ifdef EPD_BOARD_SF32LB57_EPD
        bsp_epd_poweroff_io();
#endif
#ifdef BSP_USING_NOR_FLASH2
        rt_flash_wait_idle(MPI2_MEM_BASE);
#endif
#ifdef EPD_BOARD_SF32LB57_EPD
        FLASH_HandleTypeDef *flash = rt_flash_get_handle_by_addr(MPI2_MEM_BASE);
        if (!hibernate_hcpu_dma_idle())
            error = BOARD_POWER_WAIT_DMA;
        else if (flash == NULL || flash->isNand || flash->cs_ctrl ||
                 !(flash->Instance->CR & MPI_CR_EN))
            error = BOARD_POWER_ERROR_STORAGE;
        else
            error = epd_hibernate(flash);
#else
        HAL_PMU_EnterHibernate();
        error = BOARD_POWER_ERROR_RETURNED;
#endif
    }
    hwp_pmuc->WKUP_MODE = wake_mode;
    hwp_pmuc->WER = wake_enable;
    return error;
}

static board_power_error_t power_finish_attempt(bool restart, bool *card_off)
{
    board_power_error_t error = BOARD_POWER_ERROR_NONE;
    /* Closed files have already been synchronized by their filesystem close.
     * Hold the shared I/O lock through the terminal operation: an in-flight
     * write finishes before this point, and no new managed I/O can begin.
     * SD mount/unmount ownership remains with the resident card service. */
    if (!storage_lock_for(rt_tick_from_millisecond(POWER_IO_LOCK_TIMEOUT_MS)))
        return BOARD_POWER_ERROR_STORAGE;
#ifdef RT_USING_PM
    if (!restart) rt_pm_request(PM_SLEEP_MODE_IDLE);
#endif
    if (storage_changing() || storage_has_writer())
        error = BOARD_POWER_ERROR_STORAGE;
    else if (board_pm_key_pressed())
        error = BOARD_POWER_WAIT_KEY;
    else if (!restart && !board_pm_display_idle())
        error = BOARD_POWER_ERROR_DISPLAY;
#ifdef EPD_COMPUTE_ACPU
    else if (!restart && !epd_compute_transport_parked())
        error = BOARD_POWER_WAIT_COMPUTE;
#endif
    else if (!restart && !*card_off)
    {
        rt_err_t result = board_sdcard_poweroff_ready() ? storage_prepare_poweroff() : -RT_EBUSY;
        if (result == RT_EOK) *card_off = true;
        else
        {
            if (result != -RT_EBUSY && power_error != BOARD_POWER_ERROR_STORAGE)
                LOG_W("TF card poweroff preparation failed: %d", result);
            error = BOARD_POWER_ERROR_STORAGE;
        }
    }

    if (error == BOARD_POWER_ERROR_NONE && !restart)
    {
        /* Drain touch I/O and gate new samples before cutting its supply. */
        power_touch_suspended = true;
        if (bsp_touch_power_hold_begin() != RT_EOK) error = BOARD_POWER_ERROR_TOUCH;
    }

    if (error == BOARD_POWER_ERROR_NONE)
    {
        if (!restart)
        {
            BSP_TP_PowerDown();
            LOG_I("poweroff: storage and touch off, entering Hibernate (PERI_LDO=0x%lx)",
                  (unsigned long)hwp_pmuc->PERI_LDO);
        }
        rt_base_t level = rt_hw_interrupt_disable();
        if (restart)
        {
            if (power_restart) rt_hw_cpu_reset();
            error = BOARD_POWER_ERROR_RETURNED;
        }
        else if (board_pm_key_pressed())
            error = BOARD_POWER_WAIT_KEY;
#ifdef EPD_COMPUTE_ACPU
        else if (!epd_compute_transport_parked())
            error = BOARD_POWER_WAIT_COMPUTE;
#endif
        else
            error = power_enter_hibernate();
        rt_hw_interrupt_enable(level);
        if (!restart)
        {
            BSP_TP_PowerUp();
            (void)bsp_touch_power_hold_end();
        }
    }
#ifdef RT_USING_PM
    if (!restart) rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
    storage_unlock();
    return error;
}

static void power_finish(void *parameter)
{
    (void)parameter;
    bool card_off = false;
    bool restart = power_restart;
    if (!restart && !power_light_suspended)
    {
        power_phase = POWER_REQUESTED;
        epd_power_notify();
        return;
    }
    if (!restart) LOG_I("poweroff: finalizing storage and touch");
    for (;;)
    {
        board_power_error_t error = power_finish_attempt(restart, &card_off);
        if (restart)
        {
            power_fail(error);
            return;
        }
        power_wait(error);
        rt_thread_mdelay(100);
    }
}

void board_power_process(void)
{
    power_phase_t phase = power_phase_get();
    if (power_light_suspended && (phase == POWER_SAVE || phase == POWER_STOP))
        frontlight_process();
    if (phase == POWER_REQUESTED)
    {
        if (!power_restart && system_reset_factory_busy())
        {
            power_fail(BOARD_POWER_ERROR_BUSY);
            return;
        }
        if (!power_restart)
        {
            usb_card_reader_status_t reader;
            usb_card_reader_status(&reader);
            if (reader.enabled)
            {
                (void)usb_card_reader_set_enabled(false);
                return;
            }
            if (reader.busy)
            {
                power_wait(BOARD_POWER_WAIT_CARD_READER);
                return;
            }
        }
        portal_power_prepare();
        power_portal_paused = true;
        if (!power_restart)
        {
            frontlight_set_suspended(true);
            power_light_suspended = true;
            frontlight_process();
        }
        power_phase_started = rt_tick_get();
        power_phase = POWER_SAVE;
        return;
    }
    if (phase == POWER_IDLE && power_portal_paused)
    {
        portal_power_cancel();
        power_portal_paused = false;
    }
    if (phase == POWER_RECOVER)
    {
        if (!storage_set_sleep(false)) return;
        /* Restore system policy; touch failure stays gated and is reported
         * separately by board_pm_touch_resume_result(). */
        if (power_touch_suspended && !board_pm_set_system_sleep(false)) return;
        power_touch_suspended = false;
        if (!epd_power_shutdown_cancel()) return;
        power_wallpaper_started = false;
        if (power_light_suspended) frontlight_set_suspended(false);
        power_light_suspended = false;
        power_phase = POWER_IDLE;
        portal_power_cancel();
        power_portal_paused = false;
        return;
    }
    if (phase == POWER_SAVE)
    {
        app_installer_process();
        bool expired = (rt_tick_t)(rt_tick_get() - power_phase_started) >=
                       rt_tick_from_millisecond(power_restart ? POWER_SAVE_TIMEOUT_MS : 2000);
        board_power_error_t pending = BOARD_POWER_ERROR_NONE;
        if (!power_restart && !system_reset_prepare_poweroff()) pending = BOARD_POWER_WAIT_RESET;
        else if (app_installer_busy()) pending = BOARD_POWER_WAIT_INSTALLER;
        else if (usb_card_reader_active()) pending = BOARD_POWER_WAIT_CARD_READER;
        else if (storage_changing()) pending = BOARD_POWER_ERROR_STORAGE;
        if (pending != BOARD_POWER_ERROR_NONE)
        {
            if (power_restart) power_fail(BOARD_POWER_ERROR_BUSY);
            else if (expired) power_wait(pending);
            return;
        }
        if (app_service_flush())
        {
            power_phase_started = rt_tick_get();
            power_phase = POWER_STOP;
            app_service_stop_modules();
        }
        else if (expired)
        {
            if (power_restart) power_fail(BOARD_POWER_ERROR_SAVE);
            else power_wait(BOARD_POWER_ERROR_SAVE);
        }
        return;
    }
    if (phase != POWER_STOP) return;
    if (!app_catalog_release_storage())
    {
        if ((rt_tick_t)(rt_tick_get() - power_phase_started) >=
            rt_tick_from_millisecond(POWER_DRAIN_TIMEOUT_MS))
        {
            if (power_restart) power_fail(BOARD_POWER_ERROR_BUSY);
            else power_wait(BOARD_POWER_ERROR_BUSY);
        }
        return;
    }
    if (!power_restart)
    {
        if (!power_light_suspended)
        {
            power_phase_started = rt_tick_get();
            frontlight_set_suspended(true);
            power_light_suspended = true;
            frontlight_process();
        }
        power_wallpaper_started = true;
        rt_err_t result = epd_power_shutdown_prepare();
        if (result == -RT_EBUSY) return;
        if (result != RT_EOK)
        {
            power_wait(BOARD_POWER_ERROR_PERIPHERALS);
            return;
        }
        frontlight_snapshot_t light;
        frontlight_get(&light);
        if (!light.output_valid || light.output_percent || light.output_pending)
        {
            if ((rt_tick_t)(rt_tick_get() - power_phase_started) <
                rt_tick_from_millisecond(POWER_FRONTLIGHT_TIMEOUT_MS)) return;
            bsp_frontlight_power_off();
            power_wait(BOARD_POWER_ERROR_FRONTLIGHT);
        }
    }
    /* The shared ADC key line must be released before enabling level wakeup. */
    if (board_pm_key_pressed())
    {
        if ((rt_tick_t)(rt_tick_get() - power_phase_started) >=
            rt_tick_from_millisecond(POWER_DRAIN_TIMEOUT_MS))
        {
            if (power_restart) power_fail(BOARD_POWER_ERROR_BUSY);
            else power_wait(BOARD_POWER_WAIT_KEY);
        }
        return;
    }
    rt_thread_t worker = rt_thread_create("poweroff", power_finish, RT_NULL,
                                          3072, RT_THREAD_PRIORITY_MAX - 2, 10);
    if (!worker)
    {
        if (power_restart) power_fail(BOARD_POWER_ERROR_START);
        else power_wait(BOARD_POWER_ERROR_START);
        return;
    }
    power_phase = POWER_FINALIZE;
    if (rt_thread_startup(worker) != RT_EOK)
    {
        rt_thread_delete(worker);
        if (power_restart) power_fail(BOARD_POWER_ERROR_START);
        else
        {
            power_phase = POWER_STOP;
            power_wait(BOARD_POWER_ERROR_START);
        }
    }
}

void board_prepare_to_sleep(void)
{
    (void)board_power_request(false);
}

#ifdef RT_USING_FINSH
static void sys_poweroff(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: sys_poweroff\n");
        return;
    }
    bool pending = board_power_shutdown_pending();
    if (board_power_request(false))
        rt_kprintf("sys_poweroff: %s\n", pending ? "already pending" : "requested");
    else
        rt_kprintf("sys_poweroff: factory reset or restart in progress\n");
}
MSH_CMD_EXPORT(sys_poweroff, Request system power off);
#endif
