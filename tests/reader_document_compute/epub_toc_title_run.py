"""Replay actual EPUB captures through document.c against ZIP-derived expectations.

Run epub_toc_audit.py first; pass its original-toc.tsv with --oracle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--oracle', type=Path, required=True)
    parser.add_argument('--wire', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    build = Path(tempfile.mkdtemp(prefix='titles-', dir=args.output.resolve()))
    compiler, env = compiler_environment(None)
    for name in ('app_service.h', 'rtthread.h', 'rthw.h', 'storage.h', 'storage_file.h',
                 'epd_memory.h', 'reader_compute_client.h', 'unistd.h', 'dirent.h',
                 'platform/epd_app.h', 'platform/epd_app_library.h'):
        path = build / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_text('#include "hcpu_host.h"\n')
    mocks = (HERE / 'hcpu_test.c').read_text(encoding='utf-8').split('static void pump(void)', 1)[0]
    (build / 'toc_host.inc').write_text(mocks, encoding='utf-8')
    source = (ROOT / 'modules/books/document/document.c').read_bytes()
    (build / 'document_under_test.c').write_bytes(source)
    includes = [build, HERE, ROOT / 'modules/books/document', ROOT / 'components/compute/include',
                ROOT / 'src/platform', ROOT / 'components/compute_reader_render/include',
                ROOT / 'components/compute_reading_data/include']
    flags = [compiler, '/nologo', '/std:c11', '/utf-8', '/Od', '/Zi', '/Gy', '/W3',
             '/D_CRT_SECURE_NO_WARNINGS', '/DHCPU_DOCUMENT_SOURCE', *(f'/I{p}' for p in includes)]
    if args.sanitize:
        flags.append('/fsanitize=address')
    kernels = [ROOT / 'components/compute_reader_render' / name for name in ('render_kernel.c', 'render_format.c')]
    exe = build / 'titles.exe'
    compiled = subprocess.run([*flags, str(HERE / 'epub_toc_title_test.c'), *map(str, kernels),
                               f'/Fe{exe}', '/link', '/OPT:REF'], cwd=build, env=env, capture_output=True, timeout=60)
    (build / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise SystemExit((compiled.stdout + compiled.stderr).decode('utf-8', errors='replace'))
    results = []
    for i, wire in enumerate(args.wire):
        case = build / f'{i}-{wire.stem}'
        case.mkdir()
        result = subprocess.run([str(exe), str(case), str(wire.resolve()), str(args.oracle.resolve())],
                                cwd=build, env=env, capture_output=True, timeout=60)
        log = build / f'{i}-{wire.stem}.log'
        log.write_bytes(result.stdout + result.stderr)
        print((result.stdout + result.stderr).decode('utf-8', errors='replace'), end='', flush=True)
        results.append(dict(wire=str(wire.resolve()), log=str(log), passed=result.returncode == 0,
                            exit_code=result.returncode, document_sha256=hashlib.sha256(source).hexdigest(),
                            wire_sha256=hashlib.sha256(wire.read_bytes()).hexdigest(),
                            oracle_sha256=hashlib.sha256(args.oracle.read_bytes()).hexdigest()))
    report = build / 'results.json'
    report.write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('Report:', report, flush=True)
    raise SystemExit(0 if all(r['passed'] for r in results) else 1)


if __name__ == '__main__':
    main()
