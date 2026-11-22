"""Run common reader font transactions and TXT/EPUB source reopen regression."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    command = [sys.executable, str(ROOT / "tests/reader_view_async/run.py"),
               "--case", "font", "--case", "reopen", "--case", "navigation"]
    if args.cc: command += ["--cc", args.cc]
    if args.sanitize: command += ["--sanitize"]
    subprocess.run(command, cwd=ROOT, check=True, timeout=120)

if __name__ == "__main__":
    main()
