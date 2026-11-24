"""Run the production Books save path and footer with host I/O doubles."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment
from view_run import extract


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def typedef(source, name):
    return re.search(r"typedef struct\s*\{(?:(?!typedef struct)[\s\S])*?\}\s*" + name + r";", source)[0]


def main():
    cc, env = compiler_environment(os.environ.get("CC"))
    shelf = read("modules/books/ui_bookshelf_data.c")
    document = read("modules/books/document/document_view.c")
    pages = read("modules/books/pages.c")
    types = read("modules/books/ui_bookshelf_data.h")
    document_header = read("modules/books/document/document.h")
    declarations = [
        re.search(r"typedef enum book_format[\s\S]*?} book_format_t;", read("modules/books/formats/formats.h"))[0],
        "\n".join(re.findall(r"^#define DOCUMENT_(?:TXT|MD|EPUB|SPLIT_EPUB|MOBI|SOURCE_SPOOL)\b[^\n]*", read("modules/books/document/document.h"), re.M)),
        typedef(read("src/ui/platform/epd_reader_model.h"), "epd_reader_file_t"),
        "typedef epd_reader_file_t bookshelf_item_t;",
        typedef(read("src/ui/platform/epd_reader_model.h"), "document_cursor_t"),
        typedef(read("modules/books/document/document.h"), "document_anchor_t"),
        typedef(read("modules/common/reader/reader.h"), "reader_position_t"),
        typedef(read("modules/common/reader/reader_types.h"), "reader_view_t"),
        typedef(types, "ui_reading_position_t"), typedef(types, "ui_bookshelf_book_t"),
        re.search(r"^#define DOCUMENT_CONVERSION_VERSION.*", document_header, re.M)[0],
        re.search(r"^#define HISTORY_COALESCE_MS.*", shelf, re.M)[0],
        typedef(shelf, "history_t"),
        shelf[shelf.index("#define HISTORY_PENDING_MAX"):shelf.index("} writer;") + len("} writer;")],
        re.search(r"static unsigned active_book[^;]*;\s*static bool save_rejected;", document)[0],
        "static bool reader_save_failed_shown;",
    ]
    groups = [
        (document_header, "document_history_version"),
        (shelf, "history_find history_write ui_bookshelf_history_retry ui_bookshelf_worker_delay ui_bookshelf_save_failed ui_bookshelf_flush hash ui_bookshelf_save"),
        (document, "position_compatible reopen_book save_book document_view_open document_view_close document_view_save_failed document_view_save"),
        (read("modules/common/reader/reader_view.c"), "epd_reader_save"),
        (pages, "reader_footer_render reader_save_position"),
    ]
    functions = [extract(source, name) for source, names in groups for name in names.split()]
    strings = json.loads(read("modules/common/reader/i18n/strings.json"))["strings"]
    formats = []
    for name in ("reader_unsaved", "reader_unsaved_progress", "reader_pages", "reader_page_unknown"):
        value = json.dumps(strings[name]["zh_cn"], ensure_ascii=False)
        formats.append(f"#define EPD_FMT_{name}(out, size, ...) snprintf(out, size, {value}, __VA_ARGS__)")
    fixture = (HERE / "reader_save_host.c").read_text(encoding="utf-8")
    fixture = fixture.replace("/* DECLARATIONS */", "\n".join(declarations + formats))
    fixture = fixture.replace("/* PROTOTYPES */", "\n".join(f[1] for f in functions))
    production = "\n".join(f[2] for f in functions)
    with tempfile.TemporaryDirectory(prefix="reader-save-") as directory:
        build = Path(directory)
        for regression in (False, True):
            body = production
            if regression:
                body = body.replace("if (save_failed)", "if (!reading->saved)")
            source = build / "reader_save_host.c"
            source.write_text(fixture.replace("/* PRODUCTION */", body), encoding="utf-8")
            exe = build / ("reader_save.exe" if os.name == "nt" else "reader_save")
            if Path(cc).stem.lower() == "cl":
                # The writer stores its 0/1 slot number in an unsigned char.
                args = [cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/wd4505", "/wd4244",
                        "/D_CRT_SECURE_NO_WARNINGS", "/Od", "/RTC1", str(source), f"/Fe{exe}"]
            else:
                args = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-fsanitize=address,undefined", str(source), "-o", str(exe)]
            subprocess.run(args, cwd=build, env=env, check=True, timeout=60)
            result = subprocess.run([str(exe)], cwd=build, env=env, capture_output=True, timeout=15)
            if regression:
                if result.returncode == 0 or b"pending footer" not in result.stderr:
                    raise RuntimeError("The footer regression was not detected")
                print("PASS old !saved footer regression is rejected")
            else:
                sys.stdout.buffer.write(result.stdout)
                sys.stderr.buffer.write(result.stderr)
                result.check_returncode()


if __name__ == "__main__":
    main()
