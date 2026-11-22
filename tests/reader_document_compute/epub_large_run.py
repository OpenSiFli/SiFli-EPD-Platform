"""Exercise bounded EPUB package parsing with the real remote and image codecs."""
import argparse
import concurrent.futures
import hashlib
import json
import posixpath
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import time
from urllib.parse import unquote
import xml.etree.ElementTree as ET
import zipfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TESTS = ROOT / "tests/reader_document_compute"
sys.path.insert(0, str(TESTS))
import run as converter_tests


def build_remote(build, epub_source, name, shared, source_overrides=None, host_source=None):
    compute = ROOT / "modules/books/compute"
    cfg = runpy.run_path(str(compute / "sources.py"))["configuration"]()
    sources = [Path(p) for p in cfg["sources"]]
    sources += [compute / "document_remote.c", ROOT / "components/compute/epd_compute_pipe_remote.c",
                host_source or HERE / "epub_large_host.c"]
    includes = [*cfg["includes"], str(TESTS), str(compute),
                str(ROOT / "components/compute/include"), str(ROOT / "components/compute_reader_image/include")]
    defines = ["_DEFAULT_SOURCE", "BOOK_FORMAT_ACPU", "CORE_ACPU", "LIBXML_STATIC", "TEST_REAL_IMAGE"]
    image_sources = set()
    component = ROOT / "components/compute_reader_image"
    for base in (component, component / "svg"):
        manifest = json.loads((base / "compute.json").read_text())
        group = manifest["acpu"]
        batch = [base / p for p in group["sources"]]
        sources += batch
        image_sources.update(batch)
        includes += [str(base / p) for p in group.get("includes", []) + manifest.get("public_includes", [])]
        defines += [k if v is None else f"{k}={v}" for k, v in group.get("defines", {}).items()]
    override = build / f"{name}-book_epub.c"
    source_text = epub_source.read_text(encoding="utf-8")
    override.write_text(source_text.replace('../compute/book_compute_internal.h',
                                           str(compute / 'book_compute_internal.h')), encoding="utf-8")
    sources = [override if p.name == "book_epub.c" else p for p in sources]
    sources = [(source_overrides or {}).get(p.name, p) for p in sources]
    flags = ["-g", "-O1", "-fexceptions", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-fno-pie", "-ffunction-sections", "-fdata-sections"]

    def compile_one(source):
        cpp = source.suffix == ".cpp"
        command = ["g++" if cpp else "gcc", "-std=c++14" if cpp else "-std=gnu11", *flags,
                   *("-I" + p for p in includes), *("-D" + d for d in defines)]
        if cpp:
            command += ["-fno-rtti", "-fno-threadsafe-statics", "-fno-sanitize=vptr"]
        if source not in image_sources and "third_party" not in str(source) and not source.name.startswith("mobi_"):
            command += ["-Wall", "-Wextra", "-Werror"]
        if str(source) in cfg["force_includes"]:
            command += ["-include", cfg["force_includes"][str(source)]]
        key = hashlib.sha256((str(source) + repr(command)).encode() + source.read_bytes()).hexdigest()
        if key in shared:
            return shared[key]
        obj = build / (key[:20] + ".o")
        result = subprocess.run([*command, "-c", str(source), "-o", str(obj)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(str(source) + "\n" + result.stdout + result.stderr)
        shared[key] = obj
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        objects = list(pool.map(compile_one, sources))
    exe = build / name
    subprocess.run(["g++", *map(str, objects), "-pthread", "-lm", "-fsanitize=address,undefined",
                    "-no-pie", "-Wl,--gc-sections", "-o", str(exe)], check=True)
    return exe


def fixture(path, count, variant="normal"):
    manifest = ''.join(f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>'
                       for i in range(count))
    spine = ''.join(f'<itemref idref="c{i}"/>' for i in range(count))
    if variant == "missing":
        spine += '<itemref idref="missing"/>'
    if variant == "repeat":
        spine += '<itemref idref="c0"/>'
    if variant == "nonlinear":
        spine += '<itemref idref="missing" linear="no"/>'
    if variant == "duplicate":
        manifest += '<item id="c0" href="missing.xhtml" media-type="application/xhtml+xml"/>'
    if variant == "nonhtml":
        manifest = manifest.replace('id="c0" href="c0.xhtml" media-type="application/xhtml+xml"',
                                    'id="c0" href="c0.xhtml" media-type="image/png"')
    entries = {
        "META-INF/container.xml": '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
            '<rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>',
        "OPS/book.opf": '<package xmlns="http://www.idpf.org/2007/opf" version="2.0">'
            '<metadata/><manifest>' + manifest + '<item id="ncx" href="toc.ncx" '
            'media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx">' + spine + '</spine></package>',
        "OPS/toc.ncx": '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap>' + ''.join(
            f'<navPoint id="n{i}"><navLabel><text>Chapter {i}</text></navLabel>'
            f'<content src="c{i}.xhtml#title"/></navPoint>' for i in range(count)) + '</navMap></ncx>',
        "OPS/image.png": converter_tests.png(),
    }
    for i in range(count):
        entries[f"OPS/c{i}.xhtml"] = '<html xmlns="http://www.w3.org/1999/xhtml"><body>' \
            f'<h1 id="title">Chapter {i}</h1><p>Text {i} 中文</p><img src="image.png"/></body></html>'
    if variant == "unselected_bad":
        entries[f"OPS/c{count - 1}.xhtml"] = '<html><not-well-formed>'
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, value in entries.items():
            archive.writestr(name, value)


def wire_events(path):
    data = path.read_bytes()
    at = 0
    while at < len(data):
        header = struct.unpack_from('<8I', data, at)
        kind, size, a, b, c, d, n1, n2 = header
        assert size >= 32 and at + size <= len(data)
        yield kind, a, b, c, d, data[at+32:at+32+n1], data[at+32+n1:at+32+n1+n2]
        at += size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=ROOT / 'modules/books/formats/book_epub.c')
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--epub', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    shared = {}
    candidate = build_remote(output, args.candidate.resolve(), 'candidate', shared)
    baseline = build_remote(output, args.baseline.resolve(), 'baseline', shared) if args.baseline else None
    results = []

    def invoke(exe, name, book, spine, result, chapters, toc, images):
        wire = output / f'{name}.wire'
        started = time.monotonic()
        process = subprocess.run([str(exe), '--one', str(book), str(spine), str(result), str(chapters),
                                  str(toc), str(images), str(wire)], capture_output=True, text=True, timeout=90)
        elapsed = time.monotonic() - started
        log = output / f'{name}.log'
        log.write_text(process.stdout + process.stderr, encoding='utf-8')
        print(name, process.stdout.strip(), flush=True)
        results.append(dict(case=name, passed=process.returncode == 0, seconds=round(elapsed, 3), log=str(log)))
        assert process.returncode == 0, process.stdout + process.stderr
        return wire

    cases = [(n, 'normal') for n in (1, 62, 63, 64, 65, 129)]
    cases += [(65, kind) for kind in ('missing', 'repeat', 'nonlinear', 'duplicate', 'nonhtml', 'unselected_bad')]
    for count, kind in cases:
        book = output / f'{count}-{kind}.epub'
        fixture(book, count, kind)
        expected = -1 if kind == 'missing' else -4 if kind == 'nonhtml' else 0
        total = count + (kind == 'repeat')
        chosen = 0 if kind != 'normal' else count - 1
        name = f'{count}-{kind}'
        wire = invoke(candidate, name, book, chosen, expected, total, count, 1 if not expected else 0)
        if not expected:
            rendered = list(wire_events(wire))
            text = b''.join(e[5] for e in rendered if e[0] == 1)
            assert text == f'Chapter {chosen}Text {chosen} 中文'.encode(), (name, text)
        if baseline:
            old = invoke(baseline, 'baseline-' + name, book, chosen, expected, total, count, 1 if not expected else 0)
            assert wire.read_bytes() == old.read_bytes(), name
    faults = subprocess.run([str(candidate), '--faults', str(output / '65-normal.epub'), '64'],
                            capture_output=True, text=True, timeout=180)
    (output / 'faults.log').write_text(faults.stdout + faults.stderr, encoding='utf-8')
    print(faults.stdout, end='', flush=True)
    assert faults.returncode == 0, faults.stdout + faults.stderr
    results.append(dict(case='faults', passed=True))
    corpus = output / 'corpus'
    corpus.mkdir(exist_ok=True)
    if not (corpus / 'fixtures').exists():
        converter_tests.fixtures(corpus, [], svg=True)
    for name, book in (('epub3-svg', corpus / 'fixtures/deflated.epub'),
                       ('split-svg', corpus / 'fixtures/split')):
        wire = invoke(candidate, name, book, 0, 0, 2, 2, 1)
        if baseline:
            old = invoke(baseline, 'baseline-' + name, book, 0, 0, 2, 2, 1)
            assert wire.read_bytes() == old.read_bytes(), name
    if args.epub:
        book = args.epub.resolve()
        with zipfile.ZipFile(book) as archive:
            container = ET.fromstring(archive.read('META-INF/container.xml'))
            opf = next(e.attrib['full-path'] for e in container.iter() if e.tag.endswith('}rootfile'))
            package = ET.fromstring(archive.read(opf))
            items = {e.attrib['id']: e.attrib for e in package.iter() if e.tag.endswith('}item')}
            spine = [items[e.attrib['idref']]['href'] for e in package.iter()
                     if e.tag.endswith('}itemref') and e.attrib.get('linear') != 'no']
            ncx = next(e['href'] for e in items.values() if e.get('media-type') == 'application/x-dtbncx+xml')
            navigation = ET.fromstring(archive.read(str(Path(opf).parent / ncx)))
            points = [e for e in navigation.iter() if e.tag.endswith('}navPoint')]
            expected_chapters = [posixpath.normpath(posixpath.join(posixpath.dirname(opf), unquote(p))) for p in spine]
            by_href = {p: i for i, p in enumerate(expected_chapters)}
            expected_toc = []
            for point in points:
                content = next(e for e in point if e.tag.endswith('}content'))
                href = content.attrib['src']
                target = posixpath.normpath(posixpath.join(posixpath.dirname(opf), unquote(href)))
                expected_toc.append((target, by_href.get(target.split('#')[0], 0xffffffff)))
        if baseline:
            invoke(baseline, 'original-book-before', book, 0, -3, 0, 0, 0)
        for selected in (0, 1, len(spine) - 1):
            wire = invoke(candidate, f'original-book-{selected}', book, selected, 0, len(spine), len(points), 1 if selected == 0 else 0)
            records = list(wire_events(wire))
            actual_chapters = [e for e in records if e[0] == 9]
            assert [(e[1], e[2], e[3], e[5].decode()) for e in actual_chapters] == [
                (i, len(spine), int(i == selected), path) for i, path in enumerate(expected_chapters)]
            actual_toc = [(e[5].decode(), e[2]) for e in records if e[0] == 5 and e[1] == 1]
            assert actual_toc == expected_toc
    (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('PASS', len(results), 'cases; ASan/UBSan; unchanged 1 MiB budget', flush=True)


if __name__ == '__main__':
    main()
