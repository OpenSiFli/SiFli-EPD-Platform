#ifndef WORDS_STORE_H
#define WORDS_STORE_H
#include "words_service.h"

typedef struct
{
    char scope_id[32], scope_name[WORDS_KEY_SIZE];
    uint8_t identity[24];
    uint32_t cursor, pending, quota;
} words_config_t;

bool words_store_open(void);
void words_store_close(void);
bool words_store_flush(void);
bool words_store_dirty(void);
words_message_t words_store_error(void);
words_config_t *words_store_config(void);
void words_store_config_changed(void);
uint32_t words_store_count(void);
const words_index_t *words_store_index(void);
int words_store_find(const char *word);
bool words_store_get(uint32_t slot, words_record_t *record);
bool words_store_put(uint32_t slot, const words_record_t *record);
int words_store_ensure(const words_result_t *entry);
bool words_store_content(uint32_t slot, words_result_t *result);
bool words_store_counts(int32_t day, int64_t now, words_counts_t *counts);
#endif
