#ifndef BOOKS_READER_AI_H
#define BOOKS_READER_AI_H
#include "app_service.h"
#include "lvgl.h"
#include "platform/epd_ai.h"
#include "platform/epd_speech.h"
#define BOOK_AI_INPUT_CHARS 2048u
#define BOOK_AI_DRAFT (BOOK_AI_INPUT_CHARS * 4u + 1u)
#define BOOK_AI_CONTEXT 18432u
#define BOOK_AI_TURNS 16u
typedef enum { BOOK_AI_IDLE, BOOK_AI_WAITING, BOOK_AI_STREAMING, BOOK_AI_DONE,
               BOOK_AI_STOPPED, BOOK_AI_FAILED } book_ai_status_t;
typedef struct {
    uint32_t revision, generation;
    unsigned count, page;
    bool busy, auto_read, authorized;
    book_ai_status_t status;
    epd_ai_error_t error;
    epd_speech_error_t speech_error;
    int http;
    char model[EPD_AI_MODEL_SIZE], stage[32];
} book_ai_view_t;
bool books_ai_init(epd_service_t *owner);
bool books_ai_bind(const char *identity, const char *title, unsigned page, const char *text);
bool books_ai_send(const char *question);
bool books_ai_retry(void);
void books_ai_cancel(void);
void books_ai_auto_read(bool enabled);
void books_ai_view(book_ai_view_t *out);
bool books_ai_message(unsigned index, bool assistant, char *text, size_t size);
void books_ai_draft(const char *text);
void books_ai_get_draft(char *text, size_t size);
bool books_ai_select_service(void);
void books_ai_refresh_profiles(void);
bool books_ai_shutdown(void);
void books_ai_ui_open(lv_obj_t *parent);
void books_ai_ui_process(void);
void books_ai_ui_close(void);
bool books_ai_ui_back(void);
bool books_ai_ui_action(int target);
#endif
