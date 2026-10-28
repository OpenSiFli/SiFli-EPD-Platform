"""Build the board's C layout engine and render its commands as standalone SVG."""

import argparse
import html
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "modules/board"
CJSON = ROOT / "SiFli-SDK/external/cJSON-1.7.17"


def build_host(directory, cc=None, sources=None, includes=()):
    """Compile the real SDK cJSON and board sources; never write beside sources."""
    compiler = cc or os.environ.get("CC")
    if not compiler:
        compiler = next((shutil.which(name) for name in ("cc", "gcc", "clang", "cl") if shutil.which(name)), None)
    if not compiler:
        raise RuntimeError("A C compiler is required. On Windows use an x64 Native Tools Command Prompt (cl), or pass --cc.")
    directory = Path(directory)
    msvc = Path(compiler).name.lower() in ("cl", "cl.exe")
    target = directory / ("board_host.exe" if os.name == "nt" else "board_host")
    sources = sources or [BOARD / "host/preview.c", BOARD / "board_model.c", CJSON / "cJSON.c"]
    paths = [*includes, BOARD, CJSON, BOARD / "host/include"]
    if msvc:
        command = [compiler, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS", "/DCJSON_HIDE_SYMBOLS"]
        command += ["/I" + str(p) for p in paths]
        command += [str(p) for p in sources] + ["/Fe" + str(target)]
    else:
        command = [compiler, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-DCJSON_HIDE_SYMBOLS"]
        command += ["-I" + str(p) for p in paths]
        command += [str(p) for p in sources] + ["-lm", "-o", str(target)]
    subprocess.run(command, cwd=directory, check=True, capture_output=True, text=True, errors="replace")
    return target


def commands(executable, layout, width=684, height=1216):
    result = subprocess.run([str(executable), str(layout), str(width), str(height)],
                            check=True, capture_output=True, encoding="utf-8")
    return [json.loads(line) for line in result.stdout.splitlines()]


def select_layout(package, user=None):
    if user is not None and Path(user).exists():
        return Path(user)
    return Path(package)


def svg(commands, width, height, summaries=None):
    """Coordinates and clipping boxes come exclusively from board_layout()."""
    summaries = summaries or {}
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
           '<title>内容看板预览</title>',
           '<desc>Geometry from board_layout; browser font rasterization and wrapping differ from LVGL.</desc>',
           f'<rect width="{width}" height="{height}" fill="white"/>']
    for i, c in enumerate(commands):
        x, y, w, h = (c[k] for k in ("x", "y", "width", "height"))
        attrs = f'x="{x}" y="{y}" width="{w}" height="{h}"'
        kind = c["kind"]
        if kind in ("panel", "back"):
            out.append(f'<rect data-kind="{kind}" data-card="{c["card"]}" {attrs} fill="white" stroke="black"/>')
        if kind == "panel":
            continue
        value = c["text"]
        if c["summary"]:
            value = summaries.get(c["summary"], "").encode("utf-8")[:95].decode("utf-8", errors="ignore") or c["empty"]
        value = html.escape(value)
        style = (f'font-family:"Microsoft YaHei","Noto Sans CJK SC",sans-serif;font-size:{c["font_size"]}px;'
                 'line-height:1.2;white-space:pre-wrap;overflow-wrap:anywhere;overflow:hidden;'
                 'color:#222;box-sizing:border-box;width:100%;height:100%;')
        if kind == "back":
            style += 'display:flex;align-items:center;justify-content:center;'
        elif kind != "body":
            style += 'white-space:pre;'
        out.append(f'<foreignObject id="command-{i}" data-kind="{kind}" data-card="{c["card"]}" {attrs}>'
                   f'<div xmlns="http://www.w3.org/1999/xhtml" style="{html.escape(style, quote=True)}">{value}</div></foreignObject>')
    out.append('</svg>')
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("layout", nargs="?", type=Path, default=BOARD / "res/layout.json")
    parser.add_argument("--data", type=Path, help="Optional user layout; an existing invalid override is an error")
    parser.add_argument("--output", type=Path, default=BOARD / "preview.svg")
    parser.add_argument("--width", type=int, default=684)
    parser.add_argument("--height", type=int, default=1216)
    parser.add_argument("--summary", action="append", default=[], metavar="ID=TEXT")
    parser.add_argument("--cc", help="C compiler executable (cl, gcc or clang)")
    args = parser.parse_args()
    try:
        summaries = dict(value.split("=", 1) for value in args.summary)
        with tempfile.TemporaryDirectory(prefix="board-preview-") as directory:
            executable = build_host(directory, args.cc)
            result = commands(executable, select_layout(args.layout, args.data), args.width, args.height)
        args.output.write_text(svg(result, args.width, args.height, summaries), encoding="utf-8")
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            parser.exit(1, str(error.stdout or "") + str(error.stderr or ""))
        parser.exit(1, str(error) + "\n")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
