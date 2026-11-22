"""Exercise the real asynchronous font/cache implementation with Windows workers."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import struct
import zlib
import hashlib
import json
import posixpath
import re
import xml.etree.ElementTree as ET
import zipfile
from urllib.parse import unquote

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / "reader_prefetch"))
from cache_run import compiler_environment

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--no-eager", action="store_true", help="Disable foreground eager pixels for A/B comparison.")
parser.add_argument("--host-only", action="store_true", help="Run only the asynchronous HCPU host cases.")
parser.add_argument("--source-root", type=Path, help="Use an alternate font source/header snapshot with --host-only.")
parser.add_argument("--repeat", type=int, default=1, help="Run each selected host case in a fresh process this many times.")
parser.add_argument("--case", action="append", help="Run a named host case, real_pipeline, real_preview, or ftz; repeat to select more.")
parser.add_argument("--font-pack", type=Path, help="Use an existing FTZ1 package for host integration tests.")
body_input = parser.add_mutually_exclusive_group()
body_input.add_argument("--epub", type=Path, help="Extract the opening body text of an EPUB spine into tmp.")
body_input.add_argument("--text-fixture", type=Path, help="Use an existing UTF-8 body fixture (1..65536 bytes).")
parser.add_argument("--spine-index", type=int, default=3, help="One-based EPUB spine index (default: 3).")
parser.add_argument("--body-timeout", type=float, default=30, help="Seconds per body/style process (default: 30).")
parser.add_argument("--body-render-chars", type=int, default=0, help="Render only this many leading codepoints after measuring the fixture (0: all).")
args = parser.parse_args()
if args.repeat < 1:
    parser.error("repeat must be positive")
if args.source_root and not args.host_only:
    parser.error("source-root requires --host-only")
if args.spine_index < 1 or not 0 < args.body_timeout <= 120:
    parser.error("spine-index must be positive; body-timeout must be in (0, 120].")
if args.body_render_chars < 0:
    parser.error("body-render-chars must be nonnegative")
if (args.epub or args.text_fixture) and args.case and "real_pipeline" not in args.case:
    parser.error("Body input requires --case real_pipeline (or all cases).")
tmp = ROOT / "tmp/reader_font_remote"
tmp.mkdir(parents=True, exist_ok=True)
body_directory = None
if args.epub or args.text_fixture:
    metadata = {}
    if args.epub:
        with zipfile.ZipFile(args.epub) as book:
            container = ET.fromstring(book.read("META-INF/container.xml"))
            package = container.find(".//{*}rootfile").attrib["full-path"]
            opf = ET.fromstring(book.read(package))
            manifest = {item.attrib["id"]: item.attrib["href"] for item in opf.findall(".//{*}manifest/{*}item")}
            spine = opf.findall(".//{*}spine/{*}itemref")
            if args.spine_index > len(spine):
                parser.error("spine-index exceeds this EPUB's spine length")
            href = manifest[spine[args.spine_index - 1].attrib["idref"]]
            member = posixpath.normpath(posixpath.join(posixpath.dirname(package), unquote(href.split("#")[0])))
            xhtml_bytes = book.read(member)
            xhtml = ET.fromstring(xhtml_bytes)
            body = xhtml.find(".//{*}body")
            if body is None:
                parser.error("Selected spine has no XHTML body")
            blocks, paragraphs = [], 0
            for element in body.iter():
                tag = element.tag.rsplit("}", 1)[-1]
                if tag not in {"p", "h1", "h2", "h3", "h4", "h5", "h6"}:
                    continue
                # Collapse XML formatting whitespace, preserving NBSP and ideographic spaces.
                value = re.sub(r"[\t\r\n ]+", " ", "".join(element.itertext())).strip(" ")
                if not value:
                    continue
                blocks.append(value)
                paragraphs += tag == "p"
                if paragraphs >= 3 and len("\n".join(blocks).encode("utf-8")) >= 1024:
                    break
            text = "\n".join(blocks)
            if paragraphs < 3 or len(text.encode("utf-8")) < 1024:
                parser.error("Selected spine does not contain three paragraphs and 1024 UTF-8 bytes")
            metadata = {"epub": str(args.epub.resolve()), "spine_index": args.spine_index,
                        "member": member, "paragraphs": paragraphs, "blocks": len(blocks),
                        "xhtml_sha256": hashlib.sha256(xhtml_bytes).hexdigest()}
    else:
        text = args.text_fixture.read_bytes().decode("utf-8")
        metadata = {"input": str(args.text_fixture.resolve())}
    encoded = text.encode("utf-8")
    if not 0 < len(encoded) <= 65536:
        parser.error("Body fixture must contain 1..65536 UTF-8 bytes")
    body_directory = Path(tempfile.mkdtemp(prefix="body-", dir=tmp))
    fixture = body_directory / "body.utf8.txt"
    fixture.write_bytes(encoded)
    metadata.update(bytes=len(encoded), codepoints=len(text), unique_codepoints=len(set(text)),
                    sha256=hashlib.sha256(encoded).hexdigest())
    (body_directory / "fixture.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Body fixture: {len(encoded)} bytes, {len(text)} codepoints, {len(set(text))} unique; {body_directory}", flush=True)
cc, env = compiler_environment(os.environ.get("CC"))
includes = [HERE, ROOT / "SiFli-SDK/external/lvgl_v9", ROOT / "src/ui", ROOT / "src/platform",
            ROOT / "src/services", ROOT / "src/thirdparty/miniz", ROOT / "components/compute/include"]
with tempfile.TemporaryDirectory(prefix="host-", dir=tmp) as directory:
    exe = Path(directory) / "host.exe"
    command = [cc, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC", "/Od", "/RTC1",
               "/D_CRT_SECURE_NO_WARNINGS", "/DLV_CONF_INCLUDE_SIMPLE", "/wd4201", "/wd4244",
               "/wd4214", "/wd4706", "/wd4267", *(f"/I{x}" for x in includes), str(HERE / "host.c"),
               str(ROOT / "src/ui/ui_reader_font_source_fd.c"), f"/Fe{exe}"]
    if args.source_root:
        source_root = args.source_root.resolve()
        font_source = source_root / "src/ui/ui_reader_font_remote.c"
        font_header = source_root / "src/ui/platform/epd_app_font.h"
        if not font_source.is_file() or not font_header.is_file():
            parser.error("source-root must contain the remote font source and epd_app_font.h")
        # Partial snapshots keep unchanged dependencies in the main checkout.
        # Normalize only relative include paths; compile the selected code as-is.
        selected = font_source.read_text(encoding="utf-8")
        selected = re.sub(r'#include "(\.\.?/[^"\n]+)"',
                          lambda m: '#include "' + (ROOT / "src/ui" / m[1]).resolve().as_posix() + '"', selected)
        copy = Path(directory) / "selected_font.c"
        copy.write_text(selected, encoding="utf-8")
        command[1:1] = [f'/DREADER_FONT_TEST_SOURCE="{copy.as_posix()}"', f"/FI{font_header}"]
        print(f"Font snapshot: {font_source}; SHA256={hashlib.sha256(font_source.read_bytes()).hexdigest()}", flush=True)
    if args.no_eager:
        command.insert(1, "/DUI_READER_FONT_EAGER=0")
    if args.sanitize:
        command.remove("/RTC1")
        command += ["/fsanitize=address", "/Zi"]
    subprocess.run(command, cwd=directory, env=env, check=True, timeout=60)
    cases = subprocess.check_output([str(exe), "--list"], env=env, text=True, timeout=10).splitlines()
    if args.case:
        unknown = set(args.case) - set(cases) - {"real_pipeline", "real_preview", "ftz"}
        if unknown:
            parser.error(f"Unknown cases: {', '.join(sorted(unknown))}")
        cases = [case for case in cases if case in args.case]
    for case in cases:
        for _ in range(args.repeat):
            subprocess.run([str(exe), case], env=env, check=True, timeout=20)
    print(f"Remote reader font: {len(cases)}/{len(cases)} cases passed; {len(cases) * args.repeat} runs.")
    if args.host_only or (args.case and not set(args.case) & {"real_pipeline", "real_preview", "ftz"}):
        sys.exit(0)
    # The integration executable uses both production pipe endpoints and the
    # actual ACPU font engine. Only transport/stack switching and byte storage
    # are replaced; real MiSans bytes flow through the HCPU READ service.
    component = ROOT / "components/compute_reader_font"
    font = ROOT / "font/MiSans-Normal.ttf"
    pack_file = args.font_pack.resolve() if args.font_pack else ROOT / "project/build_sf32lb57-epd-gyd7n6_hcpu/font/MiSans-Normal.ftz"
    packed = pack_file.read_bytes()
    magic, total, block, count = struct.unpack_from("<4sIII", packed)
    offsets = struct.unpack_from(f"<{count + 1}I", packed, 16)
    assert magic == b"FTZ1" and block in (16384, 32768)
    raw = b"".join(zlib.decompress(packed[offsets[i]:offsets[i + 1]]) for i in range(count))
    assert len(raw) == total and raw[:font.stat().st_size] == font.read_bytes()
    resource = Path(directory) / "resource.c"
    with resource.open("w") as out:
        out.write("const unsigned char fixture_font_pack[] = {\n")
        for i in range(0, len(packed), 64):
            out.write(",".join(map(str, packed[i:i + 64])) + ",\n")
        out.write(f"}};\nconst int fixture_font_pack_size = {len(packed)};\n")
    flags = command[:command.index(str(HERE / "host.c"))]
    flags += ["/DCRF_HOST_TEST", f"/I{component / 'include'}"]
    if args.sanitize:
        flags += ["/fsanitize=address", "/Zi"]
    objects = []
    units = [(HERE / "host.c", ["/DREAL_FONT_PIPE", f"/DREAL_FONT_BYTES={font.stat().st_size}"]),
             (HERE / "real_pipe.c", []), (ROOT / "src/ui/ui_reader_font_source_fd.c", []), (resource, []),
             (component / "font_ftz.c", ["/DCORE_ACPU"]),
             (component / "font_inflate.c", ["/DCORE_ACPU", "/W3", "/WX-"]),
             (ROOT / "components/compute/epd_compute_pipe_host.c", []),
             (ROOT / "components/compute/epd_compute_pipe_remote.c", ["/DCORE_ACPU"]),
             (component / "font_remote.c", ["/DCORE_ACPU"]),
             (component / "compute_reader_font.c", ["/DCORE_ACPU", "/W3", "/WX-"])]
    for source, extra in units:
        obj = Path(directory) / (source.stem + ".obj")
        unit_flags = [flag for flag in flags if not (source.name in ("compute_reader_font.c", "font_inflate.c") and flag in ("/W4", "/WX"))]
        subprocess.run([*unit_flags, *extra, "/c", str(source), f"/Fo{obj}"], cwd=directory,
                       env=env, check=True, timeout=60)
        objects.append(str(obj))
    integration = Path(directory) / "integration.exe"
    subprocess.run([cc, "/nologo", *objects, f"/Fe{integration}",
                    *(["/fsanitize=address", "/link", "/DEBUG"] if args.sanitize else [])], cwd=directory, env=env, check=True, timeout=60)
    if not args.case or "real_pipeline" in args.case:
        if body_directory:
            metadata.update(ftz=str(pack_file), ftz_sha256=hashlib.sha256(packed).hexdigest(),
                            ttf_sha256=hashlib.sha256(font.read_bytes()).hexdigest())
            results = []
            for px in (28, 34):
                for style in ("normal", "bold", "italic"):
                    label = f"{px}-{style}"
                    log = body_directory / f"{label}.log"
                    csv = body_directory / f"{label}.csv"
                    invocation = [str(integration), "real_pipeline", str(font), str(fixture), str(px), style, str(csv), str(args.body_render_chars)]
                    with log.open("w", encoding="utf-8") as output:
                        try:
                            result = subprocess.run(invocation, env=env, stdout=output, stderr=subprocess.STDOUT,
                                                    timeout=args.body_timeout)
                            code = result.returncode
                        except subprocess.TimeoutExpired:
                            output.write(f"FAIL process timeout after {args.body_timeout} seconds\n")
                            code = 124
                    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
                    for line in lines:
                        if line.startswith(("BODY ", "STATS ", "ENGINE ", "PASS ", "FAIL ")):
                            print(line, flush=True)
                    print(f"Body {label}: exit={code}; log={log}", flush=True)
                    results.append({"px": px, "style": style, "exit_code": code, "log": str(log), "glyphs": str(csv)})
            metadata["results"] = results
            (body_directory / "results.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
            failed = sum(result["exit_code"] != 0 for result in results)
            print(f"Real body pipeline: {len(results) - failed}/{len(results)} passed; report={body_directory / 'results.json'}", flush=True)
            if failed:
                sys.exit(1)
        else:
            subprocess.run([str(integration), "real_pipeline", str(font)], env=env, check=True, timeout=30)

    if not args.case or "real_preview" in args.case:
        for px in (28, 34):
            for style in ("normal", "bold", "italic"):
                subprocess.run([str(integration), "real_preview", str(font), str(px), style],
                               env=env, check=True, timeout=30)

    if args.case and "ftz" not in args.case:
        sys.exit(0)

    ftz_exe = Path(directory) / "ftz.exe"
    subprocess.run([*(f for f in flags if f != "/TC"), str(HERE / "ftz_host.c"),
                    str(Path(directory) / "font_ftz.obj"), str(Path(directory) / "font_inflate.obj"),
                    f"/Fe{ftz_exe}"], cwd=directory, env=env, check=True, timeout=60)
    subprocess.run([str(ftz_exe), str(pack_file), str(font)], env=env, check=True, timeout=30)
