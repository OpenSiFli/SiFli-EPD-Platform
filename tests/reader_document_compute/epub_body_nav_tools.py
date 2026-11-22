"""Build and package isolated document candidates without updating the checkout."""
import difflib
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCES = (
    'modules/books/formats/book_epub.c', 'modules/books/compute/book_compute.h',
    'modules/books/compute/book_compute.c', 'modules/books/compute/book_compute_slice.c',
    'modules/books/compute/book_compute_internal.h', 'modules/books/compute/book_compute_markup.c',
    'modules/books/compute/document_wire.h', 'modules/books/compute/document_remote.c',
    'modules/books/document/document.c', 'modules/books/document/document.h',
    'modules/books/document/document_view.c', 'modules/common/reader/reader_view.c',
    'modules/common/reader/reader.h', 'modules/books/reader_controls.c', 'modules/books/command.c')


def mirror(candidate, output):
    seen = {}

    def visit(relative):
        if relative in seen:
            return seen[relative]
        src = candidate / relative
        if not src.exists():
            src = ROOT / relative
        dest = output / relative
        seen[relative] = dest
        dest.parent.mkdir(parents=True, exist_ok=True)
        source = src.read_text(encoding='utf-8')

        def include(match):
            named = match[1]
            original = (ROOT / relative).parent / named
            isolated = (candidate / relative).parent / named
            if not original.exists() and not isolated.exists():
                return match[0]
            resolved = original.resolve()
            if not resolved.is_relative_to(ROOT):
                return match[0]
            target = visit(resolved.relative_to(ROOT))
            return '#include "' + target.as_posix() + '"'

        source = re.sub(r'#include\s+"([^"\n]+)"', include, source)
        dest.write_text(source, encoding='utf-8')
        return dest

    overrides = {}
    manifest = candidate.parent / 'manifest.json'
    records = json.loads(manifest.read_text(encoding='utf-8')) if candidate != ROOT and manifest.exists() else [dict(path=p) for p in SOURCES]
    for record in records:
        relative = Path(record['path'])
        overrides[relative.name] = visit(relative)
    return overrides


def package(work):
    if not (work/'manifest.json').exists():
        return
    patch = ''
    for record in json.loads((work / 'manifest.json').read_text(encoding='utf-8')):
        p = record['path']
        before = (work / 'base' / p).read_bytes().decode('utf-8')
        after = (work / 'candidate' / p).read_bytes().decode('utf-8')
        if before == after:
            continue
        patch += f'diff --git a/{p} b/{p}\n' + ''.join(difflib.unified_diff(
            before.splitlines(True), after.splitlines(True), 'a/' + p, 'b/' + p))
    (work / 'epub-body-navigation.patch').write_bytes(patch.encode('utf-8'))
