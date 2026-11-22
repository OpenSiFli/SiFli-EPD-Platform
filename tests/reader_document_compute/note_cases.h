static int note_test_text(void *u, const char *p, size_t n, unsigned style)
{
    output *o = u;
    printf("T %u ", style);
    for (size_t i = 0; i < n; ++i) printf("%02x", (unsigned char)p[i]);
    puts("");
    return text_out(o, p, n, style);
}
static int note_run(int argc, char **argv)
{
    assert(argc >= 4);
    output o = {0}; fake f = {0}; book_compute_selection_t q = {0};
    book_sink_t sink = make_sink(&o); char error[192];
    sink.text = note_test_text;
    f.output = &o; f.short_read = 37;
    q.flags = BOOK_COMPUTE_NOTE; q.note_target = argv[3];
    if (argc > 4) {
        o.cancel_text = atoi(argv[4]) == 1;
        f.cancel_read = atoi(argv[4]) == 2;
        if (atoi(argv[4]) == 3) fail_after = 5;
    }
    int rc = book_compute_convert_at(argv[2], &sink, &allocator, &ops, &f, &q, NULL, error, sizeof(error));
    assert(!live && !f.handles && f.opens == f.closes);
    printf("R %d\n", rc);
    return rc;
}
static int styles_run(const char *path)
{
    output o = {0}; fake f = {0}; book_sink_t sink = make_sink(&o); char error[192];
    sink.text = note_test_text; f.output = &o;
    int rc = book_compute_convert(path, &sink, &allocator, &ops, &f, error, sizeof(error));
    assert(!live && !f.handles && f.opens == f.closes);
    printf("R %d\n", rc); return rc;
}
