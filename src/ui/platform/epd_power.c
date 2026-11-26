#include "app_service_internal.h"
#include "storage_internal.h"
#include "platform/epd_lock_internal.h"
#include "ui_settings_internal.h"
#include "epd_power.h"
#include "epd_power_policy.h"
#include "epd_input.h"
#include "epd_lock.h"
#include "epd_refresh.h"
#include "ui_shell.h"
#include "ui_settings.h"
#include "ui_font.h"
#include "ui_internal.h"
#include "apps/reading_fonts.h"
#include "boards/board_pm.h"
#include "boards/board_service.h"
#include "app_service.h"
#include "sleep_guard.h"
#include "epd_compute.h"
#include "frontlight.h"
#include "reading_font.h"
#include "wallpaper.h"
#include "storage.h"
#include "system_reset_guard.h"
#include "portal/portal.h"
#include "net/network_time.h"
#include "net/bt_pan.h"
#include "boards/battery/battery.h"
#ifdef RT_USING_PM
#include "bf0_pm.h"
#endif
#if defined(RT_USING_WIFI) && defined(RT_WLAN_DEVICE_STA_NAME)
#include "net/wlan_service.h"
#endif
#include <rthw.h>
#if defined(EPD_BOARD_SF32LB57_EPD) && defined(BSP_USING_TOUCHD) && defined(PM_DEEP_ENABLE)
#include "bsp_touch_pm.h"
#include "ui_control_center.h"
#include "ui_navigation.h"
#define EPD_FOREGROUND_STANDBY 1
#endif
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif

typedef enum { POWER_ACTIVE, POWER_PREPARING, POWER_DRAWING, POWER_DRAINING, POWER_ASLEEP, POWER_RESUMING } power_state_t;
static struct rt_event wake_event;
static bool initialized;
static volatile power_state_t state;
static volatile bool key_wakeup;
static volatile bool sleep_requested;
static bool manual_sleep;
static bool input_changed;
static bool time_suspending, time_parked;
static bool battery_suspending, bt_suspending;
#if defined(RT_USING_WIFI) && defined(RT_WLAN_DEVICE_STA_NAME)
static bool wlan_suspending;
#endif
static bool resume_user_wakeup;
static bool board_resumed;
static uint32_t prepare_started, last_activity;
#define POWER_PREPARE_TIMEOUT_MS 60000u
#define FOREGROUND_STANDBY_IDLE_MS 400u
#define FOREGROUND_STANDBY_MAX_WAIT_MS 500u
#define FOREGROUND_STANDBY_MIN_WAIT_MS 30u
static epd_power_policy_t policy;
static lv_obj_t *wallpaper;
static uint32_t wallpaper_submission;
typedef enum { SHUTDOWN_IDLE, SHUTDOWN_WAIT, SHUTDOWN_DRAW, SHUTDOWN_DRAIN,
               SHUTDOWN_PERIPHERALS, SHUTDOWN_RETRY, SHUTDOWN_READY } shutdown_state_t;
static shutdown_state_t shutdown_state;
static uint32_t shutdown_started, shutdown_reported;
typedef enum {
    PERIPHERAL_TIME, PERIPHERAL_BATTERY, PERIPHERAL_BLUETOOTH, PERIPHERAL_WLAN
} power_peripheral_t;
static const char *const peripheral_names[] = {"network time", "battery", "Bluetooth", "Wi-Fi"};
static power_peripheral_t shutdown_retry_peripheral;
#define SHUTDOWN_WALLPAPER_TIMEOUT_MS 2000u

/* The UI thread owns the SDK scene bit. Ordinary event waits keep it held;
 * only the completed display-to-sleep handoff releases the run requirement. */
static void ui_run_active(bool active)
{
#ifdef RT_USING_PM
    static bool held;
    if (held == active) return;
    if (active) pm_scenario_start(PM_SCENARIO_UI);
    else pm_scenario_stop(PM_SCENARIO_UI);
    held = active;
#else
    (void)active;
#endif
}

rt_err_t epd_power_init(void)
{
    if (initialized) return RT_EOK;
    rt_err_t result = rt_event_init(&wake_event, "ui_wake", RT_IPC_FLAG_FIFO);
    if (result == RT_EOK)
    {
        ui_run_active(true);
        initialized = true;
    }
    return result;
}

void epd_power_notify(void)
{
    if (!initialized) return;
    (void)rt_event_send(&wake_event, 1u);
}

bool epd_power_key_wakeup(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool consume = state != POWER_ACTIVE;
    if (consume && !key_wakeup)
    {
        key_wakeup = true;
        epd_input_consume_wakeup();
    }
    rt_hw_interrupt_enable(level);
    if (consume) epd_power_notify();
    return consume;
}

void epd_power_activity(void)
{
    if (state == POWER_ASLEEP) return;
    input_changed = true;
    last_activity = lv_tick_get();
    epd_power_policy_activity(&policy, last_activity);
    epd_power_notify();
}

bool epd_power_sleeping(void) { return state == POWER_ASLEEP; }

bool epd_power_display_parked(void)
{
    return epd_power_sleeping() || shutdown_state >= SHUTDOWN_PERIPHERALS;
}
bool epd_power_transitioning(void)
{
    return state != POWER_ACTIVE || shutdown_state != SHUTDOWN_IDLE;
}

static bool peripheral_resume(power_peripheral_t peripheral)
{
    switch (peripheral)
    {
    case PERIPHERAL_TIME:
        if (time_suspending || time_parked) network_time_resume();
        time_suspending = time_parked = false;
        return true;
    case PERIPHERAL_BATTERY:
        if (battery_suspending) battery_resume();
        battery_suspending = false;
        return true;
    case PERIPHERAL_BLUETOOTH:
        if (bt_suspending && btpan_resume() != RT_EOK) return false;
        bt_suspending = false;
        return true;
    case PERIPHERAL_WLAN:
#if defined(RT_USING_WIFI) && defined(RT_WLAN_DEVICE_STA_NAME)
        if (wlan_suspending) wlan_service_resume();
        wlan_suspending = false;
#endif
        return true;
    }
    return false;
}

static bool peripherals_resume(void)
{
    peripheral_resume(PERIPHERAL_WLAN);
    bool complete = peripheral_resume(PERIPHERAL_BLUETOOTH);
    peripheral_resume(PERIPHERAL_BATTERY);
    peripheral_resume(PERIPHERAL_TIME);
    return complete;
}

static rt_err_t peripherals_prepare(power_peripheral_t *waiting, bool shutdown)
{
    rt_err_t result;
    if (!time_suspending)
    {
        *waiting = PERIPHERAL_TIME;
        result = network_time_suspend();
        if (result != RT_EOK) return !shutdown && result == -RT_EBUSY ? -RT_ERROR : result;
        time_suspending = true;
    }
    if (!battery_suspending)
    {
        *waiting = PERIPHERAL_BATTERY;
        result = battery_suspend();
        if (result != RT_EOK) return !shutdown && result == -RT_EBUSY ? -RT_ERROR : result;
        battery_suspending = true;
    }
    /* Finish DNS/UDP work before taking either network interface down. */
    *waiting = PERIPHERAL_TIME;
    result = network_time_suspend_result();
    if (result != RT_EOK) return result;
    *waiting = PERIPHERAL_BATTERY;
    result = battery_suspend_result();
    if (result != RT_EOK) return result;
    if (!bt_suspending)
    {
        *waiting = PERIPHERAL_BLUETOOTH;
        result = btpan_suspend();
        if (result != RT_EOK) return result;
        bt_suspending = true;
    }
#if defined(RT_USING_WIFI) && defined(RT_WLAN_DEVICE_STA_NAME)
    if (!wlan_suspending)
    {
        *waiting = PERIPHERAL_WLAN;
        result = wlan_service_suspend();
        if (result != RT_EOK) return result;
        wlan_suspending = true;
    }
#endif
    *waiting = PERIPHERAL_BLUETOOTH;
    result = btpan_suspend_result();
    if (result != RT_EOK && result != -RT_EBUSY) return result;
#if defined(RT_USING_WIFI) && defined(RT_WLAN_DEVICE_STA_NAME)
    rt_err_t wlan_result = wlan_service_suspend_result();
    if (wlan_result != RT_EOK && (wlan_result != -RT_EBUSY || result == RT_EOK))
    {
        *waiting = PERIPHERAL_WLAN;
        return wlan_result;
    }
#endif
    return result;
}

static void resume_locked(bool user_wakeup)
{
    resume_user_wakeup = resume_user_wakeup || user_wakeup;
    if (manual_sleep)
    {
        rt_kprintf("sys_sleep: cancelled\n");
        manual_sleep = false;
    }
    /* SDK scene activation may wait for SDIO before raising the clocks.
     * Keep IRQs enabled and restore it before touching display resources. */
    ui_run_active(true);
    if (!board_resumed && !board_pm_set_system_sleep(false))
    {
        state = POWER_RESUMING;
        return;
    }
    board_resumed = true;
    if (!storage_set_sleep(false))
    {
        state = POWER_RESUMING;
        return;
    }
    sleep_guard_cancel();
    epd_compute_resume_dispatch();
    ui_font_reader_resume();
    if (!peripherals_resume())
    {
        state = POWER_RESUMING;
        return;
    }
    /* Keep ordinary drawing suppressed until the retained wallpaper is gone. */
    if (wallpaper)
    {
        lv_obj_delete(wallpaper);
        wallpaper = NULL;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    user_wakeup = resume_user_wakeup || key_wakeup;
    resume_user_wakeup = false;
    key_wakeup = false;
    state = POWER_ACTIVE;
    last_activity = lv_tick_get();
    board_resumed = false;
    rt_hw_interrupt_enable(level);
    portal_resume();
    app_service_resume();
    policy.timing = false;
    frontlight_set_suspended(board_power_shutdown_pending());
    if (user_wakeup)
    {
        epd_power_policy_activity(&policy, lv_tick_get());
        /* A removed application may have removed its lock subpage as well. */
        epd_lock_process();
        frontlight_wake();
        lv_display_trigger_activity(NULL);
    }
    if (board_pm_touch_resume_result() != RT_EOK)
        epd_power_policy_fail(&policy, ui_settings_sleep_timeout_ms());
    epd_refresh_set_maintenance(false);
    epd_refresh_request_interaction();
    epd_refresh_process();
}

static void prepare_failed(const char *stage, rt_err_t result)
{
    epd_power_policy_fail(&policy, ui_settings_sleep_timeout_ms());
    rt_kprintf("sleep: %s failed (%d), automatic retry paused\n", stage, result);
    resume_locked(false);
}

static bool can_prepare(const sleep_guard_snapshot_t *guard)
{
    return board_pm_available() && board_pm_touch_resume_result() == RT_EOK &&
           guard->held_count == guard->draining_count &&
           !board_power_pending() && !system_reset_blocks_apps() &&
           !storage_changing() && !storage_release_requested() &&
           !app_service_stopping() && !ui_shell_modal_visible() &&
           !board_pm_key_pressed();
}

static void shutdown_observe_submission(void)
{
    if (shutdown_state == SHUTDOWN_DRAW &&
        epd_refresh_submission() != wallpaper_submission)
    {
        shutdown_state = SHUTDOWN_DRAIN;
        epd_refresh_set_maintenance(true);
    }
}

rt_err_t epd_power_shutdown_prepare(void)
{
    if (!initialized) return -RT_ERROR;
    if (shutdown_state == SHUTDOWN_IDLE)
    {
        if (state != POWER_ACTIVE) resume_locked(false);
        if (state != POWER_ACTIVE) return -RT_EBUSY;
        files_release_storage();
        reading_fonts_release_storage();
        shutdown_started = lv_tick_get();
        shutdown_state = SHUTDOWN_WAIT;
        epd_refresh_set_maintenance(true);
    }
    shutdown_observe_submission();
    if (shutdown_state == SHUTDOWN_READY) return RT_EOK;
    if (shutdown_state == SHUTDOWN_RETRY)
    {
        if (lv_tick_elaps(shutdown_started) < 200 ||
            !peripheral_resume(shutdown_retry_peripheral)) return -RT_EBUSY;
        shutdown_state = SHUTDOWN_PERIPHERALS;
    }
    if (shutdown_state == SHUTDOWN_PERIPHERALS)
    {
        power_peripheral_t peripheral;
        rt_err_t result = peripherals_prepare(&peripheral, true);
        if (result != RT_EOK)
        {
            if (lv_tick_elaps(shutdown_reported) >= 2000)
            {
                rt_kprintf("poweroff: waiting for %s (%d)\n", peripheral_names[peripheral], result);
                shutdown_reported = lv_tick_get();
            }
            if (result != -RT_EBUSY)
            {
                shutdown_started = lv_tick_get();
                shutdown_retry_peripheral = peripheral;
                shutdown_state = SHUTDOWN_RETRY;
            }
            return result;
        }
        shutdown_state = SHUTDOWN_READY;
        return RT_EOK;
    }
    if (!reading_font_release_storage()) return -RT_EBUSY;
    if (shutdown_state != SHUTDOWN_DRAIN &&
        lv_tick_elaps(shutdown_started) >= SHUTDOWN_WALLPAPER_TIMEOUT_MS)
    {
        rt_kprintf("poweroff: keeping the last display frame\n");
        epd_refresh_set_maintenance(true);
        shutdown_state = SHUTDOWN_DRAIN;
    }
    if (shutdown_state == SHUTDOWN_DRAIN)
    {
        if (!epd_refresh_idle() || !board_pm_display_idle()) return -RT_EBUSY;
        if (wallpaper && !board_pm_display_succeeded())
            rt_kprintf("poweroff: shutdown wallpaper refresh failed\n");
        epd_input_clear();
        ui_run_active(false);
        shutdown_reported = lv_tick_get();
        shutdown_state = SHUTDOWN_PERIPHERALS;
        return -RT_EBUSY;
    }
    if (shutdown_state == SHUTDOWN_WAIT)
    {
        if (!wallpaper_init())
        {
            shutdown_state = SHUTDOWN_DRAIN;
            return -RT_EBUSY;
        }
        wallpaper_status_t status;
        wallpaper_get_status(&status);
        if (!status.ready || status.busy ||
            !epd_refresh_idle() || !board_pm_display_idle()) return -RT_EBUSY;
        wallpaper = wallpaper_view_create(lv_layer_sys());
        if (!wallpaper)
        {
            shutdown_state = SHUTDOWN_DRAIN;
            return -RT_EBUSY;
        }
        /* Cover both application pages and the shell's power menu. */
        lv_obj_add_flag(wallpaper, LV_OBJ_FLAG_CLICKABLE);
        wallpaper_submission = epd_refresh_submission();
        shutdown_state = SHUTDOWN_DRAW;
        epd_refresh_set_maintenance(false);
        epd_refresh_request_full();
        epd_refresh_request_interaction();
        lv_obj_invalidate(wallpaper);
        epd_refresh_process();
    }
    return -RT_EBUSY;
}

bool epd_power_shutdown_cancel(void)
{
    if (shutdown_state == SHUTDOWN_IDLE) return true;
    if (!peripherals_resume()) return false;
    epd_refresh_set_maintenance(true);
    if (wallpaper)
    {
        lv_obj_delete(wallpaper);
        wallpaper = NULL;
    }
    shutdown_state = SHUTDOWN_IDLE;
    ui_run_active(true);
    reading_font_storage_changed();
    policy.timing = false;
    epd_refresh_set_maintenance(false);
    epd_refresh_request_full();
    epd_refresh_request_interaction();
    epd_refresh_process();
    return true;
}

void epd_power_process(void)
{
    if (!initialized) return;
    rt_base_t level = rt_hw_interrupt_disable();
    bool key = key_wakeup;
    bool requested = sleep_requested;
    key_wakeup = false;
    sleep_requested = false;
    rt_hw_interrupt_enable(level);
    bool activity = input_changed;
    input_changed = false;
    if (state == POWER_RESUMING)
    {
        resume_locked(key);
        return;
    }
    if (state != POWER_ACTIVE && board_power_shutdown_pending())
    {
        resume_locked(false);
        return;
    }
    if (shutdown_state != SHUTDOWN_IDLE)
    {
        /* This also runs immediately after LVGL, before another frame can
         * overwrite the shutdown image while the board waits for the panel. */
        shutdown_observe_submission();
        return;
    }
    if (key && state != POWER_ACTIVE)
    {
        resume_locked(true);
        return;
    }
    if (requested && state != POWER_ACTIVE)
    {
        rt_kprintf("sys_sleep: %s\n", state == POWER_ASLEEP ? "already asleep" : "in progress");
        requested = false;
    }
    if (state == POWER_ASLEEP) return;
    if (state == POWER_ACTIVE) wallpaper_rotation_prepare();
    sleep_guard_snapshot_t guard;
    sleep_guard_snapshot(&guard);
    if (requested)
    {
        epd_power_policy_activity(&policy, lv_tick_get());
        if (!can_prepare(&guard))
        {
            rt_kprintf("sys_sleep: unavailable or busy\n");
            return;
        }
        if (!epd_lock_enter())
        {
            rt_kprintf("sys_sleep: cannot enter lock screen\n");
            return;
        }
        sleep_guard_snapshot(&guard);
    }
    bool eligible = epd_lock_active() && can_prepare(&guard);
    if (state != POWER_ACTIVE)
    {
        if (!eligible || activity || guard.state != SLEEP_GUARD_PREPARING)
        {
            rt_kprintf("sleep: interrupted state=%u eligible=%u activity=%u guard=%u held=%u draining=%u\n",
                       (unsigned)state, eligible, activity, (unsigned)guard.state,
                       (unsigned)guard.held_count, (unsigned)guard.draining_count);
            resume_locked(false);
            return;
        }
        if (lv_tick_elaps(prepare_started) >= POWER_PREPARE_TIMEOUT_MS)
        {
            prepare_failed("preparation", -RT_ETIMEOUT);
            return;
        }
    }
    else
    {
        (void)wallpaper_init();
        if (requested)
        {
            if (!eligible)
            {
                rt_kprintf("sys_sleep: unavailable or busy\n");
                return;
            }
        }
        else if (!epd_power_policy_update(&policy, lv_tick_get(), eligible,
                                         guard.revision, ui_settings_sleep_timeout_ms())) return;
        if (!sleep_guard_prepare())
        {
            if (requested) rt_kprintf("sys_sleep: busy\n");
            return;
        }
        manual_sleep = requested;
        state = POWER_PREPARING;
        prepare_started = lv_tick_get();
        time_parked = true;
        frontlight_set_suspended(true);
        epd_input_clear();
        /* Finish the old frame before allowing the system wallpaper frame. */
        epd_refresh_set_maintenance(true);
    }

    if (state == POWER_PREPARING)
    {
        /* Close idle LAN sockets before suspending the network beneath them.
         * Accepted requests hold the sleep guard until their response drains. */
        if (!portal_prepare_sleep()) return;
        power_peripheral_t peripheral;
        rt_err_t result = peripherals_prepare(&peripheral, false);
        if (result == -RT_EBUSY)
        {
            /* A radio may defer the handoff while its current owner is busy.
             * Release the prepare hold and rearm the normal inactivity timer. */
            if ((peripheral == PERIPHERAL_BLUETOOTH && !bt_suspending)
#if defined(RT_USING_WIFI) && defined(RT_WLAN_DEVICE_STA_NAME)
                || (peripheral == PERIPHERAL_WLAN && !wlan_suspending)
#endif
            )
            {
                epd_power_policy_activity(&policy, lv_tick_get());
                resume_locked(false);
                return;
            }
        }
        else if (result != RT_EOK)
        {
            prepare_failed(peripheral_names[peripheral], result);
            return;
        }
        if (!epd_refresh_idle() || !board_pm_display_idle()) return;
        wallpaper = wallpaper_view_create(lv_layer_sys());
        if (!wallpaper)
        {
            prepare_failed("wallpaper", -RT_ENOMEM);
            return;
        }
        wallpaper_submission = epd_refresh_submission();
        state = POWER_DRAWING;
        epd_refresh_set_maintenance(false);
        epd_refresh_request_clean_full();
        epd_refresh_request_interaction();
        lv_obj_invalidate(wallpaper);
        epd_refresh_process();
    }
    if (state == POWER_DRAWING && epd_refresh_submission() != wallpaper_submission)
    {
        state = POWER_DRAINING;
        epd_refresh_set_maintenance(true);
    }
    if (state != POWER_DRAINING || !epd_refresh_idle() || !board_pm_display_idle()) return;
    if (!board_pm_display_succeeded())
    {
        prepare_failed("display", -RT_ERROR);
        return;
    }

    power_peripheral_t peripheral;
    rt_err_t peripheral_result = peripherals_prepare(&peripheral, false);
    if (peripheral_result == -RT_EBUSY) return;
    if (peripheral_result != RT_EOK)
    {
        prepare_failed(peripheral_names[peripheral], peripheral_result);
        return;
    }

    frontlight_snapshot_t light;
    frontlight_get(&light);
    if (light.available && (!light.output_valid || light.output_pending || light.output_percent)) return;
    /* commit and acquire share the guard's interrupt-protected boundary. An
     * interrupt after this check leaves a wake event for the following wait. */
    if (board_pm_key_pressed())
    {
        (void)epd_power_key_wakeup();
        resume_locked(true);
        return;
    }
    sleep_guard_snapshot(&guard);
    if (guard.held_count || !storage_set_sleep(true)) return;
    if (!sleep_guard_commit())
    {
        sleep_guard_snapshot(&guard);
        if (guard.state != SLEEP_GUARD_PREPARING) resume_locked(false);
        return;
    }
    state = POWER_ASLEEP;
    if (!board_pm_set_system_sleep(true))
    {
        if (key_wakeup || board_pm_key_pressed()) resume_locked(true);
        else prepare_failed("board", -RT_ERROR);
    }
    else
    {
        wallpaper_rotation_shown();
        if (manual_sleep) rt_kprintf("sys_sleep: sleep enabled\n");
        manual_sleep = false;
        ui_run_active(false);
    }
}

#ifdef RT_USING_FINSH
static void sys_sleep(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: sys_sleep\n");
        return;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    const char *message = !initialized ? "not ready" :
        state == POWER_ASLEEP ? "already asleep" :
        state != POWER_ACTIVE || sleep_requested ? "in progress" : NULL;
    if (!message) sleep_requested = true;
    rt_hw_interrupt_enable(level);
    rt_kprintf("sys_sleep: %s\n", message ? message : "requested");
    if (!message) epd_power_notify();
}
MSH_CMD_EXPORT(sys_sleep, Show sleep wallpaper and request system sleep);

static void sys_wake(void)
{
    rt_kprintf("sys_wake: %s\n", epd_power_key_wakeup() ? "requested" : "already active");
}
MSH_CMD_EXPORT(sys_wake, Request the normal power key wakeup);
#endif

uint32_t epd_power_next_delay(uint32_t delay)
{
    if (shutdown_state >= SHUTDOWN_PERIPHERALS)
        return delay < 100u ? delay : 100u;
    if (state == POWER_ACTIVE && !board_power_pending())
    {
        uint32_t remaining = epd_power_policy_delay(&policy, lv_tick_get());
        if (remaining < delay) delay = remaining;
    }
    if ((board_power_pending() || (state != POWER_ACTIVE && state != POWER_ASLEEP)) &&
        delay > 20u) delay = 20u;
#ifdef EPD_FOREGROUND_STANDBY
    if (state == POWER_ACTIVE && bsp_touch_disabled() && !ui_shell_locked())
    {
        uint32_t idle = lv_tick_elaps(last_activity);
        if (idle < FOREGROUND_STANDBY_IDLE_MS && FOREGROUND_STANDBY_IDLE_MS - idle < delay)
            delay = FOREGROUND_STANDBY_IDLE_MS - idle;
    }
#endif
    delay = frontlight_next_delay(delay);
    return epd_input_next_delay(delay);
}

static bool foreground_standby_allowed(uint32_t delay)
{
#ifdef EPD_FOREGROUND_STANDBY
    if (delay < FOREGROUND_STANDBY_MIN_WAIT_MS || !bsp_touch_disabled() ||
        state != POWER_ACTIVE || shutdown_state != SHUTDOWN_IDLE ||
        ui_shell_locked() || ui_control_center_visible() || ui_navigation_busy() ||
        lv_tick_elaps(last_activity) < FOREGROUND_STANDBY_IDLE_MS ||
        btpan_get_state() != BTPAN_STATE_OFF) return false;

    frontlight_snapshot_t light;
    frontlight_get(&light);
    if (light.available && (!light.output_valid || light.output_pending || light.output_percent))
        return false;

    sleep_guard_snapshot_t guard;
    sleep_guard_snapshot(&guard);
    return !guard.held_count && can_prepare(&guard) && epd_refresh_idle() && board_pm_display_idle();
#else
    (void)delay;
    return false;
#endif
}

void epd_power_wait(uint32_t delay)
{
    bool standby = foreground_standby_allowed(delay) && board_pm_set_foreground_standby(true);
    /* Non-AON peripherals (including TF detect and USB presence) are sampled
     * again after a bounded tickless wait even without a PA34 wake edge. */
    if (standby && delay > FOREGROUND_STANDBY_MAX_WAIT_MS) delay = FOREGROUND_STANDBY_MAX_WAIT_MS;
    rt_uint32_t events = 0;
    rt_int32_t timeout = delay == UINT32_MAX ? RT_WAITING_FOREVER :
        delay ? rt_tick_from_millisecond(delay) : 1;
    (void)rt_event_recv(&wake_event, 1u,
        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, timeout, &events);
    if (standby) (void)board_pm_set_foreground_standby(false);
}
