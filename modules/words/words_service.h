#ifndef WORDS_SERVICE_H
#define WORDS_SERVICE_H

#include "storage.h"
#include "platform/epd_app.h"
#include "words_dictionary.h"
#include "words_learning.h"

typedef enum
{
    WORDS_MESSAGE_NONE,
    WORDS_MESSAGE_MEMORY,
    WORDS_MESSAGE_INDEX_LIMIT,
    WORDS_MESSAGE_STORAGE_UNAVAILABLE,
    WORDS_MESSAGE_SAVE_FAILED,
    WORDS_MESSAGE_SETTINGS_SAVE_FAILED,
    WORDS_MESSAGE_RECORDS_READ_FAILED,
    WORDS_MESSAGE_SETTINGS_READ_FAILED,
    WORDS_MESSAGE_SETTINGS_CORRUPT,
    WORDS_MESSAGE_RECORDS_CORRUPT,
    WORDS_MESSAGE_ENTRY_SAVE_FAILED,
    WORDS_MESSAGE_SAVED_ENTRY_READ_FAILED,
    WORDS_MESSAGE_SAVED_ENTRY_CORRUPT,
    WORDS_MESSAGE_DICTIONARY_INVALID,
    WORDS_MESSAGE_DICTIONARY_READ_CHANGED,
    WORDS_MESSAGE_DICTIONARY_MISSING,
    WORDS_MESSAGE_DICTIONARY_CHANGED_SEARCH,
    WORDS_MESSAGE_CLOCK_REQUIRED,
    WORDS_MESSAGE_SCOPE_UNAVAILABLE,
    WORDS_MESSAGE_DICTIONARY_READ_FAILED,
    WORDS_MESSAGE_DICTIONARY_CHANGED_SCOPE,
    WORDS_MESSAGE_ALREADY_RATED,
    WORDS_MESSAGE_CLOCK_OR_PAUSED,
    WORDS_MESSAGE_BUSY,
    WORDS_MESSAGE_NO_CARDS,
    WORDS_MESSAGE_EMPTY,
    WORDS_MESSAGE_START_FAILED,
    WORDS_MESSAGE_SCOPE_CHANGED,
    WORDS_MESSAGE_LOADING_SCOPE,
    WORDS_MESSAGE_SAVE_PENDING,
    WORDS_MESSAGE_NOT_FOUND,
    WORDS_MESSAGE_MORE_LETTERS,
    WORDS_MESSAGE_SEARCHING,
    WORDS_MESSAGE_PAGE_LIMIT,
    WORDS_MESSAGE_ENTRY_INVALID,
    WORDS_MESSAGE_LOADING,
} words_message_t;

typedef enum { WORDS_SEARCH, WORDS_DETAIL, WORDS_HOME, WORDS_NEXT, WORDS_RATE,
               WORDS_COLLECTION, WORDS_COLLECT, WORDS_PAUSE, WORDS_QUOTA,
               WORDS_SCOPES, WORDS_SCOPE_SELECT } words_job_kind_t;
typedef struct
{
    uint32_t serial, entry, slot, token, value;
    words_job_kind_t kind;
    char query[WORDS_KEY_SIZE], path[STORAGE_PATH_MAX];
    unsigned char identity[24];
} words_job_t;

typedef struct
{
    uint32_t serial;
    uint32_t slot, token, flags, quota, offset;
    words_counts_t counts;
    bool clock_valid, accepted, scope_missing;
    char scope_id[32], scope_name[WORDS_KEY_SIZE];
    char scope_ids[WORDS_MATCH_MAX][32];
    words_matches_t matches;
    char names[WORDS_MATCH_MAX][WORDS_KEY_SIZE];
    char path[STORAGE_PATH_MAX];
    words_message_t error;
    unsigned char identity[24];
    char *content, *display_text;
    uint32_t content_size;
    words_entry_t view;
    const char *fields[WORDS_FIELD_COUNT];
} words_result_t;

bool words_result_reserve(words_result_t *result, uint32_t size);
bool words_result_parse(words_result_t *result);
void words_result_clear(words_result_t *result);
void words_result_free(words_result_t *result);

bool words_service_start(void);
bool words_service_flush(void);
void words_service_stop(void);
uint32_t words_submit(const words_job_t *job);
void words_cancel(uint32_t serial);
words_result_t *words_take(uint32_t serial, bool *failed);

#endif
