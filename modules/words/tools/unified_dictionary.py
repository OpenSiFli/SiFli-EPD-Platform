"""Offline ECDICT + kajweb normalizer and WDB2 writer (standard library only)."""
import csv
import hashlib
import json
from pathlib import Path
import struct
import sys
import unicodedata
import zipfile

ECDICT_REVISION = 'bc015ed2e24a7abef49fc6dbbb7fe32c1dadaf8b'
KAJWEB_REVISION = '3992bcb94c800a2fd38a9fd6ff95b2353e755363'
KEY_SIZE = 96
INDEX = struct.Struct('<96sII')
ALIAS = struct.Struct('<96sI')
SCOPE = struct.Struct('<32s64sII')
FOLD = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')


def normalize(value):
    if value is None:
        return ''
    if not isinstance(value, str):
        raise ValueError('text field is not a string')
    text = unicodedata.normalize('NFC', value.replace('\\r\\n', '\n').replace('\\n', '\n')
                                 .replace('\r\n', '\n').replace('\r', '\n')).strip()
    if '\0' in text:
        raise ValueError('embedded NUL')
    text.encode('utf-8')  # Reject unpaired JSON surrogates before merging any fields.
    return text


def key(word):
    return word.translate(FOLD).encode('utf-8')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def json_records(stream):
    # The repository distributes JSON-lines in ZIPs; exported JSON arrays also work.
    def unique_object(pairs):
        result = {}
        for name, value in pairs:
            if name in result:
                raise ValueError('duplicate JSON key: ' + name)
            result[name] = value
        return result

    def parse(text):
        def invalid_constant(value):
            raise ValueError('invalid JSON constant: ' + value)
        return json.loads(text, object_pairs_hook=unique_object, parse_constant=invalid_constant)

    try:
        text = stream.read().decode('utf-8-sig')
    except UnicodeError as error:
        yield 0, error
        return
    if text.lstrip().startswith('['):
        try:
            records = parse(text)
        except ValueError as error:
            yield 0, error
            return
        for line, record in enumerate(records, 1):
            yield line, record
    else:
        for line, text in enumerate(text.splitlines(), 1):
            if not text.strip():
                continue
            try:
                yield line, parse(text)
            except ValueError as error:
                yield line, error


def input_records(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            members = sorted((info for info in archive.infolist()
                              if not info.is_dir() and info.filename.lower().endswith('.json')),
                             key=lambda info: info.filename)
            if not members:
                yield '', 0, ValueError('ZIP contains no JSON records')
            for info in members:
                with archive.open(info) as stream:
                    for line, record in json_records(stream):
                        yield info.filename, line, record
    else:
        with Path(path).open('rb') as stream:
            for line, record in json_records(stream):
                yield Path(path).name, line, record


def csv_records(source):
    previous_limit = csv.field_size_limit(sys.maxsize)
    try:
        with source.open(encoding='utf-8-sig', newline='') as stream:
            reader = csv.DictReader(stream)
            if not {'word', 'translation'}.issubset(reader.fieldnames or ()):
                raise ValueError('CSV requires word and translation columns')
            for row in reader:
                yield reader.line_num, row
    finally:
        csv.field_size_limit(previous_limit)


def build(source, destination, kajweb=(), catalog=None, include_scopes=(),
          revision=ECDICT_REVISION, kajweb_revision=KAJWEB_REVISION, mapping=None):
    source, destination = Path(source), Path(destination)
    report_path = destination.with_suffix(destination.suffix + '.json')
    if destination.exists() or report_path.exists():
        raise ValueError('Output already exists; choose a new generation')
    mapping_path = Path(mapping) if mapping else Path(__file__).with_name('scope_map.json')
    mapping = json.loads(mapping_path.read_text(encoding='utf-8'))
    names = mapping['scopes']
    if mapping.get('version') != 1:
        raise ValueError('Unsupported scope mapping version')
    include_scopes = set(include_scopes)
    if include_scopes - names.keys():
        raise ValueError('Unknown scope: ' + ','.join(sorted(include_scopes - names.keys())))
    for scope, name in names.items():
        if (not scope.isascii() or not scope or scope != normalize(scope)
                or len(scope.encode()) >= 32 or not normalize(name) or name != normalize(name)
                or len(name.encode()) >= 64):
            raise ValueError('Scope exceeds WDB2 field width')
    for section in ('ecdict_tags', 'kajweb_catalog_tags'):
        for tag, scope in mapping[section].items():
            if not normalize(tag) or scope not in names:
                raise ValueError('Invalid scope mapping: ' + section + ':' + tag)
    books = {}
    if kajweb:
        if not catalog:
            raise ValueError('kajweb requires the corresponding bookLists.txt catalog')
        for book in json.loads(Path(catalog).read_text(encoding='utf-8-sig'))['data']['normalBooksInfo']:
            book_id = normalize(book['id'])
            if book_id in books and books[book_id] != book:
                raise ValueError('Conflicting catalog entries for bookId: ' + book_id)
            books[book_id] = book
    entries, rejected, duplicates = {}, [], []
    sources = {'ecdict': {'id': 'ecdict', 'url': 'https://github.com/skywind3000/ECDICT',
        'revision': revision, 'file': source.name, 'sha256': digest(source), 'license': 'MIT'}}
    records_read, records_accepted, unmapped = {}, {}, set()
    batches, rejected_sources, duplicate_inputs, input_files = {}, [], [], []

    def new_entry():
        return {'items': {}, 'sources': set(), 'scopes': set(), 'deduplicated': 0,
                'example_attributions': [], 'omitted_fields': {}}

    def get_entry(word):
        word = normalize(word)
        if not word or len(word.encode()) >= KEY_SIZE:
            raise ValueError('empty or oversized headword (96-byte index/legacy identity field)')
        if word not in entries:
            entries[word] = new_entry()
        return word, entries[word]

    def add(entry, kind, values, origin):
        values = tuple(normalize(value) for value in values)
        if not any(values):
            return
        # Whitespace-only differences are duplicates; preserve first source's spelling.
        identity = (kind, tuple(' '.join(value.split()) for value in values))
        if identity in entry['items']:
            entry['deduplicated'] += 1
            entry['items'][identity][1].add(origin)
        else:
            entry['items'][identity] = (values, {origin})

    def scope_add(entry, scope, origin):
        entry['scopes'].add(scope)
        add(entry, 7, (scope, names[scope]), origin)

    def merge(word, part):
        word, entry = get_entry(word)
        entry['sources'].update(part['sources'])
        entry['scopes'].update(part['scopes'])
        entry['deduplicated'] += part['deduplicated']
        entry['example_attributions'].extend(part['example_attributions'])
        for field, count in part['omitted_fields'].items():
            entry['omitted_fields'][field] = entry['omitted_fields'].get(field, 0) + count
        for (kind, _), (values, origins) in part['items'].items():
            for origin in sorted(origins):
                add(entry, kind, values, origin)

    for line, row in csv_records(source):
        try:
            if None in row or any(value is None for value in row.values()):
                raise ValueError('CSV row has an incorrect column count')
            fields = {name: normalize(row.get(name)) for name in
                      ('word', 'phonetic', 'translation', 'definition', 'exchange', 'tag')}
            word = fields['word']
            part = new_entry()
            part['sources'].add(('ecdict', word))
            if fields['phonetic']:
                add(part, 1, ('general', fields['phonetic']), 'ecdict')
            for field, lang in (('translation', 1), ('definition', 2)):
                for definition in fields[field].splitlines():
                    if definition.strip():
                        sense = ['', '', '']; sense[lang] = definition
                        add(part, 2, sense, 'ecdict')
            for item in fields['exchange'].split('/'):
                kind, separator, variants = item.partition(':')
                if separator:
                    for variant in variants.split(','):
                        if variant.strip():
                            add(part, 3, (kind, variant), 'ecdict')
            unknown = set()
            for tag in fields['tag'].split():
                scope = mapping['ecdict_tags'].get(tag)
                if scope:
                    scope_add(part, scope, 'ecdict')
                else:
                    unknown.add('ecdict:' + tag)
            # Commit only after every field and the headword are valid.
            word, entry = get_entry(word)
            if ('ecdict', word) in entry['sources']:
                duplicates.append({'source': 'ecdict', 'line': line, 'word': word})
            merge(word, part)
            unmapped.update(unknown)
            records_read['ecdict'] = records_read.get('ecdict', 0) + 1
            records_accepted['ecdict'] = records_accepted.get('ecdict', 0) + 1
        except (ValueError, UnicodeError) as error:
            rejected.append({'source': 'ecdict', 'line': line, 'reason': str(error)})

    seen_paths = set()
    for path in sorted(map(Path, kajweb)):
        resolved = path.resolve()
        if resolved in seen_paths:
            duplicate_inputs.append(str(path))
            continue
        seen_paths.add(resolved)
        file_hash = digest(path)
        input_files.append({'file': path.name, 'sha256': file_hash})
        for member, line, row in input_records(path):
            try:
                if isinstance(row, Exception):
                    raise ValueError(str(row))
                book_id = normalize(row['bookId'])
                if book_id not in books:
                    raise ValueError('bookId missing from supplied catalog: ' + book_id)
                origin = 'kajweb:' + book_id
                if not book_id or ',' in origin:
                    raise ValueError('invalid source identity')
                fingerprint = hashlib.sha256(json.dumps(row, ensure_ascii=True, sort_keys=True,
                                                        separators=(',', ':')).encode()).hexdigest()
                batch = batches.setdefault(origin, {}).setdefault(resolved,
                    {'meta': {'file': path.name, 'sha256': file_hash, 'book_id': book_id},
                     'records': {}, 'conflicts': set(), 'members': set(), 'all_rows': set(), 'observed': {}})
                batch['members'].add(member)
                batch['all_rows'].add(fingerprint)
                raw = row['content']['word']; body = raw['content']
                record_id = normalize(raw['wordId'])
                if not record_id:
                    raise ValueError('invalid source identity')
                if record_id in batch['observed'] and batch['observed'][record_id] != fingerprint:
                    batch['conflicts'].add(record_id)
                batch['observed'][record_id] = fingerprint
                headword = normalize(row['headWord'])
                if not headword or len(headword.encode()) >= KEY_SIZE:
                    raise ValueError('empty or oversized headword (96-byte index/legacy identity field)')
                if normalize(raw['wordHead']) != headword:
                    raise ValueError('headWord/wordHead mismatch')
                title = normalize(books[book_id]['title'])
                if not title:
                    raise ValueError('empty catalog title')
                tags = [normalize(tag['tagName']) for tag in books[book_id].get('tags', [])]
                # Build into a temporary record so malformed fields cannot partially merge.
                part = new_entry()
                part['sources'].add((origin, record_id))
                for region, field in (('general', 'phone'), ('uk', 'ukphone'), ('us', 'usphone')):
                    if body.get(field):
                        add(part, 1, (region, body[field]), origin)
                for sense in body.get('trans', []):
                    add(part, 2, (sense.get('pos'), sense.get('tranCn'), sense.get('tranOther')), origin)
                for item in body.get('sentence', {}).get('sentences', []):
                    add(part, 4, (item.get('sContent'), item.get('sCn')), origin)
                for item in body.get('realExamSentence', {}).get('sentences', []):
                    sentence = normalize(item.get('sContent'))
                    translation = normalize(item.get('sCn'))
                    add(part, 4, (sentence, translation), origin)
                    if sentence and item.get('sourceInfo'):
                        info = {normalize(k): normalize(v) for k, v in item['sourceInfo'].items()}
                        part['example_attributions'].append({'source': origin, 'record_id': record_id,
                            'sentence_sha256': hashlib.sha256(sentence.encode()).hexdigest(), 'source_info': info})
                for item in body.get('phrase', {}).get('phrases', []):
                    add(part, 5, (item.get('pContent'), item.get('pCn')), origin)
                for rel in body.get('relWord', {}).get('rels', []):
                    for item in rel.get('words', []):
                        add(part, 8, (rel.get('pos'), item.get('hwd'), item.get('tran')), origin)
                for tag in tags:
                    scope = mapping['kajweb_catalog_tags'].get(tag)
                    if scope:
                        scope_add(part, scope, origin)
                represented = {'phone', 'ukphone', 'usphone', 'trans', 'sentence',
                               'realExamSentence', 'phrase', 'relWord'}
                for field in body.keys() - represented:
                    if body[field]:
                        part['omitted_fields'][origin + ':' + field] = 1
                meta = {'id': origin, 'url': 'https://github.com/kajweb/dict', 'revision': kajweb_revision,
                    'file': path.name, 'member': member, 'sha256': file_hash, 'book_id': book_id,
                    'title': title, 'version': books[book_id].get('version'),
                    'catalog_tags': tags, 'license': 'unspecified',
                    'scopes': sorted(part['scopes'])}
                batch['meta'] = meta
                previous = batch['records'].get(record_id)
                if previous:
                    if previous[0] != fingerprint:
                        batch['conflicts'].add(record_id)
                    else:
                        duplicates.append({'source': origin, 'file': path.name, 'member': member,
                                           'line': line, 'word': headword, 'record_id': record_id})
                else:
                    batch['records'][record_id] = (fingerprint, headword, part, member, line)
                records_read[origin] = records_read.get(origin, 0) + 1
            except (KeyError, TypeError, ValueError, AttributeError, UnicodeError) as error:
                rejected.append({'source': str(path.name), 'member': member, 'line': line, 'reason': str(error)})

    for origin, variants in sorted(batches.items()):
        variants = list(variants.values())
        first = variants[0]
        conflict = any(batch['conflicts'] or batch['all_rows'] != first['all_rows'] for batch in variants)
        files = [{'file': batch['meta']['file'], 'sha256': batch['meta']['sha256'],
                  'members': sorted(batch['members'])} for batch in variants]
        if conflict:
            reason = 'conflicting versions or record IDs for bookId; entire source excluded'
            rejected_sources.append({'source': origin, 'files': files, 'reason': reason})
            for batch in variants:
                rejected.append({'source': batch['meta']['file'], 'book_id': batch['meta']['book_id'],
                                 'reason': reason})
            continue
        if not first['records']:
            continue
        sources[origin] = dict(first['meta'], files=files)
        records_accepted[origin] = len(first['records'])
        for batch in variants[1:]:
            for rid, (_, word, _, member, line) in sorted(batch['records'].items()):
                duplicates.append({'source': origin, 'file': batch['meta']['file'], 'member': member,
                                   'line': line, 'word': word, 'record_id': rid})
        for _, word, part, _, _ in first['records'].values():
            merge(word, part)
        if not first['meta']['scopes']:
            unmapped.add(origin + ':' + ','.join(first['meta']['catalog_tags']))

    words = sorted((word for word, entry in entries.items() if not include_scopes or entry['scopes'] & include_scopes),
                   key=lambda word: (key(word), word.encode()))
    if not words:
        raise ValueError('No usable entries')
    positions = {word: i for i, word in enumerate(words)}
    aliases, members, payloads = set(), {}, []
    for word in words:
        entry = entries[word]
        items = []
        for (kind, _), (values, origins) in sorted(entry['items'].items()):
            fields = (*values, ','.join(sorted(origins)))
            body = b'\0'.join(value.encode() for value in fields) + b'\0'
            items.append(struct.pack('<II', kind, len(body)) + body)
            if kind == 3:
                form, variant = values
                if variant and len(variant.encode()) < KEY_SIZE:
                    if form == '0' and variant in positions:
                        aliases.add((key(word), positions[variant]))
                    elif form in {'p', 'd', 'i', '3', 's', 'r', 't'}:
                        aliases.add((key(variant), positions[word]))
        for origin, original_id in sorted(entry['sources']):
            body = (origin + '\0' + original_id + '\0').encode()
            items.append(struct.pack('<II', 6, len(body)) + body)
        payloads.append(b'\0EN2' + struct.pack('<I', len(items)) + word.encode() + b'\0' + b''.join(items))
        for scope in entry['scopes']:
            members.setdefault(scope, []).append(positions[word])
    alias_data = b''.join(ALIAS.pack(k, i) for k, i in sorted(aliases))
    scopes, scope_data, member_data = [], bytearray(), bytearray()
    for scope in names:  # Stable presentation order from the explicit map.
        if include_scopes and scope not in include_scopes:
            continue
        indices = members.get(scope, [])
        if not indices:
            continue
        scope_data += SCOPE.pack(scope.encode(), names[scope].encode(), len(member_data) // 4, len(indices))
        member_data += struct.pack('<' + 'I' * len(indices), *indices)
        scopes.append({'id': scope, 'name': names[scope], 'members': len(indices)})
    alias_offset = 96 + len(words) * INDEX.size
    scope_offset = alias_offset + len(alias_data)
    member_offset = scope_offset + len(scope_data)
    data_offset = member_offset + len(member_data)
    offset, index = data_offset, bytearray()
    for word, payload in zip(words, payloads):
        if offset + len(payload) > 0x7fffffff:
            raise ValueError('Dictionary exceeds signed 32-bit device file offsets')
        index += INDEX.pack(key(word), offset, len(payload)); offset += len(payload)
    output_entries = {origin: 0 for origin in sources}
    omitted_fields, example_attributions = {}, {}
    for word in words:
        entry = entries[word]
        for origin in {item[0] for item in entry['sources']}:
            output_entries[origin] += 1
        for field, count in entry['omitted_fields'].items():
            omitted_fields[field] = omitted_fields.get(field, 0) + count
        for attribution in entry['example_attributions']:
            example_attributions[json.dumps(attribution, sort_keys=True, ensure_ascii=False)] = attribution
    for origin, count in output_entries.items():
        sources[origin]['output_entries'] = count
    report = {'format_version': 2, 'identity': 'en:<NFC case-sensitive original headword>',
        'sources': list(sources.values()), 'records_read': records_read, 'scopes': scopes,
        'records_accepted': records_accepted, 'input_files': input_files,
        'duplicate_inputs': duplicate_inputs, 'rejected_sources': rejected_sources,
        'example_attributions': [example_attributions[k] for k in sorted(example_attributions)],
        'omitted_kajweb_fields': omitted_fields,
        'mapping': mapping, 'mapping_sha256': digest(mapping_path),
        'catalog_sha256': digest(catalog) if catalog else None,
        'entries': len(words), 'aliases': len(aliases), 'include_scopes': sorted(include_scopes),
        'deduplicated_content': sum(entries[word]['deduplicated'] for word in words), 'duplicate_records': duplicates,
        'rejected': rejected, 'unmapped_tags': sorted(unmapped),
        'cross_source_entries': sum(len({s[0].split(':')[0] for s in entries[w]['sources']}) > 1 for w in words),
        'content_bytes': sum(map(len, payloads)), 'max_entry_bytes': max(map(len, payloads)),
        'redistribution': 'kajweb upstream declares no explicit license; separate permission is required' if kajweb else 'ECDICT MIT'}
    metadata = json.dumps(report, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
    size = offset + len(metadata)
    if size > 0x7fffffff:
        raise ValueError('Dictionary exceeds signed 32-bit device file offsets')
    body = bytes(index) + alias_data + scope_data + member_data + b''.join(payloads) + metadata
    generation = hashlib.sha256(body).digest()[:24]
    header = struct.pack('<8s8I24s8I', b'EPDWD02\0', 2, len(words), len(aliases), 96,
                         alias_offset, data_offset, size, KEY_SIZE, generation,
                         len(scopes), scope_offset, member_offset, offset, len(metadata), 0, 0, 0)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open('xb') as stream:
        stream.write(header); stream.write(body)
    report.update(bytes=size, sha256=digest(destination), generation=generation.hex())
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return report
