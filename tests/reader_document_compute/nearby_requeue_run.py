"""Verify withdrawal and requeue using the actual document worker functions."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CASES = ("pending", "active", "promotion", "cancel-during-queue")


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--document-source", type=Path,
                     default=ROOT / "modules/books/document/document.c")
    cli.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    cli.add_argument("--negative-control", action="store_true",
                     help="Restore the old deduplication predicate and require the three requeue failures")
    args = cli.parse_args()
    source = args.document_source.read_text(encoding="utf-8")
    start = source.index("void document_nearby_request(")
    end = source.index("static void wake_worker(void)\n{", start)
    extracted = source[start:end]
    if args.negative_control:
        fixed = "if ((requested && d->nearby_requested && d->nearby_demand == demand) ||"
        if extracted.count(fixed) != 1:
            cli.error("negative control expects the fixed deduplication predicate")
        extracted = extracted.replace(fixed, "if ((requested && d->nearby_demand == demand) ||")
    with tempfile.TemporaryDirectory(prefix="document-nearby-requeue-") as directory:
        build = Path(directory)
        (build / "nearby_requeue_production.inc").write_text(extracted, encoding="utf-8")
        exe = build / ("nearby-host.exe" if os.name == "nt" else "nearby-host")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                        "-I", str(build), str(HERE / "nearby_requeue_test.c"), "-o", str(exe)],
                       check=True, timeout=30)
        for case in CASES:
            result = subprocess.run([str(exe), case], text=True, capture_output=True, timeout=5)
            if args.negative_control and case != "cancel-during-queue":
                marker = "queued == 2 && d.nearby_requested"
                if result.returncode != 1 or marker not in result.stderr:
                    raise SystemExit(result.stdout + result.stderr or f"Unexpected result for {case}")
                print(f"PASS negative control {case}")
            elif result.returncode:
                raise SystemExit(result.stdout + result.stderr)
            else:
                print(result.stdout.strip())


if __name__ == "__main__":
    main()
