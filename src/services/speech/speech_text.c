/* SPDX-License-Identifier: Apache-2.0 */
#include "speech_text.h"
#include <stdint.h>
#include <string.h>

/* 0: incomplete; -1: malformed. Excludes overlong encodings and surrogates. */
static int decode(const char *text, size_t len, uint32_t *value)
{
    const unsigned char *s = (const unsigned char *)text;
    if (!len) return 0;
    unsigned n = s[0] < 0x80 ? 1 : s[0] >= 0xc2 && s[0] <= 0xdf ? 2 :
                 s[0] >= 0xe0 && s[0] <= 0xef ? 3 : s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : 0;
    if (!n || s[0] == 0) return -1;
    uint32_t c = s[0] & (n == 1 ? 0x7f : (0x7f >> n));
    for (unsigned i = 1; i < n && i < len; ++i) {
        if ((s[i] & 0xc0) != 0x80) return -1;
        if (i == 1 && ((s[0] == 0xe0 && s[i] < 0xa0) || (s[0] == 0xed && s[i] >= 0xa0) ||
            (s[0] == 0xf0 && s[i] < 0x90) || (s[0] == 0xf4 && s[i] >= 0x90))) return -1;
        c = (c << 6) | (s[i] & 0x3f);
    }
    if (len < n) return 0;
    if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) ||
        (n == 4 && c < 0x10000) || (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff) return -1;
    *value = c;
    return (int)n;
}

bool speech_text_prefix(const char *text, size_t len, size_t capacity, size_t *accepted)
{
    if (!accepted || (!text && len)) return false;
    *accepted = 0;
    for (size_t i = 0; i < len;) {
        uint32_t c;
        int n = decode(text + i, len - i, &c);
        if (n < 0) return false;
        if (!n) break;
        i += (size_t)n;
        if (i <= capacity) *accepted = i;
    }
    return true;
}

static bool closer(uint32_t c)
{
    return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}' ||
           c == 0x2019 || c == 0x201d || c == 0x3009 || c == 0x300b ||
           c == 0x300d || c == 0x300f || c == 0xff09 || c == 0x3011;
}

static bool punctuation(uint32_t c)
{ return c == '.' || c == '!' || c == '?' || c == 0x3002 || c == 0xff01 || c == 0xff1f; }

static bool abbreviation(const char *word, size_t len)
{
    static const char *const names[] = {"dr", "mr", "mrs", "ms", "prof", "sr", "jr", "st", "vs", "etc"};
    char lower[8];
    if (len < sizeof(lower)) {
        for (size_t i = 0; i < len; ++i) lower[i] = word[i] >= 'A' && word[i] <= 'Z' ? word[i] + 'a' - 'A' : word[i];
        lower[len] = 0;
        for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); ++i) if (!strcmp(lower, names[i])) return true;
    }
    /* Initials such as U.S.A. and e.g. */
    unsigned part = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = word[i];
        if (c == '.') { if (part != 1) return false; part = 0; }
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { if (++part > 1) return false; }
        else return false;
    }
    return len && part == 1;
}

static bool url_token(const char *text, size_t begin, size_t end)
{
    if (end - begin >= 4 && !memcmp(text + begin, "www.", 4)) return true;
    for (size_t i = begin; i < end; ++i) {
        if (text[i] == '@') return true;
        if (i + 2 < end && text[i] == ':' && text[i+1] == '/' && text[i+2] == '/') return true;
    }
    return false;
}

static bool whitespace(uint32_t c)
{
    return (c >= 0x09 && c <= 0x0d) || c == 0x20 || c == 0x85 || c == 0xa0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
           c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

speech_text_result_t speech_text_unit(const char *text, size_t len, bool finished, size_t *bytes)
{
    if (!bytes || (!text && len)) return SPEECH_TEXT_INVALID;
    *bytes = 0;
    size_t at = 0, chars = 0, boundary = 0, word = 0;
    /* Paragraph separators can arrive after the preceding unit was submitted.
     * Consume them even before finish, without billing an empty utterance. */
    while (at < len) {
        uint32_t c;
        int n = decode(text + at, len - at, &c);
        if (n <= 0) return SPEECH_TEXT_INVALID;
        if (!whitespace(c)) break;
        at += (size_t)n;
    }
    if (at) { *bytes = at; return SPEECH_TEXT_SKIP; }
    bool pending = false;
    while (at < len) {
        uint32_t c;
        int n = decode(text + at, len - at, &c);
        if (n <= 0) return SPEECH_TEXT_INVALID;
        /* Include closing punctuation in the same utterance. */
        if (pending && !closer(c) && !punctuation(c)) {
            boundary = at;
            if (chars >= 80) { *bytes = boundary; return SPEECH_TEXT_READY; }
            pending = false;
        }
        ++chars;
        if (chars > 512) {
            if (boundary) { *bytes = boundary; return SPEECH_TEXT_READY; }
            return SPEECH_TEXT_LONG;
        }
        bool url = url_token(text, word, at);
        bool stop = c == 0x3002 || c == 0xff01 || c == 0xff1f || c == '!' || c == '?' || c == '\n';
        if (url && c < 128 && c != '\n') stop = false;
        if (c == '.') {
            /* A dot inside a number, host, email or abbreviation is not a
             * sentence end. A trailing dot needs one lookahead or finish. */
            size_t look = at + (size_t)n;
            while (look < len) {
                uint32_t next;
                int width = decode(text + look, len - look, &next);
                if (width <= 0 || !closer(next)) break;
                look += (size_t)width;
            }
            bool next_space = look < len && (text[look] == ' ' || text[look] == '\n' || text[look] == '\r');
            bool last = look == len && finished;
            size_t word_len = at - word;
            stop = (next_space || last) && !url && !abbreviation(text + word, word_len);
        }
        at += (size_t)n;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') word = at;
        if (stop) pending = true;
        if (pending && closer(c)) continue;
        if (chars >= 160 && boundary) { *bytes = boundary; return SPEECH_TEXT_READY; }
    }
    if (finished) { *bytes = len; return len ? SPEECH_TEXT_READY : SPEECH_TEXT_WAIT; }
    if (boundary) { *bytes = boundary; return SPEECH_TEXT_READY; }
    return SPEECH_TEXT_WAIT;
}
