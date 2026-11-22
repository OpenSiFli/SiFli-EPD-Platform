/* Included after the fake resource harness. */
typedef struct selected_output {
    output base;
    FILE *events;
    uint32_t source;
    int located, fault;
} selected_output;
static int selected_location(void *u, uint32_t index, uint32_t actual, uint32_t requested,
                              unsigned flags, const char *id)
{
    selected_output *o = u;
    fprintf(o->events, "L %u %u %u %u %s\n", index, actual, requested, flags, id);
    o->source = actual; o->located = 1;
    if ((o->fault == 3 && (flags & BOOK_SOURCE_TARGET)) ||
        (o->fault == 4 && flags == BOOK_SOURCE_BODY_UTF8) ||
        (o->fault == 7 && (flags & BOOK_SOURCE_BEGIN))) return BOOK_NO_SPACE;
    return BOOK_OK;
}
static int selected_image(void *u, const char *id, book_stream_t *stream)
{
    selected_output *o = u;
    assert(o->located);
    fprintf(o->events, "I %u %s\n", o->source, id);
    o->located = 0;
    if (o->fault == 1) return BOOK_NO_SPACE;
    if (o->fault == 5) o->base.cancel = 1;
    return image_out(&o->base, id, stream);
}
static int selected_svg(void *u, const char *xml, size_t n, const char *base,
                        const book_resources_t *resources)
{
    selected_output *o = u;
    assert(o->located);
    fprintf(o->events, "S %u %s %zu\n", o->source, base, n);
    o->located = 0;
    if (o->fault == 2) return BOOK_NO_SPACE;
    if (o->fault == 6) o->base.cancel = 1;
    return svg_out(&o->base, xml, n, base, resources);
}
static int selected_text(void *u, const char *p, size_t n, unsigned style)
{
    selected_output *o = u; size_t i;
    assert(o->located && n <= BOOK_COMPUTE_SOURCE_TEXT_MAX);
    fprintf(o->events, "T %u %u ", o->source, style);
    for (i = 0; i < n; ++i) fprintf(o->events, "%02x", (unsigned char)p[i]);
    fputc('\n', o->events); o->located = 0;
    return text_out(&o->base, p, n, style);
}
static int selected_chapter(void *u, uint32_t index, uint32_t total, book_format_t format,
                            bool selected, uint32_t source_bytes, const char *id, const char *title)
{
    selected_output *o = u; (void)title;
    fprintf(o->events, "C %u %u %u %u %s\n", index, total, format, selected, id);
    return BOOK_OK;
}
static int selected_toc(void *u, uint32_t index, uint32_t total, const char *id,
                        const char *title, unsigned level)
{
    selected_output *o = u; (void)title;
    fprintf(o->events, "N %u %u %u %s\n", index, total, level, id);
    return BOOK_OK;
}
static int selected_anchor(void *u, const char *id, const char *title, unsigned level)
{
    selected_output *o = u;
    fprintf(o->events, "A %u %s ", level, id);
    if (title && *title) {
        for (const unsigned char *p = (const unsigned char *)title; *p; ++p)
            fprintf(o->events, "%02x", *p);
    } else fputc('-', o->events);
    fputc('\n', o->events);
    return BOOK_OK;
}
static int selected_run(int argc, char **argv)
{
    selected_output o = {0}; fake f = {0}; char error[192]; int rc;
    book_sink_t sink = make_sink(&o.base);
    book_compute_selection_t q = {0};
    book_compute_observer_t observer = {&o, selected_chapter, selected_toc, selected_location};
    assert(argc >= 9 && argc <= 12);
    q.spine = (uint32_t)strtoul(argv[4], NULL, 10);
    q.anchor_byte = (uint32_t)strtoul(argv[5], NULL, 10);
    q.skip_utf8 = (uint32_t)strtoul(argv[6], NULL, 10);
    q.context_bytes = (uint32_t)strtoul(argv[7], NULL, 10);
    q.slice_bytes = (uint32_t)strtoul(argv[8], NULL, 10);
    q.format_hint = argc >= 10 ? (uint32_t)strtoul(argv[9], NULL, 10) : 0;
    q.flags = BOOK_COMPUTE_SELECT_CHAPTER | (q.context_bytes ? BOOK_COMPUTE_CONTEXT : 0);
    if (argc >= 11 && atoi(argv[10])) q.flags |= BOOK_COMPUTE_FULL_SOURCE;
    if (argc >= 11 && atoi(argv[10]) == 2) o.base.cancel_text = 1;
    if (argc == 12) o.fault = atoi(argv[11]);
    f.output = &o.base; f.short_read = 7;
    if (strstr(argv[2], ".txt") && q.anchor_byte > 65544 && q.anchor_byte < lookup(argv[2])->size)
        f.read_floor = q.anchor_byte - 65544;
    o.events = fopen(argv[3], "wb"); assert(o.events);
    sink.user = &o; sink.text = selected_text;
    sink.image = selected_image; sink.svg = selected_svg; sink.anchor = selected_anchor;
    rc = book_compute_convert_at(argv[2], &sink, &allocator, &ops, &f, &q, &observer, error, sizeof(error));
    assert(!live && !f.handles && f.opens == f.closes && f.dirs == f.dir_closes);
    fprintf(o.events, "R %d\n", rc); fclose(o.events);
    return rc;
}
static int selected_fault_run(fake *f, const char *path, book_compute_selection_t *q)
{
    book_sink_t sink = make_sink(f->output); char error[160];
    int rc = book_compute_convert_at(path, &sink, &allocator, &ops, f, q, NULL, error, sizeof(error));
    assert(!live && !f->handles && f->opens == f->closes && f->dirs == f->dir_closes);
    return rc;
}
static void selected_faults(const char *path)
{
    output o = {0}; fake f = {0}; book_compute_selection_t q = {0}; size_t count;
    q.flags = BOOK_COMPUTE_SELECT_CHAPTER | BOOK_COMPUTE_CONTEXT;
    if (strstr(path, ".epub") || strstr(path, "split")) { q.spine = 1; q.anchor_byte = UINT32_MAX; }
    else if (strstr(path, ".txt")) { q.anchor_byte = 15000; q.skip_utf8 = 37; }
#define SELECT_RESET() do { memset(&o, 0, sizeof(o)); memset(&f, 0, sizeof(f)); f.output = &o; allocations = 0; fail_after = SIZE_MAX; } while (0)
    SELECT_RESET(); assert(selected_fault_run(&f, path, &q) == BOOK_OK); count = allocations;
    for (size_t i = 0; i < count; i += i < 64 ? 1 : 13)
    { SELECT_RESET(); fail_after = i; assert(selected_fault_run(&f, path, &q) == BOOK_NO_MEMORY); }
    SELECT_RESET(); o.cancel_text = 1; assert(selected_fault_run(&f, path, &q) == BOOK_CANCELLED);
    SELECT_RESET(); o.fail_text = 1; assert(selected_fault_run(&f, path, &q) == BOOK_NO_SPACE);
    SELECT_RESET(); f.cancel_read = 1; assert(selected_fault_run(&f, path, &q) == BOOK_CANCELLED);
    SELECT_RESET(); assert(selected_fault_run(&f, path, &q) == BOOK_OK);
    printf("selection/EOF context cancellation and allocation faults: %s\n", path);
#undef SELECT_RESET
}
