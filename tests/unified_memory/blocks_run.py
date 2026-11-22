"""Run mem_blocks against the actual SDK heap layout, allocator and manager."""
from pathlib import Path
import os
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
if os.name == "nt":
    script = "/mnt/" + HERE.drive[0].lower() + HERE.as_posix()[2:] + "/blocks_run.py"
    raise SystemExit(subprocess.call(["wsl.exe", "-d", "Ubuntu", "--exec", "python3", script]))

SDK = ROOT / "SiFli-SDK/rtos/rtthread"
build = HERE / ".build"
build.mkdir(exist_ok=True)
sources = [HERE / "blocks_host.c", ROOT / "src/platform/epd_memory_manager.c",
           ROOT / "src/platform/epd_memory_sdk.c",
           ROOT / "src/platform/epd_resource_runtime.c", ROOT / "src/ui/platform/app_memory.c",
           *(SDK / "src" / name for name in ("memheap.c", "thread.c", "idle.c"))]
includes = [HERE, SDK / "include", ROOT / "src/platform", ROOT / "src/ui/platform", ROOT / "src/services"]
for trace in (False, True):
    exe = build / f"blocks-trace-{int(trace)}"
    subprocess.run(["gcc", "-std=c11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                    "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize=alignment",
                    "-fno-omit-frame-pointer", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
                    "-Werror=implicit-function-declaration", "-Werror=return-type",
                    "-DEPD_MEMORY_HOST", "-DEPD_MEMORY_DIAGNOSTIC_HOST", "-DRT_USING_MUTEX",
                    "-include", str(ROOT / "src/platform/epd_memory_manager.h"),
                    *(["-DRT_USING_MEMTRACE"] if trace else []),
                    *(f"-I{p}" for p in includes), *map(str, sources),
                    "-Wl,--gc-sections", "-o", str(exe)], check=True)
    for case in ("descendants", "mutation", "busy", "corrupt", "arguments"):
        subprocess.run([str(exe), case], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
print("PASS mem_blocks 10/10 ASAN/UBSAN cases (SDK MEMTRACE on/off)")
