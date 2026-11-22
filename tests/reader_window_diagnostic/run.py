"""Run production window snapshots and routes with the real page cache under ASAN."""
from pathlib import Path
import argparse
import json
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CASES = ('states', 'cross', 'mixed', 'lifecycle', 'atomic', 'context', 'lifetime-race')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'modules/common/reader/reader_view.c')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--expect-context-negative', action='store_true')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    extract = runpy.run_path(str(ROOT / 'tests/reader_view_async/run.py'))['extract']
    types = source[source.index('#if defined(EPD_INPUT_TEST_ENABLED)', source.index('} view_state_t;')):
                   source.index('static view_state_t *state,')]
    routes = 'static struct { view_state_t *owner; int hop; } window_slots[4];\n'
    routes += '\n'.join(extract(n, source) for n in ('window_owner', 'window_number', 'window_start',
                        'window_end', 'window_page', 'window_entry', 'window_extra'))
    routes += '\n' + source[source.index('typedef struct { view_state_t *owner; unsigned number, distance; }'):
                              source.index('static unsigned window_priority')]
    names = ('frame_pixels_ready', 'restore_view_flags', 'window_snapshot_row', 'window_snapshot_publish',
             'window_snapshot_copy', 'epd_reader_window_diagnostic', 'worker_wait')
    functions = '\n'.join(extract(n, source) for n in names)
    code = (HERE / 'window_host.c').read_text(encoding='utf-8')
    code = code.replace('/* TYPES */', types).replace('/* ROUTES */', routes).replace('/* FUNCTIONS */', functions)
    build = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='reader-window-'))
    build.mkdir(parents=True, exist_ok=True)
    (build / 'window.c').write_text(code, encoding='utf-8')
    cc, env = runpy.run_path(str(ROOT / 'tests/reader_prefetch/cache_run.py'))['compiler_environment'](None)
    exe = build / 'window.exe'
    includes = [ROOT / p for p in ('modules/common/reader', 'modules/common/reader/compute/include',
                'components/compute_reader_render/include', 'src/platform', 'tests/unified_resource/stubs')]
    cmd = [cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4505', '/wd4200', '/Od', '/Zi',
           '/fsanitize=address', '/DEPD_INPUT_TEST_ENABLED=1', *(f'/I{p}' for p in includes),
           str(build / 'window.c'), str(ROOT / 'modules/common/reader/reader_page_cache.c'), f'/Fe{exe}']
    compiled = subprocess.run(cmd, cwd=build, env=env, capture_output=True)
    (build / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print((compiled.stdout + compiled.stderr).decode(errors='replace'))
    compiled.check_returncode()
    results = []
    for case in (('context',) if args.expect_context_negative else args.case or CASES):
        result = subprocess.run([str(exe), case], cwd=build, env=env, capture_output=True, timeout=30)
        output = result.stdout + result.stderr
        (build / (case + '.log')).write_bytes(output)
        print(output.decode(errors='replace'), end='', flush=True)
        if args.expect_context_negative:
            assert result.returncode and b'Assertion failed' in output, 'context counterexample did not fail'
        else:
            result.check_returncode()
        results.append({'case': case, 'exit': result.returncode, 'expected_failure': args.expect_context_negative})
    (build / 'results.json').write_text(json.dumps({'source': str(args.source.resolve()), 'command': cmd,
                                                  'cases': results}, indent=2) + '\n')
    print('Evidence:', build)


if __name__ == '__main__':
    main()
