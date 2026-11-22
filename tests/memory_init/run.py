"""Exercise application startup using the real SDK heap registration path."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SHARED = ROOT / "tests/unified_memory"
PLATFORM = ROOT / "src/platform"
UI_PLATFORM = ROOT / "src/ui/platform"
SDK = ROOT / "SiFli-SDK/rtos/rtthread"


def extract(source, start, end):
    first = source.index(start)
    last = source.index(end, first)
    return source[first:last]


def build(directory, startup):
    (directory / "startup_actual.inc").write_text(startup, encoding="utf-8")
    exe = directory / "memory-init"
    includes = [directory, SHARED, SDK / "include", PLATFORM, UI_PLATFORM]
    sources = [
        HERE / "host.c",
        PLATFORM / "epd_memory_manager.c",
        PLATFORM / "epd_memory_sdk.c",
        SDK / "src/memheap.c",
        SDK / "src/thread.c",
        SDK / "src/idle.c",
    ]
    subprocess.run([
        "gcc", "-std=c11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
        "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
        "-fno-sanitize=alignment", "-fno-omit-frame-pointer",
        "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
        "-Werror=implicit-function-declaration", "-DEPD_MEMORY_HOST",
        "-DEPD_MEMORY_DIAGNOSTIC_HOST", "-DRT_USING_MUTEX",
        *[f"-I{path}" for path in includes], *map(str, sources),
        "-Wl,--gc-sections", "-o", str(exe),
    ], check=True)
    return exe


def main():
    if os.name == "nt":
        script = f"/mnt/{HERE.drive[0].lower()}{HERE.as_posix()[2:]}/run.py"
        return subprocess.call(["wsl.exe", "-d", "Ubuntu", "--exec", "python3", script])

    app = (UI_PLATFORM / "app_memory.c").read_text(encoding="utf-8")
    functions = [
        extract(app, signature, "\n}") + "\n}"
        for signature in ("static void *font_pixels_alloc(", "epd_memory_result_t app_memory_init(void)")
    ]
    main_source = (ROOT / "src/main.c").read_text(encoding="utf-8")
    startup = extract(main_source, "    epd_memory_result_t memory = app_memory_init();",
                      "    board_start_filesystem();")
    host = (SHARED / "host.c").read_text(encoding="utf-8")
    host_setup = host[:host.index("static size_t account_bytes(")]
    wrapper = "\nstatic int startup_slice(void)\n{\n    int ret;\n"
    suffix = "    return 0;\n}\n"
    with tempfile.TemporaryDirectory(prefix="memory-init-") as temp:
        directory = Path(temp)
        (directory / "host_setup.inc").write_text(host_setup, encoding="utf-8")
        prefix = "\n\n".join(functions) + "\n" + wrapper
        candidate = build(directory, prefix + startup + suffix)
        for case in ("duplicate", "enumeration-full", "registry-full", "manager-busy"):
            subprocess.run([str(candidate), case], check=True)

        # Remove only the early return after main logs the memory failure.
        mutated, changes = re.subn(
            r'(LOG_E\("Application memory initialization failed:[^\n]*\);\n)[ \t]*return -RT_ERROR;\n',
            r"\1", startup,
        )
        if changes != 1:
            raise RuntimeError("Expected exactly one application memory failure gate")
        control = build(directory, prefix + mutated + suffix)
        result = subprocess.run([str(control), "enumeration-full"], capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, end="")
        continued = "boot enumeration-full result=0 json=0 operation=1 resource=1 panel=1"
        if (result.returncode != 1 or result.stderr.splitlines()[-1:] != ["INIT_FAIL_FAST_GATE"]
                or continued not in result.stdout):
            raise RuntimeError(f"Unexpected negative control result: {result.returncode}")
        print("PASS negative control: missing main return let startup continue")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
