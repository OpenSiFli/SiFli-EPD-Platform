"""Exercise the production file-page loop with the SDK's directory-position adapters.

Run all cases, or pass case names. No LVGL or firmware build is required.
"""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def generate(output):
    source = (ROOT / "src/ui/apps/app_files.c").read_text(encoding="utf-8")
    types = source[source.index("typedef struct files_view"):source.index("static bool files_private_entry(")]
    (output / "file_view_actual.inc").write_text(types, encoding="utf-8")
    functions = [function(source, name) for name in (
        "static bool files_private_entry(", "static bool picker_image(", "static bool picker_accepts(")]
    functions.append(source[source.index("/* All view/cursor"):source.index("static void files_layout(")])
    functions.append(function(source, "void files_stop("))
    functions += [line for line in source.splitlines() if line.startswith(("void files_resume(", "void storage_page_resume("))]
    (output / "file_page_actual.inc").write_text("\n\n".join(functions), encoding="utf-8")
    pages = (ROOT / "src/ui/apps/ui_pages.c").read_text(encoding="utf-8")
    rows = [line for line in pages.splitlines() if any("[" + name + "]" in line for name in ("UI_PAGE_FILES", "UI_PAGE_STORAGE", "UI_PAGE_FILE_PICKER"))]
    assert len(rows) == 3 and all("files_pause" in row for row in rows)
    assert "files_track(view);" in function(source, "void files_create(")
    assert "FILE_NEXT && view->page_failed" in function(source, "bool files_action(")
    (output / "file_ops_actual.inc").write_text("static const ui_page_ops_t page_ops[3] = {\n" + "\n".join(rows) + "\n};", encoding="utf-8")
    launcher = (ROOT / "src/ui/launcher.c").read_text(encoding="utf-8")
    prefix = function(launcher, "bool launcher_storage_process(").split("        if (!release_acknowledged)", 1)[0]
    assert "files_release_storage();" in prefix
    prefix = prefix.replace("bool launcher_storage_process(void)", "static bool release_begin(void)")
    (output / "file_release_actual.inc").write_text(prefix + "    }\n    return true;\n}\n", encoding="utf-8")
    dfs = ROOT / "SiFli-SDK/rtos/rtthread/components/dfs"
    for name, path, signatures in (
        ("posix", "src/dfs_posix.c", ("struct dirent *readdir(", "long telldir(", "void seekdir(")),
        ("fat", "filesystems/elmfat/dfs_elm.c", ("int dfs_elm_lseek(", "int dfs_elm_getdents(")),
        ("lfs", "filesystems/littlefs/dfs_lfs.c", ("static int _dfs_lfs_lseek(", "static int _dfs_lfs_getdents(")),
        ("fat_seek", "filesystems/elmfat/ff.c", ("FRESULT f_seekdir(",)),
    ):
        text = (dfs / path).read_text(encoding="utf-8")
        (output / f"{name}_actual.inc").write_text("\n\n".join(function(text, signature) for signature in signatures), encoding="utf-8")
    text = (ROOT / "src/services/storage_watch.c").read_text(encoding="utf-8")
    (output / "watch_actual.inc").write_text("\n".join(line for line in text.splitlines() if not line.startswith("#include")), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc")
    parser.add_argument("cases", nargs="*")
    args = parser.parse_args()
    compiler, environment = compiler_environment(args.cc)
    with tempfile.TemporaryDirectory(prefix="file-page-") as directory:
        output = Path(directory)
        generate(output)
        executable = output / "file_page_test.exe"
        includes = [output, ROOT / "src/ui/apps"]
        source = str(Path(__file__).with_name("file_page_host.c"))
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c11", "/utf-8", "/W3", "/Od", "/RTC1", "/D_CRT_SECURE_NO_WARNINGS"]
            command += [f"/I{path}" for path in includes] + [source, f"/Fe:{executable}"]
        else:
            command = [compiler, "-std=c11", "-Wall", "-Wextra"]
            command += [f"-I{path}" for path in includes] + [source, "-o", str(executable)]
        subprocess.run(command, cwd=output, env=environment, check=True, timeout=60)
        cases = args.cases or subprocess.check_output([str(executable), "--list"], text=True).splitlines()
        failed = []
        for case in cases:
            if subprocess.run([str(executable), case], cwd=output, timeout=30).returncode:
                failed.append(case)
        print(f"File pages: {len(cases)-len(failed)}/{len(cases)} passed", flush=True)
        if failed:
            raise SystemExit("FAILED: " + ", ".join(failed))


if __name__ == "__main__":
    main()
