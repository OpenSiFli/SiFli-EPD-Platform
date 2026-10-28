#include "epd_memory.h"
#include "words_service.h"
#include <string.h>

void words_result_clear(words_result_t *r)
{
    if (!r) return;
    epd_app_free(r->content); epd_app_free(r->display_text);
    r->content = r->display_text = NULL; r->content_size = 0;
    memset(&r->view, 0, sizeof(r->view));
    for (unsigned i = 0; i < WORDS_FIELD_COUNT; ++i) r->fields[i] = NULL;
}
void words_result_free(words_result_t *r) { if (r) { words_result_clear(r); epd_app_free(r); } }

bool words_result_reserve(words_result_t *r, uint32_t size)
{
    if (!r || !size || size > INT32_MAX) return false;
    words_result_clear(r);
    r->content = epd_app_alloc(size, EPD_APP_PSRAM);
    if (!r->content) { r->error = WORDS_MESSAGE_MEMORY; return false; }
    r->content_size = size; return true;
}

bool words_result_parse(words_result_t *r)
{
    epd_app_free(r->display_text); r->display_text = NULL;
    memset(&r->view, 0, sizeof(r->view));
    for (unsigned i = 0; i < WORDS_FIELD_COUNT; ++i) r->fields[i] = NULL;
    if (!words_entry_parse(r->content, r->content_size, &r->view)) return false;
    size_t phonetic = words_entry_text(&r->view, WORDS_TEXT_PHONETIC, NULL, 0);
    size_t brief = words_entry_text(&r->view, WORDS_TEXT_BRIEF, NULL, 0);
    if (!phonetic || !brief || brief > SIZE_MAX - phonetic) return false;
    r->display_text = epd_app_alloc(phonetic + brief, EPD_APP_PSRAM);
    if (!r->display_text) { r->error = WORDS_MESSAGE_MEMORY; return false; }
    words_entry_text(&r->view, WORDS_TEXT_PHONETIC, r->display_text, phonetic);
    words_entry_text(&r->view, WORDS_TEXT_BRIEF, r->display_text + phonetic, brief);
    for (unsigned i = 0; i < WORDS_FIELD_COUNT; ++i) r->fields[i] = "";
    r->fields[WORDS_WORD] = r->view.word;
    r->fields[WORDS_PHONETIC] = r->display_text;
    r->fields[WORDS_TRANSLATION] = r->display_text + phonetic;
    return true;
}
