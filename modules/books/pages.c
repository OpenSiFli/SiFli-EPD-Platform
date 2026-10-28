#include "platform/epd_locale.h"
#include "platform/epd_app_font.h"
#include "books_ui.h"
#include "reader_controls.h"
#include "reader_ai.h"
#include "../common/reader/reader.h"
#include "reader_gesture.h"
#include "storage.h"
#include "document/document_view.h"
#include "../common/reader/reader_geometry.h"
#include "../common/reader/reader_ui_layout.h"
#include "../common/reader/reader_view_touch_ui.h"

static lv_obj_t *book_progress_create(lv_obj_t *parent, int x, int y, int width);
static void bookshelf_page_button_update(lv_obj_t *button, bool visible, bool enabled);
static bool bookshelf_update(bool reset_focus);
static void bookshelf_create(lv_obj_t *screen);
static bool reader_status_render(void);
static bool reader_footer_render(void);
static void reader_render(void);
static void reader_update(void);
static void reader_touch_event(lv_event_t *event);
static void reader_create(lv_obj_t *screen);
static bool reader_option_render(void);
static void reader_panel_close(void);
static void reader_panel_open(void);
static void reader_save_position(void);
static void reader_resume(void);
static void reader_resume_prepare(void);
static void books_locale_refresh(void);
static void reader_controls_layout(void);

bool reader_session;
static reader_view_touch_t reader_gesture;
static unsigned saved_bookshelf_first;
static bool reader_page_live;
static bool reader_resume_pending;
static uint32_t books_storage_revision;
static unsigned refresh_page;
static uint32_t refresh_started;
static bool refresh_pending, refresh_busy;
static epd_app_font_cache_stats_t refresh_glyphs;

/* Timings start when the UI observes a pending target, not at the input event.
 * step_ms includes reader publication/save work; poll_gap_ms excludes this
 * callback. busy_polls samples the display guard, not physical panel duration. */
static struct
{
    bool active;
    uint32_t started, last, poll_gap_ms, max_gap_ms, step_ms, last_step_ms, render_ms;
    unsigned busy_polls;
} submit_trace;

static void reader_submit_poll(bool idle)
{
    if (!submit_trace.active)
    {
        if (!ui_reader_waiting()) return;
        memset(&submit_trace, 0, sizeof(submit_trace));
        submit_trace.active = true;
        submit_trace.started = submit_trace.last = lv_tick_get();
    }
    uint32_t gap = lv_tick_elaps(submit_trace.last);
    submit_trace.poll_gap_ms += gap;
    if (gap > submit_trace.max_gap_ms) submit_trace.max_gap_ms = gap;
    if (!idle) ++submit_trace.busy_polls;
}

void bookshelf_start(lv_obj_t *screen)
{
    current->bookshelf_first = saved_bookshelf_first;
    if (!reader_session)
    {
        ui_bookshelf_refresh();
        books_storage_revision = storage_revision();
    }
    bookshelf_create(screen);
}

void bookshelf_resume(void)
{
    if (!reader_session && books_storage_revision != storage_revision())
    {
        ui_bookshelf_refresh();
        books_storage_revision = storage_revision();
    }
    bookshelf_update(false);
}
void bookshelf_pause(void) { saved_bookshelf_first = current->bookshelf_first; }

void reader_start(lv_obj_t *screen)
{
    submit_trace.active = false;
    refresh_pending = refresh_busy = false;
    refresh_page = 0;
    reader_view_touch_reset(&reader_gesture);
    reader_page_live = true;
    reader_resume_pending = false;
    reader_create(screen);
}

void reader_foreground(void)
{
    reader_resume();
}

void reader_pause(void)
{
    books_menu_pause();
    submit_trace.active = false;
    refresh_pending = refresh_busy = false;
    reader_view_touch_reset(&reader_gesture);
    reader_resume_pending = false;
    if (!reader_session) return;
    ui_reader_cancel_pending();
    current->reader_confirm_pending = false;
    reader_save_position();
}

void reader_stop(void)
{
    books_menu_close();
    submit_trace.active = false;
    refresh_pending = refresh_busy = false;
    reader_view_touch_reset(&reader_gesture);
    reader_page_live = false;
    reader_resume_pending = false;
    if (document_view_active()) document_view_detach();
}

bool reader_action(int *request, bool *opened)
{
    (void)opened;
    if (launcher_current_page() != UI_PAGE_READER) return false;
    if (books_menu_action(*request)) return true;
    if (*request == READER_PREVIOUS || *request == READER_NEXT) {
        if (!current->reader_panel) ui_reader_turn(*request == READER_PREVIOUS ? -1 : 1);
        return true;
    }
    return false;
}

bool bookshelf_action(int *request, bool *opened)
{
    int target = *request;
    if (launcher_current_page() == UI_PAGE_BOOKSHELF)
    {
        if (target == BOOKSHELF_PREVIOUS || target == BOOKSHELF_NEXT)
        {
            if (target == BOOKSHELF_PREVIOUS && current->bookshelf_first > 0)
                current->bookshelf_first -= BOOKSHELF_PAGE_SIZE;
            else if (target == BOOKSHELF_NEXT && current->bookshelf_first + BOOKSHELF_PAGE_SIZE < ui_bookshelf_count())
                current->bookshelf_first += BOOKSHELF_PAGE_SIZE;
            else
                return true;
            bookshelf_update(true);
            return true;
        }
        if (target >= BOOK_OPEN_BASE && target < BOOK_OPEN_BASE + BOOKSHELF_PAGE_SIZE)
        {
            unsigned index = current->bookshelf_first + (unsigned)(target - BOOK_OPEN_BASE);
            if (!ui_bookshelf_book(index)) return true;
            /* ui_reader_open replaces the previous session even when opening fails. */
            reader_session = false;
            if (!ui_reader_open(index))
            {
                popup_show(BOOKS_NOTICE_OPEN, document_view_get()->error_code, RT_EOK);
                return true;
            }
            reader_session = true;
            *opened = true;
            target = UI_PAGE_READER;
        }
    }

    *request = target;
    return false;
}

bool books_process(void)
{
    bool runnable = false;
    bool idle = lv_refreshing_done();
    bool modal = books_menu_active() || (current && current->foreground &&
        current->popup && !lv_obj_has_flag(current->popup, LV_OBJ_FLAG_HIDDEN));
    ui_reader_background_pause(modal || books_menu_font_pending());
    if (!reader_session || !reader_page_live) {
        refresh_pending = refresh_busy = false; refresh_page = 0;
        submit_trace.active = false;
    } else if (refresh_pending) {
        if (!idle) refresh_busy = true;
        else if (refresh_busy) {
            epd_app_font_cache_stats_t glyphs;
            epd_app_font_cache_stats(&glyphs);
            unsigned source_ms = glyphs.ticks_per_second ?
                (unsigned)((glyphs.source_bitmap_ticks - refresh_glyphs.source_bitmap_ticks) * 1000 /
                           glyphs.ticks_per_second) : 0;
            rt_kprintf("books panel page=%u commit_to_idle_ms=%u glyph_hit=%u glyph_miss=%u source_calls=%u source_ms=%u prepare_fail=%u\n",
                refresh_page, (unsigned)lv_tick_elaps(refresh_started),
                (unsigned)(glyphs.glyph_hits - refresh_glyphs.glyph_hits),
                (unsigned)(glyphs.glyph_misses - refresh_glyphs.glyph_misses),
                (unsigned)(glyphs.source_bitmap_calls - refresh_glyphs.source_bitmap_calls), source_ms,
                (unsigned)(glyphs.prepare_failures - refresh_glyphs.prepare_failures));
            refresh_pending = false;
        }
    }
    if (idle && current && current->foreground)
        books_scale_refresh();
    if (idle && current && current->foreground && current->locale_revision != epd_locale_revision())
        books_locale_refresh();
    /* Reader saves use shelf indices until the reading session closes. */
    if (!reader_session && current && current->foreground &&
        current->id == UI_PAGE_BOOKSHELF && !current->popup && pending_page == NAV_IDLE)
    {
        if (ui_bookshelf_process()) bookshelf_update(false);
        if (ui_bookshelf_take_error()) popup_show(BOOKS_NOTICE_SCAN, READER_ERROR_NONE, RT_EOK);
    }
    if (reader_session)
    {
        bool leaving_page = pending_page == NAV_BACK ||
            (pending_page >= UI_PAGE_HOME && pending_page < UI_PAGE_COUNT &&
             pending_page != UI_PAGE_READER);
        bool resuming = idle && reader_page_live && launcher_current_page() == UI_PAGE_READER &&
            current->foreground && !leaving_page && !modal && !books_menu_font_pending() &&
            (reader_resume_pending || ui_reader_settings_changed());
        if (resuming)
        {
            reader_resume_pending = false;
            reader_resume_prepare();
        }
        if (idle && reader_page_live && current && current->foreground &&
            launcher_current_page() == UI_PAGE_READER && !leaving_page && !modal)
            books_menu_navigate();
        bool foreground = launcher_current_page() == UI_PAGE_READER && !leaving_page &&
            reader_page_live && current->foreground &&
            !current->popup && (!current->reader_panel || current->reader_confirm_pending ||
                                current->reader_reflow_pending);
        if (foreground) reader_submit_poll(idle);
        else submit_trace.active = false;
        /* Continue preparation while the panel scans; only commit UI at idle. */
        uint32_t step_started = lv_tick_get();
        ui_reader_work_t work = ui_reader_step(idle ? 8 : 2, foreground, idle);
        if (submit_trace.active)
        {
            submit_trace.last_step_ms = lv_tick_elaps(step_started);
            submit_trace.step_ms += submit_trace.last_step_ms;
        }
        /* A prepared target can still be waiting for an idle display. */
        runnable = work != UI_READER_WORK_IDLE || (foreground && ui_reader_waiting());
        if (resuming)
        {
            reader_update();
            reader_status_render();
            if (current->reader_panel) reader_option_render();
        }
        if (idle && launcher_current_page() == UI_PAGE_READER && current->reader_reflow_pending &&
            document_view_active() && current->reader_revision != ui_reader_view()->revision)
            reader_render();
        if (idle && launcher_current_page() == UI_PAGE_READER && current->reader_reflow_pending &&
            ((ui_reader_view()->ready && !ui_reader_waiting()) || ui_reader_view()->error[0]))
        {
            current->reader_reflow_pending = false;
            /* An untouched menu target follows the restored text position after reflow. */
            if (current->reader_jump == current->reader_reflow_page)
                current->reader_jump = ui_reader_view()->page;
            reader_update();
            if (current->reader_panel) reader_option_render();
        }
        if (idle && !reader_page_live &&
            (document_view_active() || !ui_reader_view()->indexing || ui_reader_view()->error[0] ||
             ui_reader_settings_changed()))
        {
            ui_reader_close();
            reader_session = false;
            reader_resume_pending = false;
            runnable = false;
            if (launcher_current_page() == UI_PAGE_BOOKSHELF) bookshelf_resume();
        }
    }
    if (reader_page_live && current && current->foreground && launcher_current_page() == UI_PAGE_READER &&
        (!current->reader_panel || current->reader_confirm_pending || ui_reader_view()->error[0]) &&
        !current->popup && idle)
    {
        if (current->reader_confirm_pending)
        {
            if (document_view_active() && current->reader_revision != ui_reader_view()->revision) reader_render();
            const ui_reader_view_t *reading = ui_reader_view();
            unsigned target = reading->pages && current->reader_jump > reading->pages ? reading->pages : current->reader_jump;
            if ((reading->page == target && !ui_reader_waiting()) || reading->error[0])
            {
                reader_panel_close();
                reader_render();
            }
        }
        reader_update();
    }
    if (submit_trace.active) submit_trace.last = lv_tick_get();
    return runnable;
}

static lv_obj_t *book_progress_create(lv_obj_t *parent, int x, int y, int width)
{
    lv_obj_t *track = panel_create(parent, x, y, width, 8);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_radius(track, 4, 0);
    lv_obj_t *fill = panel_create(track, 0, 0, 0, 8);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(0x222222), 0);
    lv_obj_set_style_radius(fill, 4, 0);
    return fill;
}

static void bookshelf_page_button_update(lv_obj_t *button, bool visible, bool enabled)
{
    object_visible(button, visible);
    if (lv_obj_has_state(button, LV_STATE_DISABLED) == !enabled) return;
    if (enabled)
    {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    }
    else
    {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                               lv_color_hex(enabled ? 0x222222 : 0x999999), 0);
}

static bool bookshelf_update(bool reset_focus)
{
    bool updated = true;
    static const uint32_t cover_colors[] = {0xEEEEEE, 0xDDDDDD, 0xFFFFFF, 0xCCCCCC};
    unsigned count = ui_bookshelf_count();
    char text[128];
    if (current->bookshelf_first >= count)
        current->bookshelf_first = count ? ((count - 1) / BOOKSHELF_PAGE_SIZE) * BOOKSHELF_PAGE_SIZE : 0;
    bool empty = ui_bookshelf_storage_ready() && count == 0;
    object_visible(current->bookshelf_empty, empty);
    if (empty) updated &= text_update(current->bookshelf_empty, epd_get_str(books_empty));
    for (unsigned slot = 0; slot < BOOKSHELF_PAGE_SIZE; ++slot)
    {
        unsigned index = current->bookshelf_first + slot;
        object_visible(current->books[slot].card, index < count);
        if (index >= count) continue;
        const ui_bookshelf_book_t *book = ui_bookshelf_book(index);
        lv_color_t color = lv_color_hex(cover_colors[index % 4]);
        if (!lv_color_eq(lv_obj_get_style_bg_color(current->books[slot].cover, 0), color))
            lv_obj_set_style_bg_color(current->books[slot].cover, color, 0);
        updated &= text_update(current->books[slot].title, book->file.name);
        const char *last_read = book->last_read_kind == BOOK_LAST_READ_TODAY ? epd_get_str(books_today) :
            book->last_read_kind == BOOK_LAST_READ_READ ? epd_get_str(books_read) :
            book->last_read_kind == BOOK_LAST_READ_DATE ? book->last_read : "--";
        EPD_FMT_books_metadata(text, sizeof(text), document_format_name(book->file.format),
                 (unsigned long)((book->file.size + 1023u) / 1024u), last_read);
        updated &= text_update(current->books[slot].metadata, text);
        unsigned percent = book->position.progress > 100 ? 100 : book->position.progress;
        int width = (lv_obj_get_width(lv_obj_get_parent(current->books[slot].fill)) * percent + 50) / 100;
        object_visible(current->books[slot].fill, width > 0);
        if (lv_obj_get_width(current->books[slot].fill) != width)
            lv_obj_set_width(current->books[slot].fill, width);
        if (book->position.current_page && book->position.total_pages)
            EPD_FMT_books_progress_pages(text, sizeof(text),
                     (unsigned)book->position.progress, (unsigned)book->position.current_page,
                     (unsigned)book->position.total_pages);
        else if (book->position.current_page)
            EPD_FMT_books_progress_page(text, sizeof(text),
                     (unsigned)book->position.progress, (unsigned)book->position.current_page);
        else snprintf(text, sizeof(text), "%s", epd_get_str(books_unread));
        updated &= text_update(current->books[slot].progress, text);
    }
    bookshelf_page_button_update(current->bookshelf_previous, count > 0, current->bookshelf_first > 0);
    bookshelf_page_button_update(current->bookshelf_next, count > 0,
                                 current->bookshelf_first + BOOKSHELF_PAGE_SIZE < count);
    if (reset_focus || !focus_eligible(lv_group_get_focused(current->group))) focus_restore(0);
    return updated;
}

static void bookshelf_create(lv_obj_t *screen)
{
    current->bookshelf_title = page_title_create(screen, epd_get_str(books_title), ui_font_title());
    for (unsigned slot = 0; slot < BOOKSHELF_PAGE_SIZE; ++slot)
    {
        lv_obj_t *card = button_create(screen, MARGIN, 178 + slot * 210,
                                       CONTENT_WIDTH, 190, NULL, BOOK_OPEN_BASE + (int)slot);
        current->books[slot].card = card;
        lv_obj_t *cover = panel_create(card, 20, 22, 112, 146);
        current->books[slot].cover = cover;
        lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(cover, 4, 0);
        icon_create(cover, &ui_icon_bookshelf, 15, 32);
        current->books[slot].title = label_create(card, "", 152, 22, 442, ui_font_body());
        current->books[slot].metadata = label_create(card, "", 152, 68, 442, ui_font_small());
        current->books[slot].fill = book_progress_create(card, 152, 115, 442);
        current->books[slot].progress = label_create(card, "", 152, 141, 442, ui_font_caption());
    }
    current->bookshelf_empty = centered_label(screen, "", MARGIN,
        (PAGE_CONTENT_Y + HEIGHT - MARGIN - ui_font_body()->line_height) / 2,
        CONTENT_WIDTH, ui_font_body());
    current->bookshelf_previous = button_create(screen, MARGIN, HEIGHT - MARGIN - 70,
                                                298, 70, epd_get_str(books_previous), BOOKSHELF_PREVIOUS);
    current->bookshelf_next = button_create(screen, 354, HEIGHT - MARGIN - 70,
                                            298, 70, epd_get_str(books_next), BOOKSHELF_NEXT);
    lv_obj_t *buttons[] = {current->bookshelf_previous, current->bookshelf_next};
    for (unsigned i = 0; i < 2; ++i)
    {
        lv_obj_set_style_border_color(buttons[i], lv_color_hex(0xBBBBBB), LV_STATE_DISABLED);
    }
    bookshelf_update(false);
}

static bool reader_status_render(void)
{
    bool updated = true;
    /* Update with reader operations, never from the periodic status poll. */
    char text[32];
    epd_app_status_t status;
    epd_app_status(&status);
    EPD_FMT_reader_battery(text, sizeof(text), status.battery_percent);
    updated &= text_update(current->reader_battery, text);
    updated &= text_update(current->reader_clock, status.clock);
    return updated;
}

static bool reader_footer_render(void)
{
    bool updated = true;
    const ui_reader_view_t *reading = ui_reader_view();
    if (!current->reader_footer) return true;
    char text[196];
    if (reading->error[0]) updated &= text_update(current->reader_footer, reading->error);
    else if (!reading->ready)
    {
        if (current->reader_loading_shown && current->reader_has_content)
            updated &= text_update(current->reader_footer, document_view_active() ?
                        epd_get_str(reader_preparing) : epd_get_str(reader_paginating));
        else if (!current->reader_has_content) updated &= text_update(current->reader_footer, "");
    }
    else
    {
        if (!reading->saved)
            EPD_FMT_reader_unsaved(text, sizeof(text), reading->page);
        else if (reading->pages)
            EPD_FMT_reader_pages(text, sizeof(text), reading->page, reading->pages,
                     reading->percent);
        else
            EPD_FMT_reader_page_unknown(text, sizeof(text), reading->page, reading->percent);
        updated &= text_update(current->reader_footer, text);
    }
    current->reader_rendered_pages = reading->pages;
    return updated;
}

static void reader_render(void)
{
    const ui_reader_view_t *reading = ui_reader_view();
    if (!current->reader_body) return;
    uint32_t render_started = lv_tick_get();
    bool first_content = !current->reader_has_content;
    if (!reading->ready && !reading->error[0])
    {
        if (!current->reader_loading)
        {
            current->reader_loading = true;
            current->reader_loading_shown = false;
            current->reader_loading_tick = lv_tick_get();
        }
    }
    else current->reader_loading = current->reader_loading_shown = false;
    if (current->reader_title && reading->title) text_update(current->reader_title, reading->title);
    reader_status_render();
    if (document_view_active())
    {
        if (document_view_bind(current->reader_body)) text_update(current->reader_body, "");
        else if (!reading->ready && !reading->error[0] && current->reader_loading_shown && !current->reader_has_content)
            text_update(current->reader_body, epd_get_str(reader_preparing));
    }
    else if (!reading->error[0] && !reading->ready)
    {
        if (current->reader_loading_shown && !current->reader_has_content)
            text_update(current->reader_body, epd_get_str(reader_paginating));
    }
    else if (!reading->error[0])
    {
        text_update(current->reader_body, reading->text[0] ? reading->text : epd_get_str(reader_empty));
    }
    if (current->reader_chapter) {
        char chapter[256] = {0};
        bool named = document_view_active() && epd_reader_chapter_title(chapter, sizeof(chapter), NULL);
        text_update(current->reader_chapter, chapter);
        lv_obj_set_width(current->reader_title, named ? 316 : WIDTH - 48);
    }
    if (reading->ready && !reading->error[0])
    {
        current->reader_has_content = true;
        current->reader_loading = current->reader_loading_shown = false;
        int width = lv_obj_get_width(lv_obj_get_parent(current->reader_progress)) * reading->percent / 100;
        if (width) lv_obj_remove_flag(current->reader_progress, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(current->reader_progress, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_get_width(current->reader_progress) != width) lv_obj_set_width(current->reader_progress, width);
    }
    reader_footer_render();
    current->reader_revision = reading->revision;
    if (submit_trace.active) submit_trace.render_ms += lv_tick_elaps(render_started);
    if (reading->ready && !reading->error[0] && !ui_reader_waiting())
    {
        if (submit_trace.active)
        {
            unsigned elapsed = lv_tick_elaps(submit_trace.started);
            if (first_content || elapsed >= 32)
                rt_kprintf("books submit page=%u observed_ms=%u poll_gap_ms=%u max_gap_ms=%u step_ms=%u commit_call_ms=%u render_ms=%u busy_polls=%u\n",
                    reading->page, elapsed, (unsigned)submit_trace.poll_gap_ms,
                    (unsigned)submit_trace.max_gap_ms, (unsigned)submit_trace.step_ms,
                    (unsigned)submit_trace.last_step_ms, (unsigned)submit_trace.render_ms,
                    submit_trace.busy_polls);
        }
        if (reading->page != refresh_page || submit_trace.active)
        {
            refresh_page = reading->page; refresh_started = lv_tick_get();
            epd_app_font_cache_stats(&refresh_glyphs);
            refresh_pending = true; refresh_busy = false;
        }
        submit_trace.active = false;
    }
    else if (reading->error[0]) submit_trace.active = false;
}

static void reader_update(void)
{
    const ui_reader_view_t *reading = ui_reader_view();
    if (reading->revision != current->reader_revision) reader_render();
    /* Keep existing content intact until its replacement is ready. Give an
     * empty first page time to prepare before submitting a loading-only frame. */
    else if (current->reader_loading && !current->reader_has_content && !current->reader_loading_shown &&
             lv_tick_elaps(current->reader_loading_tick) >= 1000)
    {
        current->reader_loading_shown = true;
        reader_render();
    }
    else if (reading->ready && !ui_reader_waiting() && !reading->indexing &&
             reading->pages != current->reader_rendered_pages)
    {
        /* A completed index updates only the footer; no body, clock or progress invalidation. */
        reader_footer_render();
    }
    if (!reading->error[0]) current->reader_error_reported = false;
    else if (!current->reader_error_reported && !current->popup)
    {
        reader_panel_close();
        popup_show(BOOKS_NOTICE_READER, reading->error_code ? reading->error_code : READER_ERROR_IO, RT_EOK);
        current->reader_error_reported = true;
    }
}

static void reader_touch_event(lv_event_t *event)
{
    if (!reader_view_touch_input(event)) return;
    if (!current || !current->foreground || current->id != UI_PAGE_READER ||
        lv_obj_get_screen(lv_event_get_target_obj(event)) != current->screen ||
        storage_changing() || pending_page != NAV_IDLE || current->reader_panel || current->popup ||
        ui_reader_waiting() || !ui_settings_enabled(UI_SETTING_TOUCH))
    {
        reader_view_touch_reset(&reader_gesture);
        return;
    }
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *input = lv_event_get_indev(event);
    lv_point_t point = {0};
    if (input) lv_indev_get_point(input, &point);
    reader_touch_phase_t phase = code == LV_EVENT_PRESSED ? READER_TOUCH_DOWN :
        code == LV_EVENT_RELEASED ? READER_TOUCH_UP : code == LV_EVENT_PRESSING ? READER_TOUCH_MOVE : READER_TOUCH_CANCEL;
    reader_touch_action_t action = books_gesture_update(&reader_gesture, phase, point.x, point.y);
    if (action == READER_TOUCH_SETTINGS) reader_panel_open();
    else if (books_reader_touch_enabled() && action == READER_TOUCH_PREVIOUS) pending_page = READER_PREVIOUS;
    else if (books_reader_touch_enabled() && action == READER_TOUCH_NEXT) pending_page = READER_NEXT;
}

static void reader_create(lv_obj_t *screen)
{
    const ui_reader_view_t *reading = ui_reader_view();
    const lv_font_t *title_font = ui_font_small();
    current->reader_title = label_create(screen, reading->title, PAGE_TITLE_X, (UI_READER_TOP - title_font->line_height) / 2,
                                         WIDTH - MARGIN - PAGE_TITLE_X, title_font);
    current->reader_chapter = label_create(screen, "", 364, (54 - title_font->line_height) / 2, 296, title_font);
    lv_label_set_long_mode(current->reader_chapter, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(current->reader_chapter, LV_TEXT_ALIGN_RIGHT, 0);
    current->reader_body = label_create(screen, "", reading->margin, UI_READER_TOP,
                                WIDTH - 2 * reading->margin, reading->font);
    lv_label_set_long_mode(current->reader_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(current->reader_body, UI_READER_BOTTOM - UI_READER_TOP);
    lv_obj_set_style_text_line_space(current->reader_body, reading->line_space, 0);
    lv_obj_set_style_text_letter_space(current->reader_body, 0, 0);
    lv_obj_set_style_text_color(current->reader_body, lv_color_black(), 0);
    lv_obj_add_flag(current->reader_body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(current->reader_body, reader_touch_event, LV_EVENT_ALL, NULL);
    lv_obj_t *track = panel_create(screen, MARGIN, UI_READER_BOTTOM + 6, CONTENT_WIDTH, 4);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(track, 0, 0);
    current->reader_progress = panel_create(track, 0, 0, 0, 4);
    lv_obj_remove_flag(current->reader_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(current->reader_progress, 0, 0);
    lv_obj_set_style_border_width(current->reader_progress, 0, 0);
    lv_obj_set_style_bg_color(current->reader_progress, lv_color_black(), 0);
    const lv_font_t *status_font = ui_font_body();
    const lv_font_t *page_font = ui_font_caption();
    int footer_y = HEIGHT - 2 - status_font->line_height;
    current->reader_battery = label_create(screen, "", 8, footer_y, 180, status_font);
    current->reader_footer = centered_label(screen, "", 200,
                                   footer_y + (status_font->line_height - page_font->line_height) / 2,
                                   WIDTH - 400, page_font);
    current->reader_clock = label_create(screen, "", WIDTH - 188, footer_y, 180, status_font);
    lv_obj_set_style_text_align(current->reader_clock, LV_TEXT_ALIGN_RIGHT, 0);
    reader_render();
}

static bool reader_option_render(void) { books_menu_process(); return true; }

static void reader_panel_close(void) { books_menu_close(); }

static void reader_panel_open(void) { books_menu_open(); }



static void reader_save_position(void)
{
    const ui_reader_view_t *reading = ui_reader_view();
    if (!reading->ready) return;
    ui_reader_save();
}

static void reader_resume(void)
{
    /* Lifecycle callbacks may run while the panel is still refreshing. */
    reader_resume_pending = true;
}

static void reader_resume_prepare(void)
{
    if (books_menu_active() || books_menu_font_pending() ||
        (current->popup && !lv_obj_has_flag(current->popup, LV_OBJ_FLAG_HIDDEN))) return;
    if (ui_reader_settings_changed())
    {
        unsigned page = ui_reader_view()->page;
        /* Reflow may release the old font. Detach it before touching the font cache. */
        lv_draw_wait_for_finish();
        lv_obj_set_style_text_font(current->reader_body, ui_font_body(), 0);
        bool prepared = ui_reader_reflow();
        const ui_reader_view_t *reading = ui_reader_view();
        lv_obj_set_style_text_font(current->reader_body,
                                  reading->font ? reading->font : ui_font_body(), 0);
        if (!prepared) return;
        current->reader_reflow_page = page;
        current->reader_reflow_pending = true;
        current->reader_error_reported = false;
        if (!document_view_active())
        {
            lv_obj_set_x(current->reader_body, reading->margin);
            lv_obj_set_width(current->reader_body, reading->content_width);
            lv_obj_set_y(current->reader_body, reading->top);
            lv_obj_set_height(current->reader_body, reading->bottom - reading->top);
            lv_obj_set_style_text_line_space(current->reader_body, reading->line_space, 0);
        }
    }
}

static bool button_text(lv_obj_t *button, const char *text)
{
    return !button || text_update(lv_obj_get_child(button, 0), text);
}

static void bookshelf_layout(void)
{
    lv_area_t area;
    epd_ui_content_area(current->screen, &area);
    int width = area.x2 - area.x1 + 1, gap = epd_ui_px(16);
    int button_height = epd_ui_touch(70), bottom = area.y2 + 1 - button_height;
    int row_height = (bottom - gap - area.y1) / BOOKSHELF_PAGE_SIZE - gap;
    int inset = epd_ui_px(18), cover_width = epd_ui_px(112);
    int cover_height = LV_MIN(epd_ui_px(146), row_height - 2 * inset);
    int text_x = inset + cover_width + gap, text_width = width - text_x - inset;
    reader_ui_label(current->bookshelf_title, area.x1 + epd_ui_touch(56) + gap,
                    epd_ui_px(PAGE_TITLE_Y), width - epd_ui_touch(56) - gap, EPD_FONT_TITLE);
    for (unsigned i = 0; i < BOOKSHELF_PAGE_SIZE; ++i)
    {
        reader_ui_rect(current->books[i].card, area.x1, area.y1 + i * (row_height + gap), width, row_height);
        reader_ui_rect(current->books[i].cover, inset, inset, cover_width, cover_height);
        lv_obj_t *icon = lv_obj_get_child(current->books[i].cover, 0);
        if (icon) lv_obj_center(icon);
        reader_ui_label(current->books[i].title, text_x, inset, text_width, EPD_FONT_BODY);
        reader_ui_label(current->books[i].metadata, text_x, inset + ui_font_body()->line_height + epd_ui_px(8), text_width, EPD_FONT_SMALL);
        int progress_y = row_height - inset - ui_font_caption()->line_height;
        reader_ui_label(current->books[i].progress, text_x, progress_y, text_width, EPD_FONT_CAPTION);
        lv_obj_t *track = lv_obj_get_parent(current->books[i].fill);
        reader_ui_rect(track, text_x, progress_y - epd_ui_px(18), text_width, epd_ui_px(8));
        lv_obj_set_height(current->books[i].fill, epd_ui_px(8));
        lv_label_set_long_mode(current->books[i].title, LV_LABEL_LONG_DOT);
        lv_label_set_long_mode(current->books[i].metadata, LV_LABEL_LONG_DOT);
        lv_label_set_long_mode(current->books[i].progress, LV_LABEL_LONG_DOT);
    }
    int half = (width - gap) / 2;
    reader_ui_button(current->bookshelf_previous, area.x1, bottom, half, button_height);
    reader_ui_button(current->bookshelf_next, area.x1 + half + gap, bottom, width - half - gap, button_height);
    reader_ui_label(current->bookshelf_empty, area.x1, (area.y1 + bottom - ui_font_body()->line_height) / 2, width, EPD_FONT_BODY);
    bookshelf_update(false);
}

static void reader_controls_layout(void) { books_menu_process(); }

void books_scale_refresh(void)
{
    if (!current) return;
    lv_area_t area_now;
    epd_ui_content_area(current->screen, &area_now);
    if (current->scale_revision == epd_ui_scale_revision() &&
        !memcmp(&area_now, &current->content_area, sizeof(area_now))) return;
    lv_obj_set_style_text_font(current->screen, ui_font_body(), 0);
    int margin = epd_ui_px(32);
    if (current->id == UI_PAGE_BOOKSHELF)
    {
        bookshelf_layout();
        epd_app_back_button_layout(current->back, epd_ui_px(24),
            epd_ui_px(PAGE_TITLE_Y) + ui_font_title()->line_height / 2);
    }
    else
    {
        reader_geometry_t area = reader_geometry();
        reader_resume_prepare();
        const ui_reader_view_t *reading = ui_reader_view();
        epd_app_back_button_layout(current->back, epd_ui_px(24), area.top / 2);
        reader_view_touch_controls(current->back);
        int title_x = 24;
        reader_ui_label(current->reader_title, title_x, (area.top - ui_font_small()->line_height) / 2,
                        WIDTH - margin - title_x, EPD_FONT_SMALL);
        lv_label_set_long_mode(current->reader_title, LV_LABEL_LONG_DOT);
        reader_ui_label(current->reader_chapter, 364, (area.top - ui_font_small()->line_height) / 2, 296, EPD_FONT_SMALL);
        reader_ui_rect(current->reader_body, reading->margin, area.top,
                        WIDTH - 2 * reading->margin, area.bottom - area.top);
        lv_obj_t *track = lv_obj_get_parent(current->reader_progress);
        reader_ui_rect(track, margin, area.bottom + epd_ui_px(6), WIDTH - 2 * margin, epd_ui_px(4));
        lv_obj_set_height(current->reader_progress, epd_ui_px(4));
        int edge = epd_ui_px(8), side = epd_ui_px(180), footer_y = HEIGHT - 2 - ui_font_body()->line_height;
        reader_ui_label(current->reader_battery, edge, footer_y, side, EPD_FONT_BODY);
        reader_ui_label(current->reader_clock, WIDTH - edge - side, footer_y, side, EPD_FONT_BODY);
        reader_ui_label(current->reader_footer, 2 * edge + side,
                        footer_y + (ui_font_body()->line_height - ui_font_caption()->line_height) / 2,
                        WIDTH - 4 * edge - 2 * side, EPD_FONT_CAPTION);
        lv_label_set_long_mode(current->reader_footer, LV_LABEL_LONG_DOT);
        reader_controls_layout();
        reader_render();
    }
    popup_layout();
    current->scale_revision = epd_ui_scale_revision();
    current->content_area = area_now;
}

static void books_locale_refresh(void)
{
    bool updated = true;
    if (current->id == UI_PAGE_BOOKSHELF)
    {
        updated &= text_update(current->bookshelf_title, epd_get_str(books_title));
        updated &= text_update(current->bookshelf_empty, epd_get_str(books_empty));
        updated &= button_text(current->bookshelf_previous, epd_get_str(books_previous));
        updated &= button_text(current->bookshelf_next, epd_get_str(books_next));
        updated &= bookshelf_update(false);
    }
    else
    {
        const ui_reader_view_t *reading = ui_reader_view();
        updated &= reader_status_render();
        updated &= reader_footer_render();
        if (!reading->ready && !reading->error[0] && current->reader_loading_shown && !current->reader_has_content)
            updated &= text_update(current->reader_body, document_view_active() ?
                        epd_get_str(reader_preparing) : epd_get_str(reader_paginating));
        else if (!document_view_active() && !reading->error[0] && reading->text && !reading->text[0])
            updated &= text_update(current->reader_body, epd_get_str(reader_empty));
        if (current->reader_panel_title)
            updated &= text_update(current->reader_panel_title, current->reader_toc_panel ?
                        epd_get_str(reader_contents) : epd_get_str(reader_settings));
        updated &= button_text(current->reader_confirm, epd_get_str(reader_confirm));
        updated &= button_text(current->reader_contents, epd_get_str(reader_contents));
        updated &= button_text(current->reader_text_settings, epd_get_str(reader_text_settings));
        updated &= button_text(current->reader_toc_previous, epd_get_str(reader_previous_group));
        updated &= button_text(current->reader_toc_close, epd_get_str(reader_close));
        updated &= button_text(current->reader_toc_next, epd_get_str(reader_next_group));
        if (current->reader_panel) updated &= reader_option_render();
    }
    updated &= popup_refresh();
    if (updated) current->locale_revision = epd_locale_revision();
}
