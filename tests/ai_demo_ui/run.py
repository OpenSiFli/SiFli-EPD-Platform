"""Build the real LVGL AI page and capture deterministic offscreen scenarios."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_OUTPUT = Path(tempfile.gettempdir()) / "keyboard-ui-20261029"
KEYBOARD_FRAMES = ("keyboard-idle", "keyboard-composing", "keyboard-committed")
KEYBOARD_CASES = ("keyboard-scenes", "keyboard-scenes-nav84", "keyboard-modes", "keyboard-edit", "keyboard-pointer",
                  "keyboard-long-partial", "keyboard-paging", "keyboard-group-focus", "keyboard-instances",
                  "keyboard-callback-insert-delete-root", "keyboard-callback-insert-delete-target",
                  "keyboard-callback-insert-rebind", "keyboard-callback-ready-delete-root",
                  "keyboard-callback-ready-rebind", "keyboard-callback-cancel-delete-root",
                  "keyboard-callback-cancel-rebind", "keyboard-feedback-delete-viewport",
                  "keyboard-feedback-rebind", "keyboard-feedback-cancel", "keyboard-feedback-delete-root",
                  "keyboard-feedback-delete-target", "keyboard-feedback-alias", "keyboard-feedback-parent",
                  "keyboard-feedback-delete-rebind", "keyboard-feedback-viewport-callback",
                  "keyboard-feedback-nested-delete", "keyboard-feedback-rectangles", "keyboard-ai-refresh")


def function(source, name):
    match = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*?\)\s*\{", source, re.M)
    if not match:
        raise RuntimeError("Missing platform function: " + name)
    depth = 1
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*|[{}]', source[match.end():], re.S):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():match.end() + token.end()]
    raise RuntimeError("Unterminated function: " + name)


def prepare(output):
    generated = output / "generated"
    generated.mkdir(parents=True, exist_ok=True)

    def write(name, text):
        path = generated / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8")

    for name in ("epd_app.h", "epd_ui_scale.h", "epd_keyboard.h", "epd_keyboard_internal.h", "epd_pinyin.h"):
        write("platform/" + name, (ROOT / "src/ui/platform" / name).read_text(encoding="utf-8"))
    write("epd_refresh.h", '#pragma once\n#include "lvgl.h"\n'
          'void epd_refresh_input_objects(lv_obj_t *keyboard, lv_obj_t *editor, bool enabled);\n'
          'static inline void epd_refresh_request_interaction(void) {}\n')
    refresh = (ROOT / "src/ui/platform/epd_refresh.c").read_text(encoding="utf-8")
    write("input_area.inc", function(refresh, "input_area"))
    change_type = re.search(r"typedef enum\s*\{[^}]*\}\s*change_t;", refresh).group()
    write("refresh_classification.inc", change_type + "\nstatic change_t ai_pending_change;\n" + "\n".join(
        function(refresh, name) for name in ("merge_change", "draw_mode", "invalidated")))
    write("epd_waveform.h", (ROOT / "SiFli-SDK/customer/peripherals/display/epd_e0470a03/epd_waveform.h").read_text(encoding="utf-8"))
    # Relocate this unchanged translation unit so its platform i18n include can
    # resolve to the generated host language table without firmware dependencies.
    write("epd_keyboard.c", (ROOT / "src/ui/platform/epd_keyboard.c").read_text(encoding="utf-8"))
    strings = {}
    for path in [* (ROOT / "src/ui/i18n").glob("*.json"), ROOT / "modules/ai_demo/i18n/strings.json"]:
        strings.update(json.loads(path.read_text(encoding="utf-8")).get("strings", {}))
    table = "#pragma once\n#include <stdbool.h>\nextern bool host_locale_en;\n#define epd_get_str(s) (s)\n"
    table += "\n".join("#define " + k + " (host_locale_en ? " + json.dumps(v.get("en_us", v["zh_cn"]), ensure_ascii=False)
                       + " : " + json.dumps(v["zh_cn"], ensure_ascii=False) + ")" for k, v in strings.items()) + "\n"
    write("platform/epd_i18n.h", table)
    write("epd_i18n.h", table)
    launcher = (ROOT / "src/ui/launcher.c").read_text(encoding="utf-8")
    platform = (ROOT / "src/ui/platform/epd_app.c").read_text(encoding="utf-8")
    write("platform_view.inc", "\n".join(function(launcher, n) for n in
          ("epd_obj_init", "label_create", "centered_label", "panel_create")) +
          "\nvoid epd_app_object_init(lv_obj_t *o) { epd_obj_init(o); }\n" +
          "lv_obj_t *epd_app_label(lv_obj_t *p,const char *s,int x,int y,int w,const lv_font_t *f) "
          "{return label_create(p,s,x,y,w,f);}\n" +
          "\n".join(function(platform, n) for n in
          ("epd_app_button", "epd_app_back_button_layout", "epd_app_back_button_cb")))
    scale = (ROOT / "src/ui/platform/epd_ui_scale.c").read_text(encoding="utf-8")
    write("scale.inc", "\n".join(function(scale, n) for n in ("epd_ui_px", "epd_ui_touch", "epd_ui_content_area")))
    from PIL import Image
    icons = []
    for name in ("ui_icon_nav_back", "ui_icon_wifi_on"):
        im = Image.open(ROOT / "assets/ezip/icons" / (name + ".png")).convert("RGBA")
        data = im.tobytes("raw", "BGRA")
        icons.append("_Alignas(32) static const unsigned char " + name + "_data[]={" + ",".join(map(str, data)) + "};")
        icons.append(f"static const lv_image_dsc_t {name}={{.header={{.magic=LV_IMAGE_HEADER_MAGIC,"
                     f".cf=LV_COLOR_FORMAT_ARGB8888,.w={im.width},.h={im.height},.stride={im.width*4}}},"
                     f".data_size={len(data)},.data={name}_data}};")
    write("icons.inc", "\n".join(icons))
    files = [*HERE.rglob("*.c"), *HERE.rglob("*.h"), *HERE.rglob("*.inc"), HERE / "run.py", HERE / "CMakeLists.txt",
             * (ROOT / "modules/ai_demo/src").glob("*.c"),
             * (ROOT / "modules/ai_demo/src").glob("*.h"),
             ROOT / "font/MiSans-Normal.ttf", ROOT / "src/ui/platform/epd_keyboard.c",
             ROOT / "src/ui/platform/epd_refresh.c",
             ROOT / "src/ui/platform/epd_ui_scale.c", ROOT / "src/ui/platform/epd_app.c",
             ROOT / "src/ui/ui_system_font_cache.c", ROOT / "src/ui/ui_font_style.c",
             ROOT / "modules/ai_demo/i18n/strings.json",
             *(ROOT / "src/ui/platform").glob("epd_keyboard*.h"),
             *(ROOT / "src/ui/platform").glob("epd_pinyin*"),
             *(ROOT / "src/ui/i18n").glob("*.json")]
    files = sorted({p for p in files if p.is_file()})
    manifest = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    (output / "sources.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return generated


def check_keyboard_captures(folder):
    """Check unmodified LVGL snapshots as well as the widget geometry records."""
    from PIL import Image, ImageChops, ImageDraw
    errors = []
    images, geometry = [], []
    for name in KEYBOARD_FRAMES:
        path = folder / (name + ".png")
        coordinates = folder / (name + "-geometry.json")
        if not path.is_file() or not coordinates.is_file():
            errors.append("Missing capture or geometry: " + name)
            continue
        try:
            with Image.open(path) as source:
                frame = source.convert("RGB")
            bounds = json.loads(coordinates.read_text(encoding="utf-8"))
            if len(bounds["keys"]) != 4:
                raise ValueError("keys must contain four coordinates")
        except (OSError, ValueError, KeyError, TypeError) as error:
            errors.append(f"Invalid capture or geometry for {name}: {error}")
            continue
        images.append(frame)
        geometry.append(bounds)
        if images[-1].size != (684, 1216):
            errors.append("Unexpected capture dimensions: " + name)
    if len(images) == 3 and len(geometry) == 3:
        if not geometry[0] == geometry[1] == geometry[2]:
            errors.append("Keyboard/key/composer geometry changed between the three states")
        x1, y1, x2, y2 = geometry[0]["keys"]
        box = (x1, y1, x2 + 1, y2 + 1)
        keys = [im.crop(box) for im in images]
        detail_path = folder / "keyboard-preedit.json"
        expected_keys = keys[0].copy()
        if detail_path.is_file():
            detail = json.loads(detail_path.read_text(encoding="utf-8"))
            feedback = detail.get("feedback")
            if feedback is None:
                errors.append("Missing last-key feedback geometry")
            else:
                fx1, fy1, fx2, fy2 = feedback
                ImageDraw.Draw(expected_keys).rectangle((fx1-x1, fy1-y1, fx2-x1, fy2-y1), fill="black")
        if any(lo == hi for lo, hi in keys[0].getextrema()):
            errors.append("Key capture is blank")
        for i in (1, 2):
            if ImageChops.difference(expected_keys, keys[i]).getbbox():
                errors.append("Unexpected key pixels beyond last-key feedback in " + KEYBOARD_FRAMES[i])
            if not ImageChops.difference(images[0], images[i]).getbbox():
                errors.append("State capture did not change in " + KEYBOARD_FRAMES[i])
        if not ImageChops.difference(images[1], images[2]).getbbox():
            errors.append("Composing and committed captures are identical")
        detail_path = folder / "keyboard-preedit.json"
        if not detail_path.is_file():
            errors.append("Missing preedit geometry")
        else:
            detail = json.loads(detail_path.read_text(encoding="utf-8"))
            x1, y1, x2, y2 = detail["preedit"]
            preedit_box = (x1, y1, x2 + 1, y2 + 1)
            idle_float, composing_float = (im.crop(preedit_box) for im in images[:2])
            if not ImageChops.difference(idle_float, composing_float).getbbox():
                errors.append("Preedit exists but is not rendered in the composing capture")
            if sum(composing_float.convert("L").histogram()[:96]) < 20:
                errors.append("Preedit capture has no visible text")
    (folder / "capture-checks.json").write_text(json.dumps({"errors": errors}, indent=2), encoding="utf-8")
    return errors


def keyboard_contact_sheet(folder):
    from PIL import Image, ImageDraw, ImageFont
    sheet = Image.new("RGB", (2148, 1320), "#eeeeee")
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.truetype(str(ROOT / "font/MiSans-Normal.ttf"), 32)
    for i, (name, title) in enumerate(zip(KEYBOARD_FRAMES, ("未输入", "拼音输入中", "选词后"))):
        path = folder / (name + ".png")
        if not path.is_file():
            return
        x = 24 + i * 708
        draw.text((x + 342, 38), title, font=font, fill="#333333", anchor="mm")
        with Image.open(path) as frame:
            sheet.paste(frame, (x, 80))
    sheet.save(folder / "keyboard-contact-sheet.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--scale", type=int, action="append", choices=(90, 95, 100, 110, 120))
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--english-static", action="store_true", help="Also capture English static layouts at 100%% and 120%%.")
    parser.add_argument("--suite", choices=("ai", "keyboard", "all"), default="all")
    parser.add_argument("--keyboard-case", action="append", choices=KEYBOARD_CASES,
                        help="Select keyboard cases; each runs in a separate process.")
    parser.add_argument("--prepare-only", action="store_true", help="Generate host inputs without configuring, building or running C code.")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.keyboard_case and args.suite == "ai":
        parser.error("--keyboard-case requires --suite keyboard or all")
    output = args.output.resolve()
    if output == ROOT or (ROOT in output.parents and HERE not in output.parents):
        parser.error("--output must be outside the repository or inside tests/ai_demo_ui")
    output.mkdir(parents=True, exist_ok=True)
    generated = prepare(output)
    if args.prepare_only:
        print("Prepared host inputs without building or running:", generated)
        return
    spec = importlib.util.spec_from_file_location("host_compiler", HERE.parent / "reader_prefetch/cache_run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    compiler, env = module.compiler_environment(os.environ.get("CC"))
    build = output / "build"

    def run(command, name, timeout=180):
        with (output / name).open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        if result.returncode:
            print((output / name).read_text(encoding="utf-8", errors="replace")[-14000:])
            raise SystemExit(result.returncode)

    print("Building SDK LVGL and AI application...", flush=True)
    run(["cmake", "-S", str(HERE), "-B", str(build), "-DGENERATED=" + generated.as_posix()], "configure.log")
    run(["cmake", "--build", str(build), "--config", "Release", "--parallel", "8"], "build.log", 300)
    executable = build / "Release/ai_demo_ui.exe" if os.name == "nt" else build / "ai_demo_ui"
    failures = []
    runs = []
    scales = args.scale or [100, 90, 95, 110, 120]
    cases = [(s, "zh_cn", "full") for s in scales] if args.suite in ("ai", "all") else []
    if args.english_static and args.suite in ("ai", "all"):
        cases += [(s, "en_us", "static") for s in (100, 120)]
    if args.suite in ("keyboard", "all"):
        cases += [(s, "zh_cn", mode) for s in scales for mode in (args.keyboard_case or KEYBOARD_CASES)]
    for scale, locale, mode in cases:
        base = output / mode if mode.startswith("keyboard-") else output
        folder = (base if locale == "zh_cn" else base / locale) / str(scale)
        folder.mkdir(parents=True, exist_ok=True)
        # Remove only this case's previous captures so a crashed run cannot pass
        # using older images. Build products and other cases remain available.
        for pattern in ("*.ppm", "*.png", "*-geometry.json", "keyboard-preedit.json", "capture-checks.json", "pointer-*.jsonl"):
            for old in folder.glob(pattern):
                old.unlink()
        command = [str(executable), str(scale), str(folder), str(ROOT / "font/MiSans-Normal.ttf"), locale, mode]
        try:
            result = subprocess.run(command, cwd=output, env=env, capture_output=True, timeout=args.timeout)
        except subprocess.TimeoutExpired as error:
            result = subprocess.CompletedProcess(command, 124, error.stdout or b"", (error.stderr or b"") + b"\nTIMEOUT\n")
        (folder / "run.log").write_bytes(result.stdout + result.stderr)
        transcript = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        tally = re.search(r"(\d+) checks, (\d+) failures", transcript)
        record = {"scale": scale, "locale": locale, "mode": mode, "returncode": result.returncode,
                     "checks": int(tally[1]) if tally else None,
                     "failures": int(tally[2]) if tally else None,
                     "idle_extra_flushes": {state: int(count) for state, count in
                         re.findall(r"Idle (\w+): 20 polls, (\d+) extra flushes", transcript)}}
        from PIL import Image
        capture_errors = []
        for ppm in folder.glob("*.ppm"):
            try:
                with Image.open(ppm) as im:
                    im.save(ppm.with_suffix(".png"))
            except OSError as error:
                capture_errors.append(f"Invalid LVGL snapshot {ppm.name}: {error}")
        if mode in ("keyboard-scenes", "keyboard-scenes-nav84"):
            capture_errors.extend(check_keyboard_captures(folder))
            if not capture_errors:
                keyboard_contact_sheet(folder)
        record["capture_errors"] = capture_errors
        failed = bool(result.returncode or not tally or int(tally[1]) == 0 or int(tally[2]) or capture_errors)
        record["passed"] = not failed
        runs.append(record)
        if failed:
            failures.append(f"{mode}/{locale}/{scale}")
        print(f"{mode}/{locale}/{scale}%: {'FAIL' if failed else 'PASS'}", flush=True)
        print(transcript[-4000:])
        for error in capture_errors:
            print("FAIL capture:", error)
    if 100 in scales and args.suite in ("ai", "all"):
        from PIL import ImageDraw, ImageFont
        sheet = Image.new("RGB", (2148, 1320), "#eeeeee")
        draw = ImageDraw.Draw(sheet)
        font = ImageFont.truetype(str(ROOT / "font/MiSans-Normal.ttf"), 32)
        for index, (name, title) in enumerate((("01-empty", "新对话"), ("02-chat", "对话中"), ("03-history", "会话历史"))):
            path = output / "100" / (name + ".png")
            if path.exists():
                x = 24 + index * 708
                draw.text((x + 342, 38), title, font=font, fill="#333333", anchor="mm")
                with Image.open(path) as frame:
                    sheet.paste(frame, (x, 80))
        sheet.save(output / "contact-sheet.png")
    summary = {"suite": args.suite, "scales": scales, "runs": runs, "failed_cases": failures,
               "output": str(output), "size": [684, 1216]}
    (output / "results.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print("Screenshots:", output)
    raise SystemExit(bool(failures))


if __name__ == "__main__":
    main()
