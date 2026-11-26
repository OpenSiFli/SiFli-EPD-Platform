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
#include "ui/platform/app_module.h"
#include "platform/epd_operation_runtime.h"
#include "platform/epd_resource_runtime.h"
#include "ui/platform/epd_power.h"
#include "boards/board_pm.h"
#include "ui/launcher.h"
#include "ui/ui_navigation.h"
#include "ui/ui_shell.h"
#include "ui/ui_control_center.h"
#include "platform/epd_panel_init.h"
#include "platform/epd_ui_schedule.h"
#include "ui/platform/epd_render_profile.h"

#include "board_service.h" /* 板级服务（TF 卡文件系统挂载） */

#if LVGL_VERSION_MAJOR != 9
#error "This application requires LVGL v9."
#endif

#define DBG_TAG "main"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <string.h>
typedef struct { uint32_t count, total, maximum, slow; } ui_duration_t;
static ui_duration_t ui_durations[5];
static const char *const ui_phases[] = {"power", "services", "lvgl", "deferred", "power_tail"};
static const char *const ui_render_phases[] = {
    "glyph_dsc", "glyph_bitmap", "settle", "copy", "refresh", "pre_render", "render", "flush", "flush_wait"
};
static void ui_render_event(lv_event_t *event)
{
    static uint32_t refresh, layout, draw, flush, wait;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_REFR_START:
        refresh = epd_render_profile_begin(EPD_RENDER_REFRESH);
        layout = epd_render_profile_begin(EPD_RENDER_PRE_RENDER);
        break;
    case LV_EVENT_RENDER_START:
        epd_render_profile_end(EPD_RENDER_PRE_RENDER, layout); layout = 0;
        draw = epd_render_profile_begin(EPD_RENDER_DRAW);
        break;
    case LV_EVENT_RENDER_READY:
        epd_render_profile_end(EPD_RENDER_DRAW, draw); draw = 0;
        break;
    case LV_EVENT_REFR_READY:
        epd_render_profile_end(EPD_RENDER_PRE_RENDER, layout); layout = 0;
        epd_render_profile_end(EPD_RENDER_DRAW, draw); draw = 0;
        epd_render_profile_end(EPD_RENDER_REFRESH, refresh); refresh = 0;
        break;
    case LV_EVENT_FLUSH_START: flush = epd_render_profile_begin(EPD_RENDER_FLUSH); break;
    case LV_EVENT_FLUSH_FINISH: epd_render_profile_end(EPD_RENDER_FLUSH, flush); flush = 0; break;
    case LV_EVENT_FLUSH_WAIT_START: wait = epd_render_profile_begin(EPD_RENDER_FLUSH_WAIT); break;
    case LV_EVENT_FLUSH_WAIT_FINISH: epd_render_profile_end(EPD_RENDER_FLUSH_WAIT, wait); wait = 0; break;
    default: break;
    }
}
static rt_tick_t ui_duration_record(unsigned phase, rt_tick_t started)
{
    rt_tick_t now = rt_tick_get();
    uint32_t elapsed = now - started;
    ui_duration_t *item = &ui_durations[phase];
    ++item->count;
    item->total += elapsed;
    if (elapsed > item->maximum) item->maximum = elapsed;
    if (elapsed >= rt_tick_from_millisecond(50)) ++item->slow;
    return now;
}
static void ui_perf(int argc, char **argv)
{
    ui_duration_t copy[5];
    bool reset = argc == 2 && !strcmp(argv[1], "reset");
    rt_base_t level = rt_hw_interrupt_disable();
    memcpy(copy, ui_durations, sizeof(copy));
    if (reset) memset(ui_durations, 0, sizeof(ui_durations));
    epd_render_diagnostic_window(reset);
    rt_hw_interrupt_enable(level);
    if (reset) { rt_kprintf("ui_perf reset: render window started\n"); return; }
    epd_render_diagnostic_t render;
    epd_render_diagnostic_snapshot(&render);
    for (unsigned i = 0; i < 5; ++i)
        rt_kprintf("ui_perf %s count=%lu total_ms=%lu max_ms=%lu over50=%lu\n", ui_phases[i],
            (unsigned long)copy[i].count, (unsigned long)((uint64_t)copy[i].total * 1000 / RT_TICK_PER_SECOND),
            (unsigned long)((uint64_t)copy[i].maximum * 1000 / RT_TICK_PER_SECOND), (unsigned long)copy[i].slow);
    for (unsigned i = 0; i < EPD_RENDER_DIAGNOSTIC_COUNT; ++i)
        rt_kprintf("ui_render %s count=%lu total_ms=%lu\n", ui_render_phases[i],
            (unsigned long)render.stage[i].calls,
            (unsigned long)((uint64_t)render.stage[i].ticks * 1000 / RT_TICK_PER_SECOND));
}
MSH_CMD_EXPORT(ui_perf, Report UI and render durations or reset the sampling window);

static volatile uint8_t ui_inspection_pending;
static void ui_state(int argc, char **argv)
{
    ui_inspection_pending = argc == 2 && !strcmp(argv[1], "tree") ? 2 : 1;
    epd_power_notify();
}
MSH_CMD_EXPORT(ui_state, Inspect visible UI state or tree on the UI thread);

#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED && LV_USE_SPAN
static void ui_inspect_spangroup(lv_obj_t *obj, const lv_area_t *area)
{
    lv_area_t clip = *area;
    if (!lv_obj_area_is_visible(obj, &clip)) return;

    uint32_t count = lv_spangroup_get_span_count(obj);
    size_t bytes = 0;
    uint32_t hash = UINT32_C(2166136261);
    for (uint32_t i = 0; i < count; ++i) {
        lv_span_t *span = lv_spangroup_get_child(obj, (int32_t)i);
        const unsigned char *text = (const unsigned char *)lv_span_get_text(span);
        if (!text) continue;
        for (; *text; ++text) {
            hash ^= *text;
            hash *= UINT32_C(16777619);
            ++bytes;
        }
    }
    rt_kprintf("ui_span [%ld,%ld,%ld,%ld] clip=[%ld,%ld,%ld,%ld] height=%ld spans=%lu bytes=%lu fnv1a=%08lx\n",
               (long)area->x1, (long)area->y1, (long)area->x2, (long)area->y2,
               (long)clip.x1, (long)clip.y1, (long)clip.x2, (long)clip.y2,
               (long)lv_area_get_height(area), (unsigned long)count,
               (unsigned long)bytes, (unsigned long)hash);
}
#endif

#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
static bool ui_inspect_long_label(lv_obj_t *obj, const lv_area_t *area, const char *text)
{
    size_t bytes = strlen(text);
    if (bytes < 256u) return false;

    lv_area_t clip = *area;
    if (!lv_obj_area_is_visible(obj, &clip)) return true;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= (unsigned char)text[i];
        hash *= UINT32_C(16777619);
    }
    rt_kprintf("ui_label_summary [%ld,%ld,%ld,%ld] clip=[%ld,%ld,%ld,%ld] height=%ld bytes=%lu fnv1a=%08lx\n",
               (long)area->x1, (long)area->y1, (long)area->x2, (long)area->y2,
               (long)clip.x1, (long)clip.y1, (long)clip.x2, (long)clip.y2,
               (long)lv_area_get_height(area), (unsigned long)bytes, (unsigned long)hash);
    return true;
}
#endif

static void ui_inspect_object(lv_obj_t *obj)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    if (lv_obj_check_type(obj, &lv_label_class) && lv_obj_is_visible(obj))
    {
        const char *text = lv_label_get_text(obj);
        if (!text) text = "";
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED
        if (!ui_inspect_long_label(obj, &area, text))
#endif
            rt_kprintf("ui_label [%ld,%ld,%ld,%ld] %s\n", (long)area.x1,
                       (long)area.y1, (long)area.x2, (long)area.y2, text);
    }
#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED && LV_USE_SPAN
    if (lv_obj_check_type(obj, &lv_spangroup_class)) ui_inspect_spangroup(obj, &area);
#endif
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        ui_inspect_object(lv_obj_get_child(obj, i));
}

static void ui_inspection_process(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint8_t request = ui_inspection_pending;
    ui_inspection_pending = 0;
    rt_hw_interrupt_enable(level);
    if (!request) return;
    rt_kprintf("ui_state locked=%u modal=%u center=%u navigation=%u\n",
        (unsigned)ui_shell_locked(), (unsigned)ui_shell_modal_visible(),
        (unsigned)ui_control_center_visible(), (unsigned)ui_navigation_busy());
    if (request == 2)
    {
        ui_inspect_object(lv_screen_active());
        ui_inspect_object(lv_layer_top());
        ui_inspect_object(lv_layer_sys());
    }
}
#endif

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

#if LV_USE_TINY_TTF && defined(PSRAM_CACHE_WB)
    lv_draw_buf_get_font_handlers()->flush_cache_cb = font_draw_buf_flush_cache;
#endif
    epd_memory_result_t memory = app_memory_init();
    if (memory != EPD_MEMORY_OK)
    {
        LOG_E("Application memory initialization failed: %u", (unsigned)memory);
        return -RT_ERROR;
    }
    const epd_resource_runtime_config_t resources = {.foreground_page = app_memory_foreground_page};
    if (!epd_operation_runtime_init() || epd_resource_runtime_init(&resources) != 0)
    {
        LOG_E("Application resource runtime initialization failed");
        return -RT_ERROR;
    }
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
#ifdef RT_USING_FINSH
    lv_display_add_event_cb(lv_display_get_default(), ui_render_event, LV_EVENT_ALL, NULL);
#endif
    if (system_reset_blocks_apps())
    {
        ui_navigation_run_page("settings", UI_PAGE_RESET_SETTINGS);
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
#ifdef RT_USING_FINSH
        rt_tick_t phase_started = rt_tick_get();
#endif
        epd_power_process();
#ifdef RT_USING_FINSH
        phase_started = ui_duration_record(0, phase_started);
#endif
        ui_app_process();
#ifdef RT_USING_FINSH
        phase_started = ui_duration_record(1, phase_started);
#endif
        if (!epd_power_display_parked()) lv_timer_handler();
#ifdef RT_USING_FINSH
        phase_started = ui_duration_record(2, phase_started);
#endif
        ui_app_process_deferred();
        epd_operation_runtime_ui_pump(8);
        epd_resource_runtime_ui(8);
        app_modules_process();
#ifdef RT_USING_FINSH
        phase_started = ui_duration_record(3, phase_started);
#endif
        epd_power_process();
#ifdef RT_USING_FINSH
        ui_duration_record(4, phase_started);
        ui_inspection_process();
#endif
        uint32_t ms = epd_power_display_parked() ? UINT32_MAX :
            ui_app_next_delay(epd_ui_schedule_next_delay());
        ms = app_service_ui_delay(ms);
        ms = epd_power_next_delay(ms);
        epd_power_wait(ms);
    }

    return 0;
}
