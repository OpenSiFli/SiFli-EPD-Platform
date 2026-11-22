"""Compile production settings code and exercise it with real Windows threads.

Storage, radio and notifications use host doubles. Write gates make queue,
flush, reload and reset interleavings deterministic.
"""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CASES = ("merge", "failure", "reset", "reload", "cache", "gates", "options", "startup", "shutdown", "navigation", "schema")


def main():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                  "-property", "installationPath"], text=True, timeout=30).strip()
    with tempfile.TemporaryDirectory(prefix="build-", dir=HERE) as directory:
        build = Path(directory)
        headers = ("storage_file.h", "storage.h", "dfs_posix.h", "system_reset_guard.h", "sleep_guard.h",
                   "epd_memory.h", "platform/epd_power.h", "boards/board_service.h",
                   "platform/epd_system_internal.h", "bt_pan.h", "wlan_service.h", "reading_font.h", "epd_waveform.h",
                   "rtthread.h", "rthw.h", "rtm.h", "mem_section.h")
        for name in headers:
            path = build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "host.h"\n')
        source = (ROOT / "src/ui/ui_settings.c").read_text(encoding="utf-8")
        # Copy intact so platform includes resolve to the host doubles.
        (build / "ui_settings.c").write_text(source, encoding="utf-8")
        (build / "ui_settings_store.c").write_bytes((ROOT / "src/ui/ui_settings_store.c").read_bytes())
        for name in ("epd_operation.c", "epd_operation_runtime.c"):
            (build / name).write_bytes((ROOT / "src/platform" / name).read_bytes())
        (build / "platform/epd_i18n.h").write_text(
            '#include "host.h"\n#define epd_get_str(key) #key\n'
            '#define EPD_FMT_platform_minutes(out, size, n) snprintf(out, size, "%u min", n)\n'
            '#define EPD_FMT_platform_refresh_count(out, size, n) snprintf(out, size, "%u pages", n)\n')
        service = (ROOT / "src/services/app_service.c").read_text(encoding="utf-8")
        functions = []
        for signature in ("static bool service_flush(epd_service_t *service)", "bool app_service_flush(void)"):
            start = service.index(signature)
            end = service.index("\n}\n", start) + 3
            functions.append(service[start:end])
        (build / "flush.inc").write_text("\n".join(functions))
        # Exercise the actual board SAVE branch; hardware shutdown is excluded.
        board = (ROOT / "src/boards/board_service.c").read_text(encoding="utf-8")
        start = board.index("    if (phase == POWER_SAVE)", board.index("void board_power_process(void)"))
        end = board.index("    if (phase != POWER_STOP) return;", start)
        (build / "board_save.inc").write_text(board[start:end])
        setup = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
        command = (f'call "{setup}" >nul && cl /nologo /std:c11 /utf-8 /W4 /WX /Od /RTC1 '
                   f'/D_CRT_SECURE_NO_WARNINGS /I"{HERE}" /I"{build}" /I"{ROOT / "src/ui"}" /I"{ROOT / "src/platform"}" '
                   f'"{HERE / "host_test.c"}" "{build / "ui_settings_store.c"}" '
                   f'"{build / "epd_operation.c"}" "{build / "epd_operation_runtime.c"}" '
                   f'"{build / "ui_settings.c"}" /Fe:"{build / "settings_async.exe"}"')
        script = build / "build.cmd"
        script.write_text("@echo off\n" + command + "\n")
        subprocess.run(["cmd", "/d", "/c", str(script)], cwd=build, check=True, timeout=60)
        for case in CASES:
            subprocess.run([str(build / "settings_async.exe"), case], cwd=build, check=True, timeout=15)
        print(f"settings async: {len(CASES)}/{len(CASES)} suites passed", flush=True)


if __name__ == "__main__":
    main()
