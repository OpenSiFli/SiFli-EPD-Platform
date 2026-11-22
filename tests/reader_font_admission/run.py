"""Targeted ASan regression for nonblocking reader font admission."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def function(text, name):
    m = re.search(r"^(?:static )?(?:bool|void) " + re.escape(name) + r"\([^\n]*\)\n\{", text, re.M)
    assert m, name
    begin = text.index("{", m.start())
    depth = 0
    for index in range(begin, len(text)):
        depth += (text[index] == "{") - (text[index] == "}")
        if depth == 0:
            return text[m.start():index + 1] + "\n"
    raise AssertionError(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-root", type=Path, default=ROOT, help="Production checkout by default; partial candidate trees inherit unchanged dependencies.")
    parser.add_argument("--output", type=Path, default=ROOT / "tmp/reader-font-admission")
    parser.add_argument("--case", action="append", help="Run selected positive cases only; default runs all targeted cases and negative controls.")
    args = parser.parse_args()
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    cc, env = compiler_environment(os.environ.get("CC"))
    report_dir = args.output.resolve()
    build = report_dir / "build"
    build.mkdir(parents=True, exist_ok=True)
    def source(relative):
        selected = args.source_root.resolve() / relative
        return selected if selected.is_file() else ROOT / relative
    paths = ["src/ui/ui_reader_font_remote.c", "src/ui/ui_reader_font_remote.h", "src/ui/platform/epd_app_font.h",
             "src/ui/platform/epd_app.c", "modules/common/reader/reader_view.c", "modules/books/pages.c"]
    report = {"sanitize": args.sanitize, "source_root": str(args.source_root.resolve()),
              "source_sha256": {p: sha(source(p)) for p in paths}, "runs": []}
    view = source("modules/common/reader/reader_view.c").read_text(encoding="utf-8")
    original_view = view.replace("    if (!epd_app_reading_font_can_create(6)) return false;\n", "")
    font = source("src/ui/ui_reader_font_remote.c").read_text(encoding="utf-8")
    bridge = function(source("src/ui/platform/epd_app.c").read_text(encoding="utf-8"), "epd_app_reading_font_can_create")
    includes = [ROOT / "tests/reader_font_remote", ROOT / "tests/font_cache", ROOT / "SiFli-SDK/external/lvgl_v9",
                ROOT / "src/ui", ROOT / "src/platform", ROOT / "src/services", ROOT / "src/thirdparty/miniz",
                ROOT / "components/compute/include"]
    pages = source("modules/books/pages.c").read_text(encoding="utf-8")
    original_pages = pages.replace("        if (!epd_app_reading_font_can_create(6)) return;\n", "")
    log = []

    def execute(command, timeout=30):
        result = subprocess.run(list(map(str, command)), cwd=build, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout)
        output = result.stdout.decode("utf-8", errors="replace")
        log.append("COMMAND " + subprocess.list2cmdline(list(map(str, command))) + "\n" + output)
        (report_dir / "validation.log").write_text("\n".join(log), encoding="utf-8")
        return result.returncode, output

    def compile_source(label, selected_font, selected_view, selected_pages=pages):
        directory = build / label
        directory.mkdir(exist_ok=True)
        (directory / "prepare_fonts.inc").write_text(function(selected_view, "prepare_fonts"), encoding="utf-8")
        helpers = ["ui_configuration", "epd_reader_settings_changed", "ui_configuration_rejected", "epd_reader_reflow"]
        (directory / "reflow.inc").write_text("\n".join(function(selected_view, f) for f in helpers), encoding="utf-8")
        (directory / "admission_bridge.inc").write_text(bridge, encoding="utf-8")
        (directory / "reader_resume.inc").write_text(function(selected_pages, "reader_resume_prepare"), encoding="utf-8")
        selected = re.sub(r'#include "(\.\.?/[^"\n]+)"',
                          lambda m: '#include "' + (ROOT / "src/ui" / m[1]).resolve().as_posix() + '"', selected_font)
        selected_path = directory / "selected_font.c"
        selected_path.write_text(selected, encoding="utf-8")
        exe = directory / "regression.exe"
        command = [cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC", "/Od",
                   "/D_CRT_SECURE_NO_WARNINGS", "/DLV_CONF_INCLUDE_SIMPLE", "/wd4201", "/wd4244",
                   "/wd4214", "/wd4706", "/wd4267", "/wd4389", "/wd4459", f"/I{directory}",
                   *(f"/I{x}" for x in includes),
                   f"/FI{source('src/ui/platform/epd_app_font.h')}",
                   f'/DREADER_FONT_TEST_SOURCE="{selected_path.as_posix()}"', str(HERE / "regression.c"),
                   str(ROOT / "src/ui/ui_reader_font_source_fd.c"), f"/Fe{exe}", f"/Fd{directory}/compile.pdb"]
        command += ["/fsanitize=address", "/Zi"] if args.sanitize else ["/RTC1"]
        rc, out = execute(command, 60)
        if rc:
            print(out)
            raise SystemExit(f"Build failed: {label}, {rc}")
        return exe

    def check(exe, label, case, expected=0, contains=()):
        rc, out = execute([exe, case])
        ok = rc == expected and all(value in out for value in contains)
        if expected:
            ok &= "Assertion failed:" in out and "AddressSanitizer" not in out
        report["runs"].append({"source": label, "case": case, "exit": rc, "expected_exit": expected, "passed": ok})
        (report_dir / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"{'PASS' if ok else 'FAIL'} {label}/{case} exit={rc}", flush=True)
        if not ok:
            print(out)
            raise SystemExit(1)

    candidate = compile_source("candidate", font, view)
    cases = ["roundtrip", "delayed_retirement", "held_page_and_draw", "admission_rapid_busy",
             "admission_live_old_frame", "admission_real_failure", "admission_capacity_only", "admission_ui_entry"]
    selected_cases = args.case or cases
    assert not set(selected_cases) - set(cases), "Unknown positive case"
    for case in selected_cases:
        check(candidate, "candidate", case)
    if args.case:
        report["passed"] = True
        (report_dir / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"PASS {len(selected_cases)} selected cases; {report_dir / 'validation.log'}")
        return
    assert original_view != view and original_pages != pages, "Admission gates are missing from selected source"
    no_gate = compile_source("without-admission", font, original_view)
    check(no_gate, "negative-without-admission", "admission_rapid_busy", 3,
          ("Assertion failed: !ok && !ui.configuration_failed", "slot=0 stage=create attempt=1"))
    limit16 = re.sub(r"^#define FONT_LIMIT .*", "#define FONT_LIMIT 16u", font, flags=re.M)
    old_limit = compile_source("limit16", limit16, view)
    check(old_limit, "baseline", "baseline_signature", contains=("slot=2 stage=create attempt=1", "slot=0 stage=create attempt=2"))
    check(old_limit, "negative-limit16", "admission_rapid_busy", 3, ("Assertion failed: ok",))
    ignored_error = view.replace("state_dispose(configuration); ui_configuration_rejected(); return false;",
                                "state_dispose(configuration); return false;")
    assert ignored_error != view
    silent_error = compile_source("without-error-reporting", font, ignored_error)
    check(silent_error, "negative-without-error-reporting", "admission_real_failure", 3,
          ("Assertion failed: !ok && ui.configuration_failed",))
    without_ui_gate = compile_source("without-ui-admission", font, view, original_pages)
    check(without_ui_gate, "negative-without-ui-admission", "admission_ui_entry", 3,
          ("Assertion failed: !ui_draw_waits && !ui_font_rebinds",))
    report["passed"] = True
    report["positive_cases"] = 8
    report["negative_controls"] = 4
    (report_dir / "validation.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"PASS {len(report['runs'])} targeted outcomes; {report_dir / 'validation.log'}")


if __name__ == "__main__":
    main()
