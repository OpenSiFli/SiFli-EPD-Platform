#include "epd_pinyin.h"
#include <stdint.h>
#include <string.h>

/* Compact, immutable tables; all mutable state belongs to the caller. */
#include "epd_pinyin_data.inc"

static size_t composition_length(const epd_pinyin_t *state)
{
    size_t length = 0;
    while (length <= EPD_PINYIN_MAX && state->text[length]) ++length;
    return length;
}

void epd_pinyin_reset(epd_pinyin_t *state)
{
    if (state) memset(state, 0, sizeof(*state));
}

bool epd_pinyin_insert(epd_pinyin_t *state, char key)
{
    size_t length;
    if (!state) return false;
    if (key >= 'A' && key <= 'Z') key = (char)(key - 'A' + 'a');
    if ((key < 'a' || key > 'z') && key != '\'') return false;
    length = composition_length(state);
    if (length >= EPD_PINYIN_MAX) return false;
    if (state->cursor > length) state->cursor = length;
    memmove(state->text + state->cursor + 1, state->text + state->cursor,
            length - state->cursor + 1);
    state->text[state->cursor++] = key;
    return true;
}

bool epd_pinyin_backspace(epd_pinyin_t *state)
{
    size_t length;
    if (!state) return false;
    length = composition_length(state);
    if (length > EPD_PINYIN_MAX) return false;
    if (state->cursor > length) state->cursor = length;
    if (!state->cursor) return false;
    memmove(state->text + state->cursor - 1, state->text + state->cursor,
            length - state->cursor + 1);
    --state->cursor;
    return true;
}

void epd_pinyin_move(epd_pinyin_t *state, int delta)
{
    size_t length, distance;
    if (!state) return;
    length = composition_length(state);
    if (length > EPD_PINYIN_MAX) return;
    if (state->cursor > length) state->cursor = length;
    if (delta < 0)
    {
        /* Avoid signed overflow for INT_MIN. */
        distance = (size_t)(-(delta + 1)) + 1;
        state->cursor = distance > state->cursor ? 0 : state->cursor - distance;
    }
    else
    {
        distance = (size_t)delta;
        state->cursor = distance > length - state->cursor ? length : state->cursor + distance;
    }
}

void epd_pinyin_consume(epd_pinyin_t *state, size_t count)
{
    size_t length;
    if (!state) return;
    length = composition_length(state);
    if (length > EPD_PINYIN_MAX) return;
    if (count > length) count = length;
    memmove(state->text, state->text + count, length - count + 1);
    state->cursor = state->cursor > count ? state->cursor - count : 0;
    if (state->cursor > length - count) state->cursor = length - count;
}

static const epd_pinyin_syllable_t *find_syllable(const char *key)
{
    size_t lo = 0, hi = EPD_PINYIN_SYLLABLE_COUNT;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        int order = strcmp(key, epd_pinyin_syllables[mid].key);
        if (order == 0) return &epd_pinyin_syllables[mid];
        if (order < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

static size_t phrase_lower_bound(const char *key)
{
    size_t lo = 0, hi = EPD_PINYIN_PHRASE_COUNT;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(epd_pinyin_phrase_keys + epd_pinyin_phrases[mid].key, key) < 0)
            lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Candidate strings are copied whole; an oversized dictionary entry is skipped. */
static bool emit_candidate(epd_pinyin_candidate_t *items, size_t capacity,
                           size_t *written, size_t *skip, const char *text,
                           size_t bytes, size_t consumed, uint32_t boundaries)
{
    if (bytes >= EPD_PINYIN_CANDIDATE_BYTES) return false;
    if (*skip)
    {
        --*skip;
        return false;
    }
    memcpy(items[*written].text, text, bytes);
    items[*written].text[bytes] = '\0';
    items[*written].consumed = consumed;
    items[*written].boundaries = boundaries;
    ++*written;
    return *written == capacity;
}

size_t epd_pinyin_candidates(const epd_pinyin_t *state, size_t offset,
                             epd_pinyin_candidate_t *items, size_t capacity)
{
    char key[EPD_PINYIN_MAX + 1];
    uint8_t ends[EPD_PINYIN_MAX + 1];
    uint32_t separators = 0;
    size_t length, letters = 0, i, prefix, written = 0;
    if (!state || !items || !capacity) return 0;
    length = composition_length(state);
    if (!length || length > EPD_PINYIN_MAX) return 0;
    /* An invalid suffix does not destroy an already convertible prefix.
     * A separator constrains syllable boundaries and is consumed with its left
     * syllable. Consecutive/leading separators remain editable invalid input. */
    for (i = 0; i < length; ++i)
    {
        char ch = state->text[i];
        if (ch == '\'')
        {
            if (!letters || (i && state->text[i - 1] == '\'')) break;
            if (letters <= EPD_PINYIN_PHRASE_KEY_MAX)
                separators |= (uint32_t)1 << letters;
            ends[letters] = (uint8_t)(i + 1);
        }
        else if (ch >= 'a' && ch <= 'z')
        {
            key[letters++] = ch;
            ends[letters] = (uint8_t)(i + 1);
        }
        else break;
    }
    prefix = letters < EPD_PINYIN_PHRASE_KEY_MAX ? letters : EPD_PINYIN_PHRASE_KEY_MAX;
    /* Longer complete prefixes precede shorter ones. Each dictionary group is
     * frequency ranked, so paging is stable and needs no cache or allocation. */
    for (; prefix; --prefix)
    {
        const epd_pinyin_syllable_t *syllable;
        uint32_t required = separators & (((uint32_t)1 << prefix) - 1);
        key[prefix] = '\0';
        if (prefix <= 6 && !required && (syllable = find_syllable(key)) != NULL)
        {
            size_t count = syllable->count;
            if (offset >= count) offset -= count;
            else
            {
                size_t candidate = offset;
                offset = 0;
                for (; candidate < count; ++candidate)
                    if (emit_candidate(items, capacity, &written, &offset,
                                       epd_pinyin_characters + syllable->offset + candidate * 3,
                                       3, ends[prefix], 0)) return written;
            }
        }
        for (i = phrase_lower_bound(key); i < EPD_PINYIN_PHRASE_COUNT; ++i)
        {
            const epd_pinyin_phrase_t *phrase = &epd_pinyin_phrases[i];
            const char *text;
            if (strcmp(key, epd_pinyin_phrase_keys + phrase->key) != 0) break;
            if (required & ~phrase->boundaries) continue;
            text = epd_pinyin_phrase_text + phrase->text;
            if (emit_candidate(items, capacity, &written, &offset, text,
                               strlen(text), ends[prefix], phrase->boundaries)) return written;
        }
    }
    return written;
}
