#include "words_dictionary.h"
#include <string.h>

typedef struct { char *buffer; size_t capacity, used; bool valid; } text_t;
static void append(text_t *out, const char *text)
{
    size_t n = strlen(text);
    if (!out->valid || n > SIZE_MAX - out->used - 1) { out->valid = false; return; }
    if (out->buffer && out->used < out->capacity)
    {
        size_t copy = n < out->capacity - out->used - 1 ? n : out->capacity - out->used - 1;
        memcpy(out->buffer + out->used, text, copy); out->buffer[out->used + copy] = 0;
    }
    out->used += n;
}
static void line(text_t *out, const char *text) { if (text[0]) { append(out, text); append(out, "\n"); } }
static void source(text_t *out, const char *name)
{ if (name && name[0]) { append(out, "〔"); append(out, name); append(out, "〕\n"); } }

size_t words_entry_text_labels(const words_entry_t *e, unsigned mode, char *buffer, size_t capacity,
                              const words_text_labels_t *labels)
{
    if (!e || !e->word || mode > WORDS_TEXT_DETAIL) return 0;
    text_t out = {buffer, capacity, 0, true};
    if (buffer && capacity) buffer[0] = 0;
    if (e->version == 1)
    {
        if (mode == WORDS_TEXT_PHONETIC) append(&out, e->legacy[WORDS_PHONETIC]);
        else if (mode == WORDS_TEXT_BRIEF)
            append(&out, e->legacy[WORDS_TRANSLATION][0] ? e->legacy[WORDS_TRANSLATION] : e->legacy[WORDS_DEFINITION]);
        else
        {
            line(&out, labels->titles[WORDS_ITEM_SENSE]); line(&out, e->legacy[WORDS_TRANSLATION]); line(&out, e->legacy[WORDS_DEFINITION]);
            append(&out, "\n"); line(&out, labels->titles[WORDS_ITEM_FORM]); line(&out, e->legacy[WORDS_EXCHANGE]);
            append(&out, "\n"); line(&out, labels->tags); line(&out, e->legacy[WORDS_TAGS]);
        }
    }
    else
    {
        for (uint32_t type = 1; type <= 8; ++type)
        {
            if ((mode == WORDS_TEXT_PHONETIC && type != 1) || (mode == WORDS_TEXT_BRIEF && type != 2)) continue;
            uint32_t offset = 0; bool first = true; words_item_t item;
            for (uint32_t i = 0; i < e->count; ++i)
            {
                if (!words_entry_item(e, &offset, &item)) return 0;
                if (item.type != type) continue;
                if (mode == WORDS_TEXT_DETAIL && first) { append(&out, "\n"); line(&out, labels->titles[type]); }
                if (mode == WORDS_TEXT_PHONETIC && !first) append(&out, "  ");
                first = false;
                if (type == WORDS_ITEM_PHONETIC)
                {
                    append(&out, !strcmp(item.text[0], "uk") ? labels->uk : !strcmp(item.text[0], "us") ? labels->us : "");
                    append(&out, item.text[1]);
                    if (mode == WORDS_TEXT_DETAIL) { append(&out, "\n"); source(&out, item.text[2]); }
                }
                else if (type == WORDS_ITEM_SENSE)
                {
                    if (item.text[0][0]) { append(&out, item.text[0]); append(&out, " "); }
                    line(&out, item.text[1]);
                    if (mode == WORDS_TEXT_DETAIL || !item.text[1][0]) line(&out, item.text[2]);
                    if (mode == WORDS_TEXT_DETAIL) source(&out, item.text[3]);
                }
                else if (type == WORDS_ITEM_RELATED)
                { append(&out, item.text[0]); append(&out, " "); line(&out, item.text[1]); line(&out, item.text[2]); source(&out, item.text[3]); }
                else if (type == WORDS_ITEM_FORM || type == WORDS_ITEM_SOURCE || type == WORDS_ITEM_SCOPE)
                { append(&out, item.text[0]); append(&out, ": "); line(&out, item.text[1]); if (type != WORDS_ITEM_SOURCE) source(&out, item.text[2]); }
                else { line(&out, item.text[0]); line(&out, item.text[1]); source(&out, item.text[2]); }
            }
        }
    }
    return out.valid ? out.used + 1 : 0;
}

size_t words_entry_text(const words_entry_t *e, unsigned mode, char *buffer, size_t capacity)
{
    static const words_text_labels_t labels = {
        {"", "音标", "释义", "词形", "例句", "短语", "来源", "学习范围", "同根词"},
        "标签", "英 ", "美 "
    };
    return words_entry_text_labels(e, mode, buffer, capacity, &labels);
}
