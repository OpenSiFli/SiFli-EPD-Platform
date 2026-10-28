/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"
#include "../../common/reader/reader_resource.h"

typedef struct css_rule
{
    char *selector;
    unsigned set, clear;
} css_rule;
typedef struct markup_frame
{
    unsigned style;
    int skip, block, heading;
    char *link;
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
    int in_css, space, line_start;
} markup;
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
static void css_values(const char *s, unsigned *set, unsigned *clear)
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
        else if (!bf_casecmp(prop, "white-space") && strstr(value, "pre"))
            *set |= BOOK_STYLE_PREFORMAT;
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

static int css_rule_add(markup *m, const char *selector, size_t n, unsigned set, unsigned clear)
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
    m->rules[m->count++] = (css_rule){name, set, clear};
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
        css_values(open + 1, &set, &clear);
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
                result = css_rule_add(m, p, n, set, clear);
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
           !strcmp(n, "blockquote") || !strcmp(n, "li") || !strcmp(n, "tr") || !strcmp(n, "pre") ||
           (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2]);
}
static int markup_event(void *user, int event, const char *name, const char *ns,
                        const bf_attrs *attrs, const char *text, size_t n)
{
    markup *m = user;
    markup_frame *f;
    char value[BF_PATH], id[BF_PATH], classes[512];
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
        ++m->depth;
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
        if (!strcmp(name, "pre") || !strcmp(name, "code"))
            f->style |= BOOK_STYLE_PREFORMAT;
        if (!strcmp(name, "center"))
            f->style |= BOOK_STYLE_CENTER;
        for (size_t rule_index = 0; rule_index < m->count; ++rule_index)
        {
            css_rule *rule = &m->rules[rule_index];
            const char *s = rule->selector;
            int match = 0;
            if (*s == '.')
                match = token(classes, s + 1);
            else if (*s == '#')
                match = !strcmp(id, s + 1);
            else
            {
                const char *dot = strchr(s, '.');
                match = dot ? (strlen(name) == (size_t)(dot - s) &&
                               !strncmp(name, s, (size_t)(dot - s)) && token(classes, dot + 1))
                            : !strcmp(name, s);
            }
            if (match)
                f->style = (f->style & ~rule->clear) | rule->set;
        }
        if (bf_attr(attrs, "style", NULL, value, sizeof(value)) < 0)
            return BOOK_UNSUPPORTED;
        css_values(value, &set, &clear);
        f->style = (f->style & ~clear) | set;
        f->block = is_block(name);
        if (f->block)
        {
            if ((r = bf_para(m->sink)) < 0)
                return r;
            m->line_start = 1;
            m->space = 0;
        }
        if (*id)
        {
            if (snprintf(value, sizeof(value), "#%s", id) >= (int)sizeof(value))
                return BOOK_UNSUPPORTED;
            if ((r = bf_resolve(m->base, value, id, sizeof(id))) < 0 ||
                (r = bf_anchor(m->sink, id, NULL, 0)) < 0)
                return r;
        }
        if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2])
        {
            f->heading = name[1] - '0';
            f->style |= BOOK_STYLE_HEADING | BOOK_STYLE_BOLD;
            m->heading[0] = 0;
            if (!*id)
            {
                snprintf(value, sizeof(value), "#heading-%u", ++m->heading_serial);
                if ((r = bf_resolve(m->base, value, id, sizeof(id))) < 0)
                    return r;
                if ((r = bf_anchor(m->sink, id, NULL, 0)) < 0)
                    return r;
            }
            if ((r = bf_copy(m->heading_id, sizeof(m->heading_id), id)) < 0)
                return r;
        }
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
                if ((r = bf_link(m->sink, id, true)) < 0)
                    return r;
            }
        }
        if (!strcmp(name, "img") || !strcmp(name, "image"))
        {
            if (bf_attr(attrs, !strcmp(name, "img") ? "src" : "href", NULL, value, sizeof(value)) <
                0)
                return BOOK_UNSUPPORTED;
            if ((r = bf_picture(m->sink, m->resources, m->base, value)) < 0)
                return r;
        }
        if (!strcmp(name, "br"))
        {
            m->line_start = 1;
            m->space = 0;
            return bf_text(m->sink, "\n", 1, f->style);
        }
        if (!strcmp(name, "td") || !strcmp(name, "th"))
            return bf_text(m->sink, "\t", 1, f->style);
        if (!strcmp(name, "li"))
            return bf_text(m->sink, "• ", 4, f->style);
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
                if ((r = bf_para(m->sink)) < 0)
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
        return bf_text(m->sink, text, n, f->style);
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
                if ((r = bf_text(m->sink, out, at, f->style)) < 0)
                    return r;
                at = 0;
            }
        }
        return bf_text(m->sink, out, at, f->style);
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
    return r;
}
