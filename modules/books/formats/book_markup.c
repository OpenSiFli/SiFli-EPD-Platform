/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"
#include "../../common/reader/reader_resource.h"
#include "book_paragraph_css.h"

typedef struct {
    book_picture_style_t style;
    unsigned important;
} css_picture;

typedef struct css_rule
{
    char *selector;
    unsigned set, clear;
    css_picture picture;
    css_paragraph paragraph;
} css_rule;
typedef struct markup_frame
{
    unsigned style;
    int skip, block, heading, page_start;
    char *link;
    css_paragraph box;
    epd_paragraph_style_t paragraph;
} markup_frame;
typedef struct markup
{
    const book_sink_t *sink;
    const book_resources_t *resources;
    const char *base;
    markup_frame frames[BF_DEPTH];
    unsigned depth, heading_serial;
    css_rule *rules;
    size_t count, rule_capacity;
    char *css;
    size_t css_length, css_capacity;
    char heading[512], heading_id[BF_PATH];
    int in_css, space, line_start, pending_title, paragraph_start;
    bool body_content;
} markup;
static int markup_text(markup *m, const char *text, size_t size, unsigned style)
{
    if (!size) return BOOK_OK;
    if (!(style & BOOK_STYLE_HEADING)) {
        for (size_t i = 0; i < size && !m->body_content; ++i)
            m->body_content = !isspace((unsigned char)text[i]);
    }
    for (unsigned i = 0; i < m->depth; ++i)
        if (m->frames[i].page_start) {
            style |= BOOK_STYLE_PAGE_START;
            m->frames[i].page_start = 0;
        }
    if (m->paragraph_start) { style |= BOOK_STYLE_PARAGRAPH_START; m->paragraph_start = 0; }
    if (m->sink->paragraph_style && m->depth)
        m->sink->paragraph_style(m->sink->user, &m->frames[m->depth - 1].paragraph);
    return bf_text(m->sink, text, size, style);
}
static int token(const char *s, const char *t)
{
    size_t n = strlen(t);
    while (*s)
    {
        while (isspace((unsigned char)*s))
            ++s;
        if (!strncmp(s, t, n) && (!s[n] || isspace((unsigned char)s[n])))
            return 1;
        while (*s && !isspace((unsigned char)*s))
            ++s;
    }
    return 0;
}
static const char *const picture_properties[] = {"width", "height", "max-width", "max-height"};

static int16_t picture_length(const char *value, bool html, unsigned property)
{
    if ((property < BOOK_PICTURE_MAX_WIDTH && !bf_casecmp(value, "auto")) ||
        (property >= BOOK_PICTURE_MAX_WIDTH && !bf_casecmp(value, "none")))
        return BOOK_PICTURE_AUTO;
    while (isspace((unsigned char)*value)) ++value;
    if (*value == '+') ++value;
    if (!isdigit((unsigned char)*value) && *value != '.') return 0;
    unsigned whole = 0, tenth = 0;
    bool nonzero = false;
    while (isdigit((unsigned char)*value)) {
        unsigned digit = *value++ - '0';
        nonzero |= digit != 0;
        if (whole < INT16_MAX) whole = whole * 10u + digit;
    }
    if (*value == '.') {
        ++value;
        if (!isdigit((unsigned char)*value)) return 0;
        tenth = *value - '0';
        while (isdigit((unsigned char)*value)) nonzero |= *value++ != '0';
    }
    if (*value == '%' && !value[1]) {
        unsigned amount = whole * 10u + tenth;
        if (amount > INT16_MAX) amount = INT16_MAX;
        return amount ? -(int16_t)amount : nonzero ? -1 : BOOK_PICTURE_ZERO;
    }
    if (bf_casecmp(value, "px") && (*value || (!html && nonzero))) return 0;
    if (!nonzero) return BOOK_PICTURE_ZERO;
    if (tenth >= 5u) ++whole;
    if (whole >= INT16_MAX) whole = INT16_MAX - 1u;
    return whole ? (int16_t)whole : 1;
}

static void css_picture_apply(book_picture_style_t *target, unsigned *priority,
                              const css_picture *source, unsigned specificity)
{
    for (unsigned i = 0; i < BOOK_PICTURE_LENGTHS; ++i) {
        unsigned rank = specificity + ((source->important & (1u << i)) ? 8u : 0);
        if (source->style.lengths[i] && rank >= priority[i]) {
            target->lengths[i] = source->style.lengths[i];
            priority[i] = rank;
        }
    }
}

static void css_values(const char *s, unsigned *set, unsigned *clear, css_picture *picture, css_paragraph *paragraph)
{
    char prop[64], value[128];
    size_t n;
    const char *p, *q;
    *set = *clear = 0;
    while (*s)
    {
        while (isspace((unsigned char)*s) || *s == ';')
            ++s;
        p = strchr(s, ':');
        if (!p)
            break;
        n = (size_t)(p - s);
        while (n && isspace((unsigned char)s[n - 1]))
            --n;
        if (n >= sizeof(prop))
            break;
        memcpy(prop, s, n);
        prop[n] = 0;
        s = p + 1;
        while (isspace((unsigned char)*s))
            ++s;
        q = strchr(s, ';');
        if (!q)
            q = s + strlen(s);
        n = (size_t)(q - s);
        if (n >= sizeof(value))
            n = sizeof(value) - 1;
        memcpy(value, s, n);
        value[n] = 0;
        css_paragraph_declaration(paragraph, prop, value);
        if (!bf_casecmp(prop, "font-weight"))
        {
            if (strstr(value, "bold") || atoi(value) >= 600)
                *set |= BOOK_STYLE_BOLD;
            else
                *clear |= BOOK_STYLE_BOLD;
        }
        else if (!bf_casecmp(prop, "font-style"))
        {
            if (strstr(value, "italic") || strstr(value, "oblique"))
                *set |= BOOK_STYLE_ITALIC;
            else
                *clear |= BOOK_STYLE_ITALIC;
        }
        else if (!bf_casecmp(prop, "text-align"))
        {
            *clear |= BOOK_STYLE_CENTER | BOOK_STYLE_RIGHT;
            if (strstr(value, "center"))
                *set |= BOOK_STYLE_CENTER;
            else if (strstr(value, "right"))
                *set |= BOOK_STYLE_RIGHT;
        }
        else if (!bf_casecmp(prop, "text-decoration") || !bf_casecmp(prop, "text-decoration-line"))
        {
            if (strstr(value, "none")) *clear |= BOOK_STYLE_UNDERLINE | BOOK_STYLE_STRIKE;
            if (strstr(value, "underline")) *set |= BOOK_STYLE_UNDERLINE;
            if (strstr(value, "line-through")) *set |= BOOK_STYLE_STRIKE;
        }
        else if (!bf_casecmp(prop, "white-space") && strstr(value, "pre"))
            *set |= BOOK_STYLE_PREFORMAT;
        else if (picture)
        {
            for (unsigned i = 0; i < BOOK_PICTURE_LENGTHS; ++i) {
                if (bf_casecmp(prop, picture_properties[i])) continue;
                char *bang = strchr(value, '!');
                char *end = bang ? bang : value + n;
                bool important = false;
                if (bang) {
                    char *flag = bang + 1;
                    while (isspace((unsigned char)*flag)) ++flag;
                    char *tail = value + n;
                    while (tail > flag && isspace((unsigned char)tail[-1])) --tail;
                    *tail = 0;
                    important = !bf_casecmp(flag, "important");
                    if (!important) break;
                }
                while (end > value && isspace((unsigned char)end[-1])) --end;
                *end = 0;
                int16_t length = picture_length(value, false, i);
                if (length && (important || !(picture->important & (1u << i)))) {
                    picture->style.lengths[i] = length;
                    if (important) picture->important |= 1u << i;
                }
                break;
            }
        }
        s = *q ? q + 1 : q;
    }
}
static int css_append(char **buffer, size_t *length, size_t *capacity, const char *text, size_t n)
{
    if (n >= SIZE_MAX - *length) return BOOK_NO_MEMORY;
    size_t needed = *length + n + 1;
    if (needed > *capacity)
    {
        size_t size = *capacity ? *capacity : 1024;
        while (size < needed)
            size = size > SIZE_MAX / 2 ? needed : size * 2;
        char *next = bf_realloc(*buffer, size);
        if (!next) return BOOK_NO_MEMORY;
        *buffer = next;
        *capacity = size;
    }
    memcpy(*buffer + *length, text, n);
    *length += n;
    (*buffer)[*length] = 0;
    return BOOK_OK;
}

static int css_rule_add(markup *m, const char *selector, size_t n, unsigned set, unsigned clear,
                        const css_picture *picture, const css_paragraph *paragraph)
{
    if (m->count == m->rule_capacity)
    {
        size_t max_count = SIZE_MAX / sizeof(*m->rules);
        if (m->count == max_count) return BOOK_NO_MEMORY;
        size_t size = m->rule_capacity ? m->rule_capacity : 16;
        if (size <= m->count) size = size > max_count / 2 ? max_count : size * 2;
        css_rule *next = bf_realloc(m->rules, size * sizeof(*next));
        if (!next) return BOOK_NO_MEMORY;
        m->rules = next;
        m->rule_capacity = size;
    }
    char *name = bf_alloc(n + 1);
    if (!name) return BOOK_NO_MEMORY;
    memcpy(name, selector, n);
    name[n] = 0;
    m->rules[m->count++] = (css_rule){name, set, clear, *picture, *paragraph};
    return BOOK_OK;
}

static int css_parse(markup *m, char *s)
{
    char *open, *close, *comma, *p;
    unsigned set, clear;
    if (!s) return BOOK_OK;
    while ((open = strchr(s, '{')) != NULL && (close = strchr(open, '}')) != NULL)
    {
        int result = bf_cancel(m->sink);
        if (result < 0) return result;
        *open = 0;
        *close = 0;
        css_picture picture = {0};
        css_paragraph paragraph = {0};
        css_values(open + 1, &set, &clear, &picture, &paragraph);
        p = s;
        while (p)
        {
            size_t n;
            comma = strchr(p, ',');
            if (comma)
                *comma = 0;
            while (isspace((unsigned char)*p))
                ++p;
            n = strlen(p);
            while (n && isspace((unsigned char)p[n - 1]))
                --n;
            if (n && !memchr(p, ' ', n) && !memchr(p, ':', n) &&
                p[0] != '@')
            {
                result = css_rule_add(m, p, n, set, clear, &picture, &paragraph);
                if (result < 0) return result;
            }
            p = comma ? comma + 1 : NULL;
        }
        s = close + 1;
    }
    return BOOK_OK;
}
static int css_load(markup *m, const char *href)
{
    book_stream_t s;
    char *buf = NULL;
    size_t n = 0, capacity = 0;
    char chunk[1024];
    int r;
    if (!m->resources || bf_external(href))
        return BOOK_OK;
    r = m->resources->open(m->resources->user, m->base, href, &s);
    /* Only the chapter resource adapter opts into missing-CSS fallback. */
    if (r == READER_RESOURCE_MISSING)
        return BOOK_OK;
    if (r < 0)
        return r;
    while ((r = s.read(s.user, chunk, sizeof(chunk))) > 0)
    {
        r = css_append(&buf, &n, &capacity, chunk, (size_t)r);
        if (r < 0) break;
        if ((r = bf_cancel(m->sink)) < 0)
            break;
    }
    if (r >= 0)
    {
        r = css_parse(m, buf);
    }
    bf_free(buf);
    return bf_finish(&s, r);
}
static int is_block(const char *n)
{
    return !strcmp(n, "p") || !strcmp(n, "div") || !strcmp(n, "section") || !strcmp(n, "article") ||
           !strcmp(n, "body") || !strcmp(n, "main") || !strcmp(n, "aside") ||
           !strcmp(n, "ul") || !strcmp(n, "ol") || !strcmp(n, "dl") || !strcmp(n, "dd") ||
           !strcmp(n, "blockquote") || !strcmp(n, "li") || !strcmp(n, "tr") || !strcmp(n, "pre") ||
           (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2]);
}
static int markup_boundary(markup *m, markup_frame *f, bool begin)
{
    epd_paragraph_style_t p = f->paragraph;
    p.flags = EPD_PARAGRAPH_ACTIVE | (begin ? EPD_PARAGRAPH_BEGIN : EPD_PARAGRAPH_END);
    epd_paragraph_style_t outer = m->depth > 1 ? m->frames[m->depth - 2].paragraph : (epd_paragraph_style_t){0};
    p.margin = css_box_length(f->box.values[begin ? CSS_MT : CSS_MB], outer);
    p.padding = css_box_length(f->box.values[begin ? CSS_PT : CSS_PB], outer);
    if (m->sink->paragraph_style) m->sink->paragraph_style(m->sink->user, &p);
    m->paragraph_start = 1;
    return bf_para(m->sink);
}
static int markup_event(void *user, int event, const char *name, const char *ns,
                        const bf_attrs *attrs, const char *text, size_t n)
{
    markup *m = user;
    markup_frame *f;
    char value[BF_PATH], id[BF_PATH], classes[512], semantics[128], role[128];
    unsigned i, set, clear;
    int r;
    const char *colon = strchr(name, ':');
    (void)ns;
    if (colon)
        name = colon + 1;
    if (event == 1)
    {
        if (m->depth >= BF_DEPTH)
            return BOOK_UNSUPPORTED;
        f = &m->frames[m->depth];
        memset(f, 0, sizeof(*f));
        if (m->depth)
        {
            f->style = m->frames[m->depth - 1].style;
            f->skip = m->frames[m->depth - 1].skip;
        }
        f->paragraph.flags = EPD_PARAGRAPH_ACTIVE;
        if (m->depth) f->paragraph = m->frames[m->depth - 1].paragraph;
        ++m->depth;
        if (bf_attr(attrs, "type", "http://www.idpf.org/2007/ops", semantics, sizeof(semantics)) < 0 ||
            bf_attr(attrs, "role", NULL, role, sizeof(role)) < 0 ||
            bf_attr(attrs, "class", NULL, classes, sizeof(classes)) < 0) return BOOK_UNSUPPORTED;
        if (token(semantics, "footnote") || token(semantics, "endnote") ||
            token(role, "doc-footnote") || token(role, "doc-endnote") ||
            (strcmp(name, "a") && (token(classes, "footnote") || token(classes, "endnote") ||
                                   token(classes, "footnote-text") || token(classes, "footnotes")))) f->skip = 1;
        if (!strcmp(name, "head") || !strcmp(name, "script") || !strcmp(name, "style") ||
            !strcmp(name, "title"))
            f->skip = 1;
        if (!strcmp(name, "style"))
        {
            m->in_css = 1;
            m->css_length = 0;
            if (m->css) m->css[0] = 0;
        }
        if (!strcmp(name, "link"))
        {
            if (bf_attr(attrs, "rel", NULL, value, sizeof(value)) < 0)
                return BOOK_UNSUPPORTED;
            if (token(value, "stylesheet"))
            {
                if (bf_attr(attrs, "href", NULL, value, sizeof(value)) < 0)
                    return BOOK_UNSUPPORTED;
                if (*value && (r = css_load(m, value)) < 0)
                    return r;
            }
        }
        if (f->skip)
            return BOOK_OK;
        if (bf_attr(attrs, "id", NULL, id, sizeof(id)) < 0 ||
            bf_attr(attrs, "class", NULL, classes, sizeof(classes)) < 0)
            return BOOK_UNSUPPORTED;
        if (!*id && !strcmp(name, "a") && bf_attr(attrs, "name", NULL, id, sizeof(id)) < 0)
            return BOOK_UNSUPPORTED;
        if (!strcmp(name, "b") || !strcmp(name, "strong") || !strcmp(name, "th"))
            f->style |= BOOK_STYLE_BOLD;
        if (!strcmp(name, "i") || !strcmp(name, "em"))
            f->style |= BOOK_STYLE_ITALIC;
        if (!strcmp(name, "u") || !strcmp(name, "ins")) f->style |= BOOK_STYLE_UNDERLINE;
        if (!strcmp(name, "s") || !strcmp(name, "strike") || !strcmp(name, "del")) f->style |= BOOK_STYLE_STRIKE;
        if (!strcmp(name, "pre") || !strcmp(name, "code"))
            f->style |= BOOK_STYLE_PREFORMAT;
        if (!strcmp(name, "center"))
            f->style |= BOOK_STYLE_CENTER;
        bool is_picture = !strcmp(name, "img") || !strcmp(name, "image");
        book_picture_style_t picture = {0};
        unsigned picture_priority[BOOK_PICTURE_LENGTHS] = {0};
        if (is_picture)
        {
            for (unsigned i = BOOK_PICTURE_WIDTH; i <= BOOK_PICTURE_HEIGHT; ++i) {
                if (bf_attr(attrs, picture_properties[i], NULL, value, sizeof(value)) < 0)
                    continue;
                size_t length = strlen(value);
                while (length && isspace((unsigned char)value[length - 1])) value[--length] = 0;
                picture.lengths[i] = picture_length(value, true, i);
            }
        }
        unsigned paragraph_priority[CSS_PARAGRAPH_PROPERTIES] = {0};
        if (!strcmp(name, "p") || !strcmp(name, "blockquote"))
            css_paragraph_declaration(&f->box, "margin", "1em 0");
        if (!strcmp(name, "blockquote") || !strcmp(name, "dd")) {
            css_paragraph_declaration(&f->box, "margin-left", "2em");
            css_paragraph_declaration(&f->box, "margin-right", "2em");
        }
        for (size_t rule_index = 0; rule_index < m->count; ++rule_index)
        {
            css_rule *rule = &m->rules[rule_index];
            const char *s = rule->selector;
            int match = 0;
            unsigned specificity = 1;
            if (*s == '.')
            { match = token(classes, s + 1); specificity = 2; }
            else if (*s == '#')
            { match = !strcmp(id, s + 1); specificity = 4; }
            else
            {
                const char *dot = strchr(s, '.');
                if (dot) specificity = 3;
                match = dot ? (strlen(name) == (size_t)(dot - s) &&
                               !strncmp(name, s, (size_t)(dot - s)) && token(classes, dot + 1))
                            : !strcmp(name, s);
            }
            if (match) {
                f->style = (f->style & ~rule->clear) | rule->set;
                css_paragraph_apply(&f->box, paragraph_priority, &rule->paragraph, specificity);
                if (is_picture) css_picture_apply(&picture, picture_priority, &rule->picture, specificity);
            }
        }
        if (bf_attr(attrs, "style", NULL, value, sizeof(value)) < 0)
            return BOOK_UNSUPPORTED;
        css_picture inline_picture = {0};
        css_paragraph inline_paragraph = {0};
        css_values(value, &set, &clear, is_picture ? &inline_picture : NULL, &inline_paragraph);
        css_paragraph_apply(&f->box, paragraph_priority, &inline_paragraph, 5);
        if (is_picture) css_picture_apply(&picture, picture_priority, &inline_picture, 5);
        f->style = (f->style & ~clear) | set;
        f->block = is_block(name);
        markup_frame *parent = m->depth > 1 ? &m->frames[m->depth - 2] : NULL;
        for (unsigned k = 0; k < CSS_PARAGRAPH_PROPERTIES; ++k) {
            unsigned bit = 1u << k;
            if ((f->box.inherit & bit) || (k >= CSS_INDENT && !(f->box.specified & bit)))
                f->box.values[k] = parent ? parent->box.values[k] : (epd_paragraph_length_t){0};
        }
        if (f->block) {
            epd_paragraph_style_t outer = parent ? parent->paragraph : (epd_paragraph_style_t){0};
            f->paragraph.left = css_length_add(outer.left, css_box_length(css_length_add(f->box.values[CSS_ML], f->box.values[CSS_PL]), outer));
            f->paragraph.right = css_length_add(outer.right, css_box_length(css_length_add(f->box.values[CSS_MR], f->box.values[CSS_PR]), outer));
            f->paragraph.indent = f->box.values[CSS_INDENT];
            f->paragraph.line_height = f->box.values[CSS_LINE_HEIGHT];
            f->paragraph.align = (unsigned)f->box.values[CSS_ALIGN].px;
        }
        if (f->block)
        {
            if ((r = markup_boundary(m, f, true)) < 0)
                return r;
            m->line_start = 1;
            m->space = 0;
        }
        if (*id)
        {
            if (snprintf(value, sizeof(value), "#%s", id) >= (int)sizeof(value))
                return BOOK_UNSUPPORTED;
            if ((r = bf_resolve(m->base, value, id, sizeof(id))) < 0)
                return r;
        }
        bool toc_title = *id && m->sink->heading_target && m->sink->heading_target(m->sink->user, id);
        bool title_container = !strcmp(name, "section") || !strcmp(name, "article") ||
                               !strcmp(name, "body") || !strcmp(name, "div") || !strcmp(name, "a");
        if (toc_title && title_container) m->pending_title = 1;
        if ((name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) ||
            token(role, "heading") || token(semantics, "title") || token(semantics, "fulltitle") ||
            token(semantics, "halftitle") || token(classes, "chapter-title") ||
            token(classes, "chapter_title") || token(classes, "chaptertitle") ||
            (!strcmp(name, "p") && (token(semantics, "chapter") || token(classes, "title"))) ||
            (toc_title && !title_container) || (m->pending_title && !strcmp(name, "p")))
        {
            m->pending_title = 0;
            f->heading = name[0] == 'h' && name[1] >= '1' && name[1] <= '6' ? name[1] - '0' : 1;
            f->page_start = 1;
            if (!f->block) {
                if ((r = markup_boundary(m, f, true)) < 0) return r;
                f->block = 1; m->line_start = 1; m->space = 0;
            }
            f->style |= BOOK_STYLE_HEADING | BOOK_STYLE_BOLD;
            m->heading[0] = 0;
            if (!*id)
            {
                snprintf(value, sizeof(value), "#heading-%u", ++m->heading_serial);
                if ((r = bf_resolve(m->base, value, id, sizeof(id))) < 0)
                    return r;
            }
            if ((r = bf_copy(m->heading_id, sizeof(m->heading_id), id)) < 0)
                return r;
        }
        if (*id && (r = bf_anchor(m->sink, id, NULL, (unsigned)f->heading)) < 0)
            return r;
        if (!strcmp(name, "a"))
        {
            if (bf_attr(attrs, "href", NULL, value, sizeof(value)) < 0)
                return BOOK_UNSUPPORTED;
            if (*value)
            {
                if ((r = bf_resolve(m->base, value, id, sizeof(id))) < 0)
                    return r;
                f->link = bf_strdup(id);
                if (!f->link)
                    return BOOK_NO_MEMORY;
                f->style |= BOOK_STYLE_LINK;
                char rel[128];
                if (bf_attr(attrs, "rel", NULL, rel, sizeof(rel)) < 0) return BOOK_UNSUPPORTED;
                if (!bf_external(value) && strchr(value, '#') &&
                    (token(semantics, "noteref") || token(role, "doc-noteref") || token(rel, "footnote") ||
                     token(classes, "noteref") || token(classes, "footnote-ref") || token(classes, "footnoteRef") ||
                     token(classes, "footnote") || token(classes, "duokan-footnote")))
                    f->style |= BOOK_STYLE_NOTEREF;
                if ((r = bf_link(m->sink, id, true)) < 0)
                    return r;
            }
        }
        if (is_picture)
        {
            /* A publisher icon inside a note link belongs to the text line. */
            if (f->style & BOOK_STYLE_NOTEREF)
                return markup_text(m, "〔注〕", sizeof("〔注〕") - 1u, f->style);
            for (unsigned i = 0; i < BOOK_PICTURE_LENGTHS; ++i)
                if (picture.lengths[i] == BOOK_PICTURE_ZERO) return BOOK_OK;
            if (bf_attr(attrs, !strcmp(name, "img") ? "src" : "href", NULL, value, sizeof(value)) <
                0)
                return BOOK_UNSUPPORTED;
            if (!m->body_content && !(f->style & BOOK_STYLE_LINK)) picture.flags = BOOK_STYLE_IMAGE_PAGE;
            m->body_content = true;
            if (m->sink->picture_style) m->sink->picture_style(m->sink->user, &picture);
            r = bf_picture(m->sink, m->resources, m->base, value);
            if (m->sink->picture_style) m->sink->picture_style(m->sink->user, NULL);
            if (r < 0) return r;
        }
        if (!strcmp(name, "br"))
        {
            m->line_start = 1;
            m->space = 0;
            return markup_text(m, "\n", 1, f->style);
        }
        if (!strcmp(name, "td") || !strcmp(name, "th"))
            return markup_text(m, "\t", 1, f->style);
        if (!strcmp(name, "li"))
            return markup_text(m, "• ", 4, f->style);
        return BOOK_OK;
    }
    if (!m->depth)
        return BOOK_OK;
    f = &m->frames[m->depth - 1];
    if (event == 0)
    {
        if (!strcmp(name, "style"))
        {
            m->in_css = 0;
            r = css_parse(m, m->css);
            bf_free(m->css);
            m->css = NULL;
            m->css_length = m->css_capacity = 0;
            if (r < 0) return r;
        }
        if (!f->skip)
        {
            if (f->heading &&
                (r = bf_anchor(m->sink, m->heading_id, m->heading, (unsigned)f->heading)) < 0)
                return r;
            if (f->link)
            {
                r = bf_link(m->sink, f->link, false);
                bf_free(f->link);
                f->link = NULL;
                if (r < 0)
                    return r;
            }
            if (f->block)
            {
                m->line_start = 1;
                m->space = 0;
                if ((r = markup_boundary(m, f, false)) < 0)
                    return r;
            }
        }
        --m->depth;
        return BOOK_OK;
    }
    if (m->in_css)
    {
        return css_append(&m->css, &m->css_length, &m->css_capacity, text, n);
    }
    if (f->skip)
        return BOOK_OK;
    for (i = 0; i < m->depth; ++i)
        if (m->frames[i].heading)
        {
            bf_append(m->heading, sizeof(m->heading), text, n);
            break;
        }
    if (f->style & BOOK_STYLE_PREFORMAT)
    {
        m->line_start = 0;
        return markup_text(m, text, n, f->style);
    }
    {
        char out[BF_TEXT + 4];
        size_t at = 0, j;
        for (j = 0; j < n; ++j)
        {
            unsigned char c = (unsigned char)text[j];
            if (isspace(c))
            {
                m->space = 1;
                continue;
            }
            if (m->space && !m->line_start)
                out[at++] = ' ';
            m->space = 0;
            m->line_start = 0;
            out[at++] = (char)c;
            if (at >= BF_TEXT && (j + 1 == n || ((unsigned char)text[j + 1] & 0xc0) != 0x80))
            {
                if ((r = markup_text(m, out, at, f->style)) < 0)
                    return r;
                at = 0;
            }
        }
        return markup_text(m, out, at, f->style);
    }
}
int book_markup_convert(const book_sink_t *sink, book_stream_t *stream, const char *base,
                        const book_resources_t *resources)
{
    markup *m;
    int r;
    unsigned i;
    if (!bf_active() || !sink || !stream || !stream->read)
        return BOOK_ERROR;
    m = bf_alloc(sizeof(*m));
    if (!m)
        return BOOK_NO_MEMORY;
    m->sink = sink;
    m->resources = resources;
    m->base = base ? base : "";
    m->line_start = 1;
    r = bf_anchor(sink, m->base, NULL, 0);
    if (r == BOOK_OK)
        r = bf_svg_markup(stream, sink, markup_event, m, m->base, resources);
    for (i = 0; i < m->depth; ++i)
        bf_free(m->frames[i].link);
    for (size_t rule_index = 0; rule_index < m->count; ++rule_index)
        bf_free(m->rules[rule_index].selector);
    bf_free(m->rules);
    bf_free(m->css);
    bf_free(m);
    if (sink->paragraph_style) sink->paragraph_style(sink->user, NULL);
    return r;
}
