"""Check near-layout I/O gating against the production reader and document worker."""
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
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment
from hcpu_reader_extract import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--handoff', action='store_true', help='Check pending target ownership and worker wakeups')
    parser.add_argument('--document-source', type=Path, default=ROOT / 'modules/books/document/document.c')
    parser.add_argument('--reader-source', type=Path, default=ROOT / 'modules/common/reader/reader_view.c')
    parser.add_argument('--case', action='append', choices=('new-neighbor', 'existing-neighbor', 'cancel-neighbor', 'unowned-neighbor',
                                                         'parked-display', 'completed-counter', 'same-false-wake'))
    parser.add_argument('--expect-failure', choices=('NEAR_LAYOUT_GATE', 'NEAR_OPTIONAL_IO', 'FLOW_PENDING_OWNER', 'FLOW_OWNER_WAKE'))
    args = parser.parse_args()
    compiler, env = compiler_environment(None)
    output = ROOT / 'tmp/reader-near-layout'
    output.mkdir(exist_ok=True, parents=True)
    build = Path(tempfile.mkdtemp(prefix='host-', dir=output))
    for name in ('app_service.h', 'rtthread.h', 'rthw.h', 'storage.h', 'storage_file.h',
                 'epd_memory.h', 'reader_compute_client.h', 'unistd.h', 'dirent.h',
                 'platform/epd_app.h', 'platform/epd_app_library.h'):
        path = build / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_text('#include "hcpu_host.h"\n')
    mocks = (HERE / 'hcpu_test.c').read_text(encoding='utf-8').split('static void pump(void)', 1)[0]
    mocks = '#include "activity_fixture.h"\n#include "epd_reader_render.h"\n' + mocks
    mocks += '\n#include "cache_bridge.h"\nsize_t document_cache_memory_bytes(void) { return 0; }\nsize_t document_cache_memory_reclaim(size_t n) { (void)n; return 0; }\n'
    (build / 'restore_host.inc').write_text(mocks, encoding='utf-8')
    stubs = (HERE / 'hcpu_compute_stubs.h').read_text(encoding='utf-8')
    stubs = stubs.replace('assert(r && !r->active && current_document->image_render == r);',
                          'assert(!"unexpected remote renderer in scheduling fixture");')
    (build / 'hcpu_compute_stubs.h').write_text(stubs, encoding='utf-8')
    document = args.document_source.read_bytes()
    reader = args.reader_source.read_bytes()
    (build / 'document_under_test.c').write_bytes(document)
    source = reader.decode('utf-8').replace('\r\n', '\n')
    if re.search(r'^static void document_flow\(', source, re.M):
        flow = function(source, 'document_flow')
    else:
        # Compare the former core_step admission condition without rewriting it.
        statement = re.search(r'document_prefetch_enable\(view.document,[\s\S]*?;', function(source, 'core_step'))
        assert statement
        flow = 'static void document_flow(void) {\n' + statement[0] + '\n}\n'
    input_set = function(source, 'view_input_set') if re.search(r'^static void view_input_set\(', source, re.M) else ''
    (build / 'near_flow.inc').write_text(input_set + '\n' + flow, encoding='utf-8')
    if args.handoff:
        names = ('view_input_set', 'window_contexts', 'window_conversion_document', 'request_memory_failed',
                 'navigation_clear', 'link_clear', 'core_background_pause', 'document_flow',
                 'worker_target_needed', 'worker_context_pause', 'worker_quiesce', 'target_resolve',
                 'worker_input_commit')
        (build / 'flow_handoff.inc').write_text('\n\n'.join(function(source, name) for name in names), encoding='utf-8')
    includes = [build, HERE, ROOT / 'modules/books/document', ROOT / 'components/compute/include',
                ROOT / 'src/platform', ROOT / 'components/compute_reader_render/include',
                ROOT / 'components/compute_reading_data/include', ROOT / 'tests/unified_resource',
                ROOT / 'tests/unified_resource/stubs']
    flags = [compiler, '/nologo', '/std:c11', '/utf-8', '/Od', '/Zi', '/Gy', '/W3',
             '/D_CRT_SECURE_NO_WARNINGS', '/DHCPU_DOCUMENT_SOURCE', *(f'/I{p}' for p in includes)]
    if args.sanitize:
        flags += ['/fsanitize=address']
    exe = build / 'near_flow.exe'
    kernels = [ROOT / 'components/compute_reader_render' / name for name in ('render_kernel.c', 'render_format.c')]
    kernels.append(ROOT / 'src/platform/epd_resource.c')
    fixture = HERE / ('flow_handoff_test.c' if args.handoff else 'near_flow_test.c')
    for name in (fixture.name, 'restore_gate_test.c'):
        (build / name).write_bytes((HERE / name).read_bytes())
    fixture = build / fixture.name
    compiled = subprocess.run([*flags, str(fixture), *map(str, kernels), f'/Fe{exe}', '/link', '/OPT:REF'],
                              cwd=build, env=env, capture_output=True, timeout=60)
    (build / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise SystemExit((compiled.stdout + compiled.stderr).decode('utf-8', errors='replace'))
    results = []
    cases = ('parked-display', 'completed-counter', 'same-false-wake') if args.handoff else (
        'new-neighbor', 'existing-neighbor', 'cancel-neighbor', 'unowned-neighbor')
    for case in args.case or cases:
        result = subprocess.run([str(exe), str(build), case], cwd=build, env=env, capture_output=True, timeout=20)
        log = result.stdout + result.stderr
        (build / (case + '.log')).write_bytes(log)
        passed = (result.returncode == 1 and args.expect_failure.encode() in log) if args.expect_failure else result.returncode == 0
        results.append(dict(case=case, passed=passed, expected_failure=args.expect_failure,
            exit_code=result.returncode, document_sha256=hashlib.sha256(document).hexdigest(),
            reader_sha256=hashlib.sha256(reader).hexdigest(), log=str(build / (case + '.log'))))
        (build / 'results.json').write_text(json.dumps(results, indent=2))
        print(log.decode('utf-8', errors='replace').strip(), flush=True)
        if not passed:
            raise SystemExit(f'FAIL {case}; report: {build / "results.json"}')
        if args.expect_failure:
            print('PASS reproduced ' + args.expect_failure, flush=True)
    print('Report:', build / 'results.json', flush=True)


if __name__ == '__main__':
    main()
