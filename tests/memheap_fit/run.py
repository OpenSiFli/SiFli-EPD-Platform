"""Run the SDK memheap placement cases with the host owner ledger."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SDK = REPO / "SiFli-SDK/rtos/rtthread"
FIXTURE = REPO / "tests/unified_memory"
PLATFORM = REPO / "src/platform"


def main():
    with tempfile.TemporaryDirectory(prefix="memheap-fit-") as folder:
        build = Path(folder)
        exe = build / "fit-host"
        args = [
            "gcc", "-std=c11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
            "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize=alignment",
            "-fno-sanitize-recover=all", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
            "-DEPD_MEMORY_HOST", "-DEPD_MEMORY_DIAGNOSTIC_HOST", "-DRT_USING_MUTEX",
            *[f"-I{path}" for path in (FIXTURE, SDK / "include", PLATFORM, REPO / "src/ui/platform")],
            str(HERE / "fit_host.c"),
            *[str(SDK / "src" / f"{name}.c") for name in ("memheap", "thread", "idle")],
            *[str(PLATFORM / f"{name}.c") for name in
              ("epd_memory_manager", "epd_memory_sdk", "epd_resource_runtime")],
            "-Wl,--gc-sections", "-o", str(exe),
        ]
        subprocess.run(args, check=True, cwd=build)
        for enabled in ("0", "1"):
            subprocess.run([str(exe), enabled], check=True, cwd=build, timeout=15)


if __name__ == "__main__":
    main()
