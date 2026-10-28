"""Store a pre-generated WDB as deterministic gzip and restore it for packaging."""
import argparse
from contextlib import ExitStack
import gzip
import hashlib
import json
import os
from pathlib import Path
import tempfile
import zlib


def transfer(source, report, destination, *, compress=False):
    """Verify the uncompressed content before atomically publishing the result."""
    source, report, destination = Path(source), Path(report), Path(destination)
    metadata = json.loads(report.read_text(encoding='utf-8'))
    expected_size, expected_hash = metadata['bytes'], metadata['sha256']
    if (type(expected_size) is not int or expected_size < 0 or
            not isinstance(expected_hash, str) or len(expected_hash) != 64 or
            any(c not in '0123456789abcdef' for c in expected_hash)):
        raise ValueError('Invalid dictionary size or SHA256 in ' + str(report))
    if destination.resolve() in (source.resolve(), report.resolve()):
        raise ValueError('Dictionary destination must differ from its inputs')
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with ExitStack() as stack:
            input_file = stack.enter_context(source.open('rb') if compress else gzip.open(source, 'rb'))
            output_file = stack.enter_context(tempfile.NamedTemporaryFile(
                mode='wb', dir=destination.parent, prefix=destination.name + '.', suffix='.tmp', delete=False))
            temporary = Path(output_file.name)
            writer = (stack.enter_context(gzip.GzipFile(filename='', mode='wb', fileobj=output_file,
                                                       compresslevel=9, mtime=0)) if compress else output_file)
            digest, size = hashlib.sha256(), 0
            while chunk := input_file.read(1024 * 1024):
                size += len(chunk)
                if size > expected_size:
                    raise ValueError('Dictionary exceeds the size recorded in ' + str(report))
                digest.update(chunk)
                writer.write(chunk)
            if size != expected_size or digest.hexdigest() != expected_hash:
                raise ValueError('Dictionary size or SHA256 does not match ' + str(report))
        os.replace(temporary, destination)
        temporary = None
        return destination
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def unpack(source, report, destination):
    return transfer(source, report, destination)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('pack', 'unpack'))
    parser.add_argument('source', type=Path)
    parser.add_argument('report', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    try:
        result = transfer(args.source, args.report, args.destination, compress=args.operation == 'pack')
        print(f'{result}: {result.stat().st_size} bytes')
    except (OSError, EOFError, ValueError, KeyError, TypeError, zlib.error) as error:
        parser.exit(1, f'Dictionary {args.operation} failed: {error}\n')
