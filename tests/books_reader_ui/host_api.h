#pragma once
#include "lvgl.h"
#include "epd_memory.h"
#include "platform/epd_app.h"
#include "platform/epd_locale.h"
#include "platform/epd_keyboard.h"
#include "platform/epd_i18n.h"
#include "reader_types.h"
#include "ui_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
typedef int rt_err_t;
typedef enum { STORAGE_FLASH, STORAGE_SD } storage_volume_t;
#define STORAGE_PATH_MAX 512
#include "reading_font.h"
#include "frontlight.h"
#include "reader_ai.h"
#define WIDTH 684
#define HEIGHT 1216
#define NAV_IDLE -1
#define NAV_BACK -2
#define READER_MENU -212
#define UI_PAGE_READER 1
#define UI_READER_PAGE_MAX READER_PAGE_MAX
typedef reader_view_t ui_reader_view_t;
typedef struct {
    lv_obj_t *screen,*reader_panel,*back,*popup,*reader_body;
    lv_obj_t *focus_items[64];
    lv_group_t *group;
    unsigned focus_count,reader_panel_focus_start,id;
    bool foreground,reader_confirm_pending,reader_reflow_pending,reader_error_reported;
    unsigned reader_reflow_page;
    unsigned reader_jump,reader_revision;
} books_page_t;
extern books_page_t *current;
extern int pending_page;
const lv_font_t *ui_font_body(void);
const lv_font_t *ui_font_small(void);
const lv_font_t *ui_font_title(void);
const lv_font_t *ui_font_caption(void);
lv_obj_t *label_create(lv_obj_t *,const char *,int,int,int,const lv_font_t *);
lv_obj_t *centered_label(lv_obj_t *,const char *,int,int,int,const lv_font_t *);
lv_obj_t *panel_create(lv_obj_t *,int,int,int,int);
lv_obj_t *icon_create(lv_obj_t *,const lv_image_dsc_t *,int,int);
lv_obj_t *button_create(lv_obj_t *,int,int,int,int,const char *,int);
bool text_update(lv_obj_t *,const char *);
bool storage_available(storage_volume_t);
const ui_reader_view_t *ui_reader_view(void);
bool ui_reader_waiting(void);
const char *ui_reader_identity(void);
void ui_reader_turn(int);
void ui_reader_seek(unsigned);
void ui_reader_background_pause(bool);
bool ui_reader_settings_changed(void);
bool ui_reader_reflow(void);
bool document_view_active(void);
bool document_view_toc_seek(unsigned);
unsigned document_view_toc_count(void);
bool document_view_toc_title(unsigned,char *,size_t,unsigned *);
bool epd_reader_page_text(char *,size_t);
bool epd_reader_chapter_move(int);
bool epd_reader_can_seek_progress(void);
bool epd_reader_seek_progress(unsigned);
bool epd_reader_chapter_title(char *,size_t,unsigned *);
#include "reader_controls.h"
