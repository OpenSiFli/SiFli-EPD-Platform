"""Run production music persistence and command handling with host I/O doubles.

The queue and store compile intact. The dependency-light library and service
functions are extracted unchanged; the audio backend, filesystem and UI are
test doubles. No firmware build or device access is required.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import argparse

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MUSIC = ROOT / "modules/music"


def extract(source, name):
    pattern = r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    clean = re.sub(pattern, lambda m: "".join("\n" if c == "\n" else " " for c in m[0]), source)
    matches = list(re.finditer(rf"^[A-Za-z_][\w* \t]*\b{name}\s*\([^;{{}}]*\)\s*\{{", clean, re.M))
    if len(matches) != 1:
        raise RuntimeError(f"Expected one function definition: {name}")
    start = matches[0].start()
    end = matches[0].end()
    depth = 1
    while depth:
        depth += (clean[end] == "{") - (clean[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    controls = parser.add_mutually_exclusive_group()
    controls.add_argument("--reproduce-old-wake", action="store_true",
                        help="Inject the former UI wake into the host fixture and require the starvation regression to fail")
    controls.add_argument("--reproduce-lost-ack", action="store_true",
                          help="Disable receipt consumption in the host copy and require progress starvation detection")
    controls.add_argument("--reproduce-stale-ack", action="store_true",
                          help="Remove the queued-command guard in the host copy and require stale receipt detection")
    controls.add_argument("--reproduce-stale-save-error", action="store_true",
                          help="Keep the old SAVE result after a successful retry and require regression detection")
    options = parser.parse_args()
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                  "-property", "installationPath"], text=True, timeout=30).strip()
    setup = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
    with tempfile.TemporaryDirectory(prefix="build-", dir=HERE) as directory:
        build = Path(directory)
        for name in ("storage_file.h", "storage.h", "dfs_posix.h", "platform/epd_app.h"):
            header = build / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "host.h"\n')
        library = (MUSIC / "music_library.c").read_text(encoding="utf-8")
        names = "utf8_valid music_library_resolve music_library_free music_library_stamp_equal music_favorites_free music_favorites_contains music_favorites_set"
        (build / "library.inc").write_text("\n\n".join(extract(library, n) for n in names.split()), encoding="utf-8")
        (build / "app_work.inc").write_text((MUSIC.parent.parent / "modules/common/app_work.h").read_text(encoding="utf-8").replace('#include "app_service.h"', ''), encoding="utf-8")
        (build / "app_cleanup.inc").write_text((MUSIC.parent.parent / "modules/common/app_cleanup.h").read_text(encoding="utf-8").replace('#include "app_work.h"', ''), encoding="utf-8")
        service = (MUSIC / "music_service.c").read_text(encoding="utf-8")
        if options.reproduce_stale_save_error:
            clear = '    if (working.result == MUSIC_ERR_SAVE) result(MUSIC_OK, 0, "");\n'
            assert service.count(clear) == 1
            service = service.replace(clear, '')
        if options.reproduce_old_wake:
            service = service.replace("    music_view_update();", "    if (ui_active && cover_refresh) epd_service_wake(service);\n    music_view_update();")
        if options.reproduce_lost_ack:
            receipt = ("    if (flush_ack_pending) {\n"
                       "        flush_ack_pending = false;\n"
                       "        unlock();\n"
                       "        return true;\n"
                       "    }\n")
            assert service.count(receipt) == 1
            service = service.replace(receipt, "")
        if options.reproduce_stale_ack:
            assert service.count("flush_ready = ok && !commands;") == 1
            service = service.replace("flush_ready = ok && !commands;", "flush_ready = ok;")
        names = ("invalidate_flush text_copy basename_of result model_result structure_changed start_list capture_position save "
                 "close_track clear_current controls_waiting open_current failed_next advance open_path execute "
                 "music_ui_active music_submit take_command begin_work stop_background_work settle ui_process flush wake update_track "
                 "epd_app_release_tf removable_io_step run")
        view = (MUSIC / "music_view.c").read_text(encoding="utf-8")
        (build / "service.inc").write_text(extract(view, "music_view_present") + "\n\n" +
                                           "\n\n".join(extract(service, n) for n in names.split()), encoding="utf-8")
        start = service.index('        lock();\n        uint32_t flush = flush_requested;')
        end = service.index('        if (deferred)', start)
        (build / "dispatch_save.inc").write_text(
            'static void dispatch_save(epd_service_t *service)\n{\n' + service[start:end] + '}\n',
            encoding="utf-8")
        (build / "store.c").write_text('#include "host.h"\n#define music_queue_valid host_queue_valid\n'
                                       f'#include "{(MUSIC / "music_store.c").as_posix()}"\n')
        command = (f'call "{setup}" >nul && cl /nologo /std:c11 /utf-8 /W4 /WX /Od /RTC1 '
                   f'/D_CRT_SECURE_NO_WARNINGS /DMUSIC_MODEL_HOST /I"{HERE}" /I"{build}" /I"{MUSIC}" '
                   f'"{HERE / "host_test.c"}" "{MUSIC / "music_queue.c"}" "{build / "store.c"}" '
                   f'/Fe:"{build / "music_progress.exe"}"')
        script = build / "build.cmd"
        script.write_text("@echo off\n" + command + "\n")
        subprocess.run(["cmd", "/d", "/c", str(script)], cwd=build, check=True, timeout=60)
        result = subprocess.run([str(build / "music_progress.exe")], cwd=build,
                                capture_output=True, text=True, timeout=60)
        print(result.stdout, end="")
        print(result.stderr, end="")
        if options.reproduce_old_wake:
            if result.returncode != 1 or "wakes == 1 && ui_requests == 0 && !stops && cover_refresh" not in result.stderr:
                raise RuntimeError("Expected the startup wake regression to reject the former UI callback")
            print("PASS negative control: former UI wake replenishes the main wake event on the first poll; regression detects it")
        elif options.reproduce_lost_ack or options.reproduce_stale_ack:
            expected = ("flush() && !flush_ack_pending && flush_requested == i + 1" if options.reproduce_lost_ack
                        else "!flush_ack_pending && !flush_ready && commands")
            if result.returncode != 1 or expected not in result.stderr:
                raise RuntimeError("Expected the flush interleaving regression to reject the injected receipt defect")
            print("PASS negative control: " + ("lost acknowledgement" if options.reproduce_lost_ack else "stale acknowledgement"))
        elif options.reproduce_stale_save_error:
            expected = 'working.result == MUSIC_OK && working.detail == 0 && !working.result_path[0]'
            if result.returncode != 1 or expected not in result.stderr:
                raise RuntimeError("Expected successful retry to expose the stale SAVE result")
            print("PASS negative control: successful save retained the old SAVE error")
        else:
            result.check_returncode()


if __name__ == "__main__":
    main()
