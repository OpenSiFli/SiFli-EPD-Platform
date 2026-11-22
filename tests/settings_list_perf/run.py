"""Build real settings-list/LVGL tests; optionally compare a saved source baseline."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(tempfile.gettempdir()) / "settings-list-perf")
    parser.add_argument("--reference", type=Path, help="Unmodified ui_settings_list.c for visual/behavior comparison")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    configure = ["cmake", "-S", str(HERE), "-B", str(output / "build"), "-DCMAKE_BUILD_TYPE=Release",
                 "-DREFERENCE_SOURCE=" + (args.reference.resolve().as_posix() if args.reference else "")]
    for name, command in (("configure", configure), ("build", ["cmake", "--build", str(output / "build"),
                                                            "--config", "Release", "--parallel", "8"])):
        result = subprocess.run(command, capture_output=True, timeout=300)
        (output / (name + ".log")).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            print((result.stdout + result.stderr).decode(errors="replace")[-10000:])
            raise SystemExit(result.returncode)
    states = []
    for name in (["settings_list_reference", "settings_list_perf"] if args.reference else ["settings_list_perf"]):
        executable = output / "build" / ("Release/" + name + ".exe" if os.name == "nt" else name)
        result = subprocess.run([str(executable), str(ROOT / "font/MiSans-Normal.ttf")], capture_output=True, timeout=120)
        (output / (name + ".log")).write_bytes(result.stdout + result.stderr)
        text = (result.stdout + result.stderr).decode(errors="replace").replace("\r\n", "\n")
        print(name + "\n" + text, flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
        states.append(re.findall(r"^STATE .+$", text, re.MULTILINE))
    if args.reference:
        if not states[0] or states[0] != states[1]:
            raise SystemExit("Baseline/rendered state comparison failed")
        print(f"Baseline comparison: {len(states[0])} rendered states identical")
    print("Logs:", output)


if __name__ == "__main__":
    main()
