#include "words_dictionary.h"
#include <string.h>

#define HEADER_SIZE 64u
#define ROW_SIZE (WORDS_KEY_SIZE + 8u)
#define ALIAS_SIZE (WORDS_KEY_SIZE + 4u)
static uint32_t u32(const unsigned char *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool words_dictionary_open(words_dictionary_t *d, words_read_fn read, void *context, uint32_t size)
{
    unsigned char header[96] = {0};
    if (!d || !read || size < HEADER_SIZE || size > INT32_MAX ||
        !read(context, 0, header, HEADER_SIZE) ||
        u32(header + 36) != WORDS_KEY_SIZE || u32(header + 32) != size)
        return false;
    uint32_t version = u32(header + 8), header_size = version == 2 ? 96 : HEADER_SIZE;
    if ((version != 1 && version != 2) ||
        memcmp(header, version == 2 ? "EPDWD02" : "EPDWD01", 8) || size < header_size ||
        (version == 2 && !read(context, HEADER_SIZE, header + HEADER_SIZE, 32))) return false;
    words_dictionary_t value = {.read = read, .context = context, .count = u32(header + 12),
        .aliases = u32(header + 16), .index = u32(header + 20), .alias_index = u32(header + 24),
        .data = u32(header + 28), .size = size, .version = version, .data_end = size, .scopes = 1};
    memcpy(value.identity, header + 40, sizeof(value.identity));
    if (value.index != header_size || !value.count ||
        value.count > (size - header_size) / ROW_SIZE ||
        value.alias_index != header_size + value.count * ROW_SIZE ||
        value.aliases > (size - value.alias_index) / ALIAS_SIZE ||
        value.data >= size)
        return false;
    uint32_t alias_end = value.alias_index + value.aliases * ALIAS_SIZE;
    if (version == 1) { if (value.data != alias_end) return false; }
    else
    {
        value.scopes = u32(header + 64); value.scope_index = u32(header + 68);
        value.members = u32(header + 72); value.data_end = u32(header + 76);
        if (value.scope_index != alias_end || value.scopes > (size - alias_end) / 104u ||
            value.members != alias_end + value.scopes * 104u || value.data < value.members ||
            (value.data - value.members) % 4u || value.data_end <= value.data || value.data_end > size ||
            u32(header + 80) != size - value.data_end || u32(header + 84) || u32(header + 88) || u32(header + 92))
            return false;
    }
    *d = value;
    return true;
}

static bool row(words_dictionary_t *d, bool alias, uint32_t n, unsigned char *buffer)
{
    uint32_t count = alias ? d->aliases : d->count;
    uint32_t stride = alias ? ALIAS_SIZE : ROW_SIZE;
    uint32_t offset = alias ? d->alias_index : d->index;
    return n < count && d->read(d->context, offset + n * stride, buffer, stride) &&
           buffer[0] && memchr(buffer, 0, WORDS_KEY_SIZE) != NULL;
}

static bool lower_bound(words_dictionary_t *d, const char *key, bool alias, uint32_t *position)
{
    uint32_t first = 0, last = alias ? d->aliases : d->count;
    unsigned char data[ROW_SIZE];
    while (first < last)
    {
        uint32_t middle = first + (last - first) / 2;
        if (!row(d, alias, middle, data)) return false;
        if (strcmp((char *)data, key) < 0) first = middle + 1;
        else last = middle;
    }
    *position = first;
    return true;
}

static bool collect(words_dictionary_t *d, const char *key, bool alias, bool prefix,
                    words_matches_t *matches)
{
    uint32_t pos, count = alias ? d->aliases : d->count;
    unsigned char data[ROW_SIZE];
    if (!lower_bound(d, key, alias, &pos)) return false;
    for (; pos < count; ++pos)
    {
        if (!row(d, alias, pos, data)) return false;
        int different = prefix ? strncmp((char *)data, key, strlen(key)) : strcmp((char *)data, key);
        if (different) break;
        uint32_t entry = alias ? u32(data + WORDS_KEY_SIZE) : pos;
        if (entry >= d->count) return false;
        bool duplicate = false;
        for (unsigned i = 0; i < matches->count; ++i) duplicate |= matches->entry[i] == entry;
        if (duplicate) continue;
        if (matches->count == WORDS_MATCH_MAX) { matches->more = true; break; }
        matches->entry[matches->count++] = entry;
    }
    return true;
}

bool words_dictionary_search(words_dictionary_t *d, const char *query, words_matches_t *matches)
{
    char key[WORDS_KEY_SIZE];
    if (!d || !query || !matches) return false;
    size_t length = strlen(query);
    if (!length || length >= sizeof(key)) return false;
    for (size_t i = 0; i <= length; ++i)
        key[i] = query[i] >= 'A' && query[i] <= 'Z' ? query[i] + ('a' - 'A') : query[i];
    memset(matches, 0, sizeof(*matches));
    if (!collect(d, key, false, false, matches)) return false;
    if (!matches->count && !collect(d, key, true, false, matches)) return false;
    if (!matches->count && !collect(d, key, false, true, matches)) return false;
    return true;
}

static bool entry_location(words_dictionary_t *d, uint32_t entry, uint32_t *offset, uint32_t *size,
                           char key[WORDS_KEY_SIZE])
{
    unsigned char data[ROW_SIZE];
    if (!d || !row(d, false, entry, data)) return false;
    if (key) memcpy(key, data, WORDS_KEY_SIZE);
    *offset = u32(data + WORDS_KEY_SIZE); *size = u32(data + WORDS_KEY_SIZE + 4);
    return *offset >= d->data && *offset < d->data_end && *size >= WORDS_FIELD_COUNT &&
           *size <= d->data_end - *offset;
}

bool words_dictionary_entry_size(words_dictionary_t *d, uint32_t entry, uint32_t *size)
{ uint32_t offset; return size && entry_location(d, entry, &offset, size, NULL); }

static bool word_key_equal(const char *word, const char *key)
{
    for (;; ++word, ++key)
    {
        char folded = *word >= 'A' && *word <= 'Z' ? *word + ('a' - 'A') : *word;
        if (folded != *key) return false;
        if (!*word) return true;
    }
}

bool words_entry_item(const words_entry_t *e, uint32_t *offset, words_item_t *item)
{
    if (!e || e->version != 2 || !offset || !item || *offset > e->size || e->size - *offset < 8) return false;
    const unsigned char *p = e->records + *offset;
    uint32_t type = u32(p), size = u32(p + 4);
    if (type < 1 || type > 8 || size > e->size - *offset - 8) return false;
    unsigned fields = type == WORDS_ITEM_SOURCE ? 2 : type == WORDS_ITEM_SENSE || type == WORDS_ITEM_RELATED ? 4 : 3;
    const char *start = (const char *)p + 8, *end = start + size;
    memset(item, 0, sizeof(*item)); item->type = type;
    for (unsigned i = 0; i < fields; ++i)
    {
        const char *zero = start < end ? memchr(start, 0, (size_t)(end - start)) : NULL;
        if (!zero) return false;
        item->text[i] = start; start = zero + 1;
    }
    if (start != end) return false;
    *offset += 8 + size;
    return true;
}

bool words_entry_parse(const void *buffer, size_t size, words_entry_t *out)
{
    if (!buffer || !out || size < WORDS_FIELD_COUNT || size > UINT32_MAX) return false;
    words_entry_t e = {0};
    const char *start = buffer, *end = start + size;
    if (size >= 9 && !memcmp(start, "\0EN2", 4))
    {
        e.version = 2; e.count = u32((const unsigned char *)start + 4); e.word = start + 8;
        const char *zero = memchr(e.word, 0, (size_t)(end - e.word));
        if (!zero || !e.word[0] || (size_t)(zero - e.word) >= WORDS_KEY_SIZE) return false;
        e.records = (const unsigned char *)(zero + 1); e.size = (uint32_t)(end - zero - 1);
        if (e.count > e.size / 10u) return false;
        uint32_t offset = 0; words_item_t item;
        for (uint32_t i = 0; i < e.count; ++i) if (!words_entry_item(&e, &offset, &item)) return false;
        if (offset != e.size) return false;
        *out = e; return true;
    }
    e.version = 1;
    for (unsigned i = 0; i < WORDS_FIELD_COUNT; ++i)
    {
        if (start >= end) return false;
        e.legacy[i] = start;
        const char *zero = memchr(start, 0, (size_t)(end - start));
        if (!zero) return false;
        start = zero + 1;
    }
    e.word = e.legacy[WORDS_WORD];
    if (start != end || !e.word[0] || strlen(e.word) >= WORDS_KEY_SIZE) return false;
    *out = e; return true;
}

bool words_dictionary_read_entry(words_dictionary_t *d, uint32_t entry, void *buffer,
                                 size_t capacity, words_entry_t *view)
{
    uint32_t offset, size;
    char key[WORDS_KEY_SIZE];
    return buffer && view && entry_location(d, entry, &offset, &size, key) && size <= capacity &&
        d->read(d->context, offset, buffer, size) && words_entry_parse(buffer, size, view) &&
        view->version == d->version && word_key_equal(view->word, key);
}

bool words_dictionary_entry(words_dictionary_t *d, uint32_t entry, char *buffer,
                            size_t capacity, const char *fields[WORDS_FIELD_COUNT])
{
    words_entry_t view;
    if (!fields || !words_dictionary_read_entry(d, entry, buffer, capacity, &view) || view.version != 1) return false;
    memcpy(fields, view.legacy, sizeof(view.legacy)); return true;
}

bool words_dictionary_word(words_dictionary_t *d, uint32_t entry, char word[WORDS_KEY_SIZE])
{
    uint32_t offset, size;
    char key[WORDS_KEY_SIZE]; unsigned char prefix[8 + WORDS_KEY_SIZE];
    if (!word || !entry_location(d, entry, &offset, &size, key)) return false;
    uint32_t skip = d->version == 2 ? 8 : 0;
    if (size <= skip) return false;
    if (size > skip + WORDS_KEY_SIZE) size = skip + WORDS_KEY_SIZE;
    if (!d->read(d->context, offset, prefix, size) ||
        (skip && memcmp(prefix, "\0EN2", 4))) return false;
    size -= skip;
    memcpy(word, prefix + skip, size);
    return word[0] && memchr(word, 0, size) && word_key_equal(word, key);
}

bool words_dictionary_find(words_dictionary_t *d, const char *word, uint32_t *entry, bool *found)
{
    char key[WORDS_KEY_SIZE], original[WORDS_KEY_SIZE]; unsigned char data[ROW_SIZE]; uint32_t pos;
    if (!d || !word || !entry || !found || !word[0] || strlen(word) >= sizeof(key)) return false;
    for (size_t i = 0; i <= strlen(word); ++i) key[i] = word[i] >= 'A' && word[i] <= 'Z' ? word[i] + 32 : word[i];
    *found = false;
    if (!lower_bound(d, key, false, &pos)) return false;
    for (; pos < d->count; ++pos)
    {
        if (!row(d, false, pos, data)) return false;
        if (strcmp((char *)data, key)) break;
        if (!words_dictionary_word(d, pos, original)) return false;
        if (!strcmp(original, word)) { *found = true; *entry = pos; break; }
    }
    return true;
}

bool words_dictionary_scope(words_dictionary_t *d, uint32_t index, words_scope_t *scope)
{
    if (!d || !scope || index >= d->scopes) return false;
    if (d->version == 1)
    {
        memset(scope, 0, sizeof(*scope)); strcpy(scope->id, "legacy"); strcpy(scope->name, "全部词汇");
        scope->count = d->count; return true;
    }
    unsigned char data[104];
    if (!d->read(d->context, d->scope_index + index * 104u, data, sizeof(data)) ||
        !data[0] || !data[32] || !memchr(data, 0, 32) || !memchr(data + 32, 0, 64)) return false;
    memcpy(scope->id, data, 32); memcpy(scope->name, data + 32, 64);
    scope->first = u32(data + 96); scope->count = u32(data + 100);
    uint32_t total = (d->data - d->members) / 4u;
    return scope->first <= total && scope->count <= total - scope->first;
}

bool words_dictionary_member(words_dictionary_t *d, const words_scope_t *scope, uint32_t position, uint32_t *entry)
{
    if (!d || !scope || !entry || position >= scope->count) return false;
    if (d->version == 1) { *entry = position; return position < d->count; }
    uint32_t total = (d->data - d->members) / 4u; unsigned char data[4];
    if (scope->first > total || scope->count > total - scope->first ||
        !d->read(d->context, d->members + (scope->first + position) * 4u, data, 4)) return false;
    *entry = u32(data); return *entry < d->count;
}

bool words_dictionary_unchanged(words_dictionary_t *d)
{
    words_dictionary_t current;
    return d && words_dictionary_open(&current, d->read, d->context, d->size) &&
        current.version == d->version && current.count == d->count && current.aliases == d->aliases &&
        current.index == d->index && current.alias_index == d->alias_index && current.data == d->data &&
        current.scopes == d->scopes && current.scope_index == d->scope_index && current.members == d->members &&
        current.data_end == d->data_end && !memcmp(current.identity, d->identity, sizeof(d->identity));
}
