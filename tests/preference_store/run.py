"""Exercise production preference APIs and the serial worker on real host threads."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CASES = ("merge", "failure", "superseded", "time", "legacy", "exclusive", "lifecycle", "reject",
         "load_status", "init_failure", "notifications", "timeout", "startup", "snapshot_start", "concurrent_fields", "exit_race", "failed_exit")
SOURCES = ("src/services/preference_store.c", "src/services/device_identity.c",
           "src/services/system_time.c", "src/ui/platform/epd_locale.c")


def main():
    cases = tuple(sys.argv[1:]) or CASES
    if any(case not in CASES for case in cases):
        raise SystemExit("Unknown suite; choose from: " + ", ".join(CASES))
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                  "-property", "installationPath"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="preferences-host-") as directory:
        build = Path(directory)
        for folder in ("platform", "net", "boards", "ui"):
            (build / folder).mkdir()
        headers = ("src/services/preference_store.h", "src/services/device_identity.h",
                   "src/services/system_time.h", "src/services/system_time_internal.h",
                   "src/services/app_service.h", "src/ui/platform/epd_locale.h")
        for name in SOURCES + headers:
            (build / Path(name).name).write_bytes((ROOT / name).read_bytes())
        (build / "platform/epd_locale_internal.h").write_bytes((ROOT / "src/ui/platform/epd_locale_internal.h").read_bytes())
        (build / "ui/ui_app.h").write_bytes((ROOT / "src/ui/ui_app.h").read_bytes())
        stubs = ("rtthread.h", "rtdevice.h", "rtm.h", "dfs_posix.h", "bts2_bt.h", "mem_section.h",
                 "storage_file.h", "storage_internal.h", "system_reset_guard.h", "reminder_internal.h",
                 "app_module.h", "app_catalog.h", "lv_ext_resource_manager.h",
                 "platform/epd_system_internal.h", "platform/epd_i18n.h", "net/network_time.h", "boards/board_service.h")
        for name in stubs:
            (build / name).write_text('#include "host.h"\n')
        setup = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
        script = build / "build.cmd"
        units = " ".join(f'"{build / Path(p).name}"' for p in SOURCES)
        script.write_text('@echo off\n' + f'call "{setup}" >nul && cl /nologo /std:c11 /utf-8 /W4 /WX /Od /RTC1 '
                          f'/D_CRT_SECURE_NO_WARNINGS /I"{HERE}" /I"{build}" /I"{ROOT / "src/services"}" '
                          f'"{HERE / "host.c"}" {units} /Fe:"{build / "preferences.exe"}"\n')
        subprocess.run(["cmd", "/d", "/c", str(script)], cwd=build, check=True, timeout=60)
        for case in cases:
            subprocess.run([str(build / "preferences.exe"), case], cwd=build, check=True, timeout=20)
        print(f"preferences async: {len(cases)}/{len(cases)} suites passed")


if __name__ == "__main__":
    main()
