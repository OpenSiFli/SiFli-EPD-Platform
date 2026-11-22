/* SPDX-License-Identifier: Apache-2.0 */
#define main remote_regression_main
#include "remote_host.c"
#undef main

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc >= 4);
    select_flags = BOOK_DOCUMENT_SELECT_CHAPTER;
    select_spine = (uint32_t)strtoul(argv[3], NULL, 10);
    if (!strcmp(argv[1], "--faults")) {
        assert(run(argv[2], 0, 0, 0, 0) == BOOK_OK);
        size_t count = allocation_count, checked = 0;
        for (size_t i = 0; i < count; i += i < 48 ? 1 : 19) {
            fail_allocation = i;
            int rc = run(argv[2], 0, 0, 0, 0);
            assert(rc == BOOK_NO_MEMORY || (i == 0 && rc == EPD_COMPUTE_NO_MEMORY));
            ++checked;
        }
        fail_allocation = SIZE_MAX;
        assert(run(argv[2], EPD_PIPE_READ, 0, 0, 0) == BOOK_CANCELLED);
        assert(run(argv[2], EPD_PIPE_OUTPUT, 0, 0, 0) == BOOK_CANCELLED);
        assert(run(argv[2], EPD_PIPE_OUTPUT, 1, 0, 0) == BOOK_CANCELLED);
        assert(run(argv[2], 0, 0, 1, 0) == BOOK_NO_SPACE);
        assert(run(argv[2], 0, 0, 0, 75000) == BOOK_NO_MEMORY);
        time_budget = 1;
        assert(run(argv[2], 0, 0, 0, 0) == BOOK_OK);
        assert(checkpoints);
        printf("FAULTS allocations=%zu sampled=%zu cancel/close/output/budget/yield passed\n", count, checked);
        return 0;
    }
    assert(argc == 9 && !strcmp(argv[1], "--one"));
    int expected = atoi(argv[4]);
    transcript = fopen(argv[8], "wb"); assert(transcript);
    int rc = run(argv[2], 0, 0, 0, 0);
    fclose(transcript); transcript = NULL;
    printf("ONE result=%d peak=%u chapters=%u toc=%u images=%d text=%zu allocations=%zu\n",
        rc, message.values[1], chapters, toc_count, images, total_text, allocation_count);
    assert(rc == expected);
    if (rc == BOOK_OK) {
        assert(chapters == (unsigned)strtoul(argv[5], NULL, 10));
        assert(toc_count == (unsigned)strtoul(argv[6], NULL, 10));
        assert(images >= atoi(argv[7]));
        assert(beginnings == 1 && targets == 1 && endings == 1);
    }
    return 0;
}
