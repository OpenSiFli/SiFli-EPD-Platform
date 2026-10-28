#ifndef NOTES_COMPOSER_H
#define NOTES_COMPOSER_H
#include "notes_model.h"
#include "lvgl.h"

typedef struct notes_composer notes_composer_t;
typedef enum { COMPOSE_FOCUS, COMPOSE_EDIT, COMPOSE_CHECK, COMPOSE_ATTACHMENT, COMPOSE_INSERT } notes_compose_event_t;
typedef bool (*notes_compose_cb)(notes_compose_event_t event, uint32_t block, const char *text, void *user);
typedef struct { uint32_t block, cursor, start, end; int32_t scroll; } notes_caret_t;
notes_composer_t *notes_composer_create(lv_obj_t *parent, note_t *note, bool editable,
                                      int width, int height, notes_compose_cb callback, void *user);
lv_obj_t *notes_composer_input(notes_composer_t *editor);
void notes_composer_resize(notes_composer_t *editor, int height);
void notes_composer_refresh(notes_composer_t *editor);
void notes_composer_poll(notes_composer_t *editor);
void notes_composer_caret(notes_composer_t *editor, notes_caret_t *state, bool restore);
bool notes_composer_selection(notes_composer_t *editor, uint32_t *id, uint32_t *start, uint32_t *end);
void notes_composer_scroll(notes_composer_t *editor, int direction);
void notes_composer_hide_completed(notes_composer_t *editor, bool hide);
#endif
