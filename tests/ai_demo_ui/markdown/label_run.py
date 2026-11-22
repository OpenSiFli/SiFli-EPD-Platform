"""Compare long Markdown paragraph rendering with and without the label fast path."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
RENDERER = Path("modules/ai_demo/src/chat_markdown.c")
CASES = ("contracts", "links", "failures", "style-failures")


def linux(path):
    path = Path(path).resolve()
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:]


def fixture(record):
    if record is None:
        return "".join(
            f"Synthetic paragraph {i:03d}: alphabetic words and line wrap. "
            "中文样本，检查标点、软换行和英文字宽。 "
            "Words with variable lengths, punctuation! (12345) Continue on the next line.\n"
            for i in range(1, 161)
        )
    data = json.loads(record.read_text(encoding="utf-8-sig"))
    if data.get("crc_valid") is not True:
        raise ValueError("Pass a decoded session JSON whose record CRC was verified.")
    message = next((m for m in reversed(data["messages"])
                    if m["role"] == 1 and m.get("complete")), None)
    if message is None or not isinstance(message.get("text"), str):
        raise ValueError("The decoded session needs a complete assistant message.")
    return message["text"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--record", type=Path)
    parser.add_argument("--renderer-patch", type=Path)
    args = parser.parse_args()
    if os.name == "nt":
        command = ["wsl.exe", "-d", "Ubuntu", "--exec", "python3", linux(__file__),
                   "--output", linux(args.output), "--repo", linux(args.repo)]
        for option, value in (("--record", args.record), ("--renderer-patch", args.renderer_patch)):
            if value:
                command.extend((option, linux(value)))
        raise SystemExit(subprocess.call(command))

    root, output = args.repo.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = root / RENDERER
    if args.renderer_patch:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory) / RENDERER
            temporary.parent.mkdir(parents=True)
            temporary.write_text(source.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
            subprocess.run(["git", "apply", str(args.renderer_patch.resolve())],
                           cwd=directory, check=True)
            renderer = temporary.read_bytes()
    else:
        renderer = source.read_bytes()
    if b"AI_MARKDOWN_PLAIN_LABEL" not in renderer:
        parser.error("Use a renderer with the plain-label candidate, or pass --renderer-patch.")
    for variant in ("span", "label"):
        (output / f"{variant}_renderer.inc").write_bytes(renderer)
    text = fixture(args.record)
    if not 4096 <= len(text.encode("utf-8")) <= 65536:
        parser.error("The fixture must be between 4 and 64 KiB of UTF-8.")
    sample = output / "long.txt"
    sample.write_bytes(text.encode("utf-8"))
    build = output / "build"
    subprocess.run(["cmake", "-S", str(HERE), "-B", str(build), "-G", "Ninja",
                    f"-DROOT={root}", f"-DGENERATED={output}", "-DMARKDOWN_LABEL_COMPARE=ON"], check=True)
    subprocess.run(["cmake", "--build", str(build), "-j", "4"], check=True)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1")
    results = {}
    for variant in ("span", "label"):
        for case in ("compare", *CASES):
            command = [str(build / f"markdown_{variant}"), case, str(root / "font/MiSans-Normal.ttf")]
            if case == "compare":
                command.append(str(sample))
            run = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
            (output / f"{variant}-{case}.log").write_text(run.stdout, encoding="utf-8")
            if run.returncode:
                print(run.stdout, end="", file=sys.stderr)
                run.check_returncode()
            results[variant, case] = run.stdout.splitlines()
            print(variant, case, "PASS", flush=True)

    for case, prefixes, count in (("compare", ("GEOMETRY ", "PIXELS "), 16),
                                  ("contracts", ("VISUAL ",), None)):
        span, label = [[line for line in results[variant, case] if line.startswith(prefixes)]
                       for variant in ("span", "label")]
        if not span or span != label or (count is not None and len(span) != count):
            raise SystemExit(f"{case}: label and span pixels or geometry differ. See logs in {output}")
    print("PASS matching geometry and pixels", output)


if __name__ == "__main__":
    main()
