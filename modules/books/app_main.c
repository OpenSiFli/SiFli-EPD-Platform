#include "platform/epd_render_profile.h"
#include "app_service.h"
#include "storage.h"
#include "platform/app_content.h"
#include "platform/epd_locale.h"
#include "books_ui.h"
#include "reader_controls.h"
#include "reader_ai.h"
#include "../common/reader/reader_ui_layout.h"
#include "../common/reader/reader.h"
#include "document/document.h"
#include <dlmodule.h>

EPD_APP_DEFINE("books");
MODULE_INIT_DEF(NULL)
MODULE_CLEANUP_DEF(NULL)
books_page_t *current;
int pending_page = NAV_IDLE;
static bool initialized, initialization_attempted;
static bool idle_stopping;
static lv_timer_t *restart_timer;
static bool books_service_start(void);
static books_page_t *waiting_page;
static void books_page_build(books_page_t *page);
static void books_page_resume(books_page_t *page);
epd_service_t *books_service;
static unsigned live_pages, saved_focus[UI_PAGE_COUNT];
static uint32_t storage_generation;
static char content_path[STORAGE_PATH_MAX];
static uint32_t content_session;
static rt_err_t content_error;
static uint32_t content_generation;
static const char *const page_names[] = {"root", "reader"};
static void page_message(gui_app_msg_type_t message, void *parameter);

/* One opening, HCPU ticks. Report only after the prepared page has refreshed. */
enum {
    OPEN_BEGIN, OPEN_ATTACHED, OPEN_NAV_DONE, OPEN_UI_DONE,
    OPEN_CREATE_BEGIN, OPEN_CREATE_END, OPEN_SCALE_BEGIN, OPEN_SCALE_END,
    OPEN_RESUMED, OPEN_WORKER, OPEN_SHELF_DONE, OPEN_READER_DONE,
    OPEN_REFR_BEGIN, OPEN_RENDER_BEGIN, OPEN_RENDER_END, OPEN_REFR_END,
    OPEN_STAMPS
};
static struct {
    uint32_t serial, mask, tick[OPEN_STAMPS];
    lv_display_t *display;
    lv_obj_t *screen;
    bool active, ready_busy;
    epd_render_profile_t profile[3];
    unsigned profile_mask;
} open_trace;

static void open_trace_mark(unsigned stamp)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (open_trace.active && !(open_trace.mask & (1u << stamp))) {
        open_trace.tick[stamp] = (uint32_t)rt_tick_get();
        open_trace.mask |= 1u << stamp;
    }
    rt_hw_interrupt_enable(level);
}

static void open_trace_display(lv_event_t *event)
{
    if (!open_trace.active || !open_trace.screen ||
        lv_screen_active() != open_trace.screen ||
        (open_trace.mask & (1u << OPEN_REFR_END))) return;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_REFR_START:
        open_trace_mark(OPEN_REFR_BEGIN);
        epd_render_profile_window(true);
        epd_render_profile_snapshot(&open_trace.profile[0]);
        open_trace.profile_mask |= 1u;
        break;
    case LV_EVENT_RENDER_START: open_trace_mark(OPEN_RENDER_BEGIN); break;
    case LV_EVENT_RENDER_READY:
        open_trace_mark(OPEN_RENDER_END);
        epd_render_profile_snapshot(&open_trace.profile[1]);
        open_trace.profile_mask |= 2u;
        if (open_trace.mask & (1u << OPEN_WORKER)) epd_render_profile_window(false);
        break;
    case LV_EVENT_REFR_READY:
        open_trace_mark(OPEN_REFR_END);
        if (!(open_trace.profile_mask & 2u)) epd_render_profile_window(false);
        break;
    default: break;
    }
}

static void open_trace_stop(void)
{
    epd_render_profile_window(false);
    rt_base_t level = rt_hw_interrupt_disable();
    open_trace.active = false;
    rt_hw_interrupt_enable(level);
    if (open_trace.display)
        lv_display_remove_event_cb_with_user_data(open_trace.display, open_trace_display, NULL);
    open_trace.display = NULL;
    open_trace.screen = NULL;
}

static void open_trace_begin(void)
{
    open_trace_stop();
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t serial = open_trace.serial + 1u;
    if (!serial) ++serial;
    memset(&open_trace, 0, sizeof(open_trace));
    open_trace.serial = serial;
    open_trace.active = true;
    rt_hw_interrupt_enable(level);
    open_trace_mark(OPEN_BEGIN);
    lv_display_t *display = lv_display_get_default();
    if (display) {
        lv_display_add_event_cb(display, open_trace_display, LV_EVENT_ALL, NULL);
        open_trace.display = display;
    }
}

static uint32_t open_trace_worker_begin(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    uint32_t serial = 0;
    if (open_trace.active && (open_trace.mask & (1u << OPEN_ATTACHED)) &&
        !(open_trace.mask & (1u << OPEN_WORKER))) {
        serial = open_trace.serial;
        open_trace_mark(OPEN_WORKER);
        if ((open_trace.profile_mask & 3u) == 3u) {
            epd_render_profile_snapshot(&open_trace.profile[2]);
            open_trace.profile_mask |= 4u;
            epd_render_profile_window(false);
        }
    }
    rt_hw_interrupt_enable(level);
    return serial;
}

static void open_trace_worker_mark(uint32_t serial, unsigned stamp)
{
    if (!serial) return;
    rt_base_t level = rt_hw_interrupt_disable();
    if (serial == open_trace.serial) open_trace_mark(stamp);
    rt_hw_interrupt_enable(level);
}

static int open_trace_age(unsigned stamp)
{
    return open_trace.mask & (1u << stamp) ?
        (int32_t)(open_trace.tick[stamp] - open_trace.tick[OPEN_BEGIN]) : -1;
}

static int open_trace_profile(unsigned stage, unsigned end, bool calls)
{
    unsigned begin = end - 1u;
    unsigned mask = (1u << begin) | (1u << end);
    if ((open_trace.profile_mask & mask) != mask) return -1;
    const epd_render_profile_item_t *a = &open_trace.profile[begin].stage[stage];
    const epd_render_profile_item_t *b = &open_trace.profile[end].stage[stage];
    return (int32_t)(calls ? b->calls - a->calls : b->ticks - a->ticks);
}

static void open_trace_poll(void)
{
    if (!open_trace.active || !current || current->screen != open_trace.screen ||
        !ui_reader_view()->ready || ui_reader_waiting()) return;
    if (!lv_refreshing_done()) { open_trace.ready_busy = true; return; }
    if (!open_trace.ready_busy || !(open_trace.mask & (1u << OPEN_READER_DONE))) return;
    open_trace_stop();
    rt_kprintf("books open_ui serial=%u tick=%u hz=%u mask=%x attached=%d nav=%d ui=%d "
               "create=%d:%d scale=%d:%d resume=%d\n",
        (unsigned)open_trace.serial, (unsigned)open_trace.tick[OPEN_BEGIN], (unsigned)RT_TICK_PER_SECOND,
        (unsigned)open_trace.mask, open_trace_age(OPEN_ATTACHED),
        open_trace_age(OPEN_NAV_DONE), open_trace_age(OPEN_UI_DONE),
        open_trace_age(OPEN_CREATE_BEGIN), open_trace_age(OPEN_CREATE_END),
        open_trace_age(OPEN_SCALE_BEGIN), open_trace_age(OPEN_SCALE_END),
        open_trace_age(OPEN_RESUMED));
    rt_kprintf("books open_dispatch serial=%u worker=%d shelf_done=%d reader_done=%d "
               "refr=%d:%d render=%d:%d sys_dsc=%d/%d sys_bmp=%d/%d "
               "gray=%d/%d gray_tail=%d/%d\n",
        (unsigned)open_trace.serial, open_trace_age(OPEN_WORKER),
        open_trace_age(OPEN_SHELF_DONE), open_trace_age(OPEN_READER_DONE),
        open_trace_age(OPEN_REFR_BEGIN), open_trace_age(OPEN_REFR_END),
        open_trace_age(OPEN_RENDER_BEGIN), open_trace_age(OPEN_RENDER_END),
        open_trace_profile(EPD_RENDER_DSC, 1, true), open_trace_profile(EPD_RENDER_DSC, 1, false),
        open_trace_profile(EPD_RENDER_BITMAP, 1, true), open_trace_profile(EPD_RENDER_BITMAP, 1, false),
        open_trace_profile(EPD_RENDER_SETTLE, 1, false), open_trace_profile(EPD_RENDER_COPY, 1, false),
        open_trace_profile(EPD_RENDER_SETTLE, 2, false), open_trace_profile(EPD_RENDER_COPY, 2, false));
}

static bool page_input_ready(uint32_t key)
{
    if (!initialized || !current || !current->foreground || pending_page != NAV_IDLE)
        return false;
    if (launcher_current_page() == UI_PAGE_READER && !current->popup)
    {
        if (current->reader_confirm_pending && key != LV_KEY_ENTER && key != LV_KEY_ESC)
            return false;
    }
    return true;
}

static bool page_input_move(const epd_input_event_t *events, unsigned count)
{
    if (epd_reader_note_visible()) return true;
    if (!current || !current->foreground || !count) return true;
    if (launcher_current_page() == UI_PAGE_READER && !current->reader_panel && !current->popup)
    {
        int64_t movement = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            movement += events[i].key == LV_KEY_PREV ? -(int64_t)events[i].repeat : events[i].repeat;
            if (movement < -(int64_t)UI_READER_PAGE_MAX) movement = -(int64_t)UI_READER_PAGE_MAX;
            if (movement > UI_READER_PAGE_MAX) movement = UI_READER_PAGE_MAX;
        }
        if (movement) ui_reader_turn((int)movement);
        return true;
    }

    return false;
}

static bool page_input_key(uint32_t key)
{
    if (epd_reader_note_visible()) {
        if (key == LV_KEY_ESC) epd_reader_note_close();
        return true;
    }
    if (!initialized || !current || !current->foreground ||
        pending_page != NAV_IDLE) return true;
    if (launcher_current_page() == UI_PAGE_READER)
    {
        if (current->reader_confirm_pending)
        {
            if (key == LV_KEY_ESC || key == LV_KEY_ENTER) pending_page = NAV_BACK;
            return true;
        }
        if (!current->reader_panel && !current->popup)
        {
            switch (key)
            {
            case LV_KEY_ENTER: pending_page = READER_MENU; break;
            case LV_KEY_ESC: pending_page = NAV_BACK; break;
            default: break;
            }
            return true;
        }
        if (current->reader_panel && key == LV_KEY_ESC)
        {
            pending_page = BOOKS_MENU_BACK;
            return true;
        }
    }
    if (key == LV_KEY_ESC)
    {
        if (current->popup) pending_page = POPUP_CLOSE;
        else pending_page = NAV_BACK;
        return true;
    }
    return false;
}


static const epd_input_ops_t input_ops = {page_input_ready, page_input_move, page_input_key};

static void reader_key(epd_app_key_t key, epd_app_key_event_t event, void *data)
{
    if (epd_reader_note_visible()) return;
    books_page_t *page = data;
    if (page == current && page->foreground && page->id == UI_PAGE_READER &&
        !page->reader_panel && !page->popup && event == EPD_APP_KEY_LONG_PRESSED &&
        key == EPD_APP_KEY3 && pending_page == NAV_IDLE) {
        pending_page = BOOKS_MENU_AI;
        return;
    }
    if (event != EPD_APP_KEY_CLICKED || page != current || !page->foreground ||
        page->id != UI_PAGE_READER || page->reader_panel || page->popup) return;
    uint32_t direction = key == EPD_APP_KEY2 ? LV_KEY_PREV : LV_KEY_NEXT;
    if (!page_input_ready(direction)) return;
    epd_input_event_t movement = {direction, 1};
    page_input_move(&movement, 1);
}

static unsigned focus_index(void)
{
    lv_obj_t *focused = lv_group_get_focused(current->group);
    for (unsigned i = 0; i < current->focus_count; ++i)
        if (current->focus_items[i] == focused) return i;
    return 0;
}

static void clicked(lv_event_t *event)
{
    if (storage_changing() || !current || !current->foreground ||
        lv_obj_get_screen(lv_event_get_target_obj(event)) != current->screen ||
        pending_page != NAV_IDLE) return;
    int target = (int)(intptr_t)lv_event_get_user_data(event);
    if (current->id == UI_PAGE_READER && current->reader_confirm_pending && target != NAV_BACK) return;
    lv_group_focus_obj(lv_event_get_target_obj(event));
    pending_page = target;
}

lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int width, int height, const char *text, int target)
{
    /* Modal scopes block physical keys and retain the page's explicit back
     * semantics. Closing one cannot deliver the held gesture to the reader. */
    lv_obj_t *modal = current->popup ? current->popup : current->reader_panel;
    if (modal) epd_input_set_ops(epd_app_input_group(modal), &input_ops);
    lv_obj_t *button = epd_app_button(parent, x, y, width, height, text,
                                     clicked, (void *)(intptr_t)target);
    LV_ASSERT(current->focus_count < FOCUS_CAPACITY);
    current->focus_items[current->focus_count++] = button;
    return button;
}

lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font)
{
    return label_create(screen, text, PAGE_TITLE_X, PAGE_TITLE_Y,
                         WIDTH - MARGIN - PAGE_TITLE_X, font);
}

void object_visible(lv_obj_t *object, bool visible)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == !visible) return;
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void focus_restore(unsigned index)
{
    if (index < current->focus_count && focus_eligible(current->focus_items[index]))
    {
        lv_group_focus_obj(current->focus_items[index]);
        return;
    }
    for (unsigned i = 0; i < current->focus_count; ++i)
        if (focus_eligible(current->focus_items[i]))
        {
            lv_group_focus_obj(current->focus_items[i]);
            return;
        }
}

void popup_close(void)
{
    if (!current->popup) return;
    lv_obj_delete(current->popup);
    current->popup = NULL;
    current->popup_text = current->popup_detail = current->popup_button = NULL;
    current->popup_notice = BOOKS_NOTICE_NONE;
    while (current->focus_count > current->popup_focus_start)
        current->focus_items[--current->focus_count] = NULL;
    for (unsigned i = 0; i < current->focus_count; ++i) lv_group_add_obj(current->group, current->focus_items[i]);
    if (focus_eligible(current->popup_previous_focus)) lv_group_focus_obj(current->popup_previous_focus);
    else focus_restore(0);
    current->popup_previous_focus = NULL;
}

void popup_open(const char *text, const char *detail)
{
    if (current->popup) return;
    current->popup_focus_start = current->focus_count;
    current->popup_previous_focus = lv_group_get_focused(current->group);
    lv_group_remove_all_objs(current->group);
    current->popup = panel_create(lv_screen_active(), 0, 0, WIDTH, HEIGHT);
    lv_obj_add_flag(current->popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(current->popup, 0, 0);
    lv_obj_set_style_border_width(current->popup, 0, 0);
    lv_obj_set_style_bg_color(current->popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(current->popup, LV_OPA_TRANSP, 0);
    lv_obj_t *dialog = panel_create(current->popup, 54, 438, 576, 282);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(dialog, 2, 0);
    current->popup_text = centered_label(dialog, text, 24, detail ? 44 : 74, 528, ui_font_body());
    current->popup_detail = centered_label(dialog, detail ? detail : "", 24, 102, 528, ui_font_small());
    current->popup_button = button_create(dialog, 108, 184, 360, 64, epd_get_str(books_ok), POPUP_CLOSE);
    popup_layout();
    lv_group_focus_obj(current->popup_button);
}

void popup_layout(void)
{
    if (!current->popup) return;
    int gap = epd_ui_px(24), width = WIDTH - 2 * epd_ui_px(54);
    int text_width = width - 2 * gap;
    lv_obj_t *dialog = lv_obj_get_parent(current->popup_text);
    reader_ui_label(current->popup_text, gap, gap, text_width, EPD_FONT_BODY);
    lv_label_set_long_mode(current->popup_text, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(current->popup_text);
    int detail_y = gap + lv_obj_get_height(current->popup_text) + gap;
    reader_ui_label(current->popup_detail, gap, detail_y, text_width, EPD_FONT_SMALL);
    lv_label_set_long_mode(current->popup_detail, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(current->popup_detail);
    int button_y = detail_y + lv_obj_get_height(current->popup_detail) + gap;
    int height = button_y + epd_ui_touch(64) + gap;
    reader_ui_rect(dialog, (WIDTH - width) / 2, (HEIGHT - height) / 2, width, height);
    lv_obj_set_style_radius(dialog, epd_ui_px(25), 0);
    reader_ui_button(current->popup_button, gap, button_y, text_width, epd_ui_touch(64));
    lv_obj_set_style_radius(current->popup_button, epd_ui_px(20), 0);
}

bool popup_refresh(void)
{
    if (!current->popup) return true;
    bool updated = true;
    const char *text = "", *detail = "";
    switch (current->popup_notice)
    {
    case BOOKS_NOTICE_PAGE: text = epd_get_str(books_page_failed); break;
    case BOOKS_NOTICE_CONTENT: text = app_content_error(current->popup_content_error); break;
    case BOOKS_NOTICE_OPEN:
        text = epd_get_str(books_open_failed);
        detail = current->popup_error ? reader_error_text(current->popup_error) : epd_get_str(books_check_storage);
        break;
    case BOOKS_NOTICE_STORAGE:
        text = epd_get_str(books_open_failed);
        detail = epd_get_str(books_storage_unavailable);
        break;
    case BOOKS_NOTICE_SCAN:
        text = epd_get_str(books_scan_failed);
        detail = epd_get_str(books_check_storage);
        break;
    case BOOKS_NOTICE_READER:
        text = reader_error_text(current->popup_error);
        detail = epd_get_str(books_reopen);
        break;
    default: return true;
    }
    updated &= text_update(current->popup_text, text);
    updated &= text_update(current->popup_detail, detail);
    updated &= text_update(lv_obj_get_child(current->popup_button, 0), epd_get_str(books_ok));
    return updated;
}

void popup_show(books_notice_t notice, reader_error_t error, rt_err_t content_error)
{
    if (current->popup) return;
    current->popup_notice = notice;
    current->popup_error = error;
    current->popup_content_error = content_error;
    popup_open("", "");
    popup_refresh();
}

static bool books_initialize(void)
{
    if (initialized) return true;
    initialization_attempted = true;
    if (bookshelf_service_init() != RT_EOK) return false;
    ui_bookshelf_refresh();
    storage_generation = storage_revision();
    initialized = true;
    return true;
}

static bool page_open(unsigned page)
{
    if (page == UI_PAGE_TEXT_SETTINGS) return epd_app_open_text_settings();
    if (page >= sizeof(page_names) / sizeof(page_names[0])) return false;
    int result = gui_app_is_page_present((char *)page_names[page]) ?
        gui_app_goback_to_page(page_names[page]) :
        gui_app_create_page_for_app_ext("books", page_names[page], page_message,
                                        (void *)(uintptr_t)page, sizeof(books_page_t));
    if (result != RT_EOK) return false;
    gui_app_exec_now();
    return current && current->foreground && current->id == page;
}

static void process_navigation(void)
{
    if (!current || !current->foreground || pending_page == NAV_IDLE) return;
    int target = pending_page;
    pending_page = NAV_IDLE;
    bool opening = current->id == UI_PAGE_BOOKSHELF &&
        target >= BOOK_OPEN_BASE && target < BOOK_OPEN_BASE + BOOKSHELF_PAGE_SIZE;
    if (opening) open_trace_begin();
    bool opened = false;
    bool handled = current->id == UI_PAGE_BOOKSHELF ?
        bookshelf_action(&target, &opened) : reader_action(&target, &opened);
    if (opening) {
        if (opened) open_trace_mark(OPEN_ATTACHED); else open_trace_stop();
    }
    if (handled) return;
    if (target == POPUP_CLOSE) { popup_close(); return; }
    bool navigated;
    if (target == NAV_BACK)
    {
        books_page_t *previous = current;
        navigated = gui_app_goback() == RT_EOK;
        if (navigated)
        {
            gui_app_exec_now();
            navigated = current != previous;
        }
    }
    else navigated = page_open((unsigned)target);
    if (opening) open_trace_mark(OPEN_NAV_DONE);
    if (!navigated)
    {
        if (opening) open_trace_stop();
        if (opened)
        {
            ui_reader_close();
            reader_session = false;
        }
        if (current && current->foreground) popup_show(BOOKS_NOTICE_PAGE, READER_ERROR_NONE, RT_EOK);
    }
}

static void content_process(void)
{
    if ((!content_path[0] && !content_error) || !current || !current->foreground ||
        current->id != UI_PAGE_BOOKSHELF || current->content_generation != content_generation ||
        live_pages != 1) return;
    rt_err_t error = content_error;
    content_error = RT_EOK;
    if (error)
    {
        popup_show(BOOKS_NOTICE_CONTENT, READER_ERROR_NONE, error);
        return;
    }
    bool available = storage_path_available(content_path) &&
        (strcmp(storage_path_root(content_path), STORAGE_SD_ROOT) ||
         storage_session_valid(STORAGE_SD, content_session));
    pending_page = NAV_IDLE;
    ui_reader_close();
    reader_session = false;
    unsigned index;
    open_trace_begin();
    bool opened = available && ui_bookshelf_include_path(content_path, &index) && ui_reader_open(index);
    content_path[0] = 0;
    if (opened)
    {
        reader_session = true;
        open_trace_mark(OPEN_ATTACHED);
        bool navigated = page_open(UI_PAGE_READER);
        open_trace_mark(OPEN_NAV_DONE);
        if (navigated) return;
        ui_reader_close();
        reader_session = false;
    }
    open_trace_stop();
    if (current && current->foreground)
        popup_show(available ? BOOKS_NOTICE_OPEN : BOOKS_NOTICE_STORAGE, READER_ERROR_NONE, RT_EOK);
}

static bool books_flush(void)
{
    if (!initialized) return true;
    if (reader_session && ui_reader_view()->ready) return ui_reader_save();
    return ui_bookshelf_flush();
}

static bool service_process(epd_service_t *service, bool stopping)
{
    books_service = service;
    open_trace_poll();
    epd_reader_collect();
    if (stopping)
    {
        open_trace_stop();
        books_command_ui_process(true);
        if ((live_pages && !idle_stopping) || !lv_refreshing_done() || !books_ai_shutdown()) return false;
        ui_reader_close();
        document_service_shutdown();
        ui_bookshelf_close();
        bookshelf_service_deinit();
        initialized = initialization_attempted = false;
        reader_session = false;
        books_service = NULL;
        if (!live_pages) { content_path[0] = 0; content_error = RT_EOK; }
        return true;
    }
    if (storage_changing()) return false;
    if (!initialization_attempted) books_initialize();
    if (!live_pages) {
        bool saved = books_flush();
        if (saved && books_ai_idle()) {
            idle_stopping = true;
            app_service_stop("books");
        } else epd_service_ui_request(service, 100);
        return false;
    }
    if (!initialized) return false;
    if (storage_generation != storage_revision())
    {
        storage_generation = storage_revision();
        if (launcher_current_page() == UI_PAGE_BOOKSHELF) bookshelf_resume();
    }
    content_process();
    books_command_ui_process(false);
    process_navigation();
    books_menu_process();
    if (books_process()) epd_service_ui_request(service, 16);
    open_trace_mark(OPEN_UI_DONE);
    return false;
}

static void books_worker(epd_service_t *service)
{
    while (!epd_service_cancelled(service))
    {
        uint32_t trace = open_trace_worker_begin();
        bool worked = ui_bookshelf_worker_step(service);
        open_trace_worker_mark(trace, OPEN_SHELF_DONE);
        worked |= epd_reader_worker_step(service);
        open_trace_worker_mark(trace, OPEN_READER_DONE);
        if (!worked) epd_service_wait(service, RT_WAITING_FOREVER);
    }
    ui_bookshelf_worker_shutdown();
    epd_reader_worker_shutdown();
}

const epd_background_t epd_app_background = {
    .run = books_worker,
    .stack_size = 32768,
    .priority = 22,
    .command = books_command,
    .ui_process = service_process,
    .flush = books_flush
};

static void books_restart(lv_timer_t *timer)
{
    if (live_pages && !app_service_running("books") && !books_service_start()) return;
    if (live_pages && (!books_service || epd_service_cancelled(books_service))) return;
    if (waiting_page && live_pages) {
        books_page_t *page = waiting_page;
        waiting_page = NULL;
        books_page_build(page);
        if (page->foreground) books_page_resume(page);
    }
    restart_timer = NULL;
    lv_timer_delete(timer);
}

static bool books_service_start(void)
{
    if (app_service_running("books")) {
        if ((!books_service || epd_service_cancelled(books_service)) && !restart_timer)
            restart_timer = lv_timer_create(books_restart, 20, NULL);
        return books_service && !epd_service_cancelled(books_service) ? true : restart_timer != NULL;
    }
    if (!books_initialize()) return false;
    void *module = dlmodule_find("books");
    if (!module) return false;
    idle_stopping = false;
    books_service = app_service_start("books", &epd_app_background, module);
    return books_service != NULL;
}

static void books_page_build(books_page_t *page)
{
    books_page_t *previous = current;
    current = page;
    page->group = epd_app_input_group(page->screen);
    epd_input_set_ops(page->group, &input_ops);
    epd_obj_init(page->screen);
    lv_obj_set_size(page->screen, WIDTH, HEIGHT);
    lv_obj_set_style_bg_color(page->screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(page->screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(page->screen, ui_font_body(), 0);
    if (page->id == UI_PAGE_READER)
    {
        open_trace.screen = page->screen;
        open_trace_mark(OPEN_CREATE_BEGIN);
        reader_start(page->screen);
        open_trace_mark(OPEN_CREATE_END);
        epd_app_input_listen(page->screen, reader_key, page);
    }
    else
    {
        epd_app_header(page->screen);
        bookshelf_start(page->screen);
    }
    lv_obj_t *back = epd_app_back_button_cb(page->screen, clicked, (void *)(intptr_t)NAV_BACK);
    page->back = back;
    if (page->id == UI_PAGE_READER)
    {
        epd_app_back_button_layout(back, epd_ui_px(24), UI_READER_TOP / 2);
        lv_obj_add_flag(back, LV_OBJ_FLAG_HIDDEN);
    }
    page->focus_items[page->focus_count++] = back;
    if (page->id == UI_PAGE_READER) open_trace_mark(OPEN_SCALE_BEGIN);
    books_scale_refresh();
    if (page->id == UI_PAGE_READER) open_trace_mark(OPEN_SCALE_END);
    focus_restore(saved_focus[page->id]);
    current = previous;
}

static void books_page_resume(books_page_t *page)
{
    current = page;
    if (lv_refreshing_done()) books_scale_refresh();
    if (page->id == UI_PAGE_READER) {
        open_trace.screen = page->screen;
        reader_foreground();
        open_trace_mark(OPEN_RESUMED);
    }
    else
    {
        epd_app_header_refresh(page->screen);
        bookshelf_resume();
    }
}

static void page_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    books_page_t *page = gui_app_this_page_memory();
    books_page_t *previous = current;
    current = page;
    switch (message)
    {
    case GUI_APP_MSG_ONSTART:
    {
        ++live_pages;
        page->content_generation = content_generation;
        page->locale_revision = epd_locale_revision();
        gui_app_close_anim();
        page->id = (unsigned)(uintptr_t)gui_app_this_page_userdata();
        page->screen = lv_screen_active();
        if (restart_timer) waiting_page = page;
        else books_page_build(page);
        current = previous;
        break;
    }
    case GUI_APP_MSG_ONRESUME:
        page->foreground = true;
        if (waiting_page == page) break;
        books_page_resume(page);
        break;
    case GUI_APP_MSG_ONPAUSE:
        if (page->screen == open_trace.screen) open_trace_stop();
        if (waiting_page != page) {
            saved_focus[page->id] = focus_index();
            if (page->id == UI_PAGE_READER) reader_pause();
            else bookshelf_pause();
        }
        page->foreground = false;
        pending_page = NAV_IDLE;
        current = NULL;
        break;
    case GUI_APP_MSG_ONSTOP:
        if (page->screen == open_trace.screen) open_trace_stop();
        if (waiting_page == page) waiting_page = NULL;
        if (page->id == UI_PAGE_READER) reader_stop();
        --live_pages;
        if (!live_pages) {
            if (restart_timer) { lv_timer_delete(restart_timer); restart_timer = NULL; }
            if (books_service) epd_service_ui_request(books_service, 0);
        }
        current = previous == page ? NULL : previous;
        break;
    default:
        current = previous;
        break;
    }
}

int app_main(intent_t intent)
{
    if (!books_service_start()) return -RT_ENOMEM;
    ++content_generation;
    content_error = app_content_receive(intent, content_path, sizeof(content_path));
    if (content_error == -ENOENT) content_error = RT_EOK;
    content_session = storage_card_session();
    int result = gui_app_regist_msg_handler_ext("books", page_message,
                                               (void *)(uintptr_t)UI_PAGE_BOOKSHELF, sizeof(books_page_t));
    if (result != RT_EOK && !live_pages) {
        if (restart_timer) { lv_timer_delete(restart_timer); restart_timer = NULL; }
        if (books_service) epd_service_ui_request(books_service, 0);
    }
    return result;
}
