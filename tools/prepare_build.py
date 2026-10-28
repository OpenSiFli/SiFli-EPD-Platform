"""Check the pinned SDK revision and install the local SD package before SCons."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


REPO = Path(__file__).resolve().parents[1]


def run(command, cwd, **kwargs):
    return subprocess.run(command, cwd=cwd, check=True, **kwargs)


def check_sdk_revision():
    sdk = REPO / "SiFli-SDK"
    expected = run(["git", "rev-parse", ":SiFli-SDK"], REPO,
                   capture_output=True, text=True).stdout.strip()
    revision = run(["git", "rev-parse", "HEAD"], sdk,
                   capture_output=True, text=True).stdout.strip()
    if revision != expected:
        raise RuntimeError(
            f"SDK HEAD is {revision}, expected {expected}. "
            "Preserve any SDK edits, then run git submodule update --init --recursive "
            "from the repository root; do not use submodule update --remote.")


def prepare_packages():
    sdk = REPO / "SiFli-SDK"
    configured_sdk = os.environ.get("SIFLI_SDK")
    if not configured_sdk or Path(configured_sdk).resolve() != sdk.resolve():
        raise RuntimeError("Activate this repository's SiFli-SDK/export.ps1 first.")
    component = REPO / "components/sdcard"
    lock = json.loads((component / "conan.lock").read_text(encoding="utf-8"))
    for reference in lock["python_requires"]:
        reference = reference.split("%", 1)[0]
        present = subprocess.run(
            ["conan", "cache", "path", reference], cwd=component,
            capture_output=True).returncode == 0
        if not present:
            run(["conan", "download", reference, "--only-recipe", "-r=artifactory"], component)
    run(["conan", "create", ".", "--version=1.0.0", "--no-remote",
         "--lockfile=conan.lock"], component)
    # Do not pass --board: the package Kconfig must exist before resolving it.
    run([sys.executable, str(sdk / "tools/sdk.py"), "sf-pkg", "install"], REPO / "project")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    check_sdk_revision()
    prepare_packages()
    print("SD card package prepared.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
