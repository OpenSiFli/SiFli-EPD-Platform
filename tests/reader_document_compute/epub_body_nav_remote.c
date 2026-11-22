/* SPDX-License-Identifier: Apache-2.0 */
#define main remote_standard_main
#include "remote_host.c"
#undef main

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--io-failure")) {
        select_flags = 163; select_spine = UINT32_MAX; bad_response = 1;
        assert(run(argv[2], 0, 0, 0, 0) == BOOK_IO_ERROR);
        puts("PASS entry probing preserves resource I/O failure"); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--invalid-flags")) {
        const uint32_t flags[] = {32, 64, 97, 128, 129, 67|4, 35|8, 35|16, 65|128};
        for (unsigned i = 0; i < sizeof(flags)/sizeof(flags[0]); ++i) {
            select_flags = flags[i]; assert(run("/invalid.epub", 0, 0, 0, 0) == EPD_COMPUTE_INVALID);
        }
        select_flags = 163; select_anchor = 1;
        assert(run("/invalid.epub", 0, 0, 0, 0) == EPD_COMPUTE_INVALID);
        select_anchor = 0; select_skip = 1;
        assert(run("/invalid.epub", 0, 0, 0, 0) == EPD_COMPUTE_INVALID);
        puts("PASS 11 incompatible mode/entry flags rejected without allocation"); return 0;
    }
    assert(argc == 8 || argc == 9);
    select_flags = (uint32_t)strtoul(argv[2], NULL, 10);
    select_spine = (uint32_t)strtoul(argv[3], NULL, 10);
    select_anchor = (uint32_t)strtoul(argv[4], NULL, 10);
    select_skip = (uint32_t)strtoul(argv[5], NULL, 10);
    if (argc == 9) note_target = argv[8];
    transcript = fopen(argv[6], "wb"); assert(transcript);
    int rc = run(argv[1], 0, 0, 0, 0);
    fclose(transcript); transcript = NULL;
    printf("PHASE result=%d peak=%u chapters=%u toc=%u images=%d text=%zu target=%u end=%u\n",
        rc, message.values[1], chapters, toc_count, images, total_text, actual_target, end_offset);
    assert(rc == atoi(argv[7]));
    assert(message.values[1] <= BOOK_DOCUMENT_DEFAULT_BUDGET);
    return 0;
}
