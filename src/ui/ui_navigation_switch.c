#include "ui_navigation.h"
#include "gui_app_int.h"
#include "app_service_internal.h"
#include "app_background.h"
#include "app_memory_pressure.h"
#include "epd_memory.h"
#include "platform/app_catalog.h"
#include "platform/app_module.h"
#include "platform/app_memory.h"
#include "platform/epd_power.h"
#include "platform/epd_system.h"
#include "boards/board_service.h"
#include "storage_file_internal.h"
#include "storage_internal.h"
#include "ui_shell.h"
#include "system_reset.h"
#include "sleep_guard.h"
#include <mem_section.h>
#include <rthw.h>
#include <string.h>
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif

#define DBG_TAG "ui.nav"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define NAV_POLL_MS 50u
#define NAV_TIMEOUT_MS 15000u
#define NAV_MEMORY_MS 5000u
#define NAV_OLD_MAX (GUI_MAX_RUNNING_APPS + 2u)

enum { NAV_RETRY_NONE, NAV_RETRY_MEMORY, NAV_RETRY_BLOCK, NAV_RETRY_START };

typedef struct
{
    char id[GUI_APP_ID_MAX_LEN];
    bool exited;
} nav_old_t;
static struct
{
    _intent target;
    unsigned count;
    ui_page_id_t page;
    bool pending, waiting, memory_checked;
    uint8_t load_retry;
    bool start_cleanup, start_final;
    size_t start_span;
    int start_error;
    uint32_t started, polled, generation;
    uint32_t memory_generation, memory_started;
    size_t load_required;
    lv_timer_t *timer;
    sleep_guard_owner_t hold;
} nav;
/* Reserve cleanup before a new GUI can call back into navigation. */
#define STARTUP_RETIREMENT_DYNAMIC_MAX NAV_OLD_MAX
#define STARTUP_RETIREMENT_BUILTIN_MAX 2u
#define STARTUP_RETIREMENT_MAX (STARTUP_RETIREMENT_DYNAMIC_MAX + STARTUP_RETIREMENT_BUILTIN_MAX)

typedef struct
{
    char id[GUI_APP_ID_MAX_LEN];
    epd_memory_owner_t owner;
    uint32_t polled;
    bool armed, exit_requested, stop_requested, terminate;
} startup_retirement_t;
L2_RET_BSS_SECT_BEGIN(navigation_retirement)
static nav_old_t nav_old[NAV_OLD_MAX] L2_RET_BSS_SECT(navigation_retirement);
static startup_retirement_t startup_retirements[STARTUP_RETIREMENT_MAX] L2_RET_BSS_SECT(navigation_retirement);
L2_RET_BSS_SECT_END
static struct { char app[GUI_APP_ID_MAX_LEN]; bool ready; } diagnostic;

static startup_retirement_t *startup_retirement_find(const char *id)
{
    for (unsigned i = 0; i < STARTUP_RETIREMENT_MAX; ++i)
        if (!strcmp(startup_retirements[i].id, id)) return &startup_retirements[i];
    return NULL;
}

static startup_retirement_t *startup_retirement_free(const char *id)
{
    bool builtin = app_catalog_is_builtin(id);
    unsigned first = builtin ? STARTUP_RETIREMENT_DYNAMIC_MAX : 0;
    unsigned end = builtin ? STARTUP_RETIREMENT_MAX : STARTUP_RETIREMENT_DYNAMIC_MAX;
    for (unsigned i = first; i < end; ++i)
        if (!startup_retirements[i].id[0]) return &startup_retirements[i];
    return NULL;
}

static void startup_retirement_pause_if_idle(void)
{
    if (nav.pending || !nav.timer) return;
    for (unsigned i = 0; i < STARTUP_RETIREMENT_MAX; ++i)
        if (startup_retirements[i].id[0]) return;
    lv_timer_pause(nav.timer);
}

static bool gui_present(const char *id)
{
    rt_list_t *cursor = NULL;
    gui_runing_app_t *app;
    while ((app = gui_app_trav(&cursor)) != NULL)
        if (!strcmp(app->id, id)) return true;
    return false;
}

static bool startup_retirement_conflicts(const char *target_id)
{
    if (startup_retirement_find(target_id)) return true;
    /* A new SDK launch or a return to Main can discard another live GUI.
     * Give a failed GUI its save barrier and explicit exit before dispatch. */
    for (unsigned i = 0; i < STARTUP_RETIREMENT_MAX; ++i)
        if (startup_retirements[i].id[0] && gui_present(startup_retirements[i].id)) return true;
    return false;
}

static int resume_existing_home(void)
{
    int result = RT_EOK;
    if (!startup_retirement_find("Main") && gui_present("Main"))
        result = gui_app_run("Main");
    gui_app_exec_now();
    return result;
}

static bool resident(const char *id)
{
    if (app_service_app_stopping(id)) return false;
    epd_memory_owner_t owner = app_module_owner_acquire(id);
    if (owner == EPD_MEMORY_OWNER_INVALID)
        /* Built-in services have no dynamic code to retire. */
        return !app_module_loaded(id) && !app_service_module_pins(id);
    epd_app_background_snapshot_t state;
    bool retained = app_background_snapshot(owner, &state) == EPD_MEMORY_OK &&
        !state.stopping && state.actual != EPD_APP_BG_STOPPING &&
        state.actual != EPD_APP_BG_COLD && state.actual != EPD_APP_BG_RELEASED &&
        (state.effective == EPD_APP_BG_STATE ||
         (state.effective == EPD_APP_BG_TASKS && state.tasks));
    epd_memory_owner_release_trusted(owner);
    return retained;
}

static void reap(void)
{
    for (unsigned i = 0; i < nav.count;)
    {
        nav_old_t *old = &nav_old[i];
        if (!gui_present(old->id) &&
            (resident(old->id) || (!app_service_running(old->id) && !app_module_loaded(old->id))))
            nav_old[i] = nav_old[--nav.count];
        else ++i;
    }
}

bool ui_navigation_busy(void) { return nav.pending; }

bool ui_navigation_cancel(void)
{
    bool pending = nav.pending;
    ++nav.generation;
    nav.pending = nav.waiting = false;
    nav.memory_checked = false;
    nav.load_retry = NAV_RETRY_NONE;
    nav.start_cleanup = nav.start_final = false;
    nav.start_span = 0;
    nav.start_error = 0;
    nav.memory_generation = 0;
    nav.load_required = 0;
    for (unsigned i = 0; i < nav.count;)
        if (!nav_old[i].exited) nav_old[i] = nav_old[--nav.count];
        else ++i;
    startup_retirement_pause_if_idle();
    sleep_guard_release(&nav.hold);
    return pending;
}

static void failed(const char *phase, int error)
{
    LOG_W("switch app=%s phase=%s error=%d", nav.target.content, phase, error);
    ui_navigation_cancel();
    ui_shell_navigation_failed();
}

static bool collect(void)
{
    rt_list_t *cursor = NULL;
    gui_runing_app_t *app;
    while ((app = gui_app_trav(&cursor)) != NULL)
    {
        if (!strcmp(app->id, "Main") || startup_retirement_find(app->id)) continue;
        unsigned i = 0;
        while (i < nav.count && strcmp(nav_old[i].id, app->id)) ++i;
        if (i < nav.count) continue;
        if (nav.count == NAV_OLD_MAX) return false;
        nav_old_t *old = &nav_old[nav.count++];
        memset(old, 0, sizeof(*old));
        strcpy(old->id, app->id);
    }
    return true;
}

static void timer_wake(lv_timer_t *timer) { (void)timer; epd_power_notify(); }

static bool retirement_ready(void)
{
    if (!nav.count) return true;
    for (unsigned i = 0; i < nav.count; ++i)
    {
        if (!nav_old[i].exited || gui_present(nav_old[i].id)) return false;
        /* Reopening a retiring instance must wait for its services and code. */
        if (!strcmp(nav_old[i].id, nav.target.content)) return false;
    }
    if (!strcmp(nav.target.content, "Main") || !strcmp(nav.target.content, "settings")) return true;
    /* Services retain retiring code. Unknown estimates reach real loader
     * admission; a known shortage waits for the old owners to release memory. */
    size_t needed = app_catalog_load_required(nav.target.content);
    return !needed || epd_app_psram_available() >= needed;
}

static bool load_ready(uint32_t now)
{
    /* A selected STATE/module is not reclaimed until its asynchronous work,
     * resources and code actually retire. Keep pumping the UI in the meantime. */
    if (nav.load_retry == NAV_RETRY_BLOCK && app_modules_retiring()) return false;
    if (!nav.memory_checked)
    {
        nav.memory_checked = true;
        nav.load_required = app_catalog_load_required(nav.target.content);
        nav.memory_started = now;
        if (nav.load_required)
            nav.memory_generation = app_memory_pressure_request(nav.load_required);
    }
    if (!nav.memory_generation) return true;
    app_memory_pressure_status_t state = app_memory_pressure_query();
    if (state.generation == nav.memory_generation && state.pending &&
        (nav.load_retry == NAV_RETRY_BLOCK || epd_app_psram_available() < nav.load_required) &&
        (uint32_t)(now - nav.memory_started) < rt_tick_from_millisecond(NAV_MEMORY_MS)) return false;
    LOG_I("switch app=%s memory gen=%lu result=%u need=%lu free=%lu",
          nav.target.content, (unsigned long)nav.memory_generation, (unsigned)state.result,
          (unsigned long)nav.load_required, (unsigned long)epd_app_psram_available());
    /* The estimate is a reclaim hint. The loader decides whether allocation fits. */
    nav.memory_generation = 0;
    return true;
}

static rt_err_t navigation_request(intent_t intent, bool failed_start)
{
    const _intent *input = intent;
    if (!input || !input->content_len || input->content_len >= INTENT_MAX_LEN ||
        input->content[input->content_len - 1] != 0 ||
        !memchr(input->content, 0, input->content_len)) return -RT_EINVAL;
    const char *id = input->content;
    if (!*id || strlen(id) >= GUI_APP_ID_MAX_LEN) return -RT_EINVAL;
    if (ui_shell_locked() || board_power_pending() || epd_power_sleeping() ||
        storage_changing() || storage_release_requested() ||
        (system_reset_blocks_apps() && strcmp(id, "settings"))) return -RT_EBUSY;
    gui_runing_app_t *active = gui_app_get_actived();
    if (!failed_start && !startup_retirement_find(id) && active &&
        active->param.content_len == input->content_len &&
        !memcmp(active->param.content, input->content, input->content_len))
    { ui_navigation_cancel(); return RT_EOK; }
    if (!nav.timer)
    {
        nav.timer = lv_timer_create(timer_wake, NAV_POLL_MS, NULL);
        if (!nav.timer) return -RT_ENOMEM;
        lv_timer_pause(nav.timer);
    }
    nav.hold.id = "navigation";
    /* Copy before cancelling: the source may be the current pending intent. */
    _intent copy = *input;
    if (!sleep_guard_acquire(&nav.hold)) return -RT_EBUSY;
    ui_navigation_cancel();
    if (!sleep_guard_acquire(&nav.hold)) return -RT_EBUSY;
    nav.target = copy;
    nav.page = UI_PAGE_COUNT;
    nav.started = (uint32_t)rt_tick_get();
    nav.polled = nav.started - rt_tick_from_millisecond(NAV_POLL_MS);
    nav.pending = true;
    lv_timer_resume(nav.timer);
    lv_timer_reset(nav.timer);
    epd_power_notify();
    return RT_EOK;
}

rt_err_t ui_navigation_request(intent_t intent)
{
    return navigation_request(intent, false);
}

bool ui_navigation_run(const char *app)
{
    if (!app || !*app || strlen(app) >= GUI_APP_ID_MAX_LEN) return false;
    intent_t intent = intent_init(app);
    if (!intent) return false;
    rt_err_t result = ui_navigation_request(intent);
    intent_deinit(intent);
    return result == RT_EOK;
}

bool ui_navigation_run_page(const char *app, ui_page_id_t page)
{
    if (page < UI_PAGE_HOME || page >= UI_PAGE_COUNT || !ui_navigation_run(app)) return false;
    if (!nav.pending) return ui_navigation_open(page);
    nav.page = page;
    return true;
}

static void startup_retirement_arm(startup_retirement_t *retirement, bool terminate)
{
    retirement->owner = app_module_owner_acquire(retirement->id);
    if (retirement->owner != EPD_MEMORY_OWNER_INVALID)
        epd_memory_owner_release_trusted(retirement->owner);
    retirement->polled = (uint32_t)rt_tick_get() - rt_tick_from_millisecond(NAV_POLL_MS);
    retirement->terminate = terminate;
    retirement->armed = true;
    if (nav.timer) lv_timer_resume(nav.timer);
}

static bool startup_retirement_matches(const startup_retirement_t *retirement)
{
    epd_memory_owner_t owner = app_module_owner_acquire(retirement->id);
    if (owner == EPD_MEMORY_OWNER_INVALID) return true;
    epd_memory_owner_release_trusted(owner);
    return owner == retirement->owner;
}

static void startup_retirement_end(startup_retirement_t *retirement, bool replaced)
{
    bool matching_retry = nav.pending && nav.start_cleanup && retirement->armed &&
                          !strcmp(nav.target.content, retirement->id);
    memset(retirement, 0, sizeof(*retirement));
    if (replaced && matching_retry) failed("start_owner_changed", -RT_EBUSY);
    startup_retirement_pause_if_idle();
}

static void startup_retirement_process(startup_retirement_t *retirement, uint32_t now)
{
    if (!retirement->armed || board_power_pending() || epd_power_sleeping() ||
        storage_changing() || storage_release_requested() ||
        (uint32_t)(now - retirement->polled) < rt_tick_from_millisecond(NAV_POLL_MS)) return;
    retirement->polled = now;
    const char *id = retirement->id;
    if (!startup_retirement_matches(retirement)) { startup_retirement_end(retirement, true); return; }

    if (!retirement->exit_requested)
    {
        if (!app_service_flush_app(id)) return;
        if (!startup_retirement_matches(retirement)) { startup_retirement_end(retirement, true); return; }
        if (gui_present(id))
        {
            if (gui_app_exit(id) != RT_EOK) return;
            retirement->exit_requested = true;
            gui_app_exec_now();
        }
        else retirement->exit_requested = true;
    }
    if (gui_present(id)) return;
    if (!startup_retirement_matches(retirement)) { startup_retirement_end(retirement, true); return; }
    if (!retirement->terminate) { startup_retirement_end(retirement, false); return; }
    if (app_module_loaded(id))
    {
        if (!retirement->stop_requested && app_module_request_stop(id) == EPD_MEMORY_OK)
            retirement->stop_requested = true;
        return;
    }
    if (!app_service_running(id) && !app_service_module_pins(id)) startup_retirement_end(retirement, false);
}

void ui_navigation_process(void)
{
    char command[GUI_APP_ID_MAX_LEN] = {0};
    rt_base_t level = rt_hw_interrupt_disable();
    if (diagnostic.ready)
    {
        memcpy(command, diagnostic.app, sizeof(command));
        diagnostic.ready = false;
    }
    rt_hw_interrupt_enable(level);
    if (*command)
    {
        if (!strcmp(command, "cancel")) ui_navigation_cancel();
        else if (!ui_navigation_run(command)) LOG_W("switch request rejected: %s", command);
        else
        {
            lv_display_trigger_activity(NULL);
            epd_power_activity();
            if (!ui_navigation_busy()) LOG_I("switch app=%s complete", command);
        }
    }
    reap();
    uint32_t now = (uint32_t)rt_tick_get();
    for (unsigned i = 0; i < STARTUP_RETIREMENT_MAX; ++i)
        startup_retirement_process(&startup_retirements[i], now);
    if (!nav.pending) return;
    if (ui_shell_locked() || board_power_pending() || epd_power_sleeping() ||
        storage_changing() || storage_release_requested() ||
        (system_reset_blocks_apps() && strcmp(nav.target.content, "settings")))
    { ui_navigation_cancel(); return; }
    bool retirement_conflict = startup_retirement_conflicts(nav.target.content);
    if ((retirement_conflict || nav.count || !nav.memory_checked || nav.load_retry) &&
        (uint32_t)(now - nav.started) >= rt_tick_from_millisecond(NAV_TIMEOUT_MS))
    { failed(nav.load_retry ? "load_timeout" : nav.waiting ? "cleanup_timeout" : "save_timeout", -RT_ETIMEOUT); return; }
    if (retirement_conflict ||
        (uint32_t)(now - nav.polled) < rt_tick_from_millisecond(NAV_POLL_MS)) return;
    nav.polled = now;
    uint32_t generation = nav.generation;
    if (nav.start_cleanup)
    {
        if (nav.start_final) { failed("start", nav.start_error); return; }
        int result = resume_existing_home();
        if (generation != nav.generation) return;
        if (result != RT_EOK) { failed("start_home", result); return; }
        nav.start_cleanup = false;
        nav.memory_generation = app_memory_pressure_allocation_failed_block(nav.load_required, nav.start_span);
        LOG_I("switch app=%s phase=start_reclaim need=%lu span=%lu",
              nav.target.content, (unsigned long)nav.load_required, (unsigned long)nav.start_span);
    }
    if (!gui_present(nav.target.content) && !startup_retirement_free(nav.target.content))
    { failed("start_retirement_full", -RT_EBUSY); return; }
    if (!nav.waiting)
    {
        if (!collect()) { failed("owners", -RT_EFULL); return; }
        /* A page may have closed itself before this request was made. Its
         * service still owns the module until the normal retirement completes. */
        for (unsigned i = 0; i < app_catalog_count(); ++i)
        {
            const epd_app_entry_t *entry = app_catalog_get(i);
            if (!entry || startup_retirement_find(entry->id) || resident(entry->id) ||
                !app_module_loaded(entry->id) || gui_present(entry->id)) continue;
            unsigned j = 0;
            while (j < nav.count && strcmp(nav_old[j].id, entry->id)) ++j;
            if (j < nav.count) continue;
            if (nav.count == NAV_OLD_MAX) { failed("owners", -RT_EFULL); return; }
            nav_old_t *old = &nav_old[nav.count++];
            memset(old, 0, sizeof(*old));
            strcpy(old->id, entry->id);
            old->exited = true;
        }
        /* All save barriers must finish in this UI turn before the first exit.
         * Never destroy a second editor while its draft is still being saved. */
        bool saved = true;
        for (unsigned i = 0; i < nav.count; ++i)
        {
            if (!nav_old[i].exited && !app_service_flush_app(nav_old[i].id)) saved = false;
            if (!nav.pending || generation != nav.generation) return;
        }
        if (!saved) return;
        for (unsigned i = 0; i < nav.count; ++i)
            if (!nav_old[i].exited)
            {
                int result = gui_app_exit(nav_old[i].id);
                if (result != RT_EOK) { gui_app_exec_now(); failed("exit", result); return; }
                nav_old[i].exited = true;
            }
        if (nav.count)
        {
            int result = resume_existing_home();
            if (!nav.pending || generation != nav.generation) return;
            if (result != RT_EOK) { failed("home", result); return; }
        }
        nav.waiting = true;
        reap();
    }
    if (app_service_app_stopping(nav.target.content))
    {
        if ((uint32_t)(now - nav.started) >= rt_tick_from_millisecond(NAV_TIMEOUT_MS))
            failed("target_cleanup_timeout", -RT_ETIMEOUT);
        return;
    }
    if (!retirement_ready()) return;
    if (nav.load_retry == NAV_RETRY_START)
    {
        app_memory_pressure_status_t pressure = app_memory_pressure_query();
        if (pressure.generation != nav.memory_generation ||
            (!pressure.pending && pressure.result != APP_MEMORY_PRESSURE_SATISFIED))
        { failed("start_reclaim", -RT_ENOMEM); return; }
        if (pressure.pending || app_modules_retiring()) return;
    }
    else if (!load_ready(now)) return;
    _intent target = nav.target;
    uint32_t started = nav.started;
    ui_page_id_t page = nav.page;
    bool unknown_load = !nav.load_retry && !nav.load_required;
    bool recovered = nav.load_retry == NAV_RETRY_BLOCK;
    bool start_replayed = nav.load_retry == NAV_RETRY_START;
    bool new_gui = !gui_present(target.content);
    bool fresh = new_gui && !app_module_loaded(target.content) &&
                 !app_service_running(target.content) && !app_service_module_pins(target.content);
    startup_retirement_t *reservation = new_gui ? startup_retirement_free(target.content) : NULL;
    if (new_gui && !reservation) { failed("start_retirement_full", -RT_EBUSY); return; }
    if (reservation) strcpy(reservation->id, target.content);
    /* Consume once, before entry callbacks can enqueue a replacement request. */
    ui_navigation_cancel();
    generation = nav.generation;
    app_memory_load_watch_t load;
    app_memory_load_begin(&load);
    load.target = target.content;
    load.retire_gui = reservation != NULL;
    int result = intent_runapp(&target);
    gui_app_exec_now();
    app_memory_load_end(&load);
    if (reservation)
    {
        if (load.start_error) startup_retirement_arm(reservation, fresh);
        else startup_retirement_end(reservation, false);
    }
    if (load.start_error || result != RT_EOK || !gui_app_is_actived(target.content))
    {
        if (generation != nav.generation) return;
        if (new_gui && load.start_error && navigation_request(&target, true) == RT_EOK)
        {
            nav.start_error = load.start_error;
            nav.started = started;
            nav.page = page;
            nav.waiting = nav.memory_checked = true;
            nav.load_retry = NAV_RETRY_START;
            nav.start_cleanup = true;
            nav.start_final = !fresh || start_replayed || load.start_error != -RT_ENOMEM || !load.contiguous;
            nav.start_span = load.contiguous;
            nav.load_required = load.peak > load.contiguous ? load.peak : load.contiguous;
            return;
        }
        if (start_replayed || load.start_error)
        {
            LOG_W("switch app=%s phase=start error=%d", target.content,
                  load.start_error ? load.start_error : result);
            ui_shell_navigation_failed();
            return;
        }
        if (!gui_present(target.content) && !app_module_loaded(target.content) &&
            !app_service_running(target.content) && (load.busy || load.contiguous))
        {
            /* Failed ELF allocations can have enough total free bytes. Retire
             * one legal old STATE instance and require the actual failed span.
             * A repeated failed load needs another retirement to make progress;
             * otherwise it terminates, rather than repeatedly reopening the ELF. */
            bool retiring = load.contiguous && app_modules_reclaim(target.content);
            if ((load.busy || !recovered || retiring) && ui_navigation_request(&target) == RT_EOK)
            {
                nav.started = started;
                nav.page = page;
                nav.waiting = nav.memory_checked = true;
                nav.load_retry = load.contiguous ? NAV_RETRY_BLOCK : NAV_RETRY_MEMORY;
                nav.load_required = load.peak > load.contiguous ? load.peak : load.contiguous;
                nav.memory_started = (uint32_t)rt_tick_get();
                if (load.contiguous)
                    nav.memory_generation = app_memory_pressure_allocation_failed_block(nav.load_required, load.contiguous);
                LOG_I("switch app=%s phase=load_retry need=%lu span=%lu busy=%u",
                      target.content, (unsigned long)nav.load_required,
                      (unsigned long)load.contiguous, (unsigned)load.busy);
                return;
            }
        }
        /* A first load can populate the verified estimate even when allocation
         * fails. Retry only that newly known shortage, after cooperative reclaim. */
        if (unknown_load && !nav.pending && generation == nav.generation &&
            !gui_present(target.content) && !app_module_loaded(target.content) &&
            !app_service_running(target.content))
        {
            size_t needed = app_catalog_load_required(target.content);
            if (needed && epd_app_psram_available() < needed &&
                ui_navigation_request(&target) == RT_EOK)
            {
                nav.load_retry = NAV_RETRY_MEMORY;
                nav.started = started;
                nav.page = page;
                LOG_I("switch app=%s phase=memory_retry need=%lu", target.content, (unsigned long)needed);
                return;
            }
        }
        LOG_W("switch app=%s phase=start error=%d", target.content, result);
        ui_shell_navigation_failed();
        return;
    }
    if (!nav.pending && page != UI_PAGE_COUNT && !ui_navigation_open(page)) ui_shell_navigation_failed();
    LOG_I("switch app=%s complete ms=%lu", target.content,
          (unsigned long)((uint64_t)((uint32_t)rt_tick_get() - started) * 1000u / RT_TICK_PER_SECOND));
}

#ifdef RT_USING_FINSH
static int nav_app(int argc, char **argv)
{
    if (argc != 2 || !*argv[1] || strlen(argv[1]) >= GUI_APP_ID_MAX_LEN)
    { rt_kprintf("nav_app <app-id|cancel>\n"); return -RT_EINVAL; }
    rt_base_t level = rt_hw_interrupt_disable();
    strcpy(diagnostic.app, argv[1]);
    diagnostic.ready = true;
    rt_hw_interrupt_enable(level);
    epd_power_notify();
    return RT_EOK;
}
MSH_CMD_EXPORT(nav_app, Post application navigation to the UI thread);
#endif
