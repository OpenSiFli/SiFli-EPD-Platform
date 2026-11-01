#include "ui_internal.h"
#include "reading_fonts.h"
#include "reading_fonts_browser.h"
#include "reading_font.h"
#include "ui_settings_list.h"
#include "ui_shell.h"
#include "ui_navigation.h"
#include "platform/epd_app_font.h"
#include "ui_reader_font_remote.h"

static bool install_on_open;

bool reading_fonts_open_install(void)
{
    install_on_open = true;
    if (ui_navigation_open(UI_PAGE_READING_FONTS)) return true;
    install_on_open = false;
    return false;
}

#define FONT_INSTALL (-18000)
#define FONT_APPLY (-18001)
#define FONT_DELETE (-18002)
#define FONT_DELETE_CONFIRM (-18003)
#define FONT_DEST_SD (-18004)
#define FONT_DEST_FLASH (-18005)
#define FONT_IMPORT (-18006)
#define FONT_UP (-18007)
#define FONT_PREVIOUS (-18008)
#define FONT_NEXT (-18009)
#define FONT_REFRESH (-18010)
#define FONT_STATUS (-18011)
#define FONT_DETAILS (-18012)
#define FONT_FORMAT (-18013)
#define FONT_SOURCE (-18014)
#define FONT_ITEM_BASE (-18200)
#define FONT_FILE_BASE (-18300)
#define FONT_DELETE_BASE (-18400)
#define FONT_ROWS (2 * READING_FONT_LIMIT + 6)

typedef enum { FONT_LIST, FONT_PREVIEW, FONT_BROWSER, FONT_DESTINATION } font_mode_t;
typedef enum { JOB_NONE, JOB_IMPORT, JOB_SELECT, JOB_DELETE } font_job_t;

static const char preview_text[] =
    "春风吹过书页，阅读让世界更宽广。\n"
    "Read a little every day.\n"
    "0123456789  Aa Bb  ，。！？；：“” () []";

typedef struct
{
    lv_obj_t *title, *body, *sample_box, *sample, *notice;
    ui_settings_list_t *list;
    lv_timer_t *timer;
    lv_font_t *font, *fallback;
    epd_app_font_page_t font_page;
    reading_fonts_browser_t browser;
    reading_font_info_t items[READING_FONT_LIMIT + 1], preview;
    ui_settings_list_row_t rows[FONT_ROWS];
    char details[READING_FONT_LIMIT + 1][144];
    char source[STORAGE_PATH_MAX], job_text[144], space[2][64];
    storage_volume_t destination;
    uint32_t source_session, source_size, id, storage, locale, scale;
    uint32_t glyph_offset, confirmation_serial, confirmation_id;
    unsigned count, size, weight;
    int32_t bottom, width;
    font_mode_t mode;
    reading_font_status_t status;
    bool dirty, missing, glyphs_done, preview_failed, browse_dirty;
    bool confirming, confirmed_selected, install_entry;
} reading_fonts_page_t;

static reading_fonts_page_t *active_page;
static struct
{
    uint32_t ticket;
    font_job_t kind, result_kind;
    bool preview, result_missing;
    reading_font_status_t status;
} job;

static reading_fonts_page_t *page(void)
{
    return current && current->id == UI_PAGE_READING_FONTS ? current->feature : NULL;
}

static bool working(const reading_font_status_t *status)
{
    return status->busy || (status->phase >= READING_FONT_QUEUED && status->phase <= READING_FONT_COMMITTING);
}

static const char *location(const reading_font_info_t *font)
{
    if (font->id == READING_FONT_BUILTIN) return epd_get_str(reading_fonts_builtin);
    return font->volume == STORAGE_SD ? epd_get_str(platform_card) : epd_get_str(platform_internal_space);
}

static const char *phase_text(reading_font_phase_t phase)
{
    switch (phase)
    {
    case READING_FONT_QUEUED: return epd_get_str(reading_fonts_queued);
    case READING_FONT_RECEIVING: return epd_get_str(reading_fonts_receiving);
    case READING_FONT_COPYING: return epd_get_str(reading_fonts_copying);
    case READING_FONT_VALIDATING: return epd_get_str(reading_fonts_validating);
    case READING_FONT_COMMITTING: return epd_get_str(reading_fonts_committing);
    case READING_FONT_DONE: return epd_get_str(reading_fonts_done);
    case READING_FONT_FAILED: return epd_get_str(reading_fonts_failed);
    default: return epd_get_str(reading_fonts_preparing);
    }
}

static lv_ext_str_id_t failure_title(font_job_t kind)
{
    switch (kind)
    {
    case JOB_IMPORT: return LV_EXT_STR_ID(reading_fonts_import_failed);
    case JOB_SELECT: return LV_EXT_STR_ID(reading_fonts_select_failed);
    case JOB_DELETE: return LV_EXT_STR_ID(reading_fonts_delete_failed);
    default: return LV_EXT_STR_ID(reading_fonts_failed);
    }
}

static void release_preview(reading_fonts_page_t *view)
{
    if (view->sample) lv_obj_set_style_text_font(view->sample, ui_font_body(), 0);
    if (view->sample_box) object_visible(view->sample_box, false);
    epd_app_font_page_release(view->font_page); view->font_page = NULL;
    ui_font_reader_destroy(view->font);
    ui_font_reader_destroy(view->fallback);
    view->font = view->fallback = NULL;
    view->glyph_offset = 0;
    view->glyphs_done = view->missing = false;
    view->dirty = true;
}

void reading_fonts_release_storage(void)
{
    if (!active_page) return;
    release_preview(active_page);
    active_page->dirty = active_page->browse_dirty = true;
}

static void change_mode(reading_fonts_page_t *view, font_mode_t mode)
{
    release_preview(view);
    view->mode = mode;
    view->dirty = true;
    view->preview_failed = false;
    view->confirming = false;
    ui_settings_list_set_rows(view->list, NULL, 0);
}

static unsigned reading_size(void)
{
    static const unsigned sizes[] = {28, 30, 32, 34, 36, 40, 44, 48};
    unsigned index = ui_settings_index(UI_SETTING_FONT_SIZE);
    return sizes[index < sizeof(sizes) / sizeof(sizes[0]) ? index : 2];
}

static uint32_t next_character(const char *text, uint32_t *offset)
{
    const unsigned char *p = (const unsigned char *)text + *offset;
    uint32_t character = *p++;
    unsigned extra = character < 0x80 ? 0 : character < 0xe0 ? 1 : character < 0xf0 ? 2 : 3;
    if (extra) character &= (1u << (6u - extra)) - 1u;
    for (unsigned i = 0; i < extra; ++i) character = (character << 6) | (*p++ & 0x3f);
    *offset = (uint32_t)(p - (const unsigned char *)text);
    return character;
}

static void check_glyphs(reading_fonts_page_t *view)
{
    if (!view->font || view->glyphs_done) return;
    int status = epd_app_font_status(view->font);
    if (status == 0) return;
    if (status > 0) status = epd_app_font_prepare_text(view->font, preview_text, sizeof(preview_text) - 1, true);
    if (status == 0) return;
    if (status < 0) {
        release_preview(view); view->preview_failed = true;
    } else {
        uint32_t offset = 0;
        while (preview_text[offset]) {
            uint32_t letter = next_character(preview_text, &offset);
            view->missing |= ui_reader_font_remote_glyph_fallback(view->font, letter);
        }
        view->glyphs_done = true;
        lv_obj_set_style_text_font(view->sample, view->font, 0);
    }
    view->dirty = true;
}

static void selected(int key, void *context)
{
    if (current == context && current->foreground && !current->popup && !ui_shell_modal_visible() &&
        !storage_changing() && pending_page == NAV_IDLE) pending_page = key;
}

void reading_fonts_restore_focus(void)
{
    reading_fonts_page_t *view = page();
    if (view && view->list && !current->popup) ui_settings_list_set_group(view->list, current->group);
}

static ui_settings_list_row_t row(int key, const char *title, const char *value, bool enabled)
{
    return (ui_settings_list_row_t){.key=key, .kind=UI_SETTINGS_ROW_LINK,
        .title=title, .value=value, .enabled=enabled, .min_height=epd_ui_touch(64)};
}

static ui_settings_list_row_t info_row(int key, const char *title, const char *value)
{
    ui_settings_list_row_t item = row(key, title, value, true);
    item.kind = UI_SETTINGS_ROW_INFO;
    return item;
}

static unsigned progress_percent(const reading_font_status_t *status)
{
    return status->total ? (unsigned)LV_MIN(100u,
        (uint64_t)status->completed * 100u / status->total) : 0u;
}

static unsigned status_row(reading_fonts_page_t *view, unsigned count)
{
    if (job.result_missing)
    {
        view->rows[count++] = info_row(FONT_STATUS, epd_get_str(reading_fonts_status),
            epd_get_str(reading_fonts_result_missing));
        return count;
    }
    const reading_font_status_t *status = job.ticket ? &job.status : &view->status;
    if (status->phase == READING_FONT_IDLE && status->ready) return count;
    const char *text = job.ticket && status->phase == READING_FONT_FAILED ?
        LV_EXT_STR_GET(failure_title(job.result_kind)) : phase_text(status->phase);
    if ((status->phase == READING_FONT_RECEIVING || status->phase == READING_FONT_COPYING) && status->total)
    {
        unsigned percent = progress_percent(status);
        snprintf(view->job_text, sizeof(view->job_text), "%s  %u%%", text, percent);
    }
    else if (status->phase == READING_FONT_FAILED)
        snprintf(view->job_text, sizeof(view->job_text), "%s (%d)", text, (int)status->error);
    else snprintf(view->job_text, sizeof(view->job_text), "%s", text);
    view->rows[count++] = info_row(FONT_STATUS, epd_get_str(reading_fonts_status), view->job_text);
    return count;
}

static bool source_ready(const reading_fonts_page_t *view)
{
    return view->source[0] && storage_session_valid(STORAGE_SD, view->source_session) &&
           storage_available(STORAGE_SD) && !storage_changing();
}

static bool destination_ready(reading_fonts_page_t *view, storage_volume_t volume)
{
    storage_info_t storage;
    if (!storage_info(volume, &storage) || !storage.mounted || !storage_available(volume))
    {
        snprintf(view->space[volume], sizeof(view->space[volume]), "%s", epd_get_str(reading_fonts_unavailable));
        return false;
    }
    uint64_t free_bytes = storage.total > storage.used ? storage.total - storage.used : 0;
    bool enough = free_bytes >= view->source_size;
    if (!enough) snprintf(view->space[volume], sizeof(view->space[volume]), "%s", epd_get_str(reading_fonts_no_space));
    else snprintf(view->space[volume], sizeof(view->space[volume]), "%llu KiB", (unsigned long long)(free_bytes / 1024u));
    return enough;
}

static void prepare_preview(reading_fonts_page_t *view)
{
    reading_font_info_t font;
    bool available = reading_font_get(view->id, &font) && font.available;
    if (view->font && (!available || view->preview.signature != font.signature ||
        view->size != reading_size() || view->weight != ui_settings_index(UI_SETTING_FONT_WEIGHT)))
        release_preview(view);
    if (!available) return;
    view->preview = font;
    if (view->font && ui_font_reader_faulted(view->font))
    {
        release_preview(view);
        view->preview_failed = view->dirty = true;
    }
    if (view->font || view->preview_failed || working(&view->status) || storage_changing()) return;
    view->size = reading_size();
    view->weight = ui_settings_index(UI_SETTING_FONT_WEIGHT);
    uint32_t identity;
    view->font = ui_font_reading_create(view->id, view->size, view->weight, &identity, &view->fallback);
    view->preview_failed = !view->font;
    view->dirty = true;
    if (view->font) {
        view->font_page = epd_app_font_page_create();
        bool ok = view->font_page != NULL;
        uint32_t offset = 0;
        while (ok && preview_text[offset]) {
            uint32_t character = next_character(preview_text, &offset);
            ok = epd_app_font_page_pin(view->font_page, view->font, character, 0, true) >= 0;
        }
        if (!ok) { release_preview(view); view->preview_failed = true; }
    }
}

static void layout(reading_fonts_page_t *view)
{
    int32_t width = lv_obj_get_width(current->screen);
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    int32_t top = SETTINGS_TITLE_Y + ui_font_settings_title()->line_height + epd_ui_px(32);
    lv_obj_set_pos(view->title, SETTINGS_TITLE_X, SETTINGS_TITLE_Y);
    lv_obj_set_size(view->title, LV_MAX(1, width - SETTINGS_TITLE_X - MARGIN), ui_font_settings_title()->line_height);
    lv_obj_set_style_text_font(view->title, ui_font_settings_title(), 0);
    const char *title = view->mode == FONT_PREVIEW ? epd_get_str(reading_fonts_preview) :
        view->mode == FONT_BROWSER ? epd_get_str(reading_fonts_choose) :
        view->mode == FONT_DESTINATION ? epd_get_str(reading_fonts_install) : epd_get_str(reading_fonts_title);
    text_update(view->title, title);
    bool preview = view->mode == FONT_PREVIEW && view->font && view->glyphs_done;
    object_visible(view->sample_box, preview);
    object_visible(view->notice, view->mode == FONT_PREVIEW);
    if (view->mode == FONT_PREVIEW)
    {
        int32_t margin = ui_settings_list_content_margin(width);
        int32_t available = LV_MAX(1, bottom - top);
        int32_t height = preview ? LV_MIN(available * 2 / 5,
            view->font->line_height * 5 + epd_ui_px(32)) : 0;
        lv_obj_set_pos(view->sample_box, margin, top);
        lv_obj_set_size(view->sample_box, LV_MAX(1, width - 2 * margin), LV_MAX(1, height));
        lv_obj_set_pos(view->sample, epd_ui_px(12), epd_ui_px(12));
        lv_obj_set_size(view->sample, LV_MAX(1, width - 2 * margin - epd_ui_px(24)),
                        LV_MAX(1, height - epd_ui_px(24)));
        const char *notice = !view->font ? epd_get_str(reading_fonts_preview_unavailable) :
            !view->glyphs_done ? epd_get_str(reading_fonts_checking_glyphs) :
            view->missing ? epd_get_str(reading_fonts_fallback) : epd_get_str(reading_fonts_preview_only);
        text_update(view->notice, notice);
        lv_obj_set_style_text_font(view->notice, ui_font_settings_value(), 0);
        lv_obj_set_pos(view->notice, margin, top + height + epd_ui_px(8));
        lv_obj_set_width(view->notice, LV_MAX(1, width - 2 * margin));
        lv_obj_set_height(view->notice, LV_SIZE_CONTENT);
        lv_obj_update_layout(view->notice);
        top += height + lv_obj_get_height(view->notice) + epd_ui_px(24);
    }
    lv_obj_set_pos(view->body, 0, top);
    lv_obj_set_size(view->body, width, LV_MAX(1, bottom - top));
    lv_obj_update_layout(view->body);
    view->bottom = bottom;
    view->width = width;
}

static unsigned render_list(reading_fonts_page_t *view)
{
    unsigned count = 0;
    bool idle = view->status.ready && !working(&view->status) && job.kind == JOB_NONE && !storage_changing();
    view->rows[count++] = row(FONT_INSTALL, epd_get_str(reading_fonts_install), NULL, idle && storage_available(STORAGE_SD));
    count = status_row(view, count);
    size_t length = reading_font_list(view->items, READING_FONT_LIMIT + 1);
    view->count = (unsigned)LV_MIN(length, READING_FONT_LIMIT + 1);
    for (unsigned i = 0; i < view->count; ++i)
    {
        reading_font_info_t *font = &view->items[i];
        snprintf(view->details[i], sizeof(view->details[i]), "%s%s%s", location(font),
            font->selected || !font->available ? " · " : "",
            !font->available ? epd_get_str(reading_fonts_unavailable) :
            font->selected ? epd_get_str(reading_fonts_current) : "");
        view->rows[count++] = row(FONT_ITEM_BASE + (int)i, font->name, view->details[i], true);
        if (font->id != READING_FONT_BUILTIN)
            view->rows[count++] = row(FONT_DELETE_BASE + (int)i, epd_get_str(reading_fonts_delete),
                font->name, idle && storage_available(font->volume));
    }
    return count;
}

static unsigned render_preview(reading_fonts_page_t *view)
{
    unsigned count = 0;
    bool installed = reading_font_get(view->id, &view->preview);
    bool available = installed && view->preview.available;
    bool idle = view->status.ready && !working(&view->status) && job.kind == JOB_NONE && !storage_changing();
    view->rows[count++] = info_row(FONT_DETAILS, view->preview.name,
        available ? location(&view->preview) : epd_get_str(reading_fonts_unavailable));
    view->rows[count++] = row(FONT_APPLY,
        view->status.selected == view->id ? epd_get_str(reading_fonts_current) : epd_get_str(reading_fonts_apply),
        NULL, idle && available && view->status.selected != view->id);
    if (view->id != READING_FONT_BUILTIN)
        view->rows[count++] = row(FONT_DELETE, epd_get_str(reading_fonts_delete), NULL,
            idle && installed && storage_available(view->preview.volume));
    return status_row(view, count);
}

static unsigned render_browser(reading_fonts_page_t *view)
{
    reading_fonts_browser_t *browser = &view->browser;
    if (view->browse_dirty)
    {
        reading_fonts_browser_read(browser);
        view->browse_dirty = false;
    }
    unsigned count = 0;
    view->rows[count++] = info_row(FONT_SOURCE, browser->path, epd_get_str(reading_fonts_format));
    if (strcmp(browser->path, STORAGE_SD_ROOT))
        view->rows[count++] = row(FONT_UP, epd_get_str(reading_fonts_parent), NULL, true);
    view->rows[count++] = row(FONT_REFRESH, epd_get_str(platform_refresh), NULL, storage_available(STORAGE_SD));
    if (browser->error || !browser->count)
        view->rows[count++] = info_row(FONT_STATUS, epd_get_str(reading_fonts_choose),
            browser->error ? epd_get_str(reading_fonts_browse_failed) : epd_get_str(reading_fonts_empty));
    for (unsigned i = 0; i < browser->count; ++i)
    {
        const reading_fonts_file_t *file = &browser->files[i];
        snprintf(view->details[i], sizeof(view->details[i]), "%s", file->directory ? epd_get_str(platform_folder) : ".ttf");
        view->rows[count++] = row(FONT_FILE_BASE + (int)i, file->name, view->details[i],
            !browser->error && (file->directory || (file->size > 0 && file->size <= READING_FONT_FILE_MAX)));
    }
    if (browser->first) view->rows[count++] = row(FONT_PREVIOUS, epd_get_str(platform_previous), NULL, true);
    if (browser->more) view->rows[count++] = row(FONT_NEXT, epd_get_str(platform_next), NULL, true);
    return count;
}

static unsigned render_destination(reading_fonts_page_t *view)
{
    bool ready = source_ready(view), sd = destination_ready(view, STORAGE_SD), flash = destination_ready(view, STORAGE_FLASH);
    unsigned count = 0;
    view->rows[count++] = info_row(FONT_SOURCE, view->source, epd_get_str(reading_fonts_format));
    view->rows[count++] = info_row(FONT_FORMAT, epd_get_str(reading_fonts_destination), epd_get_str(reading_fonts_space));
    view->rows[count] = row(FONT_DEST_SD, epd_get_str(platform_card), view->space[STORAGE_SD], ready && sd);
    view->rows[count].kind = UI_SETTINGS_ROW_CHOICE;
    view->rows[count++].checked = view->destination == STORAGE_SD;
    view->rows[count] = row(FONT_DEST_FLASH, epd_get_str(platform_internal_space), view->space[STORAGE_FLASH], ready && flash);
    view->rows[count].kind = UI_SETTINGS_ROW_CHOICE;
    view->rows[count++].checked = view->destination == STORAGE_FLASH;
    view->rows[count++] = row(FONT_IMPORT, epd_get_str(reading_fonts_start), NULL,
        ready && view->status.ready && !working(&view->status) && job.kind == JOB_NONE &&
        (view->destination == STORAGE_SD ? sd : flash));
    if (!ready) view->rows[count++] = info_row(FONT_STATUS, epd_get_str(reading_fonts_status), epd_get_str(reading_fonts_source_changed));
    return count;
}

void reading_fonts_process(void)
{
    reading_fonts_page_t *view = page();
    if (!view || !view->list || !current->foreground || !lv_refreshing_done()) return;
    reading_font_status_t status;
    reading_font_get_status(&status);
    uint32_t storage = storage_revision();
    if (view->storage != storage)
    {
        release_preview(view);
        view->preview_failed = false;
        view->browse_dirty = true;
        if (!storage_session_valid(STORAGE_SD, view->browser.session))
        { strcpy(view->browser.path, STORAGE_SD_ROOT); view->browser.first = 0; }
        view->dirty = true;
        view->storage = storage;
    }
    if (view->status.phase != status.phase || view->status.ticket != status.ticket ||
        progress_percent(&view->status) != progress_percent(&status) || view->status.total != status.total ||
        view->status.revision != status.revision || view->status.busy != status.busy ||
        view->status.ready != status.ready || view->status.selected != status.selected || view->status.error != status.error)
        view->dirty = true;
    view->status = status;
    if (job.kind != JOB_NONE)
    {
        reading_font_status_t own;
        if (reading_font_job_status(job.ticket, &own))
        {
            if (job.status.phase != own.phase || progress_percent(&job.status) != progress_percent(&own) ||
                job.status.total != own.total || job.status.error != own.error)
                view->dirty = true;
            job.status = own;
            job.result_missing = false;
        }
        else if ((int32_t)(status.ticket - job.ticket) > 0 &&
                 job.status.phase != READING_FONT_DONE && job.status.phase != READING_FONT_FAILED)
        { job.result_missing = true; view->dirty = true; }
    }
    if (current->popup) return;
    view->confirming = false;
    if (job.kind != JOB_NONE && job.result_missing)
    {
        job.kind = JOB_NONE;
        job.preview = false;
        popup_open_i18n(LV_EXT_STR_ID(reading_fonts_result_missing), NULL);
        return;
    }
    if (job.kind != JOB_NONE &&
        (job.status.phase == READING_FONT_DONE || job.status.phase == READING_FONT_FAILED))
    {
        if (job.kind == JOB_IMPORT && job.preview && job.status.phase == READING_FONT_DONE && view->mode == FONT_LIST)
        {
            view->id = job.status.result_id;
            change_mode(view, FONT_PREVIEW);
        }
        job.kind = JOB_NONE;
        job.preview = false;
        view->dirty = true;
        if (job.status.phase == READING_FONT_FAILED)
        {
            popup_open_i18n(failure_title(job.result_kind), NULL);
            return;
        }
    }
    if (view->mode == FONT_PREVIEW)
    {
        prepare_preview(view);
        check_glyphs(view);
    }
    uint32_t locale = epd_locale_revision(), scale = epd_ui_scale_revision();
    int32_t bottom = LV_MIN(ui_shell_content_bottom(), lv_obj_get_height(current->screen));
    if (view->locale != locale || view->scale != scale || view->bottom != bottom ||
        view->width != lv_obj_get_width(current->screen)) view->dirty = true;
    if (!view->dirty) { reading_fonts_restore_focus(); return; }
    layout(view);
    if (!ui_settings_list_refresh_fonts(view->list, ui_font_settings_body(), ui_font_settings_value())) return;
    unsigned count = view->mode == FONT_LIST ? render_list(view) : view->mode == FONT_PREVIEW ? render_preview(view) :
        view->mode == FONT_BROWSER ? render_browser(view) : render_destination(view);
    if (!ui_settings_list_set_rows(view->list, view->rows, count)) return;
    reading_fonts_restore_focus();
    view->locale = locale;
    view->scale = scale;
    view->dirty = false;
}

static void poll(lv_timer_t *timer)
{
    if (current == lv_timer_get_user_data(timer)) reading_fonts_process();
}

void reading_fonts_create(lv_obj_t *screen)
{
    reading_fonts_page_t *view = lv_malloc_zeroed(sizeof(*view));
    if (!view) return;
    current->feature = view;
    active_page = view;
    view->install_entry = install_on_open;
    view->mode = install_on_open ? FONT_BROWSER : FONT_LIST;
    install_on_open = false;
    view->dirty = true;
    view->destination = STORAGE_SD;
    strcpy(view->browser.path, STORAGE_SD_ROOT);
    view->title = label_create(screen, "", SETTINGS_TITLE_X, SETTINGS_TITLE_Y, 1, ui_font_settings_title());
    lv_label_set_long_mode(view->title, LV_LABEL_LONG_DOT);
    view->body = lv_obj_create(screen);
    epd_obj_init(view->body);
    view->sample_box = lv_obj_create(screen);
    epd_obj_init(view->sample_box);
    lv_obj_set_style_border_width(view->sample_box, 1, 0);
    lv_obj_set_style_border_color(view->sample_box, lv_color_black(), 0);
    view->sample = label_create(view->sample_box, preview_text, 0, 0, 1, ui_font_body());
    lv_label_set_long_mode(view->sample, LV_LABEL_LONG_WRAP);
    view->notice = label_create(screen, "", 0, 0, 1, ui_font_settings_value());
    lv_label_set_long_mode(view->notice, LV_LABEL_LONG_WRAP);
    view->list = ui_settings_list_create(view->body, ui_font_settings_body(), ui_font_settings_value(), selected, current);
    view->timer = lv_timer_create(poll, 200, current);
    object_visible(view->sample_box, false);
    object_visible(view->notice, false);
}

void reading_fonts_resume(void)
{
    reading_fonts_page_t *view = page();
    if (!view) return;
    view->dirty = view->browse_dirty = true;
    view->preview_failed = false;
    if (view->timer) lv_timer_resume(view->timer);
    reading_fonts_process();
}

void reading_fonts_pause(void)
{
    reading_fonts_page_t *view = page();
    if (!view) return;
    if (view->timer) lv_timer_pause(view->timer);
    release_preview(view);
    view->dirty = true;
}

void reading_fonts_stop(void)
{
    reading_fonts_page_t *view = page();
    if (!view) return;
    if (view->timer) lv_timer_delete(view->timer);
    release_preview(view);
    if (view->list) ui_settings_list_destroy(view->list);
    if (active_page == view) active_page = NULL;
    lv_free(view);
    current->feature = NULL;
}

static void request_finished(reading_fonts_page_t *view, rt_err_t error, uint32_t ticket, font_job_t kind)
{
    if (error != RT_EOK) popup_open_i18n(LV_EXT_STR_ID(reading_fonts_request_failed), NULL);
    else
    {
        job.ticket = ticket;
        job.kind = kind;
        job.result_kind = kind;
        job.result_missing = false;
        job.status = (reading_font_status_t){.ticket=ticket, .phase=READING_FONT_QUEUED};
        job.preview = kind == JOB_IMPORT;
        if (kind != JOB_SELECT) change_mode(view, FONT_LIST);
    }
    view->dirty = true;
}

bool reading_fonts_action(int *target)
{
    reading_fonts_page_t *view = page();
    if (!view || !view->list) return false;
    int action = *target;
    if (action == POPUP_CLOSE) { view->dirty = true; view->confirming = false; return false; }
    if (!lv_refreshing_done()) { pending_page = action; return true; }
    if (action == NAV_BACK && !current->popup)
    {
        if (view->mode == FONT_LIST || (view->install_entry && view->mode == FONT_BROWSER))
        { release_preview(view); return false; }
        font_mode_t mode = view->mode == FONT_DESTINATION ? FONT_BROWSER : FONT_LIST;
        change_mode(view, mode);
        view->browse_dirty = true;
        reading_fonts_process();
        return true;
    }
    if (action == FONT_DELETE_CONFIRM)
    {
        bool confirmed = view->confirming && current->popup && current->popup_serial == view->confirmation_serial &&
                         view->confirmation_id == view->id &&
                         (view->mode == FONT_PREVIEW || view->mode == FONT_LIST);
        if (!confirmed) return true;
        view->confirming = false;
        popup_close();
        reading_font_info_t font;
        reading_font_status_t status;
        reading_font_get_status(&status);
        if (storage_changing() || working(&status) || job.kind != JOB_NONE || view->id == READING_FONT_BUILTIN ||
            !reading_font_get(view->id, &font) || !storage_available(font.volume)) return true;
        if (status.selected == view->id && !view->confirmed_selected)
        { pending_page = FONT_DELETE; return true; }
        release_preview(view);
        uint32_t ticket = 0;
        rt_err_t error = reading_font_delete(view->id, &ticket);
        request_finished(view, error, ticket, JOB_DELETE);
        return true;
    }
    if (current->popup) return false;
    if (storage_changing()) return true;
    reading_font_status_t status;
    reading_font_get_status(&status);
    bool idle = status.ready && !working(&status) && job.kind == JOB_NONE;
    if (action == FONT_INSTALL && view->mode == FONT_LIST)
    {
        if (!idle || !storage_available(STORAGE_SD)) return true;
        change_mode(view, FONT_BROWSER);
        strcpy(view->browser.path, STORAGE_SD_ROOT);
        view->browser.first = 0;
        view->browse_dirty = true;
    }
    else if (view->mode == FONT_LIST && action >= FONT_ITEM_BASE && action < FONT_ITEM_BASE + (int)view->count)
    {
        reading_font_info_t font;
        uint32_t id = view->items[action - FONT_ITEM_BASE].id;
        if (!reading_font_get(id, &font)) return true;
        change_mode(view, FONT_PREVIEW);
        view->id = id;
        view->preview = font;
    }
    else if (view->mode == FONT_PREVIEW && action == FONT_APPLY)
    {
        reading_font_info_t font;
        if (!idle || !reading_font_get(view->id, &font) || !font.available) return true;
        release_preview(view);
        uint32_t ticket = 0;
        rt_err_t error = reading_font_select(view->id, &ticket);
        request_finished(view, error, ticket, JOB_SELECT);
    }
    else if (((view->mode == FONT_PREVIEW || view->mode == FONT_LIST) && action == FONT_DELETE) ||
             (view->mode == FONT_LIST && action >= FONT_DELETE_BASE && action < FONT_DELETE_BASE + (int)view->count))
    {
        reading_font_info_t font;
        if (!idle) return true;
        if (action != FONT_DELETE) view->id = view->items[action - FONT_DELETE_BASE].id;
        if (view->id == READING_FONT_BUILTIN || !reading_font_get(view->id, &font) ||
            !storage_available(font.volume)) return true;
        view->confirmed_selected = status.selected == view->id;
        popup_confirm_i18n(view->confirmed_selected ? LV_EXT_STR_ID(reading_fonts_delete_active) :
            LV_EXT_STR_ID(reading_fonts_delete_confirm), font.name, FONT_DELETE_CONFIRM);
        view->confirmation_serial = current->popup_serial;
        view->confirmation_id = view->id;
        view->confirming = true;
    }
    else if (view->mode == FONT_BROWSER)
    {
        if (action == FONT_UP) reading_fonts_browser_up(&view->browser);
        else if (action == FONT_PREVIOUS && view->browser.first)
            view->browser.first = view->browser.first > READING_FONTS_FILES ? view->browser.first - READING_FONTS_FILES : 0;
        else if (action == FONT_NEXT && view->browser.more) view->browser.first += view->browser.count;
        else if (action == FONT_REFRESH) {}
        else if (action >= FONT_FILE_BASE && action < FONT_FILE_BASE + (int)view->browser.count)
        {
            unsigned index = (unsigned)(action - FONT_FILE_BASE);
            char path[STORAGE_PATH_MAX];
            if (!storage_session_valid(STORAGE_SD, view->browser.session) ||
                !reading_fonts_browser_path(&view->browser, index, path, sizeof(path)))
            { view->browse_dirty = view->dirty = true; return true; }
            if (view->browser.files[index].directory)
            { strcpy(view->browser.path, path); view->browser.first = 0; }
            else
            {
                strcpy(view->source, path);
                view->source_session = view->browser.session;
                view->source_size = view->browser.files[index].size;
                view->destination = STORAGE_SD;
                change_mode(view, FONT_DESTINATION);
            }
        }
        else return false;
        view->browse_dirty = true;
    }
    else if (view->mode == FONT_DESTINATION && (action == FONT_DEST_SD || action == FONT_DEST_FLASH))
        view->destination = action == FONT_DEST_SD ? STORAGE_SD : STORAGE_FLASH;
    else if (view->mode == FONT_DESTINATION && action == FONT_IMPORT)
    {
        if (!idle || !source_ready(view) || !destination_ready(view, view->destination))
        { view->dirty = true; return true; }
        uint32_t ticket = 0;
        rt_err_t error = reading_font_import(view->source, view->destination, &ticket);
        request_finished(view, error, ticket, JOB_IMPORT);
    }
    else return false;
    view->dirty = true;
    reading_fonts_process();
    return true;
}
