#ifndef BOOKS_READER_CONTROLS_H
#define BOOKS_READER_CONTROLS_H
#include "books_ui.h"
#include "app_service.h"
#include "reader_icons.h"
#define BR(name) epd_get_str(books_reader_##name)
extern epd_service_t *books_service;
#define BOOKS_MENU_BACK (-500)
#define BOOKS_MENU_AI (-503)
void books_menu_open(void);
void books_menu_close(void);
void books_menu_process(void);
bool books_menu_active(void);
bool books_menu_font_pending(void);
void books_menu_navigate(void);
bool books_menu_action(int target);
bool books_reader_touch_enabled(void);
lv_obj_t *books_icon_button(lv_obj_t *parent, int x, int y, int width, int height,
                            book_icon_t icon, int target);
void books_menu_pause(void);
#endif
