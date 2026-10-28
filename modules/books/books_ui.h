#ifndef BOOKS_UI_H
#define BOOKS_UI_H
#include "platform/epd_app.h"
#include "../common/reader/reader_i18n.h"
#include "platform/epd_input.h"
#include "platform/app_content.h"
#include "ui_reader.h"
#include "ui_bookshelf_data.h"
#include "ui_settings.h"
#include "icons/ui_icons.h"
#include "storage.h"
#include <stdio.h>
#include <string.h>

#define WIDTH 684
#define HEIGHT 1216
#define MARGIN 32
#define CONTENT_WIDTH (WIDTH - 2 * MARGIN)
#define PAGE_TITLE_X (MARGIN + 72)
#define PAGE_TITLE_Y 81
#define PAGE_CONTENT_Y 174
#define ICON_BUTTON_SIZE 56
#define BACK_BUTTON_Y 72
#define FOCUS_CAPACITY 64

#define NAV_BACK (-2)
#define NAV_IDLE (-1)
#define POPUP_CLOSE (-5)
#define BOOKSHELF_PREVIOUS (-6)
#define BOOKSHELF_NEXT (-7)
#define BOOK_OPEN_BASE (-128)
#define BOOKSHELF_PAGE_SIZE 4

#define READER_PREVIOUS (-210)
#define READER_NEXT (-211)
#define READER_MENU (-212)
#define READER_CONFIRM (-213)
#define READER_OPTION_PREVIOUS (-214)
#define READER_OPTION_NEXT (-215)
#define READER_OPTION_CYCLE (-216)
#define READER_MINUS5 (-217)
#define READER_MINUS1 (-218)
#define READER_PLUS1 (-219)
#define READER_PLUS5 (-220)
#define READER_TOC (-230)
#define READER_TOC_PREVIOUS (-231)
#define READER_TOC_NEXT (-232)
#define READER_TOC_CLOSE (-233)
#define READER_TOC_BASE (-240)


enum { UI_PAGE_BOOKSHELF, UI_PAGE_READER, UI_PAGE_TEXT_SETTINGS, UI_PAGE_COUNT };
#define UI_PAGE_HOME UI_PAGE_BOOKSHELF
typedef enum {
    BOOKS_NOTICE_NONE, BOOKS_NOTICE_PAGE, BOOKS_NOTICE_CONTENT,
    BOOKS_NOTICE_OPEN, BOOKS_NOTICE_STORAGE, BOOKS_NOTICE_READER, BOOKS_NOTICE_SCAN
} books_notice_t;
typedef struct
{
    unsigned id, bookshelf_first;
    uint32_t content_generation, locale_revision, scale_revision;
    lv_area_t content_area;
    books_notice_t popup_notice;
    reader_error_t popup_error;
    rt_err_t popup_content_error;
    bool foreground;
    lv_obj_t *screen;
    lv_obj_t *back;
    lv_group_t *group;
    lv_obj_t *focus_items[FOCUS_CAPACITY];
    unsigned focus_count;
    lv_obj_t *popup, *popup_text, *popup_detail, *popup_button, *popup_previous_focus;
    unsigned popup_focus_start;
    lv_obj_t *reader_body, *reader_title, *reader_chapter, *reader_footer, *reader_progress, *reader_panel;
    lv_obj_t *reader_battery, *reader_clock, *reader_option_label, *reader_jump_label;
    lv_obj_t *reader_panel_title, *reader_confirm, *reader_contents, *reader_text_settings;
    lv_obj_t *reader_toc_previous, *reader_toc_close, *reader_toc_next;
    uint32_t reader_revision;
    uint32_t reader_loading_tick;
    unsigned reader_rendered_pages, reader_jump, reader_option, reader_panel_focus_start;
    bool reader_confirm_pending, reader_reflow_pending;
    bool reader_error_reported;
    bool reader_loading, reader_loading_shown, reader_has_content;
    bool reader_toc_panel;
    unsigned reader_toc_first;
    unsigned reader_reflow_page;
    struct { lv_obj_t *card, *cover, *title, *metadata, *fill, *progress; } books[BOOKSHELF_PAGE_SIZE];
    lv_obj_t *bookshelf_title, *bookshelf_empty, *bookshelf_previous, *bookshelf_next;
} books_page_t;
extern books_page_t *current;
extern int pending_page;
extern bool reader_session;

#define launcher_current_page() (current ? current->id : UI_PAGE_COUNT)
#define ui_font_body() epd_app_font_role(EPD_FONT_BODY)
#define ui_font_small() epd_app_font_role(EPD_FONT_SMALL)
#define ui_font_title() epd_app_font_role(EPD_FONT_TITLE)
#define ui_font_caption() epd_app_font_role(EPD_FONT_CAPTION)
#define label_create epd_app_label
#define centered_label epd_app_centered_label
#define panel_create epd_app_panel
#define icon_create epd_app_icon
#define text_update epd_app_text
#define epd_obj_init epd_app_object_init
#define lv_refreshing_done epd_app_refresh_done
#define focus_eligible epd_input_focusable

lv_obj_t *button_create(lv_obj_t *parent, int x, int y, int width, int height, const char *text, int target);
lv_obj_t *page_title_create(lv_obj_t *screen, const char *text, const lv_font_t *font);
void object_visible(lv_obj_t *object, bool visible);
void focus_restore(unsigned index);
void popup_open(const char *text, const char *detail);
void popup_close(void);
void popup_show(books_notice_t notice, reader_error_t error, rt_err_t content_error);
bool popup_refresh(void);
void popup_layout(void);
void books_scale_refresh(void);
/* True requests a short UI wake for required page computation, not prefetch. */
bool books_process(void);
void bookshelf_start(lv_obj_t *screen);
void bookshelf_resume(void);
void bookshelf_pause(void);
void reader_start(lv_obj_t *screen);
void reader_foreground(void);
void reader_pause(void);
void reader_stop(void);
bool reader_action(int *target, bool *opened);
bool bookshelf_action(int *target, bool *opened);
int books_command(int argc, char **argv);
#endif
