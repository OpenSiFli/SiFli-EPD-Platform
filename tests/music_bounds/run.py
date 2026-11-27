"""Compile the real directory scanner with deterministic filesystem doubles."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment
sys.path.insert(0, str(ROOT / "tests/music_progress"))
from run import extract


def main():
    cc, env = compiler_environment(None)
    music = ROOT / "modules/music"
    source = (music / "music_library.c").read_text(encoding="utf-8")
    names = ("lower_ascii utf8_valid equal_ascii music_library_natural_compare "
             "music_library_extension music_library_resolve basename_of music_library_free "
             "entry_compare add_entry music_path_available music_library_scan_begin music_library_scan_step "
             "music_library_scan_close music_library_read")
    scan = source[source.index("struct music_library_scan {"):source.index("};") + 2]
    with tempfile.TemporaryDirectory(prefix="music-bounds-") as directory:
        build = Path(directory)
        (build / "actual.inc").write_text(scan + "\n" + "\n\n".join(extract(source, n) for n in names.split()), encoding="utf-8")
        (build / "storage_file.h").write_text("#define STORAGE_PATH_MAX 512\n", encoding="utf-8")
        exe = build / "bounds.exe"
        subprocess.run([cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/Od", "/RTC1",
                        "/D_CRT_SECURE_NO_WARNINGS", "/DMUSIC_MODEL_HOST", f"/I{build}", f"/I{music}",
                        str(HERE / "host.c"), str(music / "music_queue.c"), f"/Fe{exe}"],
                       cwd=build, env=env, check=True, timeout=60)
        subprocess.run([str(exe)], cwd=build, check=True, timeout=60)


if __name__ == "__main__":
    main()
