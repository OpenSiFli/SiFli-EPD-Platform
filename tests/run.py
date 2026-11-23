"""Run the Windows host regression suites and retain each suite's output."""

import argparse
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from time import monotonic
from report import Report

ROOT = Path(__file__).resolve().parents[1]
QUICK = {
    "wlan": ["tests/wlan_recovery/run.py"],
    "preferences": ["tests/preference_store/run.py"],
    "reader-cache": ["tests/reader_prefetch/cache_run.py"],
    "reader-position": ["tests/books/run_reader_save_tests.py"],
}
FULL = {
    **QUICK,
    "gallery": ["tests/gallery/run.py"],
    "input": ["tests/input_injection/run.py"],
    "refresh": ["tests/epd_refresh/run.py"],
    "panel-driver": ["tests/epd_waveform/driver_run.py"],
    "app-memory-reclaim": ["tests/app_memory_pressure/run.py"],
    "sdcard-release": ["tests/sdcard_release/run.py"],
    "storage-io": ["tests/storage_perf/io_run.py"],
}


def failure_category(log):
    raw = log.read_bytes()
    if re.search(rb"FileNotFoundError|ModuleNotFoundError|fatal error C1083|CMake Error.*source file",
                 raw, flags=re.IGNORECASE):
        return "fixture_missing"
    if re.search(rb"(fatal )?error (?:C|LNK)\d+|CMake Error", raw, flags=re.IGNORECASE):
        return "build_error"
    return "test_failed"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("group", nargs="?", choices=("quick", "full"), default="quick")
    parser.add_argument("--suite", choices=FULL, action="append")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    output = args.output or ROOT / "outputs" / "host-tests" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    suites = args.suite or (QUICK if args.group == "quick" else FULL)
    report = Report(output, "host", {"board": "host", "group": args.group})
    if os.name != "nt" or any(not shutil.which(command) for command in ("cmake", "ctest")):
        report.add(case="runner", status="error", category="runner_unavailable", seconds=0,
                   error="Windows, CMake and CTest are required")
        return report.finish()
    for name in suites:
        log = output / f"{name}.log"
        start = monotonic()
        command = FULL[name]
        category = None
        with log.open("wb") as stream:
            if not (ROOT / command[0]).is_file():
                stream.write(f"Missing test entry: {command[0]}\n".encode())
                code, category = 2, "fixture_missing"
            else:
                try:
                    run = subprocess.run([sys.executable, "-u", *command], cwd=ROOT,
                                         stdout=stream, stderr=subprocess.STDOUT, timeout=180)
                    code = run.returncode
                except subprocess.TimeoutExpired:
                    stream.write(b"\nSuite exceeded its 180 s timeout.\n")
                    code, category = 124, "timeout"
        seconds = round(monotonic() - start, 3)
        if code and category is None:
            category = failure_category(log)
        status = "pass" if code == 0 else "fail" if category == "test_failed" else "error"
        report.add(case=name, status=status, category=category, exit_code=code, command=command,
                   seconds=seconds, log=log.name)
        print(f"{'PASS' if code == 0 else 'FAIL'} {name} ({seconds:.1f} s) {log}", flush=True)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
