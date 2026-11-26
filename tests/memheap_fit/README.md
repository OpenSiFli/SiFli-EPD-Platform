# APP PSRAM allocation placement

Run `python3 tests/memheap_fit/run.py` from the repository root on Linux or WSL with GCC and ASan/UBSan. The harness compiles the SDK memheap and platform memory owner ledger with the existing unified-memory fixture.

Use EPD_APP_PSRAM_BOUNDED_FIT=1 to enable capped lookahead for the application PSRAM heap. The default policy is first-fit. The host cases exercise both policies, fragmentation, owner limits, and allocation recovery.
