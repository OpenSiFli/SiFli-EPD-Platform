"""Run catalog scan fault injection against firmware sources or a candidate patch."""
import argparse
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def section(text, begin, end):
    return text[text.index(begin):text.index(end)]


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=here.parents[1])
    parser.add_argument("--patch", type=Path, help="Apply the candidate to a temporary source copy")
    args = parser.parse_args()
    repo = args.repo.resolve()
    paths = {
        "src/ui/platform/app_catalog.c",
        "src/ui/platform/app_package.c",
        "src/ui/platform/app_package.h",
        "src/ui/platform/app_package_io.h",
        "src/services/storage_file.c",
    }
    patch = args.patch.resolve() if args.patch else None
    if patch:
        paths.update(re.findall(r"^--- a/(.+)$", patch.read_text(encoding="utf-8"), re.M))

    with tempfile.TemporaryDirectory(prefix="app-catalog-scan-") as directory:
        overlay = Path(directory)
        for path in sorted(paths):
            target = overlay / path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(repo / path, target)
        if patch:
            subprocess.run(["git", "apply", str(patch)], cwd=overlay, check=True)

        platform = overlay / "src/ui/platform"
        catalog = (platform / "app_catalog.c").read_text(encoding="utf-8")
        storage = (overlay / "src/services/storage_file.c").read_text(encoding="utf-8")
        refresh = catalog[catalog.index("static int entry_order("):]
        display_reader = ""
        if "app_package_read_display(" in refresh:
            display_reader = section(catalog, "bool app_package_read_display(",
                                     "bool app_catalog_lockscreen_available(")
        (overlay / "catalog_actual.inc").write_text(
            section(catalog, "static epd_app_entry_t *entries;", "typedef struct") +
            section(catalog, "static void *array_grow", "void app_catalog_changed") +
            section(catalog, "static app_package_status_t package_read_status", "bool app_package_read(") +
            display_reader + refresh, encoding="utf-8")

        if "int storage_app_locate_checked(" in storage:
            locate = section(storage, "static int app_locate", "bool storage_app_locate(")
        else:
            locate = ""
        (overlay / "locate_actual.inc").write_text(locate, encoding="utf-8")

        source = (here / "host_test.c").read_text(encoding="utf-8-sig")
        if locate:
            source = "#define INSTALL_HOST_REAL_APP_LOOKUP\n" + source
        test = overlay / "host_test.c"
        test.write_text(source.replace("@OVERLAY@", overlay.as_posix())
                              .replace("@REPO@", repo.as_posix()), encoding="utf-8")

        runner = repo / "tests/app_install_perf/run_tests.py"
        spec = importlib.util.spec_from_file_location("app_install_perf", runner)
        module = importlib.util.module_from_spec(spec)
        sys.dont_write_bytecode = True
        spec.loader.exec_module(module)
        module.main(test_source=test)


if __name__ == "__main__":
    main()
