"""Run the production compute scheduler with Windows threads and transport doubles."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / "reader_prefetch"))
from cache_run import compiler_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--case", action="append")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--trace", action="store_true", help="Enable successful compute-session diagnostic output.")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("The RT-Thread doubles use Windows synchronization primitives.")
    cc, env = compiler_environment(args.cc)
    includes = [HERE, ROOT / "src/platform", ROOT / "src/services", ROOT / "components/compute/include"]
    output = ROOT / "tmp/compute_scheduler"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="build-", dir=output) as directory:
        build = Path(directory)
        for mode in ("acpu", "unavailable"):
            exe = build / (mode + ".exe")
            sources = [HERE / ("host.c" if mode == "acpu" else "unavailable.c"),
                       ROOT / "components/compute/epd_compute_protocol.c"]
            if Path(cc).stem.lower() == "cl":
                command = [cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC",
                           "/wd4200", "/Od", *( ["/Zi", "/fsanitize=address"] if args.sanitize else ["/RTC1"] ), "/D_CRT_SECURE_NO_WARNINGS",
                           *(f"/I{p}" for p in includes), *map(str, sources), f"/Fe{exe}"]
            else:
                command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                           *(["-fsanitize=address,undefined"] if args.sanitize else []),
                           *(f"-I{p}" for p in includes), *map(str, sources), "-o", str(exe)]
            if args.trace:
                command.insert(1, "/DEPD_COMPUTE_TRACE=1" if Path(cc).stem.lower() == "cl" else "-DEPD_COMPUTE_TRACE=1")
                if Path(cc).stem.lower() == "cl": command.insert(1, "/wd4127")
            subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
            if mode == "unavailable":
                subprocess.run([str(exe)], cwd=build, env=env, check=True, timeout=15)
                continue
            cases = subprocess.check_output([str(exe), "--list"], text=True, env=env).splitlines()
            selected = args.case or cases
            if set(selected) - set(cases):
                parser.error("Unknown case")
            for case in selected:
                subprocess.run([str(exe), case], cwd=build, env=env, check=True, timeout=15)
            print(f"Compute scheduler: {len(selected)}/{len(selected)} cases passed.", flush=True)


if __name__ == "__main__":
    main()
