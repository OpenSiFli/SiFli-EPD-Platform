"""Exercise actual close functions with a diagnostic read immediately after disposal."""
from pathlib import Path
import argparse
import json
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'modules/common/reader/reader_view.c')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--expect-negative', action='store_true')
    args = parser.parse_args()
    build = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='reader-close-'))
    build.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text(encoding='utf-8')
    extract = runpy.run_path(str(ROOT / 'tests/reader_view_async/run.py'))['extract']
    types = source[source.index('#if defined(EPD_INPUT_TEST_ENABLED)', source.index('} view_state_t;')):
                   source.index('static view_state_t *state,')]
    production = '\n'.join(extract(n, source) for n in ('window_snapshot_copy', 'font_request_cancel', 'epd_reader_close'))
    fixture = (HERE / 'close_host.c').read_text().replace('/* TYPES */', types).replace('/* PRODUCTION */', production)
    path = build / 'close.c'
    path.write_text(fixture)
    cc, env = runpy.run_path(str(ROOT / 'tests/reader_prefetch/cache_run.py'))['compiler_environment'](None)
    exe = build / 'close.exe'
    command = [cc, '/nologo', '/std:c11', '/W4', '/WX', '/Od', '/Zi', '/fsanitize=address',
               '/DEPD_INPUT_TEST_ENABLED=1', str(path), f'/Fe{exe}']
    compiled = subprocess.run(command, cwd=build, env=env, capture_output=True)
    (build / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print((compiled.stdout + compiled.stderr).decode(errors='replace'))
    compiled.check_returncode()
    results = []
    for case in (('incoming',) if args.expect_negative else ('incoming', 'font')):
        result = subprocess.run([str(exe), case], cwd=build, env=env, capture_output=True, timeout=20)
        output = result.stdout + result.stderr
        (build / (case + '.log')).write_bytes(output)
        if args.expect_negative:
            assert result.returncode and b'heap-use-after-free' in output, output.decode(errors='replace')
            print('PASS old close counterexample: ASAN heap-use-after-free detected')
        else:
            print(output.decode(errors='replace'), end='')
            result.check_returncode()
        results.append({'case': case, 'exit': result.returncode, 'expected_uaf': args.expect_negative})
    (build / 'results.json').write_text(json.dumps({'command': command, 'cases': results}, indent=2) + '\n')


if __name__ == '__main__':
    main()
