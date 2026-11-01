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
#include "boards/board_pm.h"
#include "boards/board_service.h"
#include "app_service.h"
#include "sleep_guard.h"
#include "epd_compute.h"
#include "frontlight.h"
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
static uint32_t prepare_started;
#define POWER_PREPARE_TIMEOUT_MS 60000u
static epd_power_policy_t policy;
static lv_obj_t *wallpaper;
static uint32_t wallpaper_submission;
typedef enum { SHUTDOWN_IDLE, SHUTDOWN_WAIT, SHUTDOWN_DRAW, SHUTDOWN_DRAIN } shutdown_state_t;
static shutdown_state_t shutdown_state;
static uint32_t shutdown_started;
#define SHUTDOWN_WALLPAPER_TIMEOUT_MS 60000u

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
    epd_power_policy_activity(&policy, lv_tick_get());
    epd_power_notify();
}

bool epd_power_sleeping(void) { return state == POWER_ASLEEP; }
bool epd_power_transitioning(void)
{
    return state != POWER_ACTIVE || shutdown_state != SHUTDOWN_IDLE;
}

static bool peripherals_resume(void)
{
    bool complete = true;
#if defined(RT_USING_WIFI) && defined(RT_WLAN_DEVICE_STA_NAME)
    if (wlan_suspending)
    {
        wlan_service_resume();
        wlan_suspending = false;
    }
#endif
    if (bt_suspending)
    {
        if (btpan_resume() != RT_EOK) complete = false;
        else bt_suspending = false;
    }
    if (battery_suspending)
    {
        battery_resume();
        battery_suspending = false;
    }
    /* The time worker may have observed PREPARING before its own suspend
     * request was submitted (for example while waiting for the portal). */
    if (time_suspending || time_parked)
    {
        network_time_resume();
        time_suspending = time_parked = false;
    }
    return complete;
}

static rt_err_t peripherals_prepare(void)
{
    rt_err_t result;
    if (!time_suspending)
    {
        result = network_time_suspend();
        if (result != RT_EOK) return result == -RT_EBUSY ? -RT_ERROR : result;
        time_suspending = true;
    }
    if (!battery_suspending)
    {
        result = battery_suspend();
        if (result != RT_EOK) return result == -RT_EBUSY ? -RT_ERROR : result;
        battery_suspending = true;
    }
    /* Finish DNS/UDP work before taking either network interface down. */
    result = network_time_suspend_result();
    if (result != RT_EOK) return result;
    result = battery_suspend_result();
    if (result != RT_EOK) return result;
    if (!bt_suspending)
    {
        result = btpan_suspend();
        if (result != RT_EOK) return result == -RT_EBUSY ? -RT_ERROR : result;
        bt_suspending = true;
    }
#if defined(RT_USING_WIFI) && defined(RT_WLAN_DEVICE_STA_NAME)
    if (!wlan_suspending)
    {
        result = wlan_service_suspend();
        if (result != RT_EOK) return result == -RT_EBUSY ? -RT_ERROR : result;
        wlan_suspending = true;
    }
#endif
    bool pending = false;
    result = btpan_suspend_result();
    if (result == -RT_EBUSY) pending = true;
    else if (result != RT_EOK) return result;
#if defined(RT_USING_WIFI) && defined(RT_WLAN_DEVICE_STA_NAME)
    result = wlan_service_suspend_result();
    if (result == -RT_EBUSY) pending = true;
    else if (result != RT_EOK) return result;
#endif
    return pending ? -RT_EBUSY : RT_EOK;
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
    board_resumed = false;
    rt_hw_interrupt_enable(level);
    portal_resume();
    app_service_resume();
    policy.timing = false;
    frontlight_set_suspended(false);
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
        if (!wallpaper_init()) return -RT_ENOMEM;
        shutdown_started = lv_tick_get();
        shutdown_state = SHUTDOWN_WAIT;
        epd_refresh_set_maintenance(true);
    }
    shutdown_observe_submission();
    if (shutdown_state == SHUTDOWN_DRAIN &&
        epd_refresh_idle() && board_pm_display_idle())
    {
        if (!board_pm_display_succeeded()) return -RT_ERROR;
        if (lv_tick_elaps(shutdown_started) >= SHUTDOWN_WALLPAPER_TIMEOUT_MS)
            return -RT_ETIMEOUT;
        return peripherals_prepare();
    }
    if (lv_tick_elaps(shutdown_started) >= SHUTDOWN_WALLPAPER_TIMEOUT_MS)
        return -RT_ETIMEOUT;
    if (shutdown_state == SHUTDOWN_WAIT)
    {
        wallpaper_status_t status;
        wallpaper_get_status(&status);
        if (!status.ready || status.busy ||
            !epd_refresh_idle() || !board_pm_display_idle()) return -RT_EBUSY;
        wallpaper = wallpaper_view_create(lv_layer_sys());
        if (!wallpaper) return -RT_ENOMEM;
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
        rt_err_t result = peripherals_prepare();
        if (result != RT_EOK && result != -RT_EBUSY)
        {
            prepare_failed("peripherals", result);
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
        epd_refresh_request_full();
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

    rt_err_t peripheral_result = peripherals_prepare();
    if (peripheral_result == -RT_EBUSY) return;
    if (peripheral_result != RT_EOK)
    {
        prepare_failed("peripherals", peripheral_result);
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
#endif

uint32_t epd_power_next_delay(uint32_t delay)
{
    if (state == POWER_ACTIVE)
    {
        uint32_t remaining = epd_power_policy_delay(&policy, lv_tick_get());
        if (remaining < delay) delay = remaining;
    }
    else if (state != POWER_ASLEEP && delay > 20u) delay = 20u;
    delay = frontlight_next_delay(delay);
    return epd_input_next_delay(delay);
}

void epd_power_wait(uint32_t delay)
{
    rt_uint32_t events = 0;
    rt_int32_t timeout = delay == UINT32_MAX ? RT_WAITING_FOREVER :
        delay ? rt_tick_from_millisecond(delay) : 1;
    (void)rt_event_recv(&wake_event, 1u,
        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, timeout, &events);
}
