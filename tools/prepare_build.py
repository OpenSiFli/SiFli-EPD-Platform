"""Prepare Python dependencies and the local SD package before SCons."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import yaml


REPO = Path(__file__).resolve().parents[1]


def run(command, cwd, **kwargs):
    return subprocess.run(command, cwd=cwd, check=True, **kwargs)


def check_build_environment():
    sdk = REPO / "SiFli-SDK"
    configured_sdk = os.environ.get("SIFLI_SDK")
    if not configured_sdk or Path(configured_sdk).resolve() != sdk.resolve():
        raise RuntimeError("Activate this repository's SiFli-SDK/export.ps1 first.")


def prepare_python_dependencies():
    command = ["uv", "pip", "install", "--python", sys.executable,
               "-r", str(REPO / "tools/requirements-font.txt")]
    index = os.environ.get("SIFLI_SDK_PYPI_DEFAULT_INDEX")
    if index:
        command += ["--default-index", index]
    run(command, REPO)


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


def check_package_revision(graph):
    project = REPO / "project"
    manifest = yaml.safe_load((project / "sf-pkg.yaml").read_text(encoding="utf-8"))
    lock = json.loads((project / ".sf-pkg/conan.lock").read_text(encoding="utf-8"))
    created = {node["ref"] for node in graph["graph"]["nodes"].values()
               if node.get("name") == "epd-sdcard"}
    declared = {ref for ref in manifest.get("requires", [])
                if ref.startswith("epd-sdcard/")}
    locked = {ref.split("%", 1)[0] for ref in lock.get("requires", [])
              if ref.startswith("epd-sdcard/")}
    if len(created) != 1 or created != declared or created != locked:
        raise RuntimeError(
            f"Local SD card package: {', '.join(sorted(created)) or '(missing)'}. "
            f"Declared: {', '.join(sorted(declared)) or '(missing)'}. "
            f"Locked: {', '.join(sorted(locked)) or '(missing)'}. "
            "Update the epd-sdcard reference in project/sf-pkg.yaml and "
            "project/.sf-pkg/conan.lock to match the local sources, then rerun "
            "tools/prepare_build.py.")


def prepare_packages():
    sdk = REPO / "SiFli-SDK"
    component = REPO / "components/sdcard"
    lock = json.loads((component / "conan.lock").read_text(encoding="utf-8"))
    for reference in lock["python_requires"]:
        reference = reference.split("%", 1)[0]
        present = subprocess.run(
            ["conan", "cache", "path", reference], cwd=component,
            capture_output=True).returncode == 0
        if not present:
            run(["conan", "download", reference, "--only-recipe", "-r=artifactory"], component)
    created = run(["conan", "create", ".", "--version=1.0.0", "--no-remote",
                   "--lockfile=conan.lock", "--format=json"], component,
                  stdout=subprocess.PIPE, text=True, encoding="utf-8")
    check_package_revision(json.loads(created.stdout))
    # Do not pass --board: the package Kconfig must exist before resolving it.
    run([sys.executable, str(sdk / "tools/sdk.py"), "sf-pkg", "install"], REPO / "project")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    check_build_environment()
    check_sdk_revision()
    prepare_python_dependencies()
    prepare_packages()
    print("Build dependencies prepared.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
