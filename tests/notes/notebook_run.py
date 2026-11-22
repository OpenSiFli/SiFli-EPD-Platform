"""Compile notebook commands and filters from notes_runtime.c against the host filesystem.

Run: python tests/notes/notebook_run.py
Requires Visual Studio C tools on Windows, or cc on POSIX. No CMake changes needed.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import re


def between(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def generate(repo, runtime, output):
    source = runtime.read_text(encoding="utf-8")
    header = (repo / "modules/notes/notes_runtime.h").read_text(encoding="utf-8")
    types = between(header, "typedef enum {", "void notes_runtime_init(")
    types += between(header, "typedef struct { uint32_t note, block;", "size_t notes_tasks(")
    types += between(source, "typedef struct job {", "typedef struct {\n    rt_mutex_t mutex;")
    (output / "notebook_types.inc").write_text(types, encoding="utf-8")
    first = "static bool note_matches_query(" if "static bool note_matches_query(" in source else "static void update_row("
    pieces = [
        between(source, "static void changed(", "static void wake_io("),
        between(source, "bool notes_status(", "epd_service_t *notes_owner("),
        between(source, "static void record_update(", "static void record_close("),
        between(source, "static void history_clear(", "static bool history_push("),
        between(source, "static void job_free(", "static bool stopping("),
        between(source, first, "static void preview_load("),
        between(source, "bool notes_command(", "static int player_callback("),
        between(source, "static void utf8_caption(", "size_t notes_tasks("),
        between(source, "static notes_error_t query_notes(", "static notes_error_t image_load("),
    ]
    command = between(source, "static notes_error_t io_command(", "static void ai_job(")
    pieces += [
        between(command, "static notes_error_t io_command(", "    case N_PREFERENCES:"),
        between(command, "    case N_QUERY:", "    case N_BROWSE:"),
        between(command, "    case N_SCAN:", "    case N_OPEN:"),
        between(command, "    case N_DELETE: case N_RESTORE:", "    case N_REC_START:"),
        command[command.index("    default: e = NOTES_INVALID;"):],
        "static void playback_tick(uint32_t ms) { lock();\n" +
        between(source, "                if (state->view.playback_ms != ms)", " unlock(); }") +
        "\n unlock(); }\n",
    ]
    (output / "notebook_actual.inc").write_text("\n".join(pieces), encoding="utf-8")


def compiler_environment():
    if os.name != "nt":
        return shutil.which("cc") or "cc", os.environ.copy()
    if shutil.which("cl"):
        return "cl", os.environ.copy()
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = subprocess.check_output([
        str(vswhere), "-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath",
    ], text=True).strip()
    vcvars = Path(installation) / "VC/Auxiliary/Build/vcvars64.bat"
    result = subprocess.check_output(f'cmd /d /s /c ""{vcvars}" >nul && set"', text=True)
    environment = os.environ.copy()
    for line in result.splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            environment[key.upper()] = value
    compiler = shutil.which("cl", path=environment["PATH"])
    if not compiler:
        raise SystemExit("Visual Studio C compiler was not found after vcvars64.bat")
    return compiler, environment


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, default=repo / "modules/notes/notes_runtime.c")
    parser.add_argument("cases", nargs="*")
    args = parser.parse_args()
    compiler, environment = compiler_environment()
    with tempfile.TemporaryDirectory(prefix="notes-notebook-") as directory:
        output = Path(directory)
        (output / "app_work.inc").write_text((repo / "modules/common/app_work.h").read_text(encoding="utf-8").replace('#include "app_service.h"', ''), encoding="utf-8")
        (output / "app_cleanup.inc").write_text((repo / "modules/common/app_cleanup.h").read_text(encoding="utf-8").replace('#include "app_work.h"', ''), encoding="utf-8")
        generate(repo, args.runtime, output)
        (output / 'memory_retry.inc').write_text(re.sub(r'^#include[^\n]*\n', '', (repo / 'modules/common/memory_retry.h').read_text(encoding='utf-8'), flags=re.M), encoding='utf-8')
        includes = [output, repo / "tests/notes/host", repo / "modules/notes"]
        sources = [repo / path for path in (
            "tests/notes/notebook_test.c", "tests/notes/host_fs.c",
            "modules/notes/notes_model.c", "modules/notes/notes_store.c", "modules/notes/notes_archive.c",
        )]
        executable = output / ("notebook_test.exe" if os.name == "nt" else "notebook_test")
        if os.name == "nt":
            command = [compiler, "/nologo", "/std:c11", "/utf-8", "/W3", "/DNOTES_HOST", "/D_CRT_SECURE_NO_WARNINGS"]
            command += [f"/I{path}" for path in includes] + list(map(str, sources)) + [f"/Fe:{executable}"]
        else:
            command = [compiler, "-std=c11", "-DNOTES_HOST"]
            command += [f"-I{path}" for path in includes] + list(map(str, sources)) + ["-o", str(executable)]
        subprocess.run(command, cwd=output, env=environment, check=True)
        failures = []
        for case in args.cases or ["filters", "creation", "move", "rename", "search_move", "search_rename", "validation", "search_content", "catalog", "catalog_scan", "catalog_timers"]:
            if subprocess.run([str(executable), case], cwd=output).returncode:
                failures.append(case)
        if failures:
            raise SystemExit("FAILED: " + ", ".join(failures))


if __name__ == "__main__":
    main()
