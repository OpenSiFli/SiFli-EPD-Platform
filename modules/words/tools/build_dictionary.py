"""Generate a WDB2 unified dictionary; legacy writer remains for migration tests."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import unicodedata

ECDICT_REVISION = 'bc015ed2e24a7abef49fc6dbbb7fe32c1dadaf8b'
FSRS_REVISION = '9446cb06605c597a063aeee49f7d188d42e34dc2'
KEY_SIZE = 96
ENTRY_MAX = 32768
HEADER = struct.Struct('<8s8I24s')
INDEX = struct.Struct('<96sII')
ALIAS = struct.Struct('<96sI')
FIELDS = ('word', 'phonetic', 'translation', 'definition', 'exchange', 'tag')
ASCII_FOLD = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')


def search_key(word):
    return word.translate(ASCII_FOLD).encode('utf-8')


def normalize(value):
    return unicodedata.normalize('NFC', (value or '').replace('\\n', '\n').strip())


def build(source, destination, tag=None, revision=ECDICT_REVISION):
    source, destination = Path(source), Path(destination)
    if destination.exists():
        raise ValueError('Output already exists; choose a new generation')
    entries, rejected, duplicate, tag_counts = {}, [], [], {}
    with source.open(encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(stream)
        if not {'word', 'translation'}.issubset(reader.fieldnames or ()):
            raise ValueError('CSV requires word and translation columns')
        for line, row in enumerate(reader, 2):
            values = tuple(normalize(row.get(name)) for name in FIELDS)
            word = values[0]
            tags = values[-1].split()
            if tag and tag not in tags:
                continue
            payload = b'\0'.join(value.encode('utf-8') for value in values) + b'\0'
            reason = ''
            if not word or len(word.encode()) >= KEY_SIZE:
                reason = 'empty or oversized word'
            elif any('\0' in value for value in values):
                reason = 'embedded NUL'
            elif len(payload) > ENTRY_MAX:
                reason = 'oversized entry'
            elif tag and not values[2]:
                reason = 'missing translation in learning book'
            if reason:
                rejected.append({'line': line, 'word': word, 'reason': reason})
                continue
            if word in entries:
                duplicate.append({'line': line, 'word': word})
                # Retain the first occurrence; report the duplicate rather than hide it.
                continue
            entries[word] = (values, payload)
            for value in tags:
                tag_counts[value] = tag_counts.get(value, 0) + 1
    if not entries:
        raise ValueError('No usable entries')
    words = sorted(entries, key=lambda word: (search_key(word), word.encode()))
    positions = {word: i for i, word in enumerate(words)}
    aliases = set()
    for word in words:
        for item in entries[word][0][4].split('/'):
            kind, separator, variants = item.partition(':')
            if not separator:
                continue
            for variant in variants.split(','):
                variant = normalize(variant)
                if not variant or len(variant.encode()) >= KEY_SIZE:
                    continue
                if kind == '0':
                    if variant in positions:
                        aliases.add((search_key(word), positions[variant]))
                elif kind in {'p', 'd', 'i', '3', 's', 'r', 't'}:
                    aliases.add((search_key(variant), positions[word]))
    aliases = sorted(aliases)
    alias_offset = HEADER.size + len(words) * INDEX.size
    data_offset = alias_offset + len(aliases) * ALIAS.size
    offset = data_offset
    index, data = bytearray(), bytearray()
    for word in words:
        payload = entries[word][1]
        index += INDEX.pack(search_key(word), offset, len(payload))
        data += payload
        offset += len(payload)
    if offset > 0x7fffffff:
        raise ValueError('Dictionary exceeds device file-offset range')
    alias_data = b''.join(ALIAS.pack(key, entry) for key, entry in aliases)
    digest = hashlib.sha256(index + alias_data + data).digest()
    header = HEADER.pack(b'EPDWD01\0', 1, len(words), len(aliases), HEADER.size,
                         alias_offset, data_offset, offset, KEY_SIZE, digest[:24])
    report = {
        'format_version': 1, 'source': 'https://github.com/skywind3000/ECDICT',
        'revision': revision, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'tag': tag, 'entries': len(words), 'aliases': len(aliases),
        'bytes': offset, 'index_bytes': len(index), 'alias_bytes': len(alias_data),
        'content_bytes': len(data), 'tag_counts': tag_counts,
        'rejected': rejected, 'duplicates': duplicate,
        'identity': 'en:' + '<NFC original word>; direction=en-to-meaning',
    }
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open('xb') as stream:
        stream.write(header)
        stream.write(index)
        stream.write(alias_data)
        stream.write(data)
    destination.with_suffix(destination.suffix + '.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--tag', help='Legacy WDB1 tag filter; requires --legacy')
    parser.add_argument('--revision', default=ECDICT_REVISION)
    parser.add_argument('--legacy', action='store_true', help='Generate WDB1 for migration testing')
    parser.add_argument('--kajweb', type=Path, nargs='*', default=[], help='Actual JSON/JSON-lines files or repository ZIPs')
    parser.add_argument('--catalog', type=Path, help='kajweb bookLists.txt from the same revision')
    parser.add_argument('--kajweb-revision', default='3992bcb94c800a2fd38a9fd6ff95b2353e755363')
    parser.add_argument('--include-scopes', default='', help='Keep the union of these scope IDs; omit for all headwords')
    parser.add_argument('--mapping', type=Path)
    args = parser.parse_args()
    try:
        if args.legacy:
            report = build(args.source, args.destination, args.tag, args.revision)
        else:
            if args.tag:
                raise ValueError('Use --include-scopes for WDB2')
            from unified_dictionary import build as unified_build
            report = unified_build(args.source, args.destination, args.kajweb, args.catalog,
                                   filter(None, args.include_scopes.split(',')), args.revision,
                                   args.kajweb_revision, args.mapping)
    except (ValueError, OSError, csv.Error) as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps({key: report[key] for key in ('entries', 'aliases', 'bytes')}, ensure_ascii=False))
