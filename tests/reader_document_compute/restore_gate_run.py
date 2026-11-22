"""Exercise restoration gating with the production document worker and real spool I/O."""
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
    parser.add_argument('--compare-baseline', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--document-source', type=Path, default=ROOT / 'modules/books/document/document.c')
    parser.add_argument('--case', action='append')
    parser.add_argument('--expect-failure', choices=('RESTORE_GATE_PAUSE', 'RESTORE_BODY_FRONTIER'))
    args = parser.parse_args()
    compiler, env = compiler_environment(None)
    output = ROOT / 'tmp/reader_document_restore_gate'
    output.mkdir(exist_ok=True, parents=True)
    build = Path(tempfile.mkdtemp(prefix='host-', dir=output))
    for name in ('app_service.h', 'rtthread.h', 'rthw.h', 'storage.h', 'storage_file.h',
                 'epd_memory.h', 'reader_compute_client.h', 'unistd.h', 'dirent.h',
                 'platform/epd_app.h', 'platform/epd_app_library.h'):
        path = build / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_text('#include "hcpu_host.h"\n')
    # Reuse only the established transport/cache doubles, before integration cases.
    mocks = (HERE / 'hcpu_test.c').read_text(encoding='utf-8').split('static void pump(void)', 1)[0]
    mocks = '#include "activity_fixture.h"\n#include "epd_reader_render.h"\n' + mocks
    mocks += '\n#include "cache_bridge.h"\nsize_t document_cache_memory_bytes(void) { return 0; }\nsize_t document_cache_memory_reclaim(size_t n) { (void)n; return 0; }\n'
    (build / 'restore_host.inc').write_text(mocks, encoding='utf-8')
    stubs = (HERE / 'hcpu_compute_stubs.h').read_text(encoding='utf-8')
    stubs = stubs.replace('assert(r && !r->active && current_document->image_render == r);',
                          'assert(!"unexpected remote renderer in scheduling fixture");')
    (build / 'hcpu_compute_stubs.h').write_text(stubs, encoding='utf-8')
    includes = [build, HERE, ROOT / 'modules/books/document', ROOT / 'components/compute/include',
                ROOT / 'src/platform', ROOT / 'components/compute_reader_render/include',
                ROOT / 'components/compute_reading_data/include', ROOT / 'tests/unified_resource',
                ROOT / 'tests/unified_resource/stubs']
    flags = [compiler, '/nologo', '/std:c11', '/utf-8', '/Od', '/Zi', '/Gy', '/W3',
             '/D_CRT_SECURE_NO_WARNINGS', '/DHCPU_DOCUMENT_SOURCE', *(f'/I{p}' for p in includes)]
    if args.sanitize:
        flags += ['/fsanitize=address']
    sources = [('current', args.document_source.resolve())]
    if args.compare_baseline:
        sources.insert(0, ('baseline', args.compare_baseline.resolve()))
    results = []
    for label, source in sources:
        content = source.read_bytes()
        (build / 'document_under_test.c').write_bytes(content)
        exe = build / (label + '.exe')
        kernels = [ROOT / 'components/compute_reader_render' / name for name in ('render_kernel.c', 'render_format.c')]
        kernels.append(ROOT / 'src/platform/epd_resource.c')
        fixture = build / 'restore_gate_test.c'; fixture.write_bytes((HERE / fixture.name).read_bytes())
        compiled = subprocess.run([*flags, str(fixture), *map(str, kernels), f'/Fe{exe}', '/link', '/OPT:REF'],
                                  cwd=build, env=env, capture_output=True, timeout=60)
        (build / (label + '-compile.log')).write_bytes(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise SystemExit((compiled.stdout + compiled.stderr).decode('utf-8', errors='replace'))
        cases = ['cached-target'] if label == 'baseline' else args.case or ['cached-target', 'prefetch-toggle',
            'suspend-resume', 'record-priority', 'missing-target', 'cold-conversion']
        for case in cases:
            result = subprocess.run([str(exe), str(build), case], cwd=build, env=env, capture_output=True, timeout=15)
            log = result.stdout + result.stderr
            (build / (label + '-' + case + '.log')).write_bytes(log)
            marker = 'RESTORE_GATE_PAUSE' if label == 'baseline' else args.expect_failure
            expected_failure = bool(marker and result.returncode == 1 and marker.encode() in log)
            passed = expected_failure if marker else result.returncode == 0
            results.append(dict(source=label, case=case, passed=passed, expected_failure=expected_failure,
                exit_code=result.returncode, source_sha256=hashlib.sha256(content).hexdigest(),
                log=str(build / (label + '-' + case + '.log'))))
            (build / 'results.json').write_text(json.dumps(results, indent=2))
            print(log.decode('utf-8', errors='replace').strip(), flush=True)
            if expected_failure:
                print('PASS reproduced ' + marker, flush=True)
            if not passed:
                raise SystemExit(f'FAIL {label}/{case}; report: {build / "results.json"}')
    print('Report:', build / 'results.json', flush=True)


if __name__ == '__main__':
    main()
