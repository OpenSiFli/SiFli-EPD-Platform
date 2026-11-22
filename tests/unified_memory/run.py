# Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0
"""Compile the real manager, SDK bridge, memheap and thread implementations."""
from pathlib import Path
import os
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SDK = ROOT / "SiFli-SDK/rtos/rtthread"


def main():
    if os.name == "nt":
        script = "/mnt/" + HERE.drive[0].lower() + HERE.as_posix()[2:] + "/run.py"
        raise SystemExit(subprocess.call(["wsl.exe", "-d", "Ubuntu", "--exec", "python3", script, *sys.argv[1:]]))
    build = HERE / ".build"
    build.mkdir(exist_ok=True)
    sources = [ROOT / "src/platform/epd_memory_manager.c", ROOT / "src/platform/epd_memory_sdk.c"]
    includes = ["-I" + str(path) for path in (HERE, SDK / "include", ROOT / "src/platform",
                ROOT / "src/ui/platform", ROOT / "SiFli-SDK/middleware/app_fwk")]
    requested = sys.argv[1:]
    if not requested:
        subprocess.run([sys.executable, str(HERE / "exact_fit_run.py")], check=True)
    disabled = build / "transfer-disabled"
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-DEPD_MEMORY_HOST", "-DEPD_MEMORY_TEST_OWNER_DISABLED",
                    "-ffunction-sections", "-fdata-sections", *includes,
                    str(HERE / "transfer_disabled.c"), str(sources[1]),
                    "-Wl,--gc-sections", "-o", str(disabled)], check=True)
    subprocess.run([str(disabled)], check=True)
    print("PASS transfer SDK-owner disabled", flush=True)
    for variant, rate in (("ticks-1000", 1000), ("ticks-100", 100), ("generations", 1000)):
        exe = build / f"unified-{variant}"
        defs = ["-DEPD_MEMORY_HOST", "-DEPD_MEMORY_DIAGNOSTIC_HOST", "-DRT_USING_MUTEX", f"-DRT_TICK_PER_SECOND={rate}"]
        if variant == "generations":
            defs += ["-DEPD_MEMORY_GENERATION_MAX=3", "-DEPD_MEMORY_DESCRIPTOR_COUNT=8",
                     "-DEPD_MEMORY_TEST_GENERATIONS"]
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                        *defs, *includes, *map(str, sources)], check=True)
        subprocess.run(["gcc", "-std=c11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                        "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize=alignment",
                        "-fno-omit-frame-pointer", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
                        *defs, *includes, str(HERE / "host.c"), *map(str, sources),
                        str(ROOT / "src/platform/epd_resource_runtime.c"),
                        str(ROOT / "src/ui/platform/app_memory.c"),
                        *[str(SDK / "src" / name) for name in ("memheap.c", "thread.c", "idle.c")],
                        "-Wl,--gc-sections", "-o", str(exe)], check=True)
        names = (["generation_retire"] if variant == "generations" else
                 requested or subprocess.check_output([str(exe), "--list"], text=True).splitlines())
        for name in names:
            subprocess.run([str(exe), name], check=True, cwd=build,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
        print(f"PASS {len(names)} cases: {variant}", flush=True)
    target_includes = ["-I" + str(HERE / "freestanding"), *includes]
    subprocess.run(["gcc", "-m32", "-ffreestanding", "-std=c11", "-DEPD_MEMORY_HOST",
                    "-Wall", "-Wextra", "-Werror", "-fsyntax-only", *target_includes,
                    *map(str, sources)], check=True)
    obj = build / "metadata32.o"
    subprocess.run(["gcc", "-m32", "-ffreestanding", "-std=c11", "-DEPD_MEMORY_HOST",
                    *target_includes, "-c", str(sources[0]), "-o", str(obj)], check=True)
    symbols = subprocess.check_output(["nm", "-S", str(obj)], text=True).splitlines()
    sizes = {name: int(next(line.split()[1] for line in symbols if line.endswith(" " + name)), 16)
             for name in ("manager", "descriptors")}
    print(f"PASS 32-bit source syntax; metadata = {sum(sizes.values())} bytes "
          f"(SRAM {sizes['manager']}, retained {sizes['descriptors']})", flush=True)


if __name__ == "__main__":
    main()
