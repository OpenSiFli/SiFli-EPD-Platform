"""Run Books service/page scheduling and independent common-view regression."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import re

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / "reader_prefetch"))
from cache_run import compiler_environment
from view_run import extract


def functions(path, names):
    source = path.read_text(encoding="utf-8")
    items = [extract(source, name) for name in names.split()]
    return "\n".join(item[1] for item in items) + "\n" + "\n".join(
        f'#line {source.count(chr(10), 0, offset) + 1} "{path.as_posix()}"\n{body}'
        for offset, _, body in items)


def generate(build):
    books = ROOT / "modules/books"
    app = (books / "app_main.c").read_text(encoding="utf-8")
    ui = (books / "books_ui.h").read_text(encoding="utf-8")
    trace_ids = re.search(r"enum\s*\{\s*OPEN_BEGIN\b.*?\bOPEN_STAMPS\s*\};", app, re.S).group()
    open_constants = [re.search(r"^#define " + name + r"\s+[^\n]+", ui, re.M).group()
                      for name in ("BOOK_OPEN_BASE", "BOOKSHELF_PAGE_SIZE")]
    (build / "open_trace_ids.h").write_text("\n".join(open_constants + [trace_ids]) + "\n", encoding="utf-8")
    pages = (books / "pages.c").read_text(encoding="utf-8")
    start = pages.index("static struct\n")
    end = pages.index("} submit_trace;", start) + len("} submit_trace;")
    (build / "trace_state.h").write_text(pages[start:end], encoding="utf-8")
    controls = (books / "reader_controls.c").read_text(encoding="utf-8")
    (build / "menu_state.h").write_text(
        controls[controls.index("enum { CLOSE="):controls.index("static lv_obj_t *label(")],
        encoding="utf-8")
    navigation = functions(books / "reader_controls.c",
        "books_menu_active books_menu_font_pending books_menu_open books_menu_close navigate request_navigation books_menu_navigate")
    (build / "navigation.h").write_text(navigation, encoding="utf-8")
    production = navigation + "\n" + functions(books / "reader_controls.c", "books_menu_pause")
    production += "\n" + functions(books / "pages.c",
        "reader_submit_poll reader_render reader_update reader_resume reader_resume_prepare reader_save_position "
        "reader_pause reader_stop books_process")
    production += "\n" + functions(books / "app_main.c", "page_input_move process_navigation books_flush service_process")
    (build / "production.h").write_text(production, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    cc, env = compiler_environment(args.cc)
    failures, total = [], 0
    with tempfile.TemporaryDirectory(prefix="books-scheduling-") as directory:
        build = Path(directory)
        generate(build)
        for name in ("host",):
            exe = build / (name + (".exe" if os.name == "nt" else ""))
            includes = [build, HERE, ROOT]
            if Path(cc).stem.lower() == "cl":
                command = [cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC",
                           "/wd4100", "/wd4505", "/wd4127", "/wd4244", "/wd4267", "/wd4389", "/wd4018",
                           "/Od", "/RTC1", "/D_CRT_SECURE_NO_WARNINGS",
                           *(f"/I{p}" for p in includes), str(HERE / (name + ".c")), f"/Fe{exe}"]
            else:
                command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                           "-Wno-unused-function", "-Wno-sign-compare", "-g", "-fsanitize=address,undefined",
                           *(f"-I{p}" for p in includes), str(HERE / (name + ".c")), "-o", str(exe)]
            subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
            available = subprocess.check_output([str(exe), "--list"], cwd=build, env=env,
                                               text=True, timeout=15).splitlines()
            for case in available:
                if args.case and case not in args.case:
                    continue
                total += 1
                try:
                    subprocess.run([str(exe), case], cwd=build, env=env, check=True, timeout=15)
                except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                    failures.append(case)
    if args.case and total != len(set(args.case)):
        parser.error("Unknown or duplicate case selection")
    print(f"Books scheduling: {total - len(failures)}/{total} cases passed.", flush=True)
    if failures:
        raise SystemExit(1)
    command = [sys.executable, str(ROOT / "tests/reader_view_async/run.py"),
               "--case", "worker", "--case", "navigation", "--case", "ownership"]
    if args.cc: command += ["--cc", args.cc]
    subprocess.run(command, cwd=ROOT, check=True, timeout=120)


if __name__ == "__main__":
    main()
