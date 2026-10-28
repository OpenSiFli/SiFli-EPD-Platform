/* SPDX-License-Identifier: Apache-2.0 */
#include "book_internal.h"
#include <libxml/entities.h>
#include <libxml/xmlIO.h>

typedef struct xml_run
{
    bf_event event;
    void *user;
    const book_sink_t *sink;
    xmlParserCtxtPtr parser;
    int result, depth, html;
} xml_run;
static xmlParserCtxtPtr active_parser;
long bf_xml_position(void)
{
    long position;
    xmlParserInputPtr input;
    const xmlChar *p;
    unsigned long unused = 0;
    if (!active_parser)
        return -1;
    position = xmlByteConsumed(active_parser);
    if (position >= 0)
        return position;
    input = active_parser->input;
    if (!input || !input->buf || !input->buf->encoder ||
        strncmp(input->buf->encoder->name, "UTF-16", 6))
        return -1;
    /* The read-only library omits output encoders. Count the original UTF-16
     * code units of the parser's unconsumed UTF-8 suffix instead. */
    for (p = input->cur; p < input->end; ++p)
    {
        if ((*p & 0xc0) != 0x80)
            unused += *p >= 0xf0 ? 4 : 2;
    }
    if (input->buf->rawconsumed < unused)
        return -1;
    return (long)(input->buf->rawconsumed - unused);
}
static const char *str(const xmlChar *s)
{
    return s ? (const char *)s : "";
}
static void stop(xml_run *x, int rc)
{
    if (rc < 0 && x->result == BOOK_OK)
    {
        x->result = rc;
        /* The 2.6 HTML push parser reads its current input after a SAX callback.
         * xmlStopParser replaces that input with an empty string. Leave the
         * input intact, suppress further callbacks, and exit after this chunk. */
        if (x->parser)
            x->parser->disableSAX = 1;
    }
}
static void start_ns(void *u, const xmlChar *name, const xmlChar *prefix, const xmlChar *uri,
                     int nn, const xmlChar **nv, int na, int nd, const xmlChar **av)
{
    xml_run *x = u;
    bf_attrs a = {na, av, 0};
    (void)prefix;
    (void)nn;
    (void)nv;
    (void)nd;
    if (x->result < 0)
        return;
    if (++x->depth > BF_DEPTH)
    {
        stop(x, BOOK_UNSUPPORTED);
        return;
    }
    stop(x, x->event(x->user, 1, str(name), str(uri), &a, NULL, 0));
}
static void end_ns(void *u, const xmlChar *name, const xmlChar *prefix, const xmlChar *uri)
{
    xml_run *x = u;
    (void)prefix;
    if (x->result < 0)
        return;
    stop(x, x->event(x->user, 0, str(name), str(uri), NULL, NULL, 0));
    --x->depth;
}
static void start_html(void *u, const xmlChar *name, const xmlChar **av)
{
    xml_run *x = u;
    bf_attrs a = {0, av, 1};
    if (av)
        while (av[a.count * 2])
            ++a.count;
    if (x->result < 0)
        return;
    if (++x->depth > BF_DEPTH)
    {
        stop(x, BOOK_UNSUPPORTED);
        return;
    }
    stop(x, x->event(x->user, 1, str(name), "", &a, NULL, 0));
}
static void end_html(void *u, const xmlChar *name)
{
    end_ns(u, name, NULL, NULL);
}
static void chars(void *u, const xmlChar *p, int n)
{
    xml_run *x = u;
    if (x->result == BOOK_OK)
        stop(x, x->event(x->user, 2, "", "", NULL, (const char *)p, (size_t)n));
}
static void xml_error(void *u, xmlErrorPtr error)
{
    xml_run *x = u;
    if (error && error->level >= XML_ERR_ERROR && (!x->html || error->code == XML_ERR_NO_MEMORY))
        stop(x, error->code == XML_ERR_NO_MEMORY ? BOOK_NO_MEMORY : BOOK_ERROR);
}
static void quiet(void *u, const char *fmt, ...)
{
    (void)u;
    (void)fmt;
}
static xmlEntityPtr entity(void *u, const xmlChar *name)
{
    (void)u;
    return xmlGetPredefinedEntity(name);
}
static void declaration(void *u, const xmlChar *name, int type, const xmlChar *publicid,
                        const xmlChar *systemid, xmlChar *content)
{
    (void)name;
    (void)type;
    (void)publicid;
    (void)systemid;
    (void)content;
    stop(u, BOOK_UNSUPPORTED);
}
int bf_attr(const bf_attrs *a, const char *name, const char *ns, char *out, size_t cap)
{
    int i;
    out[0] = 0;
    if (!a)
        return 0;
    for (i = 0; i < a->count; ++i)
    {
        const xmlChar **v = a->values + i * (a->html ? 2 : 5);
        const char *local = str(v[0]);
        size_t n;
        if (a->html)
        {
            const char *colon = strchr(local, ':');
            if (colon)
                local = colon + 1;
        }
        if (strcmp(local, name) || (!a->html && ns && strcmp(str(v[2]), ns)))
            continue;
        n = a->html ? strlen(str(v[1])) : (size_t)(v[4] - v[3]);
        if (n >= cap)
            return BOOK_UNSUPPORTED;
        if (n)
            memcpy(out, a->html ? v[1] : v[3], n);
        out[n] = 0;
        return 1;
    }
    return 0;
}
int bf_xml(book_stream_t *s, const book_sink_t *sink, bf_event event, void *user, int html)
{
    xmlSAXHandler sax;
    xml_run x;
    char *buffer;
    int n, rc;
    xmlParserCtxtPtr previous = active_parser;
    buffer = bf_alloc(4096);
    if (!buffer)
        return BOOK_NO_MEMORY;
    memset(&sax, 0, sizeof(sax));
    memset(&x, 0, sizeof(x));
    x.event = event;
    x.user = user;
    x.sink = sink;
    x.html = html;
    sax.characters = chars;
    sax.cdataBlock = chars;
    sax.ignorableWhitespace = chars;
    sax.warning = quiet;
    sax.error = quiet;
    sax.fatalError = quiet;
    sax.serror = xml_error;
    sax.entityDecl = declaration;
    sax.getEntity = entity;
    if (html)
    {
        sax.startElement = start_html;
        sax.endElement = end_html;
        x.parser = (xmlParserCtxtPtr)htmlCreatePushParserCtxt(&sax, &x, NULL, 0, NULL,
                                                              XML_CHAR_ENCODING_UTF8);
        if (x.parser)
            htmlCtxtUseOptions((htmlParserCtxtPtr)x.parser, HTML_PARSE_RECOVER | HTML_PARSE_NONET |
                                                                HTML_PARSE_NOERROR |
                                                                HTML_PARSE_NOWARNING);
    }
    else
    {
        sax.initialized = XML_SAX2_MAGIC;
        sax.startElementNs = start_ns;
        sax.endElementNs = end_ns;
        x.parser = xmlCreatePushParserCtxt(&sax, &x, NULL, 0, NULL);
        if (x.parser)
            xmlCtxtUseOptions(x.parser, XML_PARSE_NONET | XML_PARSE_COMPACT | XML_PARSE_NOERROR |
                                            XML_PARSE_NOWARNING);
    }
    if (!x.parser)
    {
        bf_free(buffer);
        return BOOK_NO_MEMORY;
    }
    active_parser = x.parser;
    while (x.result == BOOK_OK)
    {
        if ((rc = bf_cancel(sink)) < 0)
        {
            x.result = rc;
            break;
        }
        n = s->read(s->user, buffer, 4096);
        if (n < 0)
        {
            x.result = n;
            break;
        }
        rc = html ? htmlParseChunk((htmlParserCtxtPtr)x.parser, buffer, n, n == 0)
                  : xmlParseChunk(x.parser, buffer, n, n == 0);
        if (bf_oom())
        {
            x.result = BOOK_NO_MEMORY;
            break;
        }
        if (rc && !html && x.result == BOOK_OK)
            x.result = BOOK_ERROR;
        if (!n)
            break;
    }
    if (html)
        htmlFreeParserCtxt((htmlParserCtxtPtr)x.parser);
    else
        xmlFreeParserCtxt(x.parser);
    active_parser = previous;
    bf_free(buffer);
    return x.result;
}
int bf_xml_entry(bf_zip *z, const char *name, const book_sink_t *sink, bf_event fn, void *u)
{
    book_stream_t s;
    int r = bf_zip_entry(z, name, &s);
    if (r < 0)
        return r;
    r = bf_xml(&s, sink, fn, u, 0);
    return bf_finish(&s, r);
}
