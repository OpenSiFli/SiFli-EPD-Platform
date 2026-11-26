"""Compile the platform cache with the checkout's native LVGL cache/font code."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parents[1]
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment


def collect_cases(programs, env):
    cases = {}
    for program in programs:
        names = subprocess.check_output([str(program), "--list"], cwd=ROOT, env=env,
                                        text=True, timeout=10).splitlines()
        if not names:
            raise RuntimeError(f"{program.name} listed no font cache cases")
        for name in names:
            if not name or name in cases:
                raise RuntimeError(f"{program.name} listed an empty or duplicate case: {name!r}")
            cases[name] = program
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    compiler, env = compiler_environment(args.cc)
    lvgl = ROOT / "SiFli-SDK/external/lvgl_v9"
    units = ["misc/cache/lv_cache.c", "misc/cache/lv_cache_entry.c",
             "misc/cache/class/lv_cache_lru_rb.c", "misc/lv_rb.c", "misc/lv_ll.c",
             "misc/lv_iter.c", "misc/lv_circle_buf.c", "misc/lv_array.c", "misc/lv_math.c",
             "misc/lv_area.c", "misc/lv_color.c", "misc/lv_color_op.c", "tick/lv_tick.c",
             "draw/convert/lv_draw_buf_convert.c",
             "font/lv_font.c", "draw/lv_draw_buf.c", "stdlib/lv_mem.c",
             "stdlib/clib/lv_mem_core_clib.c", "stdlib/clib/lv_string_clib.c",
             "stdlib/clib/lv_sprintf_clib.c", "osal/lv_os.c", "libs/tiny_ttf/lv_tiny_ttf.c"]
    sources = [ROOT / "tests/unified_resource/font_host.c", ROOT / "src/platform/epd_resource.c",
               *(lvgl / "src" / name for name in units)]
    includes = [TESTS, ROOT / "tests/unified_resource/stubs", lvgl, ROOT / "src/ui", ROOT / "src/platform", ROOT / "src/services"]
    with tempfile.TemporaryDirectory(prefix="font_cache_") as output:
        build = Path(output)
        executable = build / ("host.exe" if os.name == "nt" else "host")
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c11", "/utf-8", "/W3", "/Od", "/Gy", "/UNDEBUG",
                       "/D_CRT_SECURE_NO_WARNINGS", "/DLV_CONF_INCLUDE_SIMPLE",
                       *(f"/I{p}" for p in includes), *map(str, sources), f"/Fe{executable}",
                       "/link", "/OPT:REF"]
        else:
            command = [compiler, "-std=c11", "-Wall", "-Wextra", "-g", "-O1",
                       "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
                       "-DLV_CONF_INCLUDE_SIMPLE", *(f"-I{p}" for p in includes),
                       *map(str, sources), "-Wl,--gc-sections", "-lm", "-o", str(executable)]
        subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
        chain = build / "chain.exe"
        chain_includes = [ROOT / "tests/reader_font_remote", *includes,
                          ROOT / "components/compute/include"]
        if os.name == "nt":
            subprocess.run([compiler, "/nologo", "/std:c11", "/utf-8", "/W3", "/Od", "/Gy", "/UNDEBUG",
                            "/D_CRT_SECURE_NO_WARNINGS", "/DLV_CONF_INCLUDE_SIMPLE",
                            *(f"/I{p}" for p in chain_includes), str(TESTS / "remote_chain.c"),
                            str(ROOT / "src/ui/ui_reader_font_source_fd.c"),
                            f"/Fe{chain}", "/link", "/OPT:REF"], cwd=build, env=env, check=True, timeout=60)
        programs = [executable, chain] if os.name == "nt" else [executable]
        available = collect_cases(programs, env)
        cases = args.case or available
        unknown = set(cases) - available.keys()
        if unknown:
            parser.error(f"Unknown font cache cases: {', '.join(sorted(unknown))}")
        failures = []
        for case in cases:
            result = subprocess.run([str(available[case]), case], cwd=ROOT, env=env, timeout=30)
            if result.returncode:
                failures.append(case)
                print(f"FAIL {case}: exit {result.returncode}", flush=True)
        print(f"Font cache: {len(cases) - len(failures)}/{len(cases)} passed", flush=True)
        if failures:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
