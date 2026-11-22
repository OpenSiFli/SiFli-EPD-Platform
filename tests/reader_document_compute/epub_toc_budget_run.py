"""Replay large EPUB conversion through the production HCPU document worker.

Windows/MSVC runs x86 and x64 by default. Supply --wire-dir to reuse real
converter captures, or let the runner generate them with WSL/Ubuntu.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import posixpath
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from urllib.parse import unquote
import zipfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def local_name(element):
    return element.tag.rsplit('}', 1)[-1]


def oracle(book):
    with zipfile.ZipFile(book) as archive:
        container = ET.fromstring(archive.read('META-INF/container.xml'))
        opf = next(e.attrib['full-path'] for e in container.iter() if local_name(e) == 'rootfile')
        package = ET.fromstring(archive.read(opf))
        items = {e.attrib['id']: e.attrib for e in package.iter() if local_name(e) == 'item'}
        spine = [items[e.attrib['idref']]['href'] for e in package.iter()
                 if local_name(e) == 'itemref' and e.attrib.get('linear') != 'no']
        chapters = [posixpath.normpath(posixpath.join(posixpath.dirname(opf), unquote(p))) for p in spine]
        by_href = {p: i for i, p in enumerate(chapters)}
        ncx = next(e['href'] for e in items.values() if e.get('media-type') == 'application/x-dtbncx+xml')
        ncx = posixpath.normpath(posixpath.join(posixpath.dirname(opf), unquote(ncx)))
        navigation = ET.fromstring(archive.read(ncx))
        entries = []

        def walk(parent, depth):
            for point in parent:
                if local_name(point) != 'navPoint':
                    continue
                href = next(e.attrib['src'] for e in point if local_name(e) == 'content')
                label = next(e for e in point if local_name(e) == 'navLabel')
                title = ' '.join(''.join(label.itertext()).split()) or href
                target = posixpath.normpath(posixpath.join(posixpath.dirname(ncx), unquote(href)))
                entries.append((by_href.get(target.split('#')[0], 0xffffffff), depth, target, title))
                walk(point, depth + 1)

        walk(next(e for e in navigation.iter() if local_name(e) == 'navMap'), 1)
    assert chapters and entries
    return chapters, entries


def wire_events(data):
    at = 0
    while at < len(data):
        assert len(data) - at >= 32
        h = struct.unpack_from('<8I', data, at)
        assert h[1] >= 32 and h[1] % 4 == 0 and h[1] <= len(data) - at
        assert h[6] + h[7] <= h[1] - 32
        raw = data[at:at + h[1]]
        yield h, raw
        at += h[1]


def payload(h, raw):
    return raw[32:32+h[6]].decode('utf-8'), raw[32+h[6]:32+h[6]+h[7]].decode('utf-8')


def duplicate_titles(data):
    """Recreate the former META_TOC + named ANCHOR output contract."""
    result = bytearray()
    for h, raw in wire_events(data):
        result += raw
        if h[0] == 5 and h[2] == 1:
            # ANCHOR.a is level; ANCHOR.b marks a supplied title.
            result += struct.pack('<8I', 3, h[1], h[4], 1, 0, 0, h[6], h[7]) + raw[32:]
    return bytes(result)


def strip_duplicate_titles(data):
    result = bytearray()
    preceding = None
    removed = 0
    for h, raw in wire_events(data):
        if preceding is not None:
            ph, pr = preceding
            if (h[0] == 3 and h[3] and h[2] == ph[4] and h[6:] == ph[6:]
                    and raw[32:32+h[6]+h[7]] == pr[32:32+ph[6]+ph[7]]):
                removed += 1
                preceding = None
                continue
        result += raw
        preceding = (h, raw) if h[0] == 5 and h[2] == 1 else None
    return bytes(result), removed


def compiler_environment(arch):
    env = dict(os.environ)
    vswhere = Path(env.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / (
        'Microsoft Visual Studio/Installer/vswhere.exe')
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    setup = Path(install) / 'VC/Auxiliary/Build' / ('vcvars32.bat' if arch == 'x86' else 'vcvars64.bat')
    output = subprocess.check_output(f'cmd.exe /d /s /c ""{setup}" >nul && set"', text=True, errors='replace')
    for line in output.splitlines():
        key, sep, value = line.partition('=')
        if sep and key:
            env[key.upper()] = value
    compiler = shutil.which('cl', path=env['PATH'])
    assert compiler, arch
    return compiler, env


def build_host(build, arch, sanitize):
    build.mkdir(parents=True, exist_ok=True)
    for name in ('app_service.h', 'rtthread.h', 'rthw.h', 'storage.h', 'storage_file.h',
                 'epd_memory.h', 'reader_compute_client.h', 'unistd.h', 'dirent.h',
                 'platform/epd_app.h', 'platform/epd_app_library.h'):
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "hcpu_host.h"\n', encoding='utf-8')
    mocks = (HERE / 'hcpu_test.c').read_text(encoding='utf-8').split('static void pump(void)', 1)[0]
    (build / 'toc_host.inc').write_text(mocks, encoding='utf-8')
    (build / 'document_under_test.c').write_bytes((ROOT / 'modules/books/document/document.c').read_bytes())
    includes = [build, HERE, ROOT / 'modules/books/document', ROOT / 'components/compute/include',
                ROOT / 'src/platform', ROOT / 'components/compute_reader_render/include',
                ROOT / 'components/compute_reading_data/include']
    kernels = [ROOT / 'components/compute_reader_render' / s for s in ('render_kernel.c', 'render_format.c')]
    compiler, env = compiler_environment(arch)
    exe = build / 'replay.exe'
    command = [compiler, '/nologo', '/std:c11', '/utf-8', '/Od', '/Zi', '/Gy', '/W3',
               '/D_CRT_SECURE_NO_WARNINGS', '/DHCPU_DOCUMENT_SOURCE', *(f'/I{p}' for p in includes)]
    if sanitize:
        command.append('/fsanitize=address')
    command += [str(HERE / 'epub_toc_budget_test.c'), *map(str, kernels), f'/Fe{exe}', '/link', '/OPT:REF']
    run = subprocess.run(command, cwd=build, env=env, capture_output=True, timeout=90)
    (build / 'compile.log').write_bytes(run.stdout + run.stderr)
    if run.returncode:
        raise RuntimeError((run.stdout + run.stderr).decode(errors='replace'))
    return exe, env


def wsl_path(path):
    path = path.resolve()
    if len(path.drive) != 2 or path.drive[1] != ':':
        raise ValueError('WSL input/output must use a local drive path')
    return '/mnt/' + path.drive[0].lower() + path.as_posix()[2:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--epub', type=Path, required=True, help='Large NCX EPUB with enough TOC entries to expose duplicate-budget failure')
    parser.add_argument('--wire-dir', type=Path, help='Real converter original-book-{spine}.wire captures')
    parser.add_argument('--baseline-wire-dir', type=Path, help='Optional actual pre-fix captures for exact event comparison')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--arch', choices=('x86', 'x64', 'both'), default='both')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--distro', default='Ubuntu')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    book = args.epub.resolve()
    chapters, entries = oracle(book)
    selected = sorted({0, 1, len(chapters) - 1})
    assert len(chapters) > 1
    sources = [ROOT / p for p in ('modules/books/formats/book_epub.c', 'modules/books/compute/document_remote.c',
                                  'modules/books/document/document.c', 'modules/books/document/document.h')]
    source_hashes = {str(p.relative_to(ROOT)): digest(p) for p in sources}
    wire_dir = args.wire_dir.resolve() if args.wire_dir else output / 'remote'
    if not args.wire_dir:
        subprocess.run(['wsl', '-d', args.distro, '--', 'python3', '-B',
                        wsl_path(HERE / 'epub_large_run.py'), '--epub', wsl_path(book),
                        '--output', wsl_path(wire_dir)], check=True)
    # The HCPU deduplicates identical explicit declarations while keeping their order.
    unique = list(dict.fromkeys(entries))
    oracle_file = output / 'toc-oracle.bin'
    oracle_data = bytearray(struct.pack('<3I', len(chapters), len(unique), book.stat().st_size))
    for chapter, level, target, title in unique:
        target, title = target.encode('utf-8'), title.encode('utf-8')
        oracle_data += struct.pack('<4I', chapter, level, len(target), len(title)) + target + title
    oracle_file.write_bytes(oracle_data)
    equivalence = []
    for spine in selected:
        wire = wire_dir / f'original-book-{spine}.wire'
        data = wire.read_bytes()
        rendered = list(wire_events(data))
        actual_chapters = [(h[2], h[3], h[4], payload(h, raw)[0]) for h, raw in rendered if h[0] == 9]
        assert actual_chapters == [(i, len(chapters), int(i == spine), path) for i, path in enumerate(chapters)]
        actual_toc = [(h[3], h[4], *payload(h, raw)) for h, raw in rendered if h[0] == 5 and h[2] == 1]
        assert actual_toc == entries
        stripped, duplicates = strip_duplicate_titles(data)
        assert not duplicates and stripped == data, 'Converter still emits duplicate TOC anchors'
        if args.baseline_wire_dir:
            old = (args.baseline_wire_dir / wire.name).read_bytes()
            stripped, duplicates = strip_duplicate_titles(old)
            assert duplicates == len(entries) and stripped == data
            equivalence.append(dict(spine=spine, removed=duplicates, old_bytes=len(old), new_bytes=len(data)))
    old_wire = output / 'duplicate-titles.wire'
    if args.baseline_wire_dir:
        old_wire.write_bytes((args.baseline_wire_dir / 'original-book-0.wire').read_bytes())
        control = 'captured pre-fix output'
    else:
        old_wire.write_bytes(duplicate_titles((wire_dir / 'original-book-0.wire').read_bytes()))
        control = 'reconstructed duplicate output'
    results = []
    for arch in ('x86', 'x64') if args.arch == 'both' else (args.arch,):
        build = output / arch
        exe, env = build_host(build, arch, args.sanitize)
        cases = [(old_wire, -3)] + [(wire_dir / f'original-book-{spine}.wire', 0) for spine in selected]
        for wire, expected in cases:
            case = build / wire.stem
            case.mkdir(exist_ok=True)
            run = subprocess.run([str(exe), str(case), str(wire), str(oracle_file), str(expected)],
                                 cwd=build, env=env, capture_output=True, timeout=90)
            log = case / 'result.log'
            log.write_bytes(run.stdout + run.stderr)
            print(arch, wire.stem, (run.stdout + run.stderr).decode(errors='replace'), flush=True)
            results.append(dict(arch=arch, wire=str(wire), wire_sha256=digest(wire), expected=expected,
                                passed=run.returncode == 0, log=str(log)))
    assert source_hashes == {str(p.relative_to(ROOT)): digest(p) for p in sources}, 'Production sources changed during test'
    report = dict(book=str(book), book_bytes=book.stat().st_size, book_sha256=digest(book),
                  source_hashes=source_hashes, reused_wire=bool(args.wire_dir), negative_control=control,
                  equivalence=equivalence, cases=results)
    (output / 'results.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    assert all(r['passed'] for r in results)
    print(f'PASS {len(results)} processes; each successful conversion also reopens its persisted spool')


if __name__ == '__main__':
    main()
