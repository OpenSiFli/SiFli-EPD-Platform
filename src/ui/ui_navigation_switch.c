#include "ui_navigation.h"
#include "gui_app_int.h"
#include "app_service_internal.h"
#include "app_memory_pressure.h"
#include "epd_memory.h"
#include "platform/app_catalog.h"
#include "platform/app_module.h"
#include "platform/epd_power.h"
#include "platform/epd_system.h"
#include "boards/board_service.h"
#include "storage_file_internal.h"
#include "storage_internal.h"
#include "ui_shell.h"
#include "system_reset.h"
#include "sleep_guard.h"
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

typedef struct
{
    char id[GUI_APP_ID_MAX_LEN];
    bool exited, resident;
} nav_old_t;
static struct
{
    _intent target;
    nav_old_t old[NAV_OLD_MAX];
    unsigned count;
    ui_page_id_t page;
    bool pending, waiting, memory_checked, load_retried;
    uint32_t started, polled, generation;
    uint32_t memory_generation, memory_started;
    size_t load_required;
    lv_timer_t *timer;
    sleep_guard_owner_t hold;
} nav;
static struct { char app[GUI_APP_ID_MAX_LEN]; bool ready; } diagnostic;

static bool gui_present(const char *id)
{
    rt_list_t *cursor = NULL;
    gui_runing_app_t *app;
    while ((app = gui_app_trav(&cursor)) != NULL)
        if (!strcmp(app->id, id)) return true;
    return false;
}

static bool resident(const char *id)
{
    /* The music worker owns playback and its queue beyond the GUI lifetime. */
    if (!strcmp(id, "music")) return true;
    /* The web service keeps running after the wireless transfer page closes. */
    if (!strcmp(id, "portal")) return true;
    epd_media_t media;
    if (epd_media_read(&media, NULL) && !strcmp(media.app_id, id)) return true;
    for (unsigned i = 0; i < app_catalog_count(); ++i)
    {
        const epd_app_entry_t *entry = app_catalog_get(i);
        if (entry && !strcmp(entry->id, id)) return entry->background;
    }
    return false;
}

static void reap(void)
{
    for (unsigned i = 0; i < nav.count;)
    {
        nav_old_t *old = &nav.old[i];
        if (!gui_present(old->id) &&
            (old->resident || (!app_service_running(old->id) && !app_module_loaded(old->id))))
            nav.old[i] = nav.old[--nav.count];
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
    nav.load_retried = false;
    nav.memory_generation = 0;
    nav.load_required = 0;
    for (unsigned i = 0; i < nav.count;)
        if (!nav.old[i].exited) nav.old[i] = nav.old[--nav.count];
        else ++i;
    if (nav.timer) lv_timer_pause(nav.timer);
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
        if (!strcmp(app->id, "Main")) continue;
        unsigned i = 0;
        while (i < nav.count && strcmp(nav.old[i].id, app->id)) ++i;
        if (i < nav.count) continue;
        if (nav.count == NAV_OLD_MAX) return false;
        nav_old_t *old = &nav.old[nav.count++];
        memset(old, 0, sizeof(*old));
        strcpy(old->id, app->id);
        old->resident = resident(app->id);
    }
    return true;
}

static void timer_wake(lv_timer_t *timer) { (void)timer; epd_power_notify(); }

static bool load_ready(uint32_t now)
{
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
        epd_app_psram_available() < nav.load_required &&
        (uint32_t)(now - nav.memory_started) < rt_tick_from_millisecond(NAV_MEMORY_MS)) return false;
    LOG_I("switch app=%s memory gen=%lu result=%u need=%lu free=%lu",
          nav.target.content, (unsigned long)nav.memory_generation, (unsigned)state.result,
          (unsigned long)nav.load_required, (unsigned long)epd_app_psram_available());
    /* The estimate is a reclaim hint. The loader decides whether allocation fits. */
    nav.memory_generation = 0;
    return true;
}

rt_err_t ui_navigation_request(intent_t intent)
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
    if (active && active->param.content_len == input->content_len &&
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
    if (!nav.pending) return;
    if (ui_shell_locked() || board_power_pending() || epd_power_sleeping() ||
        storage_changing() || storage_release_requested() ||
        (system_reset_blocks_apps() && strcmp(nav.target.content, "settings")))
    { ui_navigation_cancel(); return; }
    uint32_t now = (uint32_t)rt_tick_get();
    if (!nav.memory_checked &&
        (uint32_t)(now - nav.started) >= rt_tick_from_millisecond(NAV_TIMEOUT_MS))
    { failed(nav.waiting ? "cleanup_timeout" : "save_timeout", -RT_ETIMEOUT); return; }
    if ((uint32_t)(now - nav.polled) < rt_tick_from_millisecond(NAV_POLL_MS)) return;
    nav.polled = now;
    uint32_t generation = nav.generation;
    if (!nav.waiting)
    {
        if (!collect()) { failed("owners", -RT_EFULL); return; }
        /* A page may have closed itself before this request was made. Its
         * service still owns the module until the normal retirement completes. */
        for (unsigned i = 0; i < app_catalog_count(); ++i)
        {
            const epd_app_entry_t *entry = app_catalog_get(i);
            if (!entry || resident(entry->id) || !app_module_loaded(entry->id) || gui_present(entry->id)) continue;
            unsigned j = 0;
            while (j < nav.count && strcmp(nav.old[j].id, entry->id)) ++j;
            if (j < nav.count) continue;
            if (nav.count == NAV_OLD_MAX) { failed("owners", -RT_EFULL); return; }
            nav_old_t *old = &nav.old[nav.count++];
            memset(old, 0, sizeof(*old));
            strcpy(old->id, entry->id);
            old->exited = true;
        }
        /* All save barriers must finish in this UI turn before the first exit.
         * Never destroy a second editor while its draft is still being saved. */
        bool saved = true;
        for (unsigned i = 0; i < nav.count; ++i)
        {
            if (!nav.old[i].exited && !app_service_flush_app(nav.old[i].id)) saved = false;
            if (!nav.pending || generation != nav.generation) return;
        }
        if (!saved) return;
        for (unsigned i = 0; i < nav.count; ++i)
            if (!nav.old[i].exited)
            {
                int result = gui_app_exit(nav.old[i].id);
                if (result != RT_EOK) { gui_app_exec_now(); failed("exit", result); return; }
                nav.old[i].exited = true;
            }
        if (nav.count)
        {
            int result = gui_app_run("Main");
            gui_app_exec_now();
            if (!nav.pending || generation != nav.generation) return;
            if (result != RT_EOK) { failed("home", result); return; }
        }
        nav.waiting = true;
        reap();
    }
    if (nav.count) return;
    if (app_service_app_stopping(nav.target.content))
    {
        if ((uint32_t)(now - nav.started) >= rt_tick_from_millisecond(NAV_TIMEOUT_MS))
            failed("target_cleanup_timeout", -RT_ETIMEOUT);
        return;
    }
    if (!load_ready(now)) return;
    _intent target = nav.target;
    ui_page_id_t page = nav.page;
    bool unknown_load = !nav.load_retried && !nav.load_required;
    /* Consume once, before entry callbacks can enqueue a replacement request. */
    ui_navigation_cancel();
    generation = nav.generation;
    int result = intent_runapp(&target);
    gui_app_exec_now();
    if (result != RT_EOK || !gui_app_is_actived(target.content))
    {
        if (generation != nav.generation) return;
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
                nav.load_retried = true;
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
    LOG_I("switch app=%s complete", target.content);
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
