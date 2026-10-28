#ifndef WORDS_I18N_H
#define WORDS_I18N_H

#include "epd_memory.h"
#include "words_service.h"
#include "platform/epd_i18n.h"
#include <string.h>

/* Called only while rendering on the UI thread. */
static inline const char *words_message_text(words_message_t code)
{
    switch (code)
    {
    case WORDS_MESSAGE_NONE: return "";
    case WORDS_MESSAGE_MEMORY: return epd_get_str(words_memory);
    case WORDS_MESSAGE_INDEX_LIMIT: return epd_get_str(words_index_limit);
    case WORDS_MESSAGE_STORAGE_UNAVAILABLE: return epd_get_str(words_storage_unavailable);
    case WORDS_MESSAGE_SAVE_FAILED: return epd_get_str(words_save_failed);
    case WORDS_MESSAGE_SETTINGS_SAVE_FAILED: return epd_get_str(words_settings_save_failed);
    case WORDS_MESSAGE_RECORDS_READ_FAILED: return epd_get_str(words_records_read_failed);
    case WORDS_MESSAGE_SETTINGS_READ_FAILED: return epd_get_str(words_settings_read_failed);
    case WORDS_MESSAGE_SETTINGS_CORRUPT: return epd_get_str(words_settings_corrupt);
    case WORDS_MESSAGE_RECORDS_CORRUPT: return epd_get_str(words_records_corrupt);
    case WORDS_MESSAGE_ENTRY_SAVE_FAILED: return epd_get_str(words_entry_save_failed);
    case WORDS_MESSAGE_SAVED_ENTRY_READ_FAILED: return epd_get_str(words_saved_entry_read_failed);
    case WORDS_MESSAGE_SAVED_ENTRY_CORRUPT: return epd_get_str(words_saved_entry_corrupt);
    case WORDS_MESSAGE_DICTIONARY_INVALID: return epd_get_str(words_dictionary_invalid);
    case WORDS_MESSAGE_DICTIONARY_READ_CHANGED: return epd_get_str(words_dictionary_read_changed);
    case WORDS_MESSAGE_DICTIONARY_MISSING: return epd_get_str(words_dictionary_missing);
    case WORDS_MESSAGE_DICTIONARY_CHANGED_SEARCH: return epd_get_str(words_dictionary_changed_search);
    case WORDS_MESSAGE_CLOCK_REQUIRED: return epd_get_str(words_clock_required);
    case WORDS_MESSAGE_SCOPE_UNAVAILABLE: return epd_get_str(words_scope_unavailable);
    case WORDS_MESSAGE_DICTIONARY_READ_FAILED: return epd_get_str(words_dictionary_read_failed);
    case WORDS_MESSAGE_DICTIONARY_CHANGED_SCOPE: return epd_get_str(words_dictionary_changed_scope);
    case WORDS_MESSAGE_ALREADY_RATED: return epd_get_str(words_already_rated);
    case WORDS_MESSAGE_CLOCK_OR_PAUSED: return epd_get_str(words_clock_or_paused);
    case WORDS_MESSAGE_BUSY: return epd_get_str(words_busy);
    case WORDS_MESSAGE_NO_CARDS: return epd_get_str(words_no_cards);
    case WORDS_MESSAGE_EMPTY: return epd_get_str(words_empty);
    case WORDS_MESSAGE_START_FAILED: return epd_get_str(words_start_failed);
    case WORDS_MESSAGE_SCOPE_CHANGED: return epd_get_str(words_scope_changed);
    case WORDS_MESSAGE_LOADING_SCOPE: return epd_get_str(words_loading_scope);
    case WORDS_MESSAGE_SAVE_PENDING: return epd_get_str(words_save_pending);
    case WORDS_MESSAGE_NOT_FOUND: return epd_get_str(words_not_found);
    case WORDS_MESSAGE_MORE_LETTERS: return epd_get_str(words_more_letters);
    case WORDS_MESSAGE_SEARCHING: return epd_get_str(words_searching);
    case WORDS_MESSAGE_PAGE_LIMIT: return epd_get_str(words_page_limit);
    case WORDS_MESSAGE_ENTRY_INVALID: return epd_get_str(words_entry_invalid);
    case WORDS_MESSAGE_LOADING: return epd_get_str(words_loading);
    default: return epd_get_str(words_dictionary_read_failed);
    }
}

static inline const char *words_scope_text(const char *id, const char *name)
{
    if (!id[0]) return epd_get_str(words_scope_none);
    if (!strcmp(id, "legacy")) return epd_get_str(words_scope_legacy);
    if (!strcmp(id, "cet4")) return epd_get_str(words_scope_cet4);
    if (!strcmp(id, "cet6")) return epd_get_str(words_scope_cet6);
    if (!strcmp(id, "ielts")) return epd_get_str(words_scope_ielts);
    if (!strcmp(id, "toefl")) return epd_get_str(words_scope_toefl);
    if (!strcmp(id, "kaoyan")) return epd_get_str(words_scope_kaoyan);
    if (!strcmp(id, "gre")) return epd_get_str(words_scope_gre);
    if (!strcmp(id, "gaokao")) return epd_get_str(words_scope_gaokao);
    if (!strcmp(id, "zhongkao")) return epd_get_str(words_scope_zhongkao);
    if (!strcmp(id, "tem4")) return epd_get_str(words_scope_tem4);
    if (!strcmp(id, "tem8")) return epd_get_str(words_scope_tem8);
    return name;
}

static inline size_t words_ui_entry_text(const words_entry_t *entry, unsigned mode,
                                         char *buffer, size_t capacity)
{
    const words_text_labels_t labels = {
        {"", epd_get_str(words_phonetics), epd_get_str(words_senses), epd_get_str(words_forms),
         epd_get_str(words_examples), epd_get_str(words_phrases), epd_get_str(words_sources),
         epd_get_str(words_scopes), epd_get_str(words_related)},
        epd_get_str(words_tags), epd_get_str(words_uk), epd_get_str(words_us)
    };
    return words_entry_text_labels(entry, mode, buffer, capacity, &labels);
}

static inline bool words_ui_phonetic(lv_obj_t *label, const words_entry_t *entry)
{
    if (!entry) return epd_app_text(label, "");
    size_t size = words_ui_entry_text(entry, WORDS_TEXT_PHONETIC, NULL, 0);
    if (!size) return false;
    char *text = epd_app_alloc(size, EPD_APP_PSRAM);
    if (!text) return false;
    words_ui_entry_text(entry, WORDS_TEXT_PHONETIC, text, size);
    bool updated = epd_app_text(label, text);
    epd_app_free(text);
    return updated;
}

#endif
