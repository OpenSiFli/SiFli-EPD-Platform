#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GUI_APP_ID_MAX_LEN 8u
#define NAV_POLL_MS 50u
#define RT_EOK 0
#define RT_ENOMEM 12
#define RT_EBUSY 16
#define EPD_MEMORY_OK 0
#define EPD_MEMORY_OWNER_INVALID UINT32_MAX
#define EPD_MEMORY_OWNER_SYSTEM 0u
#define EPD_MEMORY_SCOPE_PAGE 1u

typedef int rt_base_t;
typedef uint32_t epd_memory_owner_t;
typedef struct { const char *id; } gui_runing_app_t;
typedef enum { page_st_created, page_st_started, page_st_resumed } page_state_t;
typedef struct { gui_runing_app_t *parent; page_state_t state; } subpage_node_t;

static gui_runing_app_t app = {"demo"};
static subpage_node_t *scheduled;
static bool gui_live, module_live, may_flush, no_scope, no_enter, no_account;
static unsigned back, stops, exits, flushes, owner_closes, owner_releases, scopes;
static uint32_t tick = 500;
static struct { void *timer; } nav;

static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
static unsigned rt_interrupt_get_nest(void) { return 0; }
static const void *rt_thread_self(void) { return &tick; }
static uint32_t rt_tick_get(void) { return tick; }
static uint32_t rt_tick_from_millisecond(uint32_t ms) { return ms; }
static void lv_timer_resume(void *timer) { assert(timer == nav.timer); }
static bool board_power_pending(void) { return false; }
static bool epd_power_sleeping(void) { return false; }
static bool storage_changing(void) { return false; }
static bool storage_release_requested(void) { return false; }
static bool gui_present(const char *id) { assert(!strcmp(id, app.id)); return gui_live; }
static bool app_module_loaded(const char *id) { assert(!strcmp(id, app.id)); return module_live; }
static bool app_service_running(const char *id) { assert(!strcmp(id, app.id)); return false; }
static bool app_service_module_pins(const char *id) { assert(!strcmp(id, app.id)); return false; }
static bool app_service_flush_app(const char *id) { assert(!strcmp(id, app.id)); ++flushes; return may_flush; }
static const subpage_node_t *app_schedule_get_this_page(void) { return scheduled; }
static int gui_app_goback(void) { ++back; return RT_EOK; }
static int gui_app_exit(const char *id) { assert(!strcmp(id, app.id) && gui_live); ++exits; return RT_EOK; }
static void gui_app_exec_now(void) { gui_live = false; }
static epd_memory_owner_t app_module_owner_acquire(const char *id)
{
    return app_module_loaded(id) && !no_account ? 1u : EPD_MEMORY_OWNER_INVALID;
}
static int app_module_request_stop(const char *id) { assert(app_module_loaded(id)); ++stops; return EPD_MEMORY_OK; }
static void epd_memory_owner_release_trusted(epd_memory_owner_t owner)
{
    assert(owner == 1 || owner == 2);
    ++owner_releases;
}
static void epd_memory_owner_close_trusted(epd_memory_owner_t owner) { assert(owner == 2); ++owner_closes; }
static epd_memory_owner_t epd_memory_owner_enter_trusted(epd_memory_owner_t owner)
{
    assert(owner == EPD_MEMORY_OWNER_SYSTEM || owner == 1 || owner == 2);
    return no_enter ? EPD_MEMORY_OWNER_INVALID : EPD_MEMORY_OWNER_SYSTEM;
}
static epd_memory_owner_t epd_memory_scope_create_trusted(epd_memory_owner_t root, unsigned scope)
{
    assert(root == 1 && scope == EPD_MEMORY_SCOPE_PAGE);
    ++scopes;
    return no_scope ? EPD_MEMORY_OWNER_INVALID : 2u;
}

#include "startup_context.inc"
#include "startup_retirement.inc"

static void startup_retirement_end(startup_retirement_t *retirement, bool replaced)
{
    assert(!replaced);
    memset(retirement, 0, sizeof(*retirement));
}

static void test_page_start(void)
{
    subpage_node_t page = {&app, page_st_created};
    app_memory_load_watch_t load;
    scheduled = &page;
    module_live = true;
    no_scope = true;
    app_memory_load_begin(&load);
    load.target = app.id;
    load.retire_gui = true;

    assert(gui_app_context_enter(app.id, &page) == EPD_MEMORY_OWNER_INVALID);
    assert(load.start_error == -RT_ENOMEM && !back && owner_releases == 1 && scopes == 1);

    /* The SDK still advances its page state when the context hook fails. */
    page.state = page_st_started;
    no_scope = false;
    assert(gui_app_context_enter(app.id, &page) == EPD_MEMORY_OWNER_INVALID && scopes == 1);
    page.state = page_st_resumed;
    assert(gui_app_context_enter(app.id, &page) == EPD_MEMORY_OWNER_INVALID && scopes == 1);
    app_memory_load_end(&load);

    page.state = page_st_created;
    app_memory_load_begin(&load);
    load.target = app.id;
    load.retire_gui = true;
    no_account = true;
    assert(gui_app_context_enter(app.id, &page) == EPD_MEMORY_OWNER_INVALID && load.start_error == -RT_EBUSY);
    no_account = false;
    app_memory_load_end(&load);

    app_memory_load_begin(&load);
    load.target = app.id;
    load.retire_gui = true;
    no_enter = true;
    assert(gui_app_context_enter(app.id, &page) == EPD_MEMORY_OWNER_INVALID);
    assert(owner_closes == 1 && load.start_error == -RT_ENOMEM);
    no_enter = false;
    app_memory_load_end(&load);

    app_memory_load_begin(&load);
    load.target = app.id;
    load.retire_gui = true;
    assert(gui_app_context_enter(app.id, &page) == EPD_MEMORY_OWNER_SYSTEM);
    epd_app_page_start_failed(-RT_ENOMEM);
    epd_app_page_start_failed(-RT_EBUSY);
    assert(load.start_error == -RT_ENOMEM && !back);
    page.state = page_st_started;
    assert(gui_app_context_enter(app.id, &page) == EPD_MEMORY_OWNER_SYSTEM);
    app_memory_load_end(&load);

    page.state = page_st_created;
    epd_app_page_start_failed(-RT_ENOMEM);
    assert(back == 1);

    app_memory_load_begin(&load);
    load.target = app.id;
    epd_app_page_start_failed(-RT_ENOMEM);
    assert(back == 2 && load.start_error == -RT_ENOMEM);
    app_memory_load_end(&load);
}

static void test_startup_retirement(void)
{
    startup_retirement_t retirement = {.id = "demo"};
    gui_live = module_live = true;
    startup_retirement_arm(&retirement, false);
    startup_retirement_process(&retirement, tick);
    assert(flushes == 1 && gui_live && retirement.armed && !stops);
    may_flush = true;
    tick += NAV_POLL_MS;
    startup_retirement_process(&retirement, tick);
    assert(!gui_live && exits == 1 && !retirement.armed && module_live && !stops);

    retirement = (startup_retirement_t){.id = "demo"};
    gui_live = true;
    startup_retirement_arm(&retirement, true);
    startup_retirement_process(&retirement, tick);
    assert(!gui_live && exits == 2 && stops == 1 && retirement.stop_requested);
    module_live = false;
    tick += NAV_POLL_MS;
    startup_retirement_process(&retirement, tick);
    assert(!retirement.armed && stops == 1);
}

int main(void)
{
    test_page_start();
    test_startup_retirement();
    puts("PASS page startup errors reach the load watcher and clean up failed GUI instances");
    return 0;
}
