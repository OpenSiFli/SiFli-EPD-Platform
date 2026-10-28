/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"

static int md_open(void *u, const char *base, const char *ref, book_stream_t *s)
{
    char p[BF_PATH], absolute[BF_PATH];
    int r;
    (void)u;
    if (bf_external(ref))
        return BOOK_UNSUPPORTED;
    if ((r = bf_resolve(base, ref, p, sizeof(p))) < 0)
        return r;
    if (base[0] == '/')
    {
        if (snprintf(absolute, sizeof(absolute), "/%s", p) >= (int)sizeof(absolute))
            return BOOK_UNSUPPORTED;
        return bf_file(absolute, s);
    }
    return bf_file(p, s);
}
static int inline_md(const book_sink_t *sink, const book_resources_t *res, const char *path,
                     const char *s, size_t len, unsigned style)
{
    size_t i = 0, start = 0;
    int r;
    unsigned current = style;
    while (i < len)
    {
        size_t mark = i, close, end;
        int image = 0;
        if (s[i] == '\\' && i + 1 < len)
        {
            if ((r = bf_text(sink, s + start, i - start, current)) < 0)
                return r;
            if ((r = bf_text(sink, s + i + 1, 1, current)) < 0)
                return r;
            i += 2;
            start = i;
            continue;
        }
        if (s[i] == '`')
        {
            for (close = i + 1; close < len && s[close] != '`'; ++close)
            {
            }
            if (close < len)
            {
                if ((r = bf_text(sink, s + start, i - start, current)) < 0)
                    return r;
                if ((r = bf_text(sink, s + i + 1, close - i - 1, current | BOOK_STYLE_PREFORMAT)) <
                    0)
                    return r;
                i = close + 1;
                start = i;
                continue;
            }
        }
        if (s[i] == '*' || s[i] == '_')
        {
            size_t k = (i + 1 < len && s[i + 1] == s[i]) ? 2 : 1;
            if ((r = bf_text(sink, s + start, i - start, current)) < 0)
                return r;
            current ^= k == 2 ? BOOK_STYLE_BOLD : BOOK_STYLE_ITALIC;
            i += k;
            start = i;
            continue;
        }
        if (s[i] == '!' && i + 1 < len && s[i + 1] == '[')
        {
            image = 1;
            ++mark;
        }
        if (s[mark] == '[')
        {
            for (close = mark + 1; close < len && s[close] != ']'; ++close)
            {
            }
            if (close + 1 < len && s[close + 1] == '(')
            {
                int nesting = 0;
                for (end = close + 2; end < len; ++end)
                {
                    if (s[end] == '(')
                        ++nesting;
                    if (s[end] == ')')
                    {
                        if (!nesting)
                            break;
                        --nesting;
                    }
                }
                if (end < len)
                {
                    char ref[BF_PATH], target[BF_PATH];
                    size_t a = close + 2, b = end, k;
                    while (a < b && isspace((unsigned char)s[a]))
                        ++a;
                    if (a < b && s[a] == '<')
                    {
                        ++a;
                        b = a;
                        while (b < end && s[b] != '>')
                            ++b;
                    }
                    else
                    {
                        b = a;
                        while (b < end && !isspace((unsigned char)s[b]))
                            ++b;
                    }
                    k = b - a;
                    if (k >= sizeof(ref))
                        return BOOK_UNSUPPORTED;
                    memcpy(ref, s + a, k);
                    ref[k] = 0;
                    if ((r = bf_text(sink, s + start, i - start, current)) < 0)
                        return r;
                    if (image)
                    {
                        r = bf_picture(sink, res, path, ref);
                        if (r < 0)
                            return r;
                    }
                    else
                    {
                        if ((r = bf_resolve(path, ref, target, sizeof(target))) < 0 ||
                            (r = bf_link(sink, target, true)) < 0)
                            return r;
                        if ((r = bf_text(sink, s + mark + 1, close - mark - 1,
                                         current | BOOK_STYLE_LINK)) < 0)
                            return r;
                        if ((r = bf_link(sink, target, false)) < 0)
                            return r;
                    }
                    i = end + 1;
                    start = i;
                    continue;
                }
            }
        }
        ++i;
    }
    return bf_text(sink, s + start, len - start, current);
}
int bf_markdown(const char *path, const book_sink_t *sink)
{
    FILE *f = fopen(path, "rb");
    char *line;
    char id[BF_PATH], title[512];
    int r = BOOK_OK, c = 0, fenced = 0, continuation = 0;
    char fence = 0;
    uint64_t offset = 0;
    book_resources_t res = {NULL, md_open};
    if (!f)
        return BOOK_IO_ERROR;
    line = bf_alloc(8196);
    if (!line)
    {
        fclose(f);
        return BOOK_NO_MEMORY;
    }
    while (r == BOOK_OK)
    {
        size_t n = 0, p = 0, end;
        unsigned heading = 0, style = 0;
        int complete = 0;
        uint64_t begin = offset;
        if ((r = bf_cancel(sink)) < 0)
            break;
        while (n < 8192 && (c = fgetc(f)) != EOF)
        {
            ++offset;
            if (c == '\n')
            {
                complete = 1;
                break;
            }
            if (c != '\r')
                line[n++] = (char)c;
        }
        if (c == EOF)
        {
            if (ferror(f))
            {
                r = BOOK_IO_ERROR;
                break;
            }
            complete = 1;
            if (!n)
                break;
        }
        if (!complete)
        {
            while ((c = fgetc(f)) != EOF)
            {
                if (((unsigned char)c & 0xc0) != 0x80)
                {
                    ungetc(c, f);
                    break;
                }
                line[n++] = (char)c;
                ++offset;
                if (n >= 8195)
                {
                    r = BOOK_ERROR;
                    break;
                }
            }
        }
        if (r < 0)
            break;
        line[n] = 0;
        if (begin == 0 && n >= 3 && !memcmp(line, "\xef\xbb\xbf", 3))
            p = 3;
        if (!continuation)
        {
            snprintf(id, sizeof(id), "md-byte-%llu", (unsigned long long)begin);
            if ((r = bf_anchor(sink, id, NULL, 0)) < 0)
                break;
            while (p < n && line[p] == ' ' && p < 3)
                ++p;
            if (p + 2 < n && (line[p] == '`' || line[p] == '~') && line[p + 1] == line[p] &&
                line[p + 2] == line[p])
            {
                if (!fenced)
                {
                    fenced = 1;
                    fence = line[p];
                }
                else if (fence == line[p])
                    fenced = 0;
                r = bf_para(sink);
                continuation = !complete;
                continue;
            }
        }
        if (fenced)
        {
            r = bf_text(sink, line, n, BOOK_STYLE_PREFORMAT);
            if (r == BOOK_OK && complete)
                r = bf_text(sink, "\n", 1, BOOK_STYLE_PREFORMAT);
            continuation = !complete;
            continue;
        }
        if (!continuation)
        {
            while (p + heading < n && line[p + heading] == '#' && heading < 6)
                ++heading;
            if (heading && p + heading < n && line[p + heading] == ' ')
            {
                p += heading + 1;
                style = BOOK_STYLE_HEADING | BOOK_STYLE_BOLD;
                end = n;
                while (end > p && (line[end - 1] == '#' || line[end - 1] == ' '))
                    --end;
                title[0] = 0;
                bf_append(title, sizeof(title), line + p, end - p);
                {
                    char slug[512], ref[516];
                    size_t k = 0, j;
                    for (j = 0; title[j] && k < sizeof(slug) - 1; ++j)
                    {
                        unsigned char x = (unsigned char)title[j];
                        if (isspace(x))
                            slug[k++] = '-';
                        else if (x >= 128 || isalnum(x) || x == '-' || x == '_')
                            slug[k++] = (char)tolower(x);
                    }
                    slug[k] = 0;
                    snprintf(ref, sizeof(ref), "#%s", slug);
                    r = bf_resolve(path, ref, id, sizeof(id));
                }
                if (r < 0 || (r = bf_anchor(sink, id, NULL, 0)) < 0 ||
                    (r = bf_anchor(sink, id, title, heading)) < 0)
                    break;
                n = end;
            }
            else
                heading = 0;
            if (p < n && line[p] == '>')
            {
                style |= BOOK_STYLE_ITALIC;
                ++p;
                if (p < n && line[p] == ' ')
                    ++p;
            }
            /* Keep list markers and table delimiters as visible structure. */
            if (p < n && line[p] == '|')
            {
                size_t j;
                int separator = 1;
                for (j = p; j < n; ++j)
                    if (line[j] != '|' && line[j] != '-' && line[j] != ':' &&
                        !isspace((unsigned char)line[j]))
                        separator = 0;
                if (separator)
                {
                    r = bf_para(sink);
                    continuation = !complete;
                    continue;
                }
            }
        }
        r = inline_md(sink, &res, path, line + p, n - p, style);
        if (r == BOOK_OK && complete)
            r = bf_para(sink);
        continuation = !complete;
    }
    bf_free(line);
    fclose(f);
    return r;
}
