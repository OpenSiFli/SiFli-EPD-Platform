/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_PARAGRAPH_CSS_H
#define BOOK_PARAGRAPH_CSS_H

enum { CSS_MT, CSS_MR, CSS_MB, CSS_ML, CSS_PT, CSS_PR, CSS_PB, CSS_PL,
       CSS_INDENT, CSS_LINE_HEIGHT, CSS_ALIGN, CSS_PARAGRAPH_PROPERTIES };
typedef struct {
    epd_paragraph_length_t values[CSS_PARAGRAPH_PROPERTIES];
    unsigned specified, important, inherit;
} css_paragraph;

static int32_t css_coefficient(int64_t v)
{ return v > 16777216 ? 16777216 : v < -16777216 ? -16777216 : (int32_t)v; }

static epd_paragraph_length_t css_length_add(epd_paragraph_length_t a, epd_paragraph_length_t b)
{
    return (epd_paragraph_length_t){css_coefficient((int64_t)a.px + b.px),
        css_coefficient((int64_t)a.em + b.em), css_coefficient((int64_t)a.percent + b.percent)};
}

static bool css_paragraph_length(const char *s, unsigned property, epd_paragraph_length_t *out)
{
    epd_paragraph_length_t v = {0};
    if (!bf_casecmp(s, "normal") && property == CSS_LINE_HEIGHT) { *out = v; return true; }
    if (!bf_casecmp(s, "auto") && property <= CSS_ML) { *out = v; return true; }
    char *end;
    double n = strtod(s, &end);
    if (end == s || n != n || n > 65536 || n < -65536) return false;
    if (n < 0 && property >= CSS_PT && property != CSS_INDENT) return false;
    if (!bf_casecmp(end, "em") || !bf_casecmp(end, "rem")) v.em = (int32_t)(n * 256);
    else if (!bf_casecmp(end, "ex")) v.em = (int32_t)(n * 128);
    else if (!strcmp(end, "%")) v.percent = (int32_t)(n * 256 / 100);
    else if (!bf_casecmp(end, "px") || (!*end && !n)) v.px = (int32_t)(n * 256);
    else if (!bf_casecmp(end, "pt")) v.px = (int32_t)(n * 256 * 4 / 3);
    else if (!bf_casecmp(end, "pc")) v.px = (int32_t)(n * 256 * 16);
    else if (!bf_casecmp(end, "in")) v.px = (int32_t)(n * 256 * 96);
    else if (!bf_casecmp(end, "cm")) v.px = (int32_t)(n * 256 * 96 / 2.54);
    else if (!bf_casecmp(end, "mm")) v.px = (int32_t)(n * 256 * 96 / 25.4);
    else if (!*end && property == CSS_LINE_HEIGHT) v.em = (int32_t)(n * 256);
    else return false;
    v.px = css_coefficient(v.px); v.em = css_coefficient(v.em); v.percent = css_coefficient(v.percent);
    *out = v; return true;
}

static void css_paragraph_set(css_paragraph *p, unsigned property,
                              epd_paragraph_length_t value, bool important, bool inherit)
{
    unsigned bit = 1u << property;
    if (!important && (p->important & bit)) return;
    p->values[property] = value; p->specified |= bit;
    p->important = (p->important & ~bit) | (important ? bit : 0);
    p->inherit = (p->inherit & ~bit) | (inherit ? bit : 0);
}

static void css_paragraph_declaration(css_paragraph *p, const char *property, const char *raw)
{
    static const char *const names[] = {"margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding-top", "padding-right", "padding-bottom", "padding-left", "text-indent", "line-height", "text-align"};
    char value[128]; size_t n = strlen(raw);
    if (!p || n >= sizeof(value)) return;
    memcpy(value, raw, n + 1);
    char *bang = strchr(value, '!'); bool important = false;
    if (bang) {
        char *flag = bang + 1; while (isspace((unsigned char)*flag)) ++flag;
        char *end = value + n; while (end > flag && isspace((unsigned char)end[-1])) --end;
        *end = 0;
        if (bf_casecmp(flag, "important")) return;
        important = true; *bang = 0; n = (size_t)(bang - value);
    }
    while (n && isspace((unsigned char)value[n - 1])) value[--n] = 0;
    bool inherit = !bf_casecmp(value, "inherit"), initial = !bf_casecmp(value, "initial");
    bool unset = !bf_casecmp(value, "unset");
    unsigned start = 0, count = 1;
    if (!bf_casecmp(property, "margin")) count = 4;
    else if (!bf_casecmp(property, "padding")) { start = CSS_PT; count = 4; }
    else {
        for (; start < CSS_PARAGRAPH_PROPERTIES; ++start) if (!bf_casecmp(property, names[start])) break;
        if (start == CSS_PARAGRAPH_PROPERTIES) return;
    }
    if (inherit || initial || unset) {
        for (unsigned i = 0; i < count; ++i)
            css_paragraph_set(p, start + i, (epd_paragraph_length_t){0}, important,
                inherit || (unset && start >= CSS_INDENT));
        return;
    }
    if (start == CSS_ALIGN) {
        unsigned align = !bf_casecmp(value, "left") || !bf_casecmp(value, "start") ? EPD_ALIGN_LEFT :
            !bf_casecmp(value, "right") || !bf_casecmp(value, "end") ? EPD_ALIGN_RIGHT :
            !bf_casecmp(value, "center") ? EPD_ALIGN_CENTER : !bf_casecmp(value, "justify") ? EPD_ALIGN_JUSTIFY : 0;
        if (align) css_paragraph_set(p, start, (epd_paragraph_length_t){(int32_t)align, 0, 0}, important, false);
        return;
    }
    epd_paragraph_length_t lengths[4] = {{0}}; unsigned used = 0;
    char *at = value;
    while (*at && used < count) {
        while (isspace((unsigned char)*at)) ++at;
        if (!*at) break;
        char *end = at; while (*end && !isspace((unsigned char)*end)) ++end;
        char delimiter = *end; *end = 0;
        if (!css_paragraph_length(at, start, &lengths[used++])) return;
        at = delimiter ? end + 1 : end;
    }
    while (isspace((unsigned char)*at)) ++at;
    if (*at || !used) return;
    if (count == 1) css_paragraph_set(p, start, lengths[0], important, false);
    else {
        unsigned indices[] = {0, used > 1 ? 1 : 0, used > 2 ? 2 : 0, used > 3 ? 3 : used > 1 ? 1 : 0};
        for (unsigned i = 0; i < 4; ++i) css_paragraph_set(p, start + i, lengths[indices[i]], important, false);
    }
}

static void css_paragraph_apply(css_paragraph *target, unsigned *priorities,
                                 const css_paragraph *source, unsigned specificity)
{
    for (unsigned i = 0; i < CSS_PARAGRAPH_PROPERTIES; ++i) {
        unsigned bit = 1u << i, rank = specificity + ((source->important & bit) ? 8u : 0);
        if ((source->specified & bit) && rank >= priorities[i]) {
            target->values[i] = source->values[i]; target->specified |= bit;
            target->inherit = (target->inherit & ~bit) | (source->inherit & bit);
            priorities[i] = rank;
        }
    }
}

/* Percentages on nested boxes are relative to the containing content width. */
static epd_paragraph_length_t css_box_length(epd_paragraph_length_t v, epd_paragraph_style_t parent)
{
    epd_paragraph_length_t sides = css_length_add(parent.left, parent.right);
    return (epd_paragraph_length_t){css_coefficient((int64_t)v.px - (int64_t)v.percent * sides.px / 256),
        css_coefficient((int64_t)v.em - (int64_t)v.percent * sides.em / 256),
        css_coefficient((int64_t)v.percent * (256 - sides.percent) / 256)};
}
#endif
