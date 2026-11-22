# Catalog scan fault injection

Run from the repository root with Python and the MSVC toolchain used by
`tests/app_install_perf`.

    python tests/app_catalog_scan/run.py

To exercise an unapplied candidate, pass its patch. The runner applies the
patch to a temporary copy of the relevant source files.

    python tests/app_catalog_scan/run.py --patch PATH_TO_PATCH

Use `--repo PATH` when running the test directory outside the checkout.
Both modes run the same assertions against the catalog refresh, package
metadata reader, and checked volume lookup when available. The test verifies
rollback on allocation and I/O failures, and successful publication with
missing or malformed entries skipped. The unfixed baseline is expected to
fail the rollback assertion when manifest allocation fails.
