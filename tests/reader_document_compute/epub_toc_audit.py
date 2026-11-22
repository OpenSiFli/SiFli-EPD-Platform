"""Compare the actual Si Shi Tong Tang EPUB with production converter wire events.

Expected TOC entries come from ZIP/OPF/NCX and HTML, never from META_TOC.
--capture builds the production converter on Linux/WSL with the remote host harness.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import posixpath
import re
import runpy
import struct
import subprocess
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BOOK_SHA256 = '04007833ecd08efd71df6e87d41ff8c9d2a6a54faddb23216a97c11468b2d18c'
BLOCKS = {'body', 'div', 'p', 'h1', 'h2', 'h3'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def resolve(base, href):
    return posixpath.normpath(posixpath.join(posixpath.dirname(base), href))


def original_book(path):
    assert sha(path) == BOOK_SHA256, 'This HTML normalization oracle is for the audited original EPUB'
    with zipfile.ZipFile(path) as archive:
        container = ET.fromstring(archive.read('META-INF/container.xml'))
        opf_path = container.find('.//{*}rootfile').get('full-path')
        opf = ET.fromstring(archive.read(opf_path))
        manifest = {e.get('id'): resolve(opf_path, e.get('href')) for e in opf.find('{*}manifest')}
        spine = opf.find('{*}spine')
        chapters = [manifest[e.get('idref')] for e in spine if e.get('linear') != 'no']
        ncx_path = manifest[spine.get('toc')]
        ncx = ET.fromstring(archive.read(ncx_path))
        bodies, anchors, headings = {}, {}, {}
        for chapter in chapters:
            body = ET.fromstring(archive.read(chapter)).find('{*}body')
            chunks = []
            length = 0

            def walk(element):
                nonlocal length
                tag = element.tag.rsplit('}', 1)[-1]
                # The original book uses nested block containers and inline a/span.
                # A leaf block contributes its visible text once; inter-block XML
                # indentation and paragraph layout do not contribute body bytes.
                if tag in BLOCKS and not any(c.tag.rsplit('}', 1)[-1] in BLOCKS for c in element):
                    text = re.sub(r'[\t\n\r\f ]+', ' ', ''.join(element.itertext())).strip(' \t\r\n\f')
                    if element.get('id'):
                        target = chapter + '#' + element.get('id')
                        anchors[target] = length
                        headings[target] = text
                    encoded = text.encode('utf-8')
                    chunks.append(encoded)
                    length += len(encoded)
                else:
                    # Nested HTML TOC containers also have a direct link before
                    # their child blocks; retain that parent's label in order.
                    prefix = re.sub(r'[\t\n\r\f ]+', ' ', element.text or '').strip(' \t\r\n\f').encode('utf-8')
                    chunks.append(prefix)
                    length += len(prefix)
                    for child in element:
                        if child.tag.rsplit('}', 1)[-1] in BLOCKS:
                            walk(child)
                        else:
                            inline = re.sub(r'[\t\n\r\f ]+', ' ', ''.join(child.itertext())).strip(' \t\r\n\f').encode('utf-8')
                            chunks.append(inline)
                            length += len(inline)
                        tail = re.sub(r'[\t\n\r\f ]+', ' ', child.tail or '').strip(' \t\r\n\f').encode('utf-8')
                        chunks.append(tail)
                        length += len(tail)

            walk(body)
            bodies[chapter] = b''.join(chunks)
            anchors[chapter] = 0
            first_heading = next((e for e in body.iter() if e.tag.rsplit('}', 1)[-1] in {'h1', 'h2', 'h3'}), None)
            if first_heading is not None:
                headings[chapter] = ''.join(first_heading.itertext())
        entries = []

        def visit(parent, level):
            for point in parent.findall('{*}navPoint'):
                title = ''.join(point.find('{*}navLabel/{*}text').itertext())
                target = resolve(ncx_path, point.find('{*}content').get('src'))
                chapter_path = target.split('#', 1)[0]
                assert headings[target] == title, (target, title, headings.get(target))
                entries.append(dict(index=len(entries), title=title, level=level, target=target,
                                    chapter=chapters.index(chapter_path), offset=anchors[target]))
                visit(point, level + 1)

        visit(ncx.find('{*}navMap'), 1)
        # At split-file boundaries NCX uses the root and HTML TOC uses the
        # first heading's fragment. Compare their independently resolved positions.
        html_toc = ET.fromstring(archive.read('OEBPS/Text/TOC.xhtml'))
        links = [(resolve('OEBPS/Text/TOC.xhtml', e.get('href')), ''.join(e.itertext()))
                 for e in html_toc.iter('{http://www.w3.org/1999/xhtml}a')]
        assert len(links) == len(entries), 'HTML TOC count'
        for (target, title), entry in zip(links, entries):
            assert (target.split('#', 1)[0], anchors[target], title) == (
                entry['target'].split('#', 1)[0], entry['offset'], entry['title']), 'HTML TOC position differs from NCX'
            entry['html_toc_target'] = target
        assert len(chapters) == 14 and len(entries) == 108
        assert [(entries[i]['chapter'], entries[i]['offset']) for i in (0, 4, 5)] == [(2, 94), (2, 1697), (2, 12359)]
        return dict(epub=str(Path(path).resolve()), sha256=BOOK_SHA256, opf=opf_path, ncx=ncx_path,
                    chapters=chapters, entries=entries), bodies


def events(data):
    at = 0
    while at < len(data):
        assert at + 32 <= len(data), 'Truncated wire header'
        kind, size, a, b, c, d, n, m = struct.unpack_from('<8I', data, at)
        assert size >= 32 + n + m and size % 4 == 0 and at + size <= len(data), 'Invalid wire size'
        yield dict(at=at, kind=kind, a=a, b=b, c=c, d=d,
                   first=data[at+32:at+32+n], second=data[at+32+n:at+32+n+m])
        at += size


def check_wire(data, model, bodies):
    stream = list(events(data))
    chapters = [e for e in stream if e['kind'] == 9]
    assert len(chapters) == len(model['chapters']), 'CHAPTER count'
    for i, (actual, path) in enumerate(zip(chapters, model['chapters'])):
        assert (actual['a'], actual['b'], actual['first'].decode()) == (i, len(chapters), path), 'CHAPTER mapping'
    toc = [e for e in stream if e['kind'] == 5 and e['a'] == 1]
    assert len(toc) == len(model['entries']), 'META_TOC count'
    for actual, expected in zip(toc, model['entries']):
        assert (actual['b'], actual['c'], actual['d'], actual['first'].decode(), actual['second'].decode()) == (
            expected['chapter'], expected['level'], len(chapters), expected['target'], expected['title']), (
            'META_TOC mismatch', expected['index'], expected['title'])
    beginnings = [e for e in stream if e['kind'] == 10 and e['d'] & 0x100]
    ends = [e for e in stream if e['kind'] == 10 and e['d'] & 0x400]
    targets = [e for e in stream if e['kind'] == 10 and e['d'] & 0x200]
    assert len(beginnings) == len(ends) == len(targets) == 1
    chapter = beginnings[0]['a']
    start = beginnings[0]['b']
    assert start <= targets[0]['b'] == targets[0]['c'] <= ends[0]['b'], 'Exact requested source target'
    path = model['chapters'][chapter]
    visible = b''.join(e['first'] for e in stream if e['kind'] == 1)
    assert visible == bodies[path][start:], ('HTML body differs from conversion', chapter, len(visible), len(bodies[path]) - start)
    assert ends[0]['b'] == start + len(visible), 'SOURCE_END body byte offset'
    anchors = {}
    offset = start
    for e in stream:
        if e['kind'] == 10:
            assert e['a'] == chapter and e['d'] & 255 == 2 and e['first'].decode() == path, 'SOURCE chapter'
            assert e['b'] == offset, 'SOURCE offset must count preceding visible UTF-8 bytes'
        if e['kind'] == 1:
            offset += len(e['first'])
        if e['kind'] == 3 and not e['second']:
            anchors[e['first'].decode()] = offset
    resolved = []
    for expected in model['entries']:
        if expected['chapter'] != chapter or expected['offset'] < start:
            continue
        assert anchors.get(expected['target']) == expected['offset'], ('Body fragment offset', expected)
        assert visible[expected['offset'] - start:].startswith(expected['title'].encode()), ('Heading text', expected)
        resolved.append(expected['index'])
    declared = {(e['target'], e['title'], e['level']) for e in model['entries']}
    extras = [dict(target=e['first'].decode(), title=e['second'].decode(), level=e['a']) for e in stream
              if e['kind'] == 3 and e['second'] and (e['first'].decode(), e['second'].decode(), e['a']) not in declared]
    return dict(chapter=chapter, body_bytes=len(visible), toc_entries=len(toc), resolved_indices=resolved,
                source_begin=start, source_target=[2, chapter, targets[0]['b']], extra_title_declarations=extras)


def mutation_checks(data, model, bodies):
    """Prove the oracle rejects incorrect chapter, fragment, and body location."""
    stream = list(events(data))
    fifth = [e for e in stream if e['kind'] == 5 and e['a'] == 1][5]
    wrong_chapter = bytearray(data)
    struct.pack_into('<I', wrong_chapter, fifth['at'] + 12, 5)
    wrong_fragment = bytearray(data)
    start = fifth['at'] + 32
    wrong_fragment[start:start+len(fifth['first'])] = fifth['first'][:-1] + b'5'
    anchor = next(e for e in stream if e['kind'] == 3 and not e['second'] and e['first'] == fifth['first'])
    wrong_location = bytearray(data)
    wrong_location[anchor['at']+32:anchor['at']+32+len(anchor['first'])] = anchor['first'][:-1] + b'5'
    passed = []
    for name, altered in [('wrong chapter 5', wrong_chapter), ('wrong fragment id_5', wrong_fragment),
                          ('missing body fragment id_6', wrong_location)]:
        try:
            check_wire(altered, model, bodies)
        except AssertionError:
            passed.append(name)
        else:
            raise AssertionError('Oracle accepted mutation: ' + name)
    return passed


def capture(epub, output, chapter_count):
    compute = ROOT / 'modules/books/compute'
    config = runpy.run_path(str(compute / 'sources.py'))['configuration']()
    sources = [Path(s) for s in config['sources']] + [compute / 'document_remote.c',
        ROOT / 'components/compute/epd_compute_pipe_remote.c', HERE / 'remote_host.c']
    includes = [*map(Path, config['includes']), ROOT / 'components/compute/include',
                ROOT / 'components/compute_reader_image/include']
    flags = ['-std=gnu11', '-g', '-O1', '-fexceptions', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie',
             '-ffunction-sections', '-fdata-sections', '-D_DEFAULT_SOURCE', '-DBOOK_FORMAT_ACPU',
             '-DCORE_ACPU', '-DLIBXML_STATIC', *('-I' + str(p) for p in includes)]
    build = output / 'converter'
    build.mkdir(exist_ok=False)
    before = {str(p): sha(p) for p in sources}

    def compile_one(item):
        i, source = item
        obj = build / f'{i}.o'
        force = config['force_includes'].get(str(source))
        command = ['gcc', *flags, *(['-include', force] if force else []), '-c', str(source), '-o', str(obj)]
        result = subprocess.run(command, capture_output=True, timeout=120)
        (build / f'{i}.log').write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, f'Compile failed: {source}; see {build / str(i)}.log'
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        objects = list(pool.map(compile_one, enumerate(sources)))
    assert before == {str(p): sha(p) for p in sources}, 'Sources changed while compiling'
    (build / 'source-sha256.json').write_text(json.dumps(before, indent=2), encoding='utf-8')
    exe = build / 'capture'
    subprocess.run(['gcc', *map(str, objects), '-pthread', '-lm', '-fsanitize=address,undefined',
                    '-no-pie', '-Wl,--gc-sections', '-o', str(exe)], check=True)
    wires = []
    for chapter in range(chapter_count):
        wire = output / f'spine-{chapter:02}.wire'
        result = subprocess.run([str(exe), '--capture', str(epub), str(wire), str(chapter), '0', '0', '1', '0'],
                                capture_output=True, timeout=60)
        (output / f'spine-{chapter:02}.log').write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0, f'Capture failed: chapter {chapter}'
        wires.append(wire)
    return wires


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--epub', type=Path, required=True)
    parser.add_argument('--wire', type=Path, action='append', default=[])
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    model, bodies = original_book(args.epub)
    (output / 'original-toc.json').write_text(json.dumps(model, ensure_ascii=False, indent=2), encoding='utf-8')
    (output / 'original-toc.tsv').write_text(''.join(
        f"{e['chapter']}\t{e['level']}\t{e['offset']}\t{e['target']}\t{e['title']}\n" for e in model['entries']), encoding='utf-8')
    wires = [p.resolve() for p in args.wire]
    if args.capture:
        wires += capture(args.epub.resolve(), output, len(model['chapters']))
    assert wires, 'Supply --wire or --capture to compare actual converter output'
    results = []
    for wire in wires:
        data = wire.read_bytes()
        result = check_wire(data, model, bodies)
        if result['chapter'] == 2 and result['source_begin'] == 0:
            result['rejected_mutations'] = mutation_checks(data, model, bodies)
        result.update(wire=str(wire), sha256=sha(wire))
        results.append(result)
        print(f"PASS chapter={result['chapter']} META_TOC=108 body_bytes={result['body_bytes']} fragments={len(result['resolved_indices'])}", flush=True)
    (output / 'audit-results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('Report:', output / 'audit-results.json', flush=True)


if __name__ == '__main__':
    main()
