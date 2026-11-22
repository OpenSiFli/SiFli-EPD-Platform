"""Check refresh-pause behavior using the SDK LVGL and actual platform scheduler."""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(tempfile.gettempdir()) / "reader-first-frame-api")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    configure = ["cmake", "-S", str(HERE), "-B", str(output)]
    if args.sanitize:
        configure += ["-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g",
                      "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"]
    for label, command in (
        ("configure", configure),
        ("build", ["cmake", "--build", str(output), "--config", "Debug", "--parallel", "4"]),
        ("test", ["ctest", "--test-dir", str(output), "-C", "Debug", "--output-on-failure", "-V"]),
    ):
        with (output / f"{label}.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=300)
        content = (output / f"{label}.log").read_text(encoding="utf-8", errors="replace")
        print(content if label == "test" else content[-1500:])
        if result.returncode:
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
