"""Exercise production catalog functions against the real RT-Thread ownership ledger."""
import os
from pathlib import Path
import re
import subprocess
import tempfile


def section(source, start, end):
    return source[source.index(start):source.index(end)]


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    if os.name == "nt":
        script = "/mnt/" + HERE.drive[0].lower() + HERE.as_posix()[2:] + "/run.py"
        raise SystemExit(subprocess.call(["wsl.exe", "-d", "Ubuntu", "--exec", "python3", script]))
    with tempfile.TemporaryDirectory(prefix="app-catalog-memory-") as directory:
        run(Path(directory))


def run(build):
    repo = ROOT
    here = HERE
    packages = build / "packages"
    packages.mkdir(exist_ok=True)
    (packages / "external").touch()
    platform = repo / "src/platform"
    ui = repo / "src/ui/platform"
    fixture = repo / "tests/unified_memory"
    sdk = repo / "SiFli-SDK/rtos/rtthread"
    source_file = ui / "app_catalog.c"
    source = source_file.read_text()
    guarded = "catalog_snapshot_grow(next, &capacity, count)"
    assert source.count(guarded) == 2, "Update the negative control for catalog growth call sites"
    baseline = source.replace(guarded, "array_grow(next, &capacity, count, sizeof(*next))")
    entry = re.search(r"typedef struct\s*\{[^}]+\} epd_app_entry_t;",
                      (ui / "app_catalog.h").read_text()).group()
    (build / "catalog_entry.inc").write_text(entry + "\n")
    (build / "allocator.inc").write_text(section(
        (platform / "epd_memory.c").read_text(), "void *epd_app_alloc(", "size_t epd_app_psram_available("))
    for name, code in [("baseline", baseline), ("candidate", source)]:
        pieces = [
            section(code, "static epd_app_entry_t *entries;", "typedef struct"),
            section(code, "static void *array_grow", "static bool valid_id"),
            section(code, "void app_catalog_process(", "static bool release_storage("),
            section(code, "static int entry_order(", "const epd_app_entry_t *app_catalog_get(") +
            re.search(r"const epd_app_entry_t \*app_catalog_get\(unsigned index\)\n\{[^}]+\}",
                      code).group(),
        ]
        (build / "catalog.inc").write_text("\n".join(pieces))
        exe = build / name
        command = [
            "gcc", "-std=c11", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
            "-fno-pie", "-no-pie", "-fsanitize=address,undefined", "-fno-sanitize=alignment",
            "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
            "-DEPD_MEMORY_HOST", "-DEPD_MEMORY_DIAGNOSTIC_HOST", "-DRT_USING_MUTEX",
            f"-DCATALOG_BASELINE={int(name == 'baseline')}",
            *[f"-I{p}" for p in (fixture, sdk / "include", platform, ui, build)],
            str(here / "catalog_host.c"), str(platform / "epd_memory_manager.c"),
            str(platform / "epd_memory_sdk.c"),
            str(platform / "epd_resource_runtime.c"), str(ui / "app_memory.c"),
            *[str(sdk / "src" / f"{stem}.c") for stem in ("memheap", "thread", "idle")],
            "-Wl,--gc-sections", "-Wl,--wrap=rt_thread_mem_context_set", "-o", str(exe),
        ]
        subprocess.run(command, check=True, cwd=build)
        result = subprocess.run([str(exe), str(packages)], text=True,
                                capture_output=True, cwd=build)
        diagnostics = result.stdout + result.stderr
        print(diagnostics, end="")
        result.check_returncode()
        contract = subprocess.run([str(exe), str(packages), "--require-system"],
                                  text=True, capture_output=True, cwd=build)
        if contract.returncode:
            print(contract.stderr, end="")
        if name == "baseline":
            assert contract.returncode == 3
            assert "FAIL published catalog snapshot must belong to SYSTEM" in contract.stderr
            print("PASS baseline rejected by SYSTEM ownership contract")
        else:
            contract.check_returncode()
            print("PASS candidate satisfies SYSTEM ownership contract")


if __name__ == "__main__":
    main()
