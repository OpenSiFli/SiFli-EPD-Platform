"""Run real HCPU/ACPU missing-glyph paths with distinct, valid font sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib

from fontTools import subset
from fontTools.ttLib import TTFont

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get("READER_FONT_REPO", HERE.parents[1])).resolve()
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--font", type=Path, default=ROOT / "font/MiSans-Normal.ttf")
    parser.add_argument("--source", type=Path, help="Alternate production remote source for negative tests")
    parser.add_argument("--expect-negative", action="store_true")
    parser.add_argument("--expect-border-negative", action="store_true")
    parser.add_argument("--metrics-profile", choices=("normal", "narrow", "extreme"), default="normal")
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    primary = TTFont(args.font)
    sub = subset.Subsetter(); sub.populate(unicodes=[32, 63, 65, 66, 67])
    sub.subset(primary)
    if args.metrics_profile != "normal":
        primary["head"].unitsPerEm = 512 if args.metrics_profile == "narrow" else 4096
        primary["hhea"].ascent = 48
        primary["hhea"].descent = primary["hhea"].lineGap = 0
    primary.save(out / "primary.ttf")
    assert 65 in primary.getBestCmap() and 0x5101 not in primary.getBestCmap()
    for table in primary["cmap"].tables:
        if table.isUnicode(): table.cmap.pop(65, None)
    primary.save(out / "style.ttf"); primary.close()
    raw = (out / "primary.ttf").read_bytes()
    blocks = [zlib.compress(raw[i:i+32768]) for i in range(0, len(raw), 32768)]
    offsets = [20 + 4 * len(blocks)]
    for block in blocks: offsets.append(offsets[-1] + len(block))
    packed = struct.pack("<4sIII", b"FTZ1", len(raw), 32768, len(blocks))
    packed += struct.pack("<" + "I" * len(offsets), *offsets) + b"".join(blocks)
    (out / "primary.ftz").write_bytes(packed)
    resource = out / "resource.c"
    resource.write_text("const unsigned char fixture_font_pack[]={\n" +
                        "\n".join(",".join(map(str, packed[i:i+64])) + "," for i in range(0, len(packed), 64)) +
                        f"\n}};\nconst int fixture_font_pack_size={len(packed)};\n")
    cc, env = compiler_environment(os.environ.get("CC"))
    component = ROOT / "components/compute_reader_font"
    includes = [HERE, ROOT / "tests/reader_font_remote", ROOT / "SiFli-SDK/external/lvgl_v9", ROOT / "src/ui", ROOT / "src/platform",
                ROOT / "src/services", ROOT / "src/thirdparty/miniz", ROOT / "components/compute/include",
                component / "include"]
    flags = ["/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC", "/Od", "/fsanitize=address", "/Zi",
             "/D_CRT_SECURE_NO_WARNINGS", "/DLV_CONF_INCLUDE_SIMPLE", "/DCRF_HOST_TEST",
             "/wd4201", "/wd4244", "/wd4214", "/wd4706", "/wd4267", *[f"/I{x}" for x in includes]]
    selected = args.source or ROOT / "src/ui/ui_reader_font_remote.c"
    selection = []
    if args.source:
        text = selected.read_text(encoding="utf-8")
        text = re.sub(r'#include "(\.\.?/[^"\n]+)"',
                      lambda m: '#include "' + (ROOT / "src/ui" / m[1]).resolve().as_posix() + '"', text)
        copy = out / "selected.c"; copy.write_text(text, encoding="utf-8")
        selection = [f'/DREADER_FONT_TEST_SOURCE="{copy.as_posix()}"']
    units = [(HERE / "missing_host.c", ["/DREAL_FONT_PIPE", f"/DREAL_FONT_BYTES={len(raw)}", *selection]),
             (ROOT / "tests/reader_font_remote/real_pipe.c", []), (ROOT / "src/ui/ui_reader_font_source_fd.c", []), (resource, []),
             (ROOT / "SiFli-SDK/external/lvgl_v9/src/font/lv_font.c", []),
             (component / "font_ftz.c", ["/DCORE_ACPU"]),
             (component / "font_inflate.c", ["/DCORE_ACPU", "/W3", "/WX-"]),
             (ROOT / "components/compute/epd_compute_pipe_host.c", []),
             (ROOT / "components/compute/epd_compute_pipe_remote.c", ["/DCORE_ACPU"]),
             (component / "font_remote.c", ["/DCORE_ACPU"]),
             (component / "compute_reader_font.c", ["/DCORE_ACPU", "/W3", "/WX-"])]
    commands = []; objects = []
    for source, extra in units:
        obj = out / (source.stem + ".obj")
        command = [cc, *flags, *extra, "/c", str(source), f"/Fo{obj}"]
        result = subprocess.run(command, cwd=out, env=env, capture_output=True, timeout=90)
        (out / (source.stem + "-compile.log")).write_bytes(result.stdout + result.stderr)
        commands.append(command)
        if result.returncode: print((result.stdout + result.stderr).decode(errors="replace"))
        result.check_returncode(); objects.append(str(obj))
    exe = out / "missing.exe"
    result = subprocess.run([cc, "/nologo", *objects, f"/Fe{exe}", "/fsanitize=address", "/link", "/DEBUG"],
                            cwd=out, env=env, capture_output=True, timeout=60)
    (out / "link.log").write_bytes(result.stdout + result.stderr); result.check_returncode()
    cases = args.case or ["two_levels", "primary_square", "reclaim", "pins_close_reopen", "square_oom", "cancel",
                         "engine_oom", "io", "short", "corrupt", "transport", "malformed_missing", "lvgl_wrapper"]
    results = []
    for case in cases:
        result = subprocess.run([str(exe), case, str(out / "style.ttf")], cwd=out, env=env,
                                capture_output=True, timeout=30)
        log = result.stdout + result.stderr; (out / (case + ".log")).write_bytes(log)
        print(log.decode(errors="replace"), end="", flush=True)
        results.append({"case": case, "exit": result.returncode})
        if args.expect_border_negative:
            assert result.returncode == 3 and b"missing square interior" in log, log
        elif args.expect_negative:
            assert result.returncode == 3 and b"ui_reader_font_remote_ready(f, cp, 0, pixels) == 1" in log, log
        else: result.check_returncode()
    metadata = {"source": str(selected), "sha256": hashlib.sha256(selected.read_bytes()).hexdigest(),
                "font_source": str(args.font), "font_sha256": hashlib.sha256(args.font.read_bytes()).hexdigest(),
                "fixture": "Primary cmap A/B/C/space/question; style additionally lacks A; both lack U+5101",
                "negative": args.expect_negative or args.expect_border_negative,
                "metrics_profile": args.metrics_profile, "results": results, "commands": commands}
    (out / "results.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"PASS {len(cases)} real missing-glyph {'negative' if args.expect_negative or args.expect_border_negative else 'ASAN'} cases; {out}")


if __name__ == "__main__":
    main()
