/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"
#include <libxml/entities.h>
#include <libxml/parserInternals.h>

typedef struct svg_buffer
{
    char *data;
    size_t length, capacity;
} svg_buffer;

static int append(svg_buffer *b, const char *text, size_t length)
{
    size_t needed, capacity;
    char *next;
    if (length > BF_SVG_LIMIT - b->length)
        return BOOK_UNSUPPORTED;
    needed = b->length + length + 1;
    if (needed > b->capacity)
    {
        capacity = b->capacity ? b->capacity : 1024;
        while (capacity < needed)
            capacity = capacity > (BF_SVG_LIMIT + 1) / 2 ? BF_SVG_LIMIT + 1 : capacity * 2;
        next = bf_realloc(b->data, capacity);
        if (!next)
            return BOOK_NO_MEMORY;
        b->data = next;
        b->capacity = capacity;
    }
    if (length) memcpy(b->data + b->length, text, length);
    b->length += length;
    b->data[b->length] = 0;
    return BOOK_OK;
}

static const char *str(const xmlChar *s)
{
    return s ? (const char *)s : "";
}

static int literal(svg_buffer *b, const char *s)
{
    return append(b, s, strlen(s));
}

static int escape(svg_buffer *b, const char *s, size_t length, int attribute)
{
    size_t start = 0, i;
    int r;
    for (i = 0; i < length; ++i)
    {
        const char *replacement = s[i] == '&' ? "&amp;" : s[i] == '<' ? "&lt;" :
            s[i] == '>' ? "&gt;" : s[i] == '\r' ? "&#13;" :
            attribute && s[i] == '"' ? "&quot;" :
            attribute && s[i] == '\n' ? "&#10;" : attribute && s[i] == '\t' ? "&#9;" : NULL;
        if (!replacement) continue;
        if ((r = append(b, s + start, i - start)) < 0 ||
            (r = literal(b, replacement)) < 0) return r;
        start = i + 1;
    }
    return append(b, s + start, length - start);
}

static int qname(svg_buffer *b, const xmlChar *prefix, const xmlChar *name)
{
    int r;
    if (prefix && *prefix)
    {
        if ((r = literal(b, str(prefix))) < 0 || (r = literal(b, ":")) < 0) return r;
    }
    return literal(b, str(name));
}

static int namespace(svg_buffer *b, const xmlChar *prefix, const xmlChar *uri)
{
    int r;
    if ((r = literal(b, " ")) < 0 ||
        (r = qname(b, prefix ? BAD_CAST "xmlns" : NULL, prefix ? prefix : BAD_CAST "xmlns")) < 0 ||
        (r = literal(b, "=\"")) < 0 ||
        (r = escape(b, str(uri), strlen(str(uri)), 1)) < 0) return r;
    return literal(b, "\"");
}

typedef struct markup_xml
{
    xmlParserCtxtPtr parser;
    const book_sink_t *sink;
    const book_resources_t *resources;
    const char *base;
    bf_event event;
    void *user;
    svg_buffer svg;
    xmlEntity html_entity;
    xmlChar entity_text[8];
    int result, depth, svg_depth, html, decided, use_html;
} markup_xml;

static void stop(markup_xml *x, int result)
{
    if (result < 0 && x->result == BOOK_OK)
    {
        x->result = result;
        /* Keep the HTML push parser's current input alive until it returns. */
        if (x->parser) x->parser->disableSAX = 1;
    }
}

static int start_svg(markup_xml *x, const xmlChar *name, const xmlChar *prefix,
                     int nn, const xmlChar **nv, int na, const xmlChar **av)
{
    int i, j, r;
    svg_buffer *b = &x->svg;
    if ((r = literal(b, "<")) < 0 || (r = qname(b, prefix, name)) < 0) return r;
    if (x->svg_depth == x->depth)
    {
        /* Copy all effective bindings: prefixes can be declared on XHTML ancestors,
         * including prefixes used only by QName-valued attributes in this subtree. */
        nn = x->parser->nsNr / 2;
        nv = x->parser->nsTab;
    }
    for (i = 0; i < nn; ++i)
    {
        for (j = i + 1; j < nn; ++j)
            if (!strcmp(str(nv[i * 2]), str(nv[j * 2]))) break;
        if (j != nn) continue;
        if ((r = namespace(b, nv[i * 2], nv[i * 2 + 1])) < 0) return r;
    }
    for (i = 0; i < na; ++i)
    {
        const xmlChar **v = av + i * 5;
        if ((r = literal(b, " ")) < 0 || (r = qname(b, v[1], v[0])) < 0 ||
            (r = literal(b, "=\"")) < 0 ||
            (r = escape(b, str(v[3]), (size_t)(v[4] - v[3]), 1)) < 0 ||
            (r = literal(b, "\"")) < 0) return r;
    }
    return literal(b, ">");
}

static void start_ns(void *u, const xmlChar *name, const xmlChar *prefix, const xmlChar *uri,
                     int nn, const xmlChar **nv, int na, int nd, const xmlChar **av)
{
    markup_xml *x = u;
    bf_attrs attrs = {na, av, 0};
    (void)nd;
    if (x->result < 0 || x->use_html) return;
    if (!x->decided)
    {
        x->decided = 1;
        if (strcmp(str(uri), "http://www.w3.org/1999/xhtml") &&
            strcmp(str(uri), "http://www.w3.org/2000/svg"))
        {
            x->use_html = 1;
            xmlStopParser(x->parser);
            return;
        }
    }
    if (++x->depth > BF_DEPTH) { stop(x, BOOK_UNSUPPORTED); return; }
    stop(x, bf_cancel(x->sink));
    if (x->result < 0) return;
    if (!x->svg_depth && !strcmp(str(name), "svg"))
    {
        if (!x->sink->svg) { stop(x, BOOK_UNSUPPORTED); return; }
        x->svg_depth = x->depth;
    }
    if (x->svg_depth)
        stop(x, start_svg(x, name, prefix, nn, nv, na, av));
    else
        stop(x, x->event(x->user, 1, str(name), str(uri), &attrs, NULL, 0));
}

static void end_ns(void *u, const xmlChar *name, const xmlChar *prefix, const xmlChar *uri)
{
    markup_xml *x = u;
    int r;
    if (x->result < 0 || x->use_html) return;
    if (x->svg_depth)
    {
        r = literal(&x->svg, "</");
        if (r == BOOK_OK) r = qname(&x->svg, prefix, name);
        if (r == BOOK_OK) r = literal(&x->svg, ">");
        if (x->depth == x->svg_depth)
        {
            if (r == BOOK_OK) r = bf_cancel(x->sink);
            if (r == BOOK_OK)
                r = x->sink->svg(x->sink->user, x->svg.data, x->svg.length, x->base, x->resources);
            bf_free(x->svg.data);
            memset(&x->svg, 0, sizeof(x->svg));
            x->svg_depth = 0;
        }
        stop(x, r);
    }
    else
        stop(x, x->event(x->user, 0, str(name), str(uri), NULL, NULL, 0));
    --x->depth;
}

static void start_html(void *u, const xmlChar *name, const xmlChar **av)
{
    markup_xml *x = u;
    bf_attrs attrs = {0, av, 1};
    if (x->result < 0) return;
    /* libxml's legacy HTML parser does not preserve SVG case or tree structure. */
    if (!strcmp(str(name), "svg")) { stop(x, BOOK_UNSUPPORTED); return; }
    if (++x->depth > BF_DEPTH) { stop(x, BOOK_UNSUPPORTED); return; }
    if (av) while (av[attrs.count * 2]) ++attrs.count;
    stop(x, x->event(x->user, 1, str(name), "", &attrs, NULL, 0));
}

static void end_html(void *u, const xmlChar *name)
{
    end_ns(u, name, NULL, NULL);
}

static void chars(void *u, const xmlChar *p, int n)
{
    markup_xml *x = u;
    if (x->result < 0 || !x->decided || x->use_html) return;
    stop(x, bf_cancel(x->sink));
    if (x->result < 0) return;
    stop(x, x->svg_depth ? escape(&x->svg, str(p), (size_t)n, 0) :
         x->event(x->user, 2, "", "", NULL, str(p), (size_t)n));
}

static void comment(void *u, const xmlChar *text)
{
    markup_xml *x = u;
    int r;
    if (x->result < 0 || !x->svg_depth) return;
    r = literal(&x->svg, "<!--");
    if (r == BOOK_OK) r = literal(&x->svg, str(text));
    if (r == BOOK_OK) r = literal(&x->svg, "-->");
    stop(x, r);
}

static void instruction(void *u, const xmlChar *target, const xmlChar *data)
{
    markup_xml *x = u;
    int r;
    if (x->result < 0 || !x->svg_depth) return;
    r = literal(&x->svg, "<?");
    if (r == BOOK_OK) r = literal(&x->svg, str(target));
    if (r == BOOK_OK && data) r = literal(&x->svg, " ");
    if (r == BOOK_OK) r = literal(&x->svg, str(data));
    if (r == BOOK_OK) r = literal(&x->svg, "?>");
    stop(x, r);
}

static void quiet(void *u, const char *format, ...)
{
    (void)u;
    (void)format;
}

static void error(void *u, xmlErrorPtr e)
{
    markup_xml *x = u;
    if (!e || e->level < XML_ERR_ERROR) return;
    if (e->code == XML_ERR_NO_MEMORY) stop(x, BOOK_NO_MEMORY);
    else if (!x->html && x->svg_depth) stop(x, BOOK_ERROR);
}

static xmlEntityPtr entity(void *u, const xmlChar *name)
{
    markup_xml *x = u;
    xmlEntityPtr predefined = xmlGetPredefinedEntity(name);
    const htmlEntityDesc *html;
    int n;
    if (predefined) return predefined;
    html = htmlEntityLookup(name);
    if (!html) return NULL;
    n = xmlCopyCharMultiByte(x->entity_text, (int)html->value);
    x->entity_text[n] = 0;
    /* A nameless borrowed SAX entity is emitted directly as UTF-8 text. General
     * entity type also makes libxml decode all UTF-8 bytes inside attributes. */
    memset(&x->html_entity, 0, sizeof(x->html_entity));
    x->html_entity.type = XML_ENTITY_DECL;
    x->html_entity.etype = XML_INTERNAL_GENERAL_ENTITY;
    x->html_entity.content = x->entity_text;
    x->html_entity.length = n;
    return &x->html_entity;
}

static void declaration(void *u, const xmlChar *name, int type, const xmlChar *publicid,
                        const xmlChar *systemid, xmlChar *content)
{
    (void)name; (void)type; (void)publicid; (void)systemid; (void)content;
    stop(u, BOOK_UNSUPPORTED);
}

static int create_parser(markup_xml *x)
{
    xmlSAXHandler sax;
    memset(&sax, 0, sizeof(sax));
    sax.characters = chars;
    sax.ignorableWhitespace = chars;
    sax.cdataBlock = chars;
    sax.comment = comment;
    sax.processingInstruction = instruction;
    sax.warning = sax.error = sax.fatalError = quiet;
    sax.serror = error;
    sax.entityDecl = declaration;
    sax.getEntity = entity;
    if (x->html)
    {
        sax.startElement = start_html;
        sax.endElement = end_html;
        x->parser = (xmlParserCtxtPtr)htmlCreatePushParserCtxt(&sax, x, NULL, 0, NULL,
                                                             XML_CHAR_ENCODING_UTF8);
        if (x->parser)
            htmlCtxtUseOptions((htmlParserCtxtPtr)x->parser, HTML_PARSE_RECOVER | HTML_PARSE_NONET |
                              HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    }
    else
    {
        sax.initialized = XML_SAX2_MAGIC;
        sax.startElementNs = start_ns;
        sax.endElementNs = end_ns;
        x->parser = xmlCreatePushParserCtxt(&sax, x, NULL, 0, NULL);
        if (x->parser)
            xmlCtxtUseOptions(x->parser, XML_PARSE_RECOVER | XML_PARSE_NONET | XML_PARSE_COMPACT | XML_PARSE_NOENT |
                             XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    }
    return x->parser ? BOOK_OK : BOOK_NO_MEMORY;
}

int bf_svg_markup(book_stream_t *s, const book_sink_t *sink, bf_event event, void *user,
                  const char *base, const book_resources_t *resources)
{
    markup_xml x;
    char *buffer = bf_alloc(BF_TEXT);
    int n, rc, used = 0, fed = 0;
    if (!buffer) return BOOK_NO_MEMORY;
    memset(&x, 0, sizeof(x));
    x.sink = sink; x.resources = resources; x.base = base; x.event = event; x.user = user;
    x.result = create_parser(&x);
    /* Probe with libxml itself. The same input buffer is replayed into the HTML
     * parser when the root is legacy HTML; no document or event list is retained. */
    while (x.result == BOOK_OK && !x.decided && !x.use_html)
    {
        if ((x.result = bf_cancel(sink)) < 0) break;
        n = s->read(s->user, buffer + used, (size_t)(BF_TEXT - used));
        if (n < 0) { x.result = n; break; }
        used += n;
        if (used < 9 && n) continue;
        /* XML declarations and UTF-16/32 signatures select XML before the root. */
        if (!fed && ((used >= 6 && !memcmp(buffer, "<?xml", 5) && isspace((unsigned char)buffer[5])) ||
            (used >= 9 && !memcmp(buffer, "\xef\xbb\xbf<?xml", 8) && isspace((unsigned char)buffer[8])) ||
            (used >= 2 && ((unsigned char)buffer[0] == 0xff ||
                          (unsigned char)buffer[0] == 0xfe || !buffer[0] || !buffer[1]))))
            x.decided = 1;
        rc = xmlParseChunk(x.parser, buffer + fed, used - fed, n == 0);
        fed = used;
        if (bf_oom()) x.result = BOOK_NO_MEMORY;
        if (x.decided && !x.use_html) break;
        if (!rc && used == BF_TEXT && (bf_suffix(base, ".xhtml") || bf_suffix(base, ".xml")))
            x.decided = 1;
        else if (rc || !n || used == BF_TEXT) x.use_html = 1;
    }
    if (x.result == BOOK_OK && x.use_html)
    {
        xmlFreeParserCtxt(x.parser);
        x.parser = NULL;
        x.html = x.decided = 1;
        x.use_html = 0;
        x.result = create_parser(&x);
        if (x.result == BOOK_OK)
            htmlParseChunk((htmlParserCtxtPtr)x.parser, buffer, used, 0);
    }
    while (x.result == BOOK_OK)
    {
        if ((x.result = bf_cancel(sink)) < 0) break;
        n = s->read(s->user, buffer, BF_TEXT);
        if (n < 0) { x.result = n; break; }
        if (x.html) htmlParseChunk((htmlParserCtxtPtr)x.parser, buffer, n, n == 0);
        else xmlParseChunk(x.parser, buffer, n, n == 0);
        if (bf_oom()) x.result = BOOK_NO_MEMORY;
        if (!n) break;
    }
    if (x.svg_depth && x.result == BOOK_OK) x.result = BOOK_ERROR;
    if (x.html) htmlFreeParserCtxt((htmlParserCtxtPtr)x.parser);
    else xmlFreeParserCtxt(x.parser);
    bf_free(x.svg.data);
    bf_free(buffer);
    return x.result;
}

int bf_svg_stream(book_stream_t *s, const book_sink_t *sink, const char *base,
                  const book_resources_t *resources)
{
    svg_buffer b = {0};
    char chunk[4096];
    int r, n;
    if (!sink->svg) return BOOK_UNSUPPORTED;
    if (!s->read) return BOOK_IO_ERROR;
    if (s->size > BF_SVG_LIMIT) return BOOK_UNSUPPORTED;
    while ((r = bf_cancel(sink)) == BOOK_OK)
    {
        n = s->read(s->user, chunk, sizeof(chunk));
        if (n <= 0) { r = n; break; }
        if ((r = append(&b, chunk, (size_t)n)) < 0) break;
    }
    if (r == BOOK_OK) r = bf_cancel(sink);
    if (r == BOOK_OK)
        r = b.length ? sink->svg(sink->user, b.data, b.length, base, resources) : BOOK_ERROR;
    bf_free(b.data);
    return r;
}
