#include "epd_memory.h"
#include "platform/epd_keyboard.h"
#include "platform/epd_locale.h"
#include "words_i18n.h"
#include "words_layout.h"
#include <string.h>
#include <stdio.h>

typedef struct
{
    lv_obj_t *screen, *back, *title, *input, *keyboard, *status, *search, *rows[WORDS_MATCH_MAX];
    lv_timer_t *timer;
    words_result_t *displayed;
    words_job_t selected;
    uint32_t request, request_tick, locale_revision, scale_revision;
    words_message_t status_code;
} lookup_t;

typedef struct
{
    lv_obj_t *screen, *back, *title, *word, *phonetic, *body, *status, *previous, *next, *collect, *pause;
    lv_timer_t *timer;
    words_job_t job;
    words_result_t *displayed;
    char *text;
    uint32_t request, request_tick, locale_revision, scale_revision;
    words_message_t status_code;
    size_t offset, end, *history;
    unsigned page, history_capacity;
    bool text_pending, reflow_pending;
} detail_t;

static void detail_message(gui_app_msg_type_t message, void *parameter);

static void visible(lv_obj_t *object, bool show)
{
    if (show) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *words_prepare_back(const char *title, lv_event_cb_t back, void *data, lv_obj_t **heading, lv_obj_t **button)
{
    gui_app_close_anim();
    lv_obj_t *screen = lv_screen_active();
    epd_app_object_init(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, epd_app_font(), 0);
    epd_app_input_group(screen);
    epd_app_header(screen);
    if (back) *button = epd_app_back_button_cb(screen, back, data);
    else *button = epd_app_back_button(screen);
    *heading = epd_app_label(screen, title, 104, 81, 540, epd_app_font_role(EPD_FONT_TITLE));
    return screen;
}



static void lookup_status(lookup_t *page, words_message_t code)
{
    page->status_code = code;
    epd_app_text(page->status, words_message_text(code));
}

static void detail_status(detail_t *page, words_message_t code)
{
    page->status_code = code;
    epd_app_text(page->status, words_message_text(code));
}

static void lookup_layout(lookup_t *page)
{
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    int x = area.x1, width = area.x2 - area.x1 + 1, gap = epd_ui_px(12);
    int height = epd_ui_touch(88), search_width = epd_ui_px(150);
    int y = area.y1, small = words_line(EPD_FONT_SMALL);
    words_title_layout(page->screen, page->title, page->back);
    words_box(page->input, x, y, width - search_width - gap, height);
    lv_obj_set_style_text_font(page->input, epd_app_font_role(EPD_FONT_BODY), 0);
    lv_obj_set_style_pad_left(page->input, gap, 0);
    lv_obj_set_style_pad_top(page->input, epd_ui_px(20), 0);
    words_button_box(page->search, area.x2 - search_width + 1, y, search_width, height);
    y += height + gap;
    words_label_box(page->status, x, y, width, small * 2, EPD_FONT_SMALL);
    lv_label_set_long_mode(page->status, LV_LABEL_LONG_DOT);
    y += small * 2 + gap;
    int row_height = LV_MIN(epd_ui_touch(84), (area.y2 + 1 - y) / WORDS_MATCH_MAX - gap);
    for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i)
        words_button_box(page->rows[i], x, y + i * (row_height + gap), width, row_height);
    epd_keyboard_refresh(page->keyboard);
    int keyboard_height = LV_MIN(epd_keyboard_preferred_height(page->keyboard), LV_MAX(1, area.y2 + 1 - y));
    words_box(page->keyboard, x, area.y2 + 1 - keyboard_height, width, keyboard_height);
    page->scale_revision = epd_ui_scale_revision();
}

static void lookup_localize(lookup_t *page)
{
    if (page->scale_revision != epd_ui_scale_revision()) lookup_layout(page);
    uint32_t revision = epd_locale_revision();
    if (page->locale_revision == revision) return;
    bool updated = epd_app_text(page->title, epd_get_str(words_lookup));
    updated &= epd_app_text(lv_obj_get_child(page->search, 0), epd_get_str(words_search));
    updated &= epd_app_text(page->status, words_message_text(page->status_code));
    if (updated) page->locale_revision = revision;
}

static void select_word(lv_event_t *event)
{
    lookup_t *page = lv_event_get_user_data(event);
    if (!page->displayed || page->request) return;
    for (unsigned i = 0; i < page->displayed->matches.count; ++i)
    {
        if (page->rows[i] != lv_event_get_target_obj(event)) continue;
        memset(&page->selected, 0, sizeof(page->selected));
        page->selected.kind = WORDS_DETAIL;
        page->selected.entry = page->displayed->matches.entry[i];
        strcpy(page->selected.path, page->displayed->path);
        memcpy(page->selected.identity, page->displayed->identity, sizeof(page->selected.identity));
        gui_app_create_page_for_app_ext("words", "detail", detail_message, &page->selected, sizeof(detail_t));
        break;
    }
}

static void lookup_poll(lv_timer_t *timer)
{
    lookup_t *page = lv_timer_get_user_data(timer);
    lookup_localize(page);
    if (!page->request) return;
    bool failed;
    words_result_t *result = words_take(page->request, &failed);
    if (!result && !failed)
    {
        if (lv_tick_elaps(page->request_tick) >= 200 && page->status_code != WORDS_MESSAGE_SEARCHING)
            lookup_status(page, WORDS_MESSAGE_SEARCHING);
        return;
    }
    page->request = 0;
    lv_obj_remove_state(page->search, LV_STATE_DISABLED);
    words_result_free(page->displayed);
    page->displayed = result;
    for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i)
        visible(page->rows[i], result && !result->error && i < result->matches.count);
    if (!result || result->error)
    {
        lookup_status(page, result ? result->error : WORDS_MESSAGE_MEMORY);
        return;
    }
    for (unsigned i = 0; i < result->matches.count; ++i)
    {
        epd_app_text(lv_obj_get_child(page->rows[i], 0), result->names[i]);
        visible(page->rows[i], true);
    }
    lookup_status(page, !result->matches.count ? WORDS_MESSAGE_NOT_FOUND :
                  (result->matches.more ? WORDS_MESSAGE_MORE_LETTERS : WORDS_MESSAGE_NONE));
}

static void search(lv_event_t *event)
{
    lookup_t *page = lv_event_get_user_data(event);
    if (page->request) return;
    const char *text = lv_textarea_get_text(page->input);
    if (!text[0]) return;
    words_job_t job = {.kind = WORDS_SEARCH};
    snprintf(job.query, sizeof(job.query), "%s", text);
    size_t length = strlen(job.query);
    while (length && job.query[length - 1] == ' ') job.query[--length] = 0;
    size_t first = 0;
    while (job.query[first] == ' ') ++first;
    if (first) memmove(job.query, job.query + first, length - first + 1);
    if (!job.query[0]) return;
    visible(page->keyboard, false);
    lv_obj_add_state(page->search, LV_STATE_DISABLED);
    if (page->status_code == WORDS_MESSAGE_SEARCHING) lookup_status(page, WORDS_MESSAGE_NONE);
    page->request_tick = lv_tick_get();
    page->request = words_submit(&job);
    if (page->request) lv_timer_resume(page->timer);
    else { lookup_status(page, WORDS_MESSAGE_BUSY); lv_obj_remove_state(page->search, LV_STATE_DISABLED); }
}

static void edit(lv_event_t *event)
{
    lookup_t *page = lv_event_get_user_data(event);
    if (!page->request) visible(page->keyboard, true);
}

static void keyboard_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) search(event);
    else if (lv_event_get_code(event) == LV_EVENT_CANCEL)
    {
        lookup_t *page = lv_event_get_user_data(event);
        visible(page->keyboard, false);
    }
}

void words_lookup_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    lookup_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        page->screen = words_prepare_back(epd_get_str(words_lookup), NULL, NULL, &page->title, &page->back);
        page->input = lv_textarea_create(page->screen);
        epd_app_object_init(page->input);
        lv_obj_set_style_bg_color(page->input, lv_color_white(), 0);
        lv_obj_set_style_border_width(page->input, 1, 0);
        lv_obj_set_style_border_color(page->input, lv_color_black(), 0);
        lv_obj_set_style_pad_left(page->input, 12, 0);
        lv_obj_set_style_pad_top(page->input, 20, 0);
        lv_obj_set_pos(page->input, 32, 188);
        lv_obj_set_size(page->input, 470, 88);
        lv_textarea_set_one_line(page->input, true);
        lv_textarea_set_max_length(page->input, WORDS_KEY_SIZE - 1);
        lv_textarea_set_accepted_chars(page->input,
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 '-.&()/+");
        lv_obj_set_style_anim_duration(page->input, 0, 0);
        lv_obj_set_style_anim_duration(page->input, 0, LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(page->input, LV_OPA_TRANSP, LV_PART_CURSOR);
        lv_obj_set_style_border_width(page->input, 0, LV_PART_CURSOR);
        lv_obj_set_style_shadow_width(page->input, 0, LV_PART_CURSOR);
        lv_obj_add_event_cb(page->input, edit, LV_EVENT_CLICKED, page);
        page->search = epd_app_button(page->screen, 518, 188, 134, 88, epd_get_str(words_search), search, page);
        page->status = epd_app_label(page->screen, "", 32, 292, 620, epd_app_font_role(EPD_FONT_SMALL));
        for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i)
        {
            page->rows[i] = epd_app_button(page->screen, 32, 354 + i * 99, 620, 84, "", select_word, page);
            visible(page->rows[i], false);
        }
        const epd_keyboard_config_t keyboard_config = {
            .mode = EPD_KEYBOARD_ASCII, .chinese = false, .voice = false
        };
        page->keyboard = epd_keyboard_create(page->screen, page->input, &keyboard_config);
        lv_obj_add_event_cb(page->keyboard, keyboard_event, LV_EVENT_READY, page);
        lv_obj_add_event_cb(page->keyboard, keyboard_event, LV_EVENT_CANCEL, page);
        lookup_layout(page);
        page->locale_revision = epd_locale_revision();
        page->timer = lv_timer_create(lookup_poll, 100, page);
        lv_timer_pause(page->timer);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
        lookup_localize(page);
        lv_timer_resume(page->timer);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer);
        words_cancel(page->request);
        if (page->request)
        {
            words_result_free(page->displayed);
            page->displayed = NULL;
            for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i) visible(page->rows[i], false);
            lookup_status(page, WORDS_MESSAGE_NONE);
        }
        page->request = 0;
        lv_obj_remove_state(page->search, LV_STATE_DISABLED);
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer);
        words_result_free(page->displayed);
    }
}

static void detail_layout(detail_t *page)
{
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    int x = area.x1, width = area.x2 - area.x1 + 1, gap = epd_ui_px(12);
    int half = (width - gap) / 2, height = epd_ui_touch(76), y = area.y1;
    int line = words_line(EPD_FONT_BODY), small = words_line(EPD_FONT_SMALL);
    int title = words_line(EPD_FONT_TITLE), nav_y = area.y2 + 1 - height;
    words_title_layout(page->screen, page->title, page->back);
    words_label_box(page->word, x, y, width, title * 2, EPD_FONT_TITLE);
    y += title * 2 + gap;
    words_label_box(page->phonetic, x, y, width, line * 2, EPD_FONT_BODY);
    y += line * 2 + gap;
    words_button_box(page->collect, x, y, half, height);
    words_button_box(page->pause, x + half + gap, y, width - half - gap, height);
    y += height + gap;
    words_label_box(page->status, x, y, width, small * 2, EPD_FONT_SMALL);
    lv_label_set_long_mode(page->status, LV_LABEL_LONG_DOT);
    y += small * 2 + gap;
    words_label_box(page->body, x, y, width, nav_y - gap - y, EPD_FONT_BODY);
    lv_obj_set_style_text_line_space(page->body, epd_ui_px(8), 0);
    words_button_box(page->previous, x, nav_y, half, height);
    words_button_box(page->next, x + half + gap, nav_y, width - half - gap, height);
    page->scale_revision = epd_ui_scale_revision();
    page->reflow_pending = page->text != NULL;
}

static bool detail_render(detail_t *page)
{
    lv_obj_update_layout(page->body);
    size_t total = strlen(page->text), low = 1, high = total - page->offset;
    const lv_font_t *font = epd_app_font_role(EPD_FONT_BODY);
    size_t best = 0;
    while (low <= high)
    {
        size_t length = low + (high - low) / 2;
        size_t boundary = page->offset + length;
        while (boundary < total && ((unsigned char)page->text[boundary] & 0xc0) == 0x80) --boundary;
        char saved = page->text[boundary];
        page->text[boundary] = 0;
        lv_point_t size;
        lv_text_get_size(&size, page->text + page->offset, font, 0, epd_ui_px(8),
                         lv_obj_get_width(page->body), LV_TEXT_FLAG_NONE);
        page->text[boundary] = saved;
        if (size.y <= lv_obj_get_height(page->body)) { best = boundary - page->offset; low = length + 1; }
        else high = length - 1;
    }
    if (!best && page->offset < total)
    {
        best = 1;
        while (page->offset + best < total &&
               ((unsigned char)page->text[page->offset + best] & 0xc0) == 0x80) ++best;
    }
    page->end = page->offset + best;
    char saved = page->text[page->end];
    page->text[page->end] = 0;
    bool updated = epd_app_text(page->body, page->text + page->offset);
    page->text[page->end] = saved;
    visible(page->previous, page->page > 0);
    visible(page->next, page->end < total);
    page->reflow_pending = !updated;
    return updated;
}

static void detail_turn(lv_event_t *event)
{
    detail_t *page = lv_event_get_user_data(event);
    if (!page->text) return;
    if (lv_event_get_target_obj(event) == page->previous)
    {
        if (!page->page) return;
        page->offset = page->history[--page->page];
    }
    else
    {
        if (page->end >= strlen(page->text)) return;
        if (page->page == page->history_capacity)
        {
            if (page->history_capacity > UINT32_MAX / 2 || page->history_capacity > SIZE_MAX / sizeof(size_t) / 2)
            { detail_status(page, WORDS_MESSAGE_PAGE_LIMIT); return; }
            unsigned capacity = page->history_capacity ? page->history_capacity * 2 : 16;
            size_t *history = epd_app_alloc(capacity * sizeof(*history), EPD_APP_PSRAM);
            if (!history) { detail_status(page, WORDS_MESSAGE_MEMORY); return; }
            if (page->history) memcpy(history, page->history, page->page * sizeof(*history));
            epd_app_free(page->history);
            page->history = history;
            page->history_capacity = capacity;
        }
        page->history[page->page++] = page->offset;
        page->offset = page->end;
    }
    detail_render(page);
}

static bool detail_text(detail_t *page)
{
    if (!page->displayed) return true;
    page->text_pending = true;
    words_result_t *result = page->displayed;
    size_t length = words_ui_entry_text(&result->view, WORDS_TEXT_DETAIL, NULL, 0);
    if (!length) { detail_status(page, WORDS_MESSAGE_ENTRY_INVALID); return false; }
    char *text = epd_app_alloc(length, EPD_APP_PSRAM);
    if (!text) { detail_status(page, WORDS_MESSAGE_MEMORY); return false; }
    words_ui_entry_text(&result->view, WORDS_TEXT_DETAIL, text, length);
    if (!words_ui_phonetic(page->phonetic, &result->view))
    { epd_app_free(text); detail_status(page, WORDS_MESSAGE_MEMORY); return false; }
    unsigned target = page->page;
    epd_app_free(page->text);
    page->text = text;
    page->offset = page->end = 0;
    page->page = 0;
    bool updated = detail_render(page);
    while (page->page < target && page->end < strlen(page->text))
    {
        page->history[page->page++] = page->offset;
        page->offset = page->end;
        updated &= detail_render(page);
    }
    page->text_pending = !updated;
    return updated;
}

static bool detail_buttons(detail_t *page)
{
    unsigned flags = page->displayed ? page->displayed->flags : 0;
    bool updated = epd_app_text(lv_obj_get_child(page->collect, 0),
                 flags & WORDS_COLLECTED ? epd_get_str(words_uncollect) : epd_get_str(words_collect));
    updated &= epd_app_text(lv_obj_get_child(page->pause, 0),
                 flags & WORDS_PAUSED ? epd_get_str(words_unpause) : epd_get_str(words_pause));
    return updated;
}

static void detail_localize(detail_t *page)
{
    if (page->scale_revision != epd_ui_scale_revision()) detail_layout(page);
    if (page->reflow_pending && page->text) detail_render(page);
    uint32_t revision = epd_locale_revision();
    if (page->locale_revision == revision && !page->text_pending) return;
    bool updated = epd_app_text(page->title, epd_get_str(words_title));
    updated &= epd_app_text(lv_obj_get_child(page->previous, 0), epd_get_str(words_previous));
    updated &= epd_app_text(lv_obj_get_child(page->next, 0), epd_get_str(words_next));
    updated &= detail_buttons(page);
    updated &= epd_app_text(page->status, words_message_text(page->status_code));
    updated &= detail_text(page);
    if (updated) page->locale_revision = revision;
}

static void detail_poll(lv_timer_t *timer)
{
    detail_t *page = lv_timer_get_user_data(timer);
    detail_localize(page);
    if (!page->request) return;
    bool failed;
    words_result_t *result = words_take(page->request, &failed);
    if (!result && !failed)
    {
        if (!page->displayed && lv_tick_elaps(page->request_tick) >= 200 && page->status_code != WORDS_MESSAGE_LOADING)
            detail_status(page, WORDS_MESSAGE_LOADING);
        return;
    }
    page->request = 0;
    if (!result || result->error)
    {
        detail_status(page, result ? result->error : WORDS_MESSAGE_MEMORY);
        words_result_free(result);
        return;
    }
    bool same_content = page->displayed && page->displayed->content_size == result->content_size &&
        !memcmp(page->displayed->content, result->content, result->content_size);
    words_result_free(page->displayed);
    page->displayed = result;
    page->job.slot = result->slot;
    epd_app_text(lv_obj_get_child(page->collect, 0), (result->flags & WORDS_COLLECTED) ? epd_get_str(words_uncollect) : epd_get_str(words_collect));
    epd_app_text(lv_obj_get_child(page->pause, 0), (result->flags & WORDS_PAUSED) ? epd_get_str(words_unpause) : epd_get_str(words_pause));
    visible(page->collect, true); visible(page->pause, true);
    detail_status(page, WORDS_MESSAGE_NONE);
    if (page->text && same_content) return;
    epd_app_free(page->text); page->text = NULL;
    epd_app_free(page->history); page->history = NULL;
    page->offset = page->end = 0; page->page = page->history_capacity = 0;
    epd_app_text(page->word, result->fields[WORDS_WORD]);
    detail_text(page);
}

static void detail_action(lv_event_t *event)
{
    detail_t *page = lv_event_get_user_data(event);
    if (!page->displayed || page->request) return;
    words_job_t job = page->job;
    bool collect = lv_event_get_target_obj(event) == page->collect;
    job.kind = collect ? WORDS_COLLECT : WORDS_PAUSE;
    job.value = !(page->displayed->flags & (collect ? WORDS_COLLECTED : WORDS_PAUSED));
    page->request = words_submit(&job);
    if (page->request) lv_timer_resume(page->timer);
    else detail_status(page, WORDS_MESSAGE_BUSY);
}

static void detail_message(gui_app_msg_type_t message, void *parameter)
{
    (void)parameter;
    detail_t *page = gui_app_this_page_memory();
    if (message == GUI_APP_MSG_ONSTART)
    {
        page->job = *(const words_job_t *)gui_app_this_page_userdata();
        page->screen = words_prepare_back(epd_get_str(words_title), NULL, NULL, &page->title, &page->back);
        page->word = epd_app_label(page->screen, "", 32, 204, 620, epd_app_font_role(EPD_FONT_TITLE));
        lv_obj_set_height(page->word, 64);
        lv_label_set_long_mode(page->word, LV_LABEL_LONG_DOT);
        page->phonetic = epd_app_label(page->screen, "", 32, 282, 620, epd_app_font());
        lv_obj_set_height(page->phonetic, 60);
        lv_label_set_long_mode(page->phonetic, LV_LABEL_LONG_DOT);
        page->collect = epd_app_button(page->screen, 32, 354, 294, 76, epd_get_str(words_collect), detail_action, page);
        page->pause = epd_app_button(page->screen, 358, 354, 294, 76, epd_get_str(words_pause), detail_action, page);
        visible(page->collect, false); visible(page->pause, false);
        page->status_code = WORDS_MESSAGE_NONE;
        page->status = epd_app_label(page->screen, words_message_text(page->status_code), 32, 444, 620, epd_app_font_role(EPD_FONT_SMALL));
        page->body = epd_app_label(page->screen, "", 32, 496, 620, epd_app_font());
        lv_obj_set_height(page->body, 568);
        lv_label_set_long_mode(page->body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_letter_space(page->body, 0, 0);
        lv_obj_set_style_text_line_space(page->body, 8, 0);
        page->previous = epd_app_button(page->screen, 32, 1100, 294, 76, epd_get_str(words_previous), detail_turn, page);
        page->next = epd_app_button(page->screen, 358, 1100, 294, 76, epd_get_str(words_next), detail_turn, page);
        visible(page->previous, false);
        visible(page->next, false);
        detail_layout(page);
        page->locale_revision = epd_locale_revision();
        page->timer = lv_timer_create(detail_poll, 100, page);
        lv_timer_pause(page->timer);
    }
    else if (message == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
        detail_localize(page);
        lv_timer_resume(page->timer);
        page->job.kind = WORDS_DETAIL;
        page->request_tick = lv_tick_get();
        if (page->status_code == WORDS_MESSAGE_LOADING) detail_status(page, WORDS_MESSAGE_NONE);
        page->request = words_submit(&page->job);
        if (page->request) lv_timer_resume(page->timer);
        else detail_status(page, WORDS_MESSAGE_BUSY);
    }
    else if (message == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer);
        words_cancel(page->request);
        page->request = 0;
    }
    else if (message == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer);
        epd_app_free(page->text);
        epd_app_free(page->history);
        words_result_free(page->displayed);
    }
}

void words_open_detail(const words_job_t *job)
{
    gui_app_create_page_for_app_ext("words", "detail", detail_message, (void *)job, sizeof(detail_t));
}

void words_open_lookup(void)
{
    gui_app_create_page_for_app_ext("words", "lookup", words_lookup_message, NULL, sizeof(lookup_t));
}
