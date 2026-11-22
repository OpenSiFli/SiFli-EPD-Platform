/* SPDX-License-Identifier: Apache-2.0 */
static void descriptor_budget_tests(void)
{
    assert(!documents && !fd_live);
    snprintf(fd_source_path, sizeof(fd_source_path), "%s/fd-source.bin", test_root);
    FILE *file = fopen(fd_source_path, "wb"); assert(file);
    for (unsigned i = 0; i < 65536; ++i) fputc((i * 17 + i / 256) & 255, file);
    assert(!fclose(file));
    document_t contexts[4] = {0}; document_reader_t *readers[3][4];
    fd_limit = 16; fd_reserved = 3; fd_peak = 0;
    for (unsigned i = 0; i < 4; ++i) {
        contexts[i].input = contexts[i].output = -1;
        contexts[i].serial = i + 1;
        contexts[i].status.end = 32768;
        strcpy(contexts[i].cache, fd_source_path);
        if (i < 3) contexts[i].next = &contexts[i + 1];
    }
    documents = contexts;
    contexts[3].output = open(fd_source_path, O_WRONLY); assert(contexts[3].output >= 0);
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j) {
            readers[i][j] = document_reader_create(&contexts[i]); assert(readers[i][j]);
        }
    size_t capacity = psram_capacity; psram_capacity = 0;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j) {
            unsigned at = j * 8192 + i; uint8_t value;
            assert(document_reader_read(readers[i][j], at, &value, 1));
            assert(value == ((at * 17 + at / 256) & 255));
        }
    document_remote_host_t host = {0}; host.document = &contexts[3];
    for (unsigned i = 0; i < DOCUMENT_RESOURCE_LIMIT; ++i) host.resources[i].descriptor = -1;
    epd_compute_pipe_message_t m = {0}; uint8_t data[EPD_COMPUTE_PIPE_BYTES];
    m.operation = EPD_PIPE_OPEN; strcpy(m.path, "/flash/fd-source.epub");
    int rc = remote_service(&host, &m, data);
    printf("FD pressure: result=%d live=%u reserved=%u limit=%u\n", rc, fd_live, fd_reserved, fd_limit);
    assert(rc == BOOK_OK && fd_live == 5);
    unsigned source_handle = m.handle;
    m.operation = EPD_PIPE_READ; m.offset = 123; m.length = 1;
    assert(remote_service(&host, &m, data) == BOOK_OK && data[0] == ((123 * 17) & 255));
    fd_read_error = EIO; m.length = 1;
    assert(remote_service(&host, &m, data) == BOOK_IO_ERROR);
    assert(strstr(fd_last_log, "phase=read") && strstr(fd_last_log, "cache_fds=4"));
    fd_read_error = 0; fd_seek_error = EIO;
    assert(remote_service(&host, &m, data) == BOOK_IO_ERROR && strstr(fd_last_log, "phase=seek"));
    fd_seek_error = 0;
    /* The writer and source plus external descriptors remain exhausted even
     * after the three cache read descriptors are released. */
    fd_reserved = fd_limit - 2;
    m.operation = EPD_PIPE_OPEN;
    assert(remote_service(&host, &m, data) == BOOK_IO_ERROR && strstr(fd_last_log, "phase=open"));
    char expected[32]; snprintf(expected, sizeof(expected), "errno=%d", -ENOMEM);
    assert(strstr(fd_last_log, expected) && strstr(fd_last_log, "/flash/fd-source.epub"));
    assert(strstr(fd_pressure_log, "released=3 recovered=0"));
    fd_reserved = 3; m.operation = EPD_PIPE_CLOSE; m.handle = source_handle;
    assert(remote_service(&host, &m, data) == BOOK_OK);
    for (unsigned i = 0; i < 3; ++i) {
        contexts[i].status.end = 65536;
        for (unsigned round = 0; round < 3; ++round)
            for (unsigned j = 0; j < 4; ++j) {
                unsigned at = 32768 + ((j + round) % 4) * 8192 + i; uint8_t value;
                assert(document_reader_read(readers[i][j], at, &value, 1));
                assert(value == ((at * 17 + at / 256) & 255));
            }
        document_reader_destroy(readers[i][0]);
        readers_close(&contexts[i]);
        uint8_t value; assert(document_reader_read(readers[i][1], 1, &value, 1) && value == 17);
        for (unsigned j = 1; j < 4; ++j) document_reader_destroy(readers[i][j]);
    }
    assert(close(contexts[3].output) == 0 && !fd_live);
    documents = NULL; psram_capacity = capacity; fd_reserved = 0; fd_limit = 512;
    assert(!allocated_bytes); _unlink(fd_source_path);
    printf("FD budget/cursor interleaving/growth/reopen/destroy/error diagnostics passed; peak=%u (+3 reserved)\n", fd_peak);
}

static void descriptor_error_tests(void)
{
    const int results[] = {BOOK_IO_ERROR, BOOK_NO_MEMORY, BOOK_UNSUPPORTED, BOOK_ERROR};
    for (unsigned i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        document_t *d = lifecycle_open(false); pipe_done_error = results[i];
        pump(); assert_terminal(d, results[i]);
        assert(!strcmp(d->status.error, "Source I/O failed"));
        if (results[i] == BOOK_IO_ERROR) assert(d->status.error_code == READER_ERROR_IO);
        pipe_done_error = 0; lifecycle_close(d);
    }
    for (unsigned i = 0; i < 2; ++i) {
        document_t *d = lifecycle_open(false); pipe_done_error = BOOK_IO_ERROR;
        pipe_bad_done = i == 0; pipe_terminal_status = i == 1 ? EPD_COMPUTE_INVALID : 0;
        pump(); assert_terminal(d, BOOK_ERROR);
        pipe_done_error = pipe_terminal_status = 0; pipe_bad_done = false; lifecycle_close(d);
    }
    assert(!fd_live);
    puts("Component-error DONE preserves book result/payload; malformed DONE and transport error rejected");
}
