"""Run document fixture regressions with ASan and strict behavioral negative controls.

The remote directory contains first/body/restore/nav wires captured by the real
EPUB converter, plus the fixture directory from epub_body_nav_fixture_run.py.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
from hcpu_reader_extract import function


def mutate(source, name, old, new):
    body = function(source, name)
    if body.count(old) != 1:
        raise ValueError(f'{name}: expected exactly one mutation site: {old}')
    return source.replace(body, body.replace(old, new, 1), 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--remote', type=Path, default=ROOT / 'tmp/epub-body-navigation/remote')
    parser.add_argument('--output', type=Path, default=ROOT / 'tmp/reader-global-window-regression/fixture-refresh')
    args = parser.parse_args()
    remote = args.remote.resolve()
    for relative in ('first.wire', 'body.wire', 'restore.wire', 'nav.wire', 'fixture/note.wire',
                     'fixture/short/first.wire', 'fixture/image-restore/first.wire'):
        if not (remote / relative).is_file():
            parser.error(f'missing real converter fixture: {remote / relative}')
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='asan-', dir=args.output.resolve()))
    paths = {name: ROOT / path for name, path in (
        ('document', 'modules/books/document/document.c'),
        ('reader', 'modules/common/reader/reader_view.c'))}
    originals = {name: path.read_bytes() for name, path in paths.items()}
    sources = {name: content.decode('utf-8').replace('\r\n', '\n') for name, content in originals.items()}
    hashes = {name: hashlib.sha256(content).hexdigest() for name, content in originals.items()}
    fixture_names = ('worker_idle_run.py', 'worker_idle_test.c', 'restore_gate_run.py', 'restore_gate_test.c',
                     'near_flow_run.py', 'near_flow_test.c', 'epub_body_nav_hcpu_run.py',
                     'epub_body_nav_hcpu.c', 'fixture_regression_run.py')
    fixtures = {str((HERE / name).relative_to(ROOT)): hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                for name in fixture_names}
    results = []

    def run(label, script, extra, expected=False):
        command = [sys.executable, '-B', str(HERE / script), *map(str, extra)]
        print('RUN', label, flush=True)
        log = output / (label + '.log')
        with log.open('wb') as stream:
            result = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, timeout=900)
        text = log.read_text(encoding='utf-8', errors='replace')
        report = re.findall(r'^Report: (.+)$', text, re.M)
        cases = json.loads(Path(report[-1].strip()).read_text()) if report else []
        passed = result.returncode == 0 and bool(cases) and all(case['passed'] for case in cases)
        if expected:
            passed = passed and all(case['expected_failure'] and case['exit_code'] == 1 for case in cases)
        results.append(dict(suite=label, command=command, passed=passed, expected_failure=expected,
            exit_code=result.returncode, cases=len(cases), log=str(log), report=report[-1].strip() if report else None))
        (output / 'validation.json').write_text(json.dumps(dict(source_sha256=hashes, fixture_sha256=fixtures,
            remote=str(remote), results=results), indent=2), encoding='utf-8')
        if not passed:
            raise SystemExit(f'FAIL {label}: {log}\n{text[-3000:]}')
        print(f'PASS {label}: {len(cases)} cases', flush=True)

    for label, script in (('worker-idle', 'worker_idle_run.py'), ('restore-gate', 'restore_gate_run.py'),
                          ('near-flow', 'near_flow_run.py')):
        run(label, script, ['--sanitize'])
    run('phased-host', 'epub_body_nav_hcpu_run.py', ['--remote', remote, '--output', output / 'phased-host'])

    variants = [
        ('idle-old-admission', 'document', 'document_worker_step', 'adjacent_runnable(p)',
         '(!epd_reader_suspended() && p->owners && p->prefetch_enabled && p->adjacent_pending && p->status.complete)',
         'worker_idle_run.py', ['--case', 'preempted-owner', '--case', 'cancelled-cache'], 'DOCUMENT_WORKER_IDLE'),
        ('restore-ungated', 'document', 'chapter_restore_runnable',
         'return d->chapter_restore && (', 'return d->chapter_restore && (true || ',
         'restore_gate_run.py', ['--case', 'cached-target'], 'RESTORE_GATE_PAUSE'),
        ('restore-target-only', 'document', 'chapter_restore_runnable',
         'd->nav_requested || target_pending ||\n        body_pending ||', 'd->nav_requested ||',
         'restore_gate_run.py', ['--case', 'cached-target'], 'RESTORE_BODY_FRONTIER'),
        ('near-ungated', 'reader', 'document_flow', '!near_frames_pending()', 'true',
         'near_flow_run.py', ['--case', 'new-neighbor'], 'NEAR_LAYOUT_GATE'),
        ('optional-ungated', 'document', 'flow_allows', 'return allowed;', 'return true;',
         'near_flow_run.py', ['--case', 'existing-neighbor'], 'NEAR_OPTIONAL_IO'),
        ('nav-ungated', 'document', 'navigation_allowed', ' && flow_allows(d)', '',
         'epub_body_nav_hcpu_run.py', ['--case', 'flow', '--remote', remote, '--output', output / 'nav-ungated'], 'NAV_FLOW_GATE'),
    ]
    for label, kind, name, old, new, script, extra, marker in variants:
        source = output / (label + '.c')
        source.write_text(mutate(sources[kind], name, old, new), encoding='utf-8')
        flags = [] if script == 'epub_body_nav_hcpu_run.py' else ['--sanitize']
        run(label, script, [*flags, '--' + kind + '-source', source, *extra, '--expect-failure', marker], True)
    for name, path in paths.items():
        if path.read_bytes() != originals[name]:
            raise SystemExit(f'Production source changed during validation: {path}')
    print('Report:', output / 'validation.json', flush=True)


if __name__ == '__main__':
    main()
