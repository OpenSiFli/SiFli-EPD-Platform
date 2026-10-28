#ifndef WORDS_DICTIONARY_H
#define WORDS_DICTIONARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WORDS_KEY_SIZE 96u
#define WORDS_MATCH_MAX 8u
#define WORDS_FIELD_COUNT 6u
enum { WORDS_WORD, WORDS_PHONETIC, WORDS_TRANSLATION, WORDS_DEFINITION, WORDS_EXCHANGE, WORDS_TAGS };
typedef bool (*words_read_fn)(void *context, uint32_t offset, void *buffer, size_t size);
typedef struct
{
    words_read_fn read;
    void *context;
    uint32_t count, aliases, index, alias_index, data, size;
    uint32_t version, scopes, scope_index, members, data_end;
    unsigned char identity[24];
} words_dictionary_t;
typedef struct
{
    uint32_t count, entry[WORDS_MATCH_MAX];
    bool more;
} words_matches_t;

typedef struct { char id[32], name[64]; uint32_t first, count; } words_scope_t;
typedef struct
{
    const char *word, *legacy[WORDS_FIELD_COUNT];
    const unsigned char *records;
    uint32_t size, count, version;
} words_entry_t;
typedef struct { uint32_t type; const char *text[4]; } words_item_t;
enum { WORDS_ITEM_PHONETIC = 1, WORDS_ITEM_SENSE, WORDS_ITEM_FORM, WORDS_ITEM_EXAMPLE,
       WORDS_ITEM_PHRASE, WORDS_ITEM_SOURCE, WORDS_ITEM_SCOPE, WORDS_ITEM_RELATED };
enum { WORDS_TEXT_PHONETIC, WORDS_TEXT_BRIEF, WORDS_TEXT_DETAIL };

typedef struct
{
    const char *titles[9];
    const char *tags, *uk, *us;
} words_text_labels_t;

/* An immutable dictionary generation is required for the lifetime of this handle. */
bool words_dictionary_open(words_dictionary_t *dictionary, words_read_fn read,
                           void *context, uint32_t file_size);
bool words_dictionary_search(words_dictionary_t *dictionary, const char *query, words_matches_t *matches);
bool words_dictionary_entry(words_dictionary_t *dictionary, uint32_t entry, char *buffer,
                            size_t capacity, const char *fields[WORDS_FIELD_COUNT]);
bool words_dictionary_entry_size(words_dictionary_t *dictionary, uint32_t entry, uint32_t *size);
bool words_dictionary_read_entry(words_dictionary_t *dictionary, uint32_t entry, void *buffer,
                                 size_t capacity, words_entry_t *view);
bool words_entry_parse(const void *buffer, size_t size, words_entry_t *view);
bool words_entry_item(const words_entry_t *entry, uint32_t *offset, words_item_t *item);
/* Returns the required bytes including NUL, or zero on overflow/invalid mode. */
size_t words_entry_text(const words_entry_t *entry, unsigned mode, char *buffer, size_t capacity);
/* Labels affect presentation only; entry payload and identity are unchanged. */
size_t words_entry_text_labels(const words_entry_t *entry, unsigned mode, char *buffer,
                              size_t capacity, const words_text_labels_t *labels);
bool words_dictionary_word(words_dictionary_t *dictionary, uint32_t entry, char word[WORDS_KEY_SIZE]);
/* Exact original headword only; no alias or case-fold identity substitution. */
bool words_dictionary_find(words_dictionary_t *dictionary, const char *word, uint32_t *entry, bool *found);
bool words_dictionary_scope(words_dictionary_t *dictionary, uint32_t index, words_scope_t *scope);
bool words_dictionary_member(words_dictionary_t *dictionary, const words_scope_t *scope,
                             uint32_t position, uint32_t *entry);
bool words_dictionary_unchanged(words_dictionary_t *dictionary);

#endif
