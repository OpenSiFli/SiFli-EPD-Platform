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
#include "bsp_touch_pm.h"
#include "frontlight.h"
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


#ifndef _WIN32
#include "drv_flash.h"
#endif

#include "battery/battery.h"

#define DBG_TAG "board"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 板级/SDK 提供的电源与触摸控制接口 */
extern void BSP_TP_PowerUp(void);
extern void BSP_TP_PowerDown(void);

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
#if defined(BSP_USING_CHARGER) && !1
        else if ((PMUC_WSR_PIN0 << (pm_get_charger_pin_wakeup())) & pm_get_wakeup_src())
        {
            /* 充电插入唤醒：正常开机 */
        }
#endif
        else if (PMUC_WSR_PIN_ALL & pm_get_wakeup_src())
        {
            /* 57 平台：按键/充电等引脚唤醒统一正常开机 */
        }
        else if (0 == pm_get_wakeup_src())
        {
            RT_ASSERT(0);
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
    /* Configure the PA51 card-detect input before starting storage services. */
    board_sdcard_prepare();
    storage_init();
}

typedef enum { POWER_IDLE, POWER_SAVE, POWER_STOP, POWER_FINALIZE, POWER_RECOVER } power_phase_t;
#define POWER_SAVE_TIMEOUT_MS 35000
static power_phase_t power_phase;
static board_power_error_t power_error;
static bool power_restart;
static bool power_light_suspended;
static bool power_wallpaper_started;
static bool power_touch_suspended;
static bool power_portal_paused;
static rt_tick_t power_save_started;

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

board_power_error_t board_power_error(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    board_power_error_t error = power_error;
    rt_hw_interrupt_enable(level);
    return error;
}

static void power_fail(board_power_error_t error)
{
    LOG_W("power request stopped: error=%d", (int)error);
    rt_base_t level = rt_hw_interrupt_disable();
    power_error = error;
    /* Device output is restored by the UI, never by the terminal worker. */
    power_phase = (power_light_suspended || power_wallpaper_started) ? POWER_RECOVER : POWER_IDLE;
    rt_hw_interrupt_enable(level);
    epd_power_notify();
}

bool board_power_request(bool restart)
{
    if (board_power_pending()) return false;
    if (system_reset_busy() || app_installer_busy() || storage_changing() || usb_card_reader_active())
    {
        power_fail(BOARD_POWER_ERROR_BUSY);
        return false;
    }
    power_restart = restart;
    portal_power_prepare();
    power_portal_paused = true;
    power_error = BOARD_POWER_ERROR_NONE;
    power_save_started = rt_tick_get();
    power_phase = POWER_SAVE;
    epd_power_notify();
    return true;
}

static bool power_key_pressed(void)
{
    return rt_pin_read(BSP_KEY1_PIN) != 0;
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

static void power_finish(void *parameter)
{
    (void)parameter;
    board_power_error_t error = BOARD_POWER_ERROR_NONE;
    bool wakeup_changed = false;
    /* Closed files have already been synchronized by their filesystem close.
     * Hold the shared I/O lock through the terminal operation: an in-flight
     * write finishes before this point, and no new managed I/O can begin.
     * SD mount/unmount ownership remains with the resident card service. */
    storage_lock();
    if (storage_changing() || storage_has_writer() || power_key_pressed())
        error = BOARD_POWER_ERROR_BUSY;
    else if (!power_restart && !board_pm_display_idle())
        error = BOARD_POWER_ERROR_BUSY;
    else if (!power_restart)
    {
        wakeup_changed = true;
        if (!power_wakeup_configure()) error = BOARD_POWER_ERROR_WAKEUP;
    }

    if (error == BOARD_POWER_ERROR_NONE && !power_restart)
    {
        /* Drain touch I/O and gate new samples before cutting its supply. */
        power_touch_suspended = true;
        if (bsp_touch_power_hold_begin() != RT_EOK) error = BOARD_POWER_ERROR_TOUCH;
    }

    if (error == BOARD_POWER_ERROR_NONE)
    {
        if (!power_restart)
        {
#ifdef RT_USING_PM
            rt_pm_request(PM_SLEEP_MODE_IDLE);
#endif
            battery_stop();
            BSP_TP_PowerDown();
        }
        rt_base_t level = rt_hw_interrupt_disable();
        if (!power_restart && power_key_pressed())
            error = BOARD_POWER_ERROR_BUSY;
        else
        {
            if (power_restart)
                rt_hw_cpu_reset();
            else
            {
                HAL_PMU_EnterHibernate();
            }
            /* A returning operation is a failure, never shutdown success. */
            error = BOARD_POWER_ERROR_RETURNED;
        }
        rt_hw_interrupt_enable(level);
        if (!power_restart)
        {
            BSP_TP_PowerUp();
            (void)bsp_touch_power_hold_end();
            battery_init();
#ifdef RT_USING_PM
            rt_pm_release(PM_SLEEP_MODE_IDLE);
#endif
        }
    }
#if defined(RT_USING_PM)
    /* Also restore after a partial wake-source setup or touch-suspend failure. */
    if (wakeup_changed)
    {
        int8_t pin = HAL_HPAON_QueryWakeupPin(GET_GPIO_INSTANCE(BSP_KEY1_PIN),
                                            GET_GPIOx_PIN(BSP_KEY1_PIN));
        if (pin >= 0) (void)pm_enable_pin_wakeup((uint8_t)pin, AON_PIN_MODE_POS_EDGE);
    }
#else
    (void)wakeup_changed;
#endif
    storage_unlock();
    power_fail(error);
}

void board_power_process(void)
{
    power_phase_t phase = power_phase_get();
    if (phase == POWER_IDLE && power_portal_paused)
    {
        portal_power_cancel();
        power_portal_paused = false;
    }
    if (phase == POWER_RECOVER)
    {
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
        if (app_installer_busy() || storage_changing())
            power_fail(BOARD_POWER_ERROR_BUSY);
        else if ((rt_tick_t)(rt_tick_get() - power_save_started) >=
                 rt_tick_from_millisecond(POWER_SAVE_TIMEOUT_MS))
            power_fail(BOARD_POWER_ERROR_SAVE);
        else if (app_service_flush())
        {
            power_phase = POWER_STOP;
            app_service_stop_modules();
        }
        /* A pending save keeps workers, storage sessions and UI alive. The
         * next UI loop polls again; timeout leaves their cleanup running. */
        return;
    }
    if (phase != POWER_STOP) return;
    if (!app_catalog_release_storage()) return;
    if (!power_restart)
    {
        power_wallpaper_started = true;
        rt_err_t result = epd_power_shutdown_prepare();
        if (result == -RT_EBUSY) return;
        if (result != RT_EOK)
        {
            power_fail(BOARD_POWER_ERROR_DISPLAY);
            return;
        }
        frontlight_set_suspended(true);
        power_light_suspended = true;
        frontlight_process();
        frontlight_snapshot_t light;
        frontlight_get(&light);
        if (!light.output_valid || light.output_percent || light.output_pending)
        {
            power_fail(BOARD_POWER_ERROR_FRONTLIGHT);
            return;
        }
    }
    /* The shared ADC key line must be released before enabling level wakeup. */
    if (power_key_pressed()) return;
    rt_thread_t worker = rt_thread_create("poweroff", power_finish, RT_NULL,
                                          2048, RT_THREAD_PRIORITY_MAX - 2, 10);
    if (!worker)
    {
        power_fail(BOARD_POWER_ERROR_START);
        return;
    }
    power_phase = POWER_FINALIZE;
    if (rt_thread_startup(worker) != RT_EOK)
    {
        rt_thread_delete(worker);
        power_fail(BOARD_POWER_ERROR_START);
    }
}

void board_prepare_to_sleep(void)
{
    (void)board_power_request(false);
}
