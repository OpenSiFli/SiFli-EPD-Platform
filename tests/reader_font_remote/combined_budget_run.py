"""Run real font/document/image/SVG resource and fault regressions on Linux/WSL.

All generated files and frozen evidence stay under combined_budget-artifacts.
An existing --build directory reuses objects only while their dependency hashes match.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DOC = ROOT / "modules/books/compute"
IMAGE = ROOT / "components/compute_reader_image"
FONT = ROOT / "components/compute_reader_font"
ARTIFACTS = HERE / "combined_budget-artifacts"


@lru_cache(maxsize=None)
def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--synthetic-ftz-pressure", action="store_true",
                        help="Compatibility option; dual-FTZ OOM and recovery are now always tested.")
    parser.add_argument("--pack", type=Path, default=ROOT / "project/build_sf32lb57-epd-gyd7n6_hcpu/font/MiSans-Normal.ftz")
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    build = (args.build or ARTIFACTS / stamp).resolve()
    if ARTIFACTS.resolve() not in build.parents:
        parser.error("--build must be inside combined_budget-artifacts")
    build.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "src/ui/ui_reader_font_remote.c").read_text(encoding="utf-8")
    match = re.search(r"^#define SOURCE_MEMORY_BYTES ([^\r\n]+)", source, re.M)
    if not match:
        raise RuntimeError("HCPU source budget macro was not found")
    budget_header = build / "source_budget.h"
    budget_header.write_text("#define COMBINED_SOURCE_MEMORY_BYTES " + match[1] + "\n")
    fixtures = build / "fixtures"
    if not fixtures.exists():
        runpy.run_path(str(ROOT / "tests/reader_document_compute/run.py"))["fixtures"](build, [], svg=True)
    svg = '<svg width="684" height="1216" viewBox="0 0 64 114">' + ''.join(
        '<g transform="translate(0 ' + str(i * 8) + ') scale(0.8)"><path d="M4 32 C12 2 52 2 60 32 Q32 62 4 32 Z" fill="gray" stroke="black" stroke-width="3" stroke-dasharray="4 2"/></g>'
        for i in range(10)) + '</svg>'
    (fixtures / "complex.svg").write_text(svg)
    (fixtures / "budget.xhtml").write_text('<html><body><p>字体与图像共同预算</p><img src="image.png"/><img src="complex.svg"/></body></html>')
    (fixtures / "unsupported.xhtml").write_text('<html xmlns="http://www.w3.org/1999/xhtml"><body><svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><text>Unsupported</text></svg></body></html>')
    (fixtures / "missing-resource.xhtml").write_text('<html xmlns="http://www.w3.org/1999/xhtml"><body><svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><image href="missing.png" width="32" height="32"/></svg></body></html>')
    config = runpy.run_path(str(DOC / "sources.py"))["configuration"]()
    sources = [Path(p) for p in config["sources"]]
    sources += [DOC / "document_remote.c", ROOT / "components/compute/epd_compute_pipe_remote.c",
                FONT / "font_remote.c", FONT / "compute_reader_font.c", FONT / "font_ftz.c",
                FONT / "font_inflate.c", HERE / "combined_budget.c"]
    includes = [*map(Path, config["includes"]), ROOT / "components/compute/include", FONT / "include"]
    defines = ["CRF_HOST_TEST", "_DEFAULT_SOURCE", "BOOK_FORMAT_ACPU", "CORE_ACPU", "LIBXML_STATIC", "TEST_REAL_IMAGE"]
    image_sources = set()
    for base in (IMAGE, IMAGE / "svg"):
        manifest = json.loads((base / "compute.json").read_text())
        group = manifest["acpu"]
        batch = [base / s for s in group["sources"]]
        sources += batch
        image_sources.update(batch)
        includes += [base / p for p in group.get("includes", [])]
        includes += [base / p for p in manifest.get("public_includes", [])]
        defines += [k if v is None else f"{k}={v}" for k, v in group.get("defines", {}).items()]
    initial_sources = {str(p): digest(p) for p in sources}
    flags = ["-g", "-O1", "-fexceptions", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-fno-pie", "-ffunction-sections", "-fdata-sections"]

    def compile_one(item):
        index, source = item
        cpp = source.suffix == ".cpp"
        obj, dep, cached = (build / f"{index}{suffix}" for suffix in (".o", ".d", ".json"))
        command = ["g++" if cpp else "gcc", "-std=c++14" if cpp else "-std=gnu11", *flags,
                   *("-I" + str(p) for p in includes), *("-D" + d for d in defines)]
        if source in image_sources:
            command += ["-O2"]
        if cpp:
            command += ["-fno-rtti", "-fno-threadsafe-statics", "-fno-sanitize=vptr"]
        if str(source) in config["force_includes"]:
            command += ["-include", config["force_includes"][str(source)]]
        if source == HERE / "combined_budget.c":
            command += ["-include", str(budget_header), "-Wall", "-Wextra", "-Werror"]
        command += ["-MMD", "-MF", str(dep), "-c", str(source), "-o", str(obj)]
        if obj.exists() and cached.exists():
            previous = json.loads(cached.read_text())
            if previous["command"] == command and all(Path(p).exists() and digest(p) == h for p, h in previous["inputs"].items()):
                return obj, previous
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(str(source) + "\n" + result.stdout + result.stderr)
        dependencies = shlex.split(dep.read_text().replace("\\\n", " ").split(":", 1)[1])
        record = {"command": command, "inputs": {p: digest(p) for p in dependencies}}
        cached.write_text(json.dumps(record, indent=2))
        return obj, record

    print(f"Build directory: {build}\nCompiling {len(sources)} real units with ASan/UBSan", flush=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        compiled = list(pool.map(compile_one, enumerate(sources)))
    digest.cache_clear()
    if any(digest(p) != h for p, h in initial_sources.items()):
        raise RuntimeError("A source changed during compilation; rerun against a stable snapshot")
    exe = build / "combined"
    subprocess.run(["g++", *(str(p) for p, _ in compiled), "-pthread", "-lm", "-fsanitize=address,undefined",
                    "-no-pie", "-Wl,--gc-sections", "-Wl,--wrap=ri_svg_render_active", "-o", str(exe)], check=True)
    command = [str(exe), str(ROOT / "font/MiSans-Normal.ttf"), str(args.pack.resolve()),
               "fixtures/deflated.epub", "fixtures/budget.xhtml"]
    log = build / f"regression-{stamp}.log"
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    env.pop("READER_FONT_SYNTHETIC_PRESSURE", None)
    if args.synthetic_ftz_pressure:
        env["READER_FONT_SYNTHETIC_PRESSURE"] = "1"
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run(command, cwd=build, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=240)
    evidence = {}
    for _, record in compiled:
        evidence.update(record["inputs"])
    evidence.update({str(p): digest(p) for p in [Path(__file__), ROOT / "font/MiSans-Normal.ttf", args.pack.resolve(),
                                               *sorted(p for p in fixtures.rglob("*") if p.is_file())]})
    digest.cache_clear()
    if any(digest(p) != h for p, h in evidence.items()):
        raise RuntimeError("A compiled dependency changed during execution; results cannot be frozen")
    report = {"utc": stamp, "command": command, "exit_code": result.returncode,
              "compiler": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
              "hcpu_source_budget": match[1], "private_heap_bytes": 2 * 1024 * 1024,
              "synthetic_ftz_pressure": True,
              "model": "M33 heap headers/alignment plus reserved target fiber stacks; native host structure sizes",
              "sanitizers": ["address", "undefined", "leak"], "inputs_sha256": evidence,
              "executable_sha256": digest(exe), "log": str(log), "log_sha256": digest(log)}
    report_path = build / f"result-{stamp}.json"
    report_path.write_text(json.dumps(report, indent=2))
    if not result.returncode:
        frozen = build / f"snapshot-{stamp}"
        frozen.mkdir()
        for path in (HERE / "combined_budget.c", Path(__file__)):
            (frozen / path.name).write_bytes(path.read_bytes())
    print(log.read_text(), end="")
    print(f"Evidence: {report_path}", flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
