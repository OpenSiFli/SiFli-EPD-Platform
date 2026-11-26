"""Run the Windows host regression suites and retain each suite's output."""

import argparse
import json
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from time import monotonic

ROOT = Path(__file__).resolve().parents[1]
QUICK = {
    "wlan": ["tests/wlan_recovery/run.py"],
    "preferences": ["tests/preference_store/run.py"],
    "reader-cache": ["tests/reader_prefetch/cache_run.py"],
    "reader-position": ["tests/books/run_reader_save_tests.py"],
    "font-cache": ["tests/font_cache/run.py"],
}
FULL = {
    **QUICK,
    "gallery": ["tests/gallery/run.py"],
    "input": ["tests/input_injection/run.py"],
    "refresh": ["tests/epd_refresh/run.py"],
    "panel-driver": ["tests/epd_waveform/driver_run.py"],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("group", nargs="?", choices=("quick", "full"), default="quick")
    parser.add_argument("--suite", choices=FULL, action="append")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    output = args.output or ROOT / "outputs" / "host-tests" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output.mkdir(parents=True, exist_ok=True)
    suites = args.suite or (QUICK if args.group == "quick" else FULL)
    results = []
    for name in suites:
        log = output / f"{name}.log"
        start = monotonic()
        with log.open("wb") as stream:
            try:
                run = subprocess.run([sys.executable, "-u", *FULL[name]], cwd=ROOT,
                                     stdout=stream, stderr=subprocess.STDOUT, timeout=900)
                code = run.returncode
            except subprocess.TimeoutExpired:
                stream.write(b"\nSuite exceeded its 900 s timeout.\n")
                code = 124
        seconds = round(monotonic() - start, 3)
        results.append({"suite": name, "exit_code": code, "seconds": seconds, "log": log.name})
        print(f"{'PASS' if code == 0 else 'FAIL'} {name} ({seconds:.1f} s) {log}", flush=True)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return int(any(result["exit_code"] for result in results))


if __name__ == "__main__":
    sys.exit(main())
