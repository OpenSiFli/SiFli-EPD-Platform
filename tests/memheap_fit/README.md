# APP PSRAM allocation placement

Run `python3 tests/memheap_fit/run.py` from the repository root on Linux or WSL with GCC and ASan/UBSan. The harness compiles the SDK memheap and platform memory owner ledger with the existing unified-memory fixture.

For device A/B, compile both images with `EPD_APP_PSRAM_FIT_TRACE=1`. Set `EPD_APP_PSRAM_BOUNDED_FIT=0` for first-fit and `=1` for the capped lookahead. The policy defaults to first-fit and trace defaults to `EPD_INPUT_TEST_ENABLED`.

The `mem_fit` command reports cumulative allocation samples, failures, alternative selections, and free-list nodes visited. Allocation sizes, heap capacity, and reclaim amounts in test cases and memory diagnostics use bytes (B). The `lock_elapsed_cycles_avg/max` fields use DWT cycles elapsed while the allocator owns the heap lock, including time spent preempted or in interrupts. They exclude lock acquisition wait, the full-heap failure snapshot walk, and statistics reporting. If `clock=0`, disregard cycle measurements.
