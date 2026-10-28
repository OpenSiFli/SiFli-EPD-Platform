/**
 * @file main.c
 * @brief LVGL v9 Launcher, data services and display event loop.
 */
#include "app_service_internal.h"
#include "reminder_internal.h"
#include "network_internal.h"
#include "ui_settings_internal.h"
#include <rtthread.h>
#include "bf0_hal.h"
#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "src/draw/lv_draw_buf_private.h"
#include "ui/ui_app.h"
#include "ui/ui_settings.h"
#include "bt_pan.h"
#include "network.h"
#include "network_time.h"
#include "app_service.h"
#include "system_reset.h"
#include "device_identity.h"
#include "wlan_service.h"
#include "ui/platform/app_installer.h"
#include "ui/platform/app_recovery.h"
#include "ui/platform/app_memory.h"
#include "ui/platform/epd_power.h"
#include "boards/board_pm.h"
#include "ui/launcher.h"
#include "ui/ui_navigation.h"
#include "platform/epd_panel_init.h"
#include "platform/epd_ui_schedule.h"

#include "board_service.h" /* 板级服务（TF 卡文件系统挂载） */

#if LVGL_VERSION_MAJOR != 9
#error "This application requires LVGL v9."
#endif

#define DBG_TAG "main"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#if LV_USE_TINY_TTF && defined(PSRAM_CACHE_WB)
static void font_draw_buf_flush_cache(const lv_draw_buf_t *draw_buf, const lv_area_t *area)
{
    LV_UNUSED(area);
    /* Write back the whole glyph, including row padding, before EPIC reads it. */
    mpu_dcache_clean(draw_buf->data, (uint32_t)draw_buf->header.stride * draw_buf->header.h);
}
#endif

int main(void)
{
    if (epd_power_init() != RT_EOK) return -RT_ERROR;
    board_pm_init();
    /* Allocate display buffers before starting the Bluetooth services. */
    rt_err_t ret = littlevgl2rtt_init("lcd");
    if (ret != RT_EOK)
    {
        LOG_E("LVGL init failed: %d", ret);
        return ret;
    }

#if LV_USE_TJPGD
    /* Applications decode JPEG in their workers and submit RGB pixels.
     * Remove the LVGL probe before drawing starts: its 4 KiB local buffer
     * exceeds the EPIC draw thread's stack even for non-JPEG sources. */
    lv_tjpgd_deinit();
#endif

    app_memory_init();
#if LV_USE_TINY_TTF && defined(PSRAM_CACHE_WB)
    lv_draw_buf_get_font_handlers()->flush_cache_cb = font_draw_buf_flush_cache;
#endif
    ret = epd_panel_boot();
    if (ret != RT_EOK)
    {
        LOG_E("EPD initialization refresh failed: %d", ret);
        return ret;
    }

    /* 内部文件系统挂载到 /flash，TF 卡挂载到 /sdcard，并启动热插拔服务。 */
    board_start_filesystem();

    system_reset_init();
    if (!system_reset_blocks_apps()) app_installer_init();
    ret = network_init();
    if (ret != RT_EOK) LOG_W("network init failed: %d", ret);
    char device_name[DEVICE_IDENTITY_NAME_CAPACITY] = DEVICE_IDENTITY_DEFAULT_NAME;
    ret = device_identity_init();
    if (ret != RT_EOK) LOG_W("device name load failed: %d", ret);
    device_identity_get_name(device_name, sizeof(device_name));
    ret = btpan_init(device_name);
    if (ret != RT_EOK) LOG_W("btpan init failed: %d", ret);
    if (!system_reset_blocks_apps()) ui_settings_init();
    ret = wlan_service_start(!system_reset_blocks_apps() && ui_settings_enabled(UI_SETTING_WIFI));
    if (ret != RT_EOK && ret != -RT_ENOSYS) LOG_W("WLAN service init failed: %d", ret);
    ret = network_time_init();
    if (ret != RT_EOK) LOG_W("time init failed: %d", ret);

    ret = ui_app_init();
    if (ret != RT_EOK)
    {
        LOG_E("Launcher init failed: %d", ret);
        return ret;
    }
    if (system_reset_blocks_apps())
    {
        ui_navigation_run("settings");
        launcher_open(UI_PAGE_RESET_SETTINGS);
    }
    else if (app_recovery_active()) launcher_open(UI_PAGE_SETTINGS);
    if (!system_reset_blocks_apps()) reminder_start();
    ret = epd_ui_schedule_start();
    if (ret != RT_EOK)
    {
        LOG_E("UI scheduling initialization failed: %d", ret);
        return ret;
    }

    while (1)
    {
        epd_power_process();
        ui_app_process();
        uint32_t ms = epd_power_sleeping() ? UINT32_MAX : lv_timer_handler();
        ui_app_process_deferred();
        epd_power_process();
        if (epd_power_sleeping()) ms = UINT32_MAX;
        else if (ms == LV_NO_TIMER_READY || ms > 20)
        {
            ms = 20;
        }
        ms = app_service_ui_delay(ms);
        ms = epd_power_next_delay(ms);
        epd_power_wait(ms);
    }

    return 0;
}
