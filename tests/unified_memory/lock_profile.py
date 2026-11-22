# Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0
"""Profile production core critical sections; host times are not target latency."""
import json
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    if os.name == "nt":
        script = "/mnt/" + HERE.drive[0].lower() + HERE.as_posix()[2:] + "/lock_profile.py"
        raise SystemExit(subprocess.call(["wsl.exe", "-d", "Ubuntu", "--exec", "python3", script]))
    build = HERE / ".build"
    build.mkdir(exist_ok=True)
    sdk = ROOT / "SiFli-SDK/rtos/rtthread"
    flags = ["-std=c11", "-O2", "-g", "-fno-pie", "-ffunction-sections", "-fdata-sections",
             "-DEPD_MEMORY_HOST", "-DEPD_MEMORY_LOCK_PROFILE", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
             *["-I"+str(p) for p in (HERE,sdk/"include",ROOT/"src/platform",ROOT/"src/ui/platform")]]
    core = build / "profile-core.o"
    subprocess.run(["gcc", *flags, "-finstrument-functions", "-c",
                    str(ROOT/"src/platform/epd_memory_manager.c"), "-o", str(core)],check=True)
    exe = build / "lock-profile"
    subprocess.run(["gcc", *flags, "-no-pie", str(HERE/"lock_profile.c"), str(core),
                    str(ROOT/"src/platform/epd_memory_sdk.c"), str(ROOT/"src/ui/platform/app_memory.c"),
                    *[str(sdk/"src"/p) for p in ("memheap.c","thread.c","idle.c")],
                    "-Wl,--gc-sections", "-lasan", "-o", str(exe)],check=True)
    results = []
    for name in ("heap0", "heap16", "heap111", "close111", "poll111", "share64", "share64association", "share62accounts"):
        result = json.loads(subprocess.check_output([str(exe),name],text=True))
        print(json.dumps(result),flush=True)
        results.append(result)
    (build/"lock-profile.json").write_text(json.dumps(results,indent=2)+"\n")


if __name__ == "__main__":
    main()
