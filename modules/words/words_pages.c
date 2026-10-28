#include "platform/epd_locale.h"
#include "words_i18n.h"
#include "words_layout.h"
#include <stdio.h>
#include <string.h>

EPD_APP_DEFINE("words");
MODULE_INIT_DEF(NULL)
MODULE_CLEANUP_DEF(NULL)
lv_obj_t *words_prepare_back(const char *title, lv_event_cb_t back, void *data, lv_obj_t **heading, lv_obj_t **button);
void words_open_detail(const words_job_t *job);
void words_open_lookup(void);

typedef enum { PAGE_HOME, PAGE_STUDY, PAGE_COLLECTION, PAGE_SCOPES, PAGE_SETTINGS } page_kind_t;
typedef struct
{
    page_kind_t kind;
    lv_obj_t *screen, *back, *status, *title, *scope_label, *heading, *phonetic, *body, *info;
    lv_obj_t *buttons[WORDS_MATCH_MAX], *previous, *next;
    lv_timer_t *timer;
    words_result_t *result;
    words_job_t child;
    uint32_t request, request_tick, offset, quota, locale_revision, scale_revision;
    words_message_t status_code, pending_status;
    unsigned start_attempts;
    bool root, started, answer, text_pending;
} page_t;

static void message(gui_app_msg_type_t message, void *parameter);
static void click(lv_event_t *event);
static void show(lv_obj_t *object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
static bool button_text(lv_obj_t *button, const char *text) { return epd_app_text(lv_obj_get_child(button, 0), text); }

static void layout(page_t *page)
{
    lv_area_t area;
    epd_ui_content_area(page->screen, &area);
    int x = area.x1, width = area.x2 - area.x1 + 1;
    int gap = epd_ui_px(12), half = (width - gap) / 2;
    int small = words_line(EPD_FONT_SMALL), line = words_line(EPD_FONT_BODY);
    int y = area.y1, bottom = area.y2 + 1;
    words_title_layout(page->screen, page->title, page->back);
    words_label_box(page->status, x, y, width, small * 2, EPD_FONT_SMALL);
    lv_label_set_long_mode(page->status, LV_LABEL_LONG_DOT);
    y += small * 2 + gap;
    if (page->kind == PAGE_HOME)
    {
        int reserved = words_line(EPD_FONT_TITLE) * 2 + line * 5 + gap * 2;
        int height = LV_MIN(epd_ui_touch(88), (bottom - y - reserved - gap * 4) / 5);
        height = LV_MAX(56, height);
        int buttons_y = bottom - 5 * (height + gap) + gap;
        words_label_box(page->heading, x, y, width, words_line(EPD_FONT_TITLE) * 2, EPD_FONT_TITLE);
        lv_label_set_long_mode(page->heading, LV_LABEL_LONG_DOT);
        y += words_line(EPD_FONT_TITLE) * 2 + gap;
        words_label_box(page->body, x, y, width, buttons_y - gap - y, EPD_FONT_BODY);
        for (unsigned i = 0; i < 5; ++i)
            words_button_box(page->buttons[i], x, buttons_y + i * (height + gap), width, height);
    }
    else if (page->kind == PAGE_SETTINGS)
    {
        int height = epd_ui_touch(88);
        words_label_box(page->heading, x, y, width, words_line(EPD_FONT_TITLE) * 2, EPD_FONT_TITLE);
        y += words_line(EPD_FONT_TITLE) * 2 + gap;
        words_button_box(page->buttons[0], x, y, half, height);
        words_button_box(page->buttons[1], x + half + gap, y, width - half - gap, height);
        y += height + gap * 2;
        words_label_box(page->scope_label, x, y, width, line, EPD_FONT_BODY);
        y += line + gap;
        words_label_box(page->body, x, y, width, line * 2, EPD_FONT_BODY);
        lv_label_set_long_mode(page->body, LV_LABEL_LONG_DOT);
        y += line * 2 + gap;
        words_button_box(page->buttons[2], x, y, width, height);
    }
    else if (page->kind == PAGE_STUDY)
    {
        int height = epd_ui_touch(76);
        int ratings_y = bottom - small * 2 - gap - height;
        int actions_y = ratings_y - gap - height;
        words_label_box(page->heading, x, y, width, epd_ui_px(104), EPD_FONT_TEMPERATURE);
        y += epd_ui_px(104) + gap;
        words_label_box(page->phonetic, x, y, width, line * 2, EPD_FONT_BODY);
        lv_label_set_long_mode(page->phonetic, LV_LABEL_LONG_DOT);
        y += line * 2 + gap;
        words_label_box(page->body, x, y, width, actions_y - gap - y, EPD_FONT_BODY);
        words_label_box(page->info, x, bottom - small * 2, width, small * 2, EPD_FONT_SMALL);
        int column = (width - gap * 3) / 4;
        for (unsigned i = 0; i < 4; ++i)
            words_button_box(page->buttons[i], x + i * (column + gap), ratings_y, column, height);
        words_button_box(page->buttons[4], x, ratings_y, width, height);
        words_button_box(page->buttons[5], x, actions_y, half, height);
        words_button_box(page->buttons[6], x + half + gap, actions_y, width - half - gap, height);
        words_button_box(page->buttons[7], x, ratings_y, width, height);
    }
    else
    {
        int height = epd_ui_touch(76), nav_y = bottom - height;
        int row_height = LV_MIN(epd_ui_touch(88), (nav_y - gap - y) / WORDS_MATCH_MAX - gap);
        for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i)
            words_button_box(page->buttons[i], x, y + i * (row_height + gap), width, row_height);
        words_button_box(page->previous, x, nav_y, half, height);
        words_button_box(page->next, x + half + gap, nav_y, width - half - gap, height);
    }
    lv_obj_update_layout(page->screen);
    page->scale_revision = epd_ui_scale_revision();
}

static bool status(page_t *page, words_message_t code)
{
    page->status_code = code;
    return epd_app_text(page->status, words_message_text(code));
}

static void request(page_t *page, const words_job_t *job)
{
    page->request_tick = lv_tick_get();
    page->pending_status = job->kind == WORDS_SCOPE_SELECT ? WORDS_MESSAGE_LOADING_SCOPE : WORDS_MESSAGE_NONE;
    if (page->status_code == WORDS_MESSAGE_LOADING_SCOPE)
        status(page, WORDS_MESSAGE_NONE);
    page->request = words_submit(job);
    if (page->request) lv_timer_resume(page->timer);
    else status(page, WORDS_MESSAGE_BUSY);
}
static void refresh(page_t *page)
{
    words_job_t job = {.kind = WORDS_HOME, .value = page->offset};
    if (page->kind == PAGE_STUDY) job.kind = WORDS_NEXT;
    else if (page->kind == PAGE_COLLECTION) job.kind = WORDS_COLLECTION;
    else if (page->kind == PAGE_SCOPES) job.kind = WORDS_SCOPES;
    request(page, &job);
}
static void open_page(page_kind_t kind)
{
    static const char *ids[] = {"home", "study", "collection", "scopes", "settings"};
    gui_app_create_page_for_app_ext("words", ids[kind], message, (void *)(uintptr_t)kind, sizeof(page_t));
}

static bool render_study(page_t *page)
{
    bool updated = true;
    words_result_t *r = page->result;
    bool card = r && r->slot && r->fields[WORDS_WORD];
    const char *word = card ? r->fields[WORDS_WORD] : epd_get_str(words_no_cards);
    const lv_font_t *font = epd_app_font_role(EPD_FONT_TEMPERATURE);
    lv_point_t size;
    lv_text_get_size(&size, word, font, 0, 0, lv_obj_get_width(page->heading), LV_TEXT_FLAG_NONE);
    if (size.y > lv_obj_get_height(page->heading)) font = epd_app_font_role(EPD_FONT_TITLE);
    lv_obj_set_style_text_font(page->heading, font, 0);
    updated &= epd_app_text(page->heading, card ? r->fields[WORDS_WORD] : epd_get_str(words_no_cards));
    page->text_pending = !words_ui_phonetic(page->phonetic, card ? &r->view : NULL);
    updated &= epd_app_text(page->body, card && page->answer ? r->fields[WORDS_TRANSLATION] : "");
    for (unsigned i = 0; i < 4; ++i) show(page->buttons[i], card && page->answer);
    show(page->buttons[4], card && !page->answer);
    show(page->buttons[5], card && page->answer);
    show(page->buttons[6], card);
    show(page->buttons[7], !card);
    if (card) updated &= button_text(page->buttons[6], r->flags & WORDS_COLLECTED ? epd_get_str(words_uncollect) : epd_get_str(words_collect));
    char text[128];
    EPD_FMT_words_study_counts(text, sizeof(text), (unsigned long)(r ? r->counts.added : 0),
             (unsigned long)(r ? r->quota : 0), (unsigned long)(r ? r->counts.due : 0));
    updated &= epd_app_text(page->info, text);
    return updated && !page->text_pending;
}

static bool render(page_t *page)
{
    bool updated = true;
    words_result_t *r = page->result;
    char text[160];
    if (!r) return true;
    if (page->kind == PAGE_STUDY) return render_study(page);
    if (page->kind == PAGE_HOME)
    {
        updated &= epd_app_text(page->heading, words_scope_text(r->scope_missing ? "" : r->scope_id, r->scope_name));
        EPD_FMT_words_home_counts(text, sizeof(text),
                 (unsigned long)r->counts.due, (unsigned long)r->counts.added,
                 (unsigned long)r->quota, (unsigned long)r->counts.reviewed);
        updated &= epd_app_text(page->body, text);
        for (unsigned i = 0; i < 5; ++i) lv_obj_remove_state(page->buttons[i], LV_STATE_DISABLED);
        if (!r->clock_valid && page->status_code == WORDS_MESSAGE_NONE) updated &= status(page, WORDS_MESSAGE_CLOCK_REQUIRED);
    }
    else if (page->kind == PAGE_SETTINGS)
    {
        page->quota = r->quota;
        EPD_FMT_words_quota(text, sizeof(text), (unsigned long)r->quota);
        updated &= epd_app_text(page->heading, text); updated &= epd_app_text(page->body,
            words_scope_text(r->scope_missing ? "" : r->scope_id, r->scope_name));
        for (unsigned i = 0; i < 3; ++i) lv_obj_remove_state(page->buttons[i], LV_STATE_DISABLED);
    }
    else
    {
        for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i)
        {
            if (i < r->matches.count) updated &= button_text(page->buttons[i], page->kind == PAGE_SCOPES ?
                words_scope_text(r->scope_ids[i], r->names[i]) : r->names[i]);
            show(page->buttons[i], i < r->matches.count);
        }
        show(page->previous, page->offset > 0); show(page->next, r->matches.more);
        if (!r->matches.count && page->status_code == WORDS_MESSAGE_NONE) updated &= status(page, WORDS_MESSAGE_EMPTY);
        else if (page->kind == PAGE_SCOPES && page->status_code == WORDS_MESSAGE_NONE)
        {
            EPD_FMT_words_selected_scope(text, sizeof(text),
                words_scope_text(r->scope_missing ? "" : r->scope_id, r->scope_name));
            updated &= epd_app_text(page->status, text);
        }
    }
    return updated;
}

static void localize(page_t *page)
{
    uint32_t revision = epd_locale_revision();
    bool scaled = page->scale_revision != epd_ui_scale_revision();
    if (scaled) layout(page);
    if (page->locale_revision == revision && !page->text_pending && !scaled) return;
    bool updated = true;
    const char *titles[] = {epd_get_str(words_title), epd_get_str(words_study),
        epd_get_str(words_collection), epd_get_str(words_scopes), epd_get_str(words_settings_title)};
    updated &= epd_app_text(page->title, titles[page->kind]);
    if (page->kind == PAGE_HOME)
    {
        const char *labels[] = {epd_get_str(words_start_study), epd_get_str(words_lookup),
            epd_get_str(words_collection), epd_get_str(words_scopes), epd_get_str(words_settings)};
        for (unsigned i = 0; i < 5; ++i) updated &= button_text(page->buttons[i], labels[i]);
    }
    else if (page->kind == PAGE_SETTINGS)
    {
        updated &= button_text(page->buttons[0], epd_get_str(words_decrease));
        updated &= button_text(page->buttons[1], epd_get_str(words_increase));
        updated &= button_text(page->buttons[2], epd_get_str(words_choose_scope));
        updated &= epd_app_text(page->scope_label, epd_get_str(words_current_scope));
    }
    else if (page->kind == PAGE_STUDY)
    {
        const char *labels[] = {epd_get_str(words_again), epd_get_str(words_hard),
            epd_get_str(words_good), epd_get_str(words_easy), epd_get_str(words_show_answer),
            epd_get_str(words_full_entry), epd_get_str(words_collect), epd_get_str(words_continue)};
        for (unsigned i = 0; i < 8; ++i) updated &= button_text(page->buttons[i], labels[i]);
    }
    else
    {
        updated &= button_text(page->previous, epd_get_str(words_previous));
        updated &= button_text(page->next, epd_get_str(words_next));
    }
    updated &= epd_app_text(page->status, words_message_text(page->status_code));
    updated &= render(page);
    if (!updated) return;
    page->locale_revision = revision;
}

static void poll(lv_timer_t *timer)
{
    page_t *page = lv_timer_get_user_data(timer);
    localize(page);
    if (!page->started)
    {
        if (page->start_attempts >= 20) return;
        page->started = words_service_start();
        if (page->started) refresh(page);
        else if (++page->start_attempts >= 20)
        { status(page, WORDS_MESSAGE_START_FAILED); }
        return;
    }
    if (!page->request) return;
    bool failed;
    words_result_t *result = words_take(page->request, &failed);
    if (!result && !failed)
    {
        if (page->pending_status != WORDS_MESSAGE_NONE && lv_tick_elaps(page->request_tick) >= 200 &&
            page->status_code != page->pending_status)
            status(page, page->pending_status);
        return;
    }
    page->request = 0;
    if (!result || result->error)
    {
        status(page, result ? result->error : WORDS_MESSAGE_MEMORY);
        if (page->kind == PAGE_STUDY && (!page->result || (result && result->accepted)))
        {
            words_result_free(page->result); page->result = NULL; page->answer = false;
            for (unsigned i = 0; i < 7; ++i) show(page->buttons[i], false);
            show(page->buttons[7], true);
        }
        words_result_free(result); return;
    }
    if (page->kind == PAGE_SCOPES && result->accepted)
    {
        words_result_free(result); status(page, WORDS_MESSAGE_SCOPE_CHANGED); refresh(page); return;
    }
    if (page->kind == PAGE_COLLECTION && page->offset && !result->matches.count)
    {
        page->offset = page->offset >= WORDS_MATCH_MAX ? page->offset - WORDS_MATCH_MAX : 0;
        words_result_free(result); refresh(page); return;
    }
    bool same = page->kind == PAGE_STUDY && page->result && result->slot &&
                page->result->slot == result->slot && page->result->token == result->token;
    if (!same) page->answer = false;
    words_result_free(page->result); page->result = result;
    status(page, WORDS_MESSAGE_NONE); render(page);
}

static void click(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!page->started || page->request) return;
    lv_obj_t *target = lv_event_get_target_obj(event);
    if (target == page->previous || target == page->next)
    {
        if (target == page->previous) page->offset = page->offset >= WORDS_MATCH_MAX ? page->offset - WORDS_MATCH_MAX : 0;
        else page->offset += WORDS_MATCH_MAX;
        refresh(page); return;
    }
    unsigned i;
    for (i = 0; i < WORDS_MATCH_MAX && target != page->buttons[i]; ++i) {}
    if (i == WORDS_MATCH_MAX) return;
    if (page->kind == PAGE_HOME)
    {
        if (i == 0) open_page(PAGE_STUDY);
        else if (i == 1) words_open_lookup();
        else if (i == 2) open_page(PAGE_COLLECTION);
        else if (i == 3) open_page(PAGE_SCOPES);
        else open_page(PAGE_SETTINGS);
    }
    else if (page->kind == PAGE_SETTINGS)
    {
        if (i == 2) { open_page(PAGE_SCOPES); return; }
        words_job_t job = {.kind = WORDS_QUOTA};
        job.value = i == 0 ? (page->quota ? page->quota - 1 : 0) : (page->quota < 200 ? page->quota + 1 : 200);
        request(page, &job);
    }
    else if (page->kind == PAGE_STUDY)
    {
        words_result_t *r = page->result;
        if (i == 7) { refresh(page); return; }
        if (!r || !r->slot) return;
        if (i == 4) { page->answer = true; render_study(page); return; }
        if (i == 5)
        {
            page->child = (words_job_t){.kind = WORDS_DETAIL, .slot = r->slot};
            words_open_detail(&page->child); return;
        }
        words_job_t job = {.slot = r->slot, .token = r->token};
        if (i < 4)
        {
            if (!page->answer) return;
            job.kind = WORDS_RATE; job.value = i + 1;
        }
        else { job.kind = WORDS_COLLECT; job.value = !(r->flags & WORDS_COLLECTED); }
        request(page, &job);
    }
    else if (page->kind == PAGE_COLLECTION && page->result && i < page->result->matches.count)
    {
        page->child = (words_job_t){.kind = WORDS_DETAIL, .slot = page->result->matches.entry[i]};
        words_open_detail(&page->child);
    }
    else if (page->kind == PAGE_SCOPES && page->result && i < page->result->matches.count)
    {
        words_job_t job = {.kind = WORDS_SCOPE_SELECT};
        strcpy(job.query, page->result->scope_ids[i]);
        memcpy(job.identity, page->result->identity, sizeof(job.identity));
        request(page, &job);
    }
}

static void leave(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!page->root || words_service_flush()) gui_app_goback();
    else status(page, WORDS_MESSAGE_SAVE_PENDING);
}

static void build(page_t *page)
{
    const char *titles[] = {epd_get_str(words_title), epd_get_str(words_study), epd_get_str(words_collection), epd_get_str(words_scopes), epd_get_str(words_settings_title)};
    page->screen = words_prepare_back(titles[page->kind], leave, page, &page->title, &page->back);
    page->status = epd_app_label(page->screen, "", 32, 154, 620, epd_app_font_role(EPD_FONT_SMALL));
    if (page->kind == PAGE_HOME)
    {
        page->heading = epd_app_label(page->screen, "", 32, 222, 620, epd_app_font_role(EPD_FONT_TITLE));
        page->body = epd_app_label(page->screen, "", 32, 310, 620, epd_app_font());
        lv_obj_set_height(page->body, 260); lv_label_set_long_mode(page->body, LV_LABEL_LONG_WRAP);
        const char *labels[] = {epd_get_str(words_start_study), epd_get_str(words_lookup), epd_get_str(words_collection), epd_get_str(words_scopes), epd_get_str(words_settings)};
        for (unsigned i = 0; i < 5; ++i)
        {
            page->buttons[i] = epd_app_button(page->screen, 32, 620 + i * 112, 620, 88, labels[i], click, page);
            lv_obj_add_state(page->buttons[i], LV_STATE_DISABLED);
        }
    }
    else if (page->kind == PAGE_SETTINGS)
    {
        page->heading = epd_app_label(page->screen, "", 32, 236, 620, epd_app_font_role(EPD_FONT_TITLE));
        page->buttons[0] = epd_app_button(page->screen, 32, 338, 294, 88, epd_get_str(words_decrease), click, page);
        page->buttons[1] = epd_app_button(page->screen, 358, 338, 294, 88, epd_get_str(words_increase), click, page);
        page->scope_label = epd_app_label(page->screen, epd_get_str(words_current_scope), 32, 502, 620, epd_app_font());
        page->body = epd_app_label(page->screen, "", 32, 572, 620, epd_app_font());
        page->buttons[2] = epd_app_button(page->screen, 32, 672, 620, 88, epd_get_str(words_choose_scope), click, page);
        for (unsigned i = 0; i < 3; ++i) lv_obj_add_state(page->buttons[i], LV_STATE_DISABLED);
    }
    else if (page->kind == PAGE_STUDY)
    {
        page->heading = epd_app_label(page->screen, "", 32, 248, 620, epd_app_font_role(EPD_FONT_TEMPERATURE));
        lv_obj_set_height(page->heading, 104);
        page->phonetic = epd_app_label(page->screen, "", 32, 374, 620, epd_app_font());
        page->body = epd_app_label(page->screen, "", 32, 470, 620, epd_app_font());
        lv_obj_set_height(page->body, 422); lv_label_set_long_mode(page->body, LV_LABEL_LONG_DOT);
        page->info = epd_app_label(page->screen, "", 32, 1154, 620, epd_app_font_role(EPD_FONT_SMALL));
        const char *ratings[] = {epd_get_str(words_again), epd_get_str(words_hard), epd_get_str(words_good), epd_get_str(words_easy)};
        for (unsigned i = 0; i < 4; ++i)
            page->buttons[i] = epd_app_button(page->screen, 32 + i * 158, 1040, 146, 86, ratings[i], click, page);
        page->buttons[4] = epd_app_button(page->screen, 32, 1040, 620, 86, epd_get_str(words_show_answer), click, page);
        page->buttons[5] = epd_app_button(page->screen, 32, 934, 294, 80, epd_get_str(words_full_entry), click, page);
        page->buttons[6] = epd_app_button(page->screen, 358, 934, 294, 80, epd_get_str(words_collect), click, page);
        page->buttons[7] = epd_app_button(page->screen, 32, 1040, 620, 86, epd_get_str(words_continue), click, page);
        for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i) show(page->buttons[i], false);
    }
    else
    {
        for (unsigned i = 0; i < WORDS_MATCH_MAX; ++i)
        {
            page->buttons[i] = epd_app_button(page->screen, 32, 210 + i * 108, 620, 88, "", click, page);
            show(page->buttons[i], false);
        }
        page->previous = epd_app_button(page->screen, 32, 1100, 294, 76, epd_get_str(words_previous), click, page);
        page->next = epd_app_button(page->screen, 358, 1100, 294, 76, epd_get_str(words_next), click, page);
        show(page->previous, false); show(page->next, false);
    }
    layout(page);
    page->locale_revision = epd_locale_revision();
    page->timer = lv_timer_create(poll, 100, page); lv_timer_pause(page->timer);
}

static void message(gui_app_msg_type_t msg, void *parameter)
{
    (void)parameter;
    page_t *page = gui_app_this_page_memory();
    if (msg == GUI_APP_MSG_ONSTART)
    {
        uintptr_t value = (uintptr_t)gui_app_this_page_userdata();
        page->root = (value & 0x100u) != 0;
        page->kind = (page_kind_t)(value & 0xffu);
        build(page); page->started = !page->root || words_service_start();
    }
    else if (msg == GUI_APP_MSG_ONRESUME)
    {
        epd_app_header_refresh(page->screen);
        localize(page);
        lv_timer_resume(page->timer);
        if (page->started) refresh(page);
    }
    else if (msg == GUI_APP_MSG_ONPAUSE)
    {
        lv_timer_pause(page->timer); words_cancel(page->request); page->request = 0;
    }
    else if (msg == GUI_APP_MSG_ONSTOP)
    {
        lv_timer_delete(page->timer); words_result_free(page->result);
        if (page->root) words_service_stop();
    }
}

int app_main(intent_t intent)
{
    const char *entry = intent_get_string(intent, "page");
    uintptr_t root = (entry && !strcmp(entry, "settings") ? PAGE_SETTINGS : PAGE_HOME) | 0x100u;
    return gui_app_regist_msg_handler_ext("words", message, (void *)root, sizeof(page_t));
}
