"""Compile and exercise complete production resource sources on the host."""
import importlib.util
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("host_compiler", ROOT / "tests/reader_prefetch/cache_run.py")
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    cc, env = compiler.compiler_environment(os.environ.get("CC"))
    with tempfile.TemporaryDirectory(prefix="host-", dir=HERE) as folder:
        build = Path(folder)
        exe = build / ("resource_host.exe" if os.name == "nt" else "resource_host")
        sources = [HERE / "host.c", ROOT / "src/platform/epd_resource.c"]
        includes = [HERE / "stubs", ROOT / "src/platform"]
        if Path(cc).stem.lower() == "cl":
            command = [cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC", "/Od",
                       *(['/fsanitize=address', '/Zi'] if args.asan else ['/RTC1']),
                       *(f"/I{p}" for p in includes), *map(str, sources), f"/Fe{exe}"]
        else:
            command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       *(f"-I{p}" for p in includes), *map(str, sources), "-o", str(exe)]
        subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
        names = subprocess.check_output([str(exe), "--list"], cwd=build, env=env, text=True, timeout=15).splitlines()
        for name in names:
            subprocess.run([str(exe), name], cwd=build, env=env, check=True, timeout=15)
        reader = ROOT / "modules/common/reader"
        sources = [HERE / "runtime_host.c", ROOT / "src/platform/epd_resource.c",
                   ROOT / "src/platform/epd_resource_runtime.c", ROOT / "src/platform/epd_memory_manager.c",
                   reader / "reader_page_cache.c", reader / "reader_page_resource.c"]
        includes += [reader, reader / "compute/include"]
        runtime_exe = build / ("runtime_host.exe" if os.name == "nt" else "runtime_host")
        for rate in (1000, 100):
            if Path(cc).stem.lower() == "cl":
                command = [cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC", "/Od",
                           *(['/fsanitize=address', '/Zi'] if args.asan else ['/RTC1']),
                           "/wd4200", "/DEPD_MEMORY_HOST", f"/DRT_TICK_PER_SECOND={rate}",
                           *(f"/I{p}" for p in includes), *map(str, sources), f"/Fe{runtime_exe}"]
            else:
                command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                           "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-DEPD_MEMORY_HOST",
                           f"-DRT_TICK_PER_SECOND={rate}", *(f"-I{p}" for p in includes),
                           *map(str, sources), "-o", str(runtime_exe)]
            subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
            runtime_names = subprocess.check_output([str(runtime_exe), "--list"], cwd=build, env=env, text=True, timeout=15).splitlines()
            for name in runtime_names:
                subprocess.run([str(runtime_exe), name], cwd=build, env=env, check=True, timeout=15)
    print(f"Unified resource: {len(names)} production-source cases passed.")
    print(f"Real core/runtime/reader: {len(runtime_names) * 2} cases passed at 1000/100 ticks per second.")


if __name__ == "__main__":
    main()
