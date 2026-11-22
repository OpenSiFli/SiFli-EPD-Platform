"""Compile the production page cache and run isolated host behavior tests.

Usage: python tests/reader_prefetch/cache_run.py [--cc COMPILER] [--case NAME]
MSVC users can use an x64 Native Tools prompt. Installed Visual Studio toolchains
are also discovered automatically on Windows. No device libraries are linked.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def compiler_environment(requested):
    env = os.environ.copy()
    if requested and Path(requested).stem.lower() != "cl":
        return requested, env
    compiler = shutil.which(requested or "cl")
    if compiler:
        return compiler, env
    if os.name == "nt":
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / (
            "Microsoft Visual Studio/Installer/vswhere.exe"
        )
        if vswhere.is_file():
            install = subprocess.check_output(
                [str(vswhere), "-latest", "-products", "*", "-requires",
                 "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                text=True, timeout=30,
            ).strip()
            vcvars = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
            if install and vcvars.is_file():
                output = subprocess.check_output(
                    f'cmd.exe /d /s /c ""{vcvars}" >nul && set"',
                    text=True, errors="replace", timeout=30,
                )
                for line in output.splitlines():
                    key, sep, value = line.partition("=")
                    if sep and key:
                        env[key.upper()] = value
                compiler = shutil.which("cl", path=env.get("PATH"))
                if compiler:
                    return compiler, env
    if requested:
        raise SystemExit(f"Cannot find compiler: {requested}")
    for name in ("cc", "clang", "gcc"):
        compiler = shutil.which(name)
        if compiler:
            return compiler, env
    raise SystemExit("Set CC, use --cc, or install an MSVC C toolchain.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--case", action="append", help="Run a named case; repeat to select more.")
    parser.add_argument("--timeout", type=float, default=10, help="Timeout per case in seconds.")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    compiler, env = compiler_environment(args.cc)
    tests = Path(__file__).resolve().parent
    reader = tests.parents[1] / "modules/common/reader"
    with tempfile.TemporaryDirectory(prefix="cache_build_", dir=tests) as output:
        build = Path(output)
        executable = build / ("cache_host.exe" if os.name == "nt" else "cache_host")
        sources = [str(tests / "cache_host.c"), str(reader / "reader_page_cache.c")]
        includes = [reader, reader / "compute/include"]
        if Path(compiler).stem.lower() == "cl":
            # MSVC emits C4200 for the cache's C11 flexible array member.
            command = [compiler, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC",
                       "/wd4200", "/Od", "/RTC1", "/D_CRT_SECURE_NO_WARNINGS",
                       *(f"/I{path}" for path in includes), *sources, f"/Fe{executable}"]
        else:
            command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       *(f"-I{path}" for path in includes), *sources, "-o", str(executable)]
        print(f"Compiler: {compiler}", flush=True)
        subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
        available = subprocess.check_output(
            [str(executable), "--list"], cwd=build, env=env, text=True, timeout=args.timeout,
        ).splitlines()
        selected = args.case or available
        unknown = set(selected) - set(available)
        if unknown:
            parser.error(f"Unknown cases: {', '.join(sorted(unknown))}")
        failures = []
        for name in selected:
            try:
                subprocess.run([str(executable), name], cwd=build, env=env, check=True, timeout=args.timeout)
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                failures.append(name)
                print(f"FAIL {name}: {error}", flush=True)
        print(f"Page cache: {len(selected) - len(failures)}/{len(selected)} cases passed.", flush=True)
        if failures:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
