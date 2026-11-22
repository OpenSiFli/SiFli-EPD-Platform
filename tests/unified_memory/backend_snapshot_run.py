"""Check backend snapshot notification semantics with the real manager and SDK heap."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent


def linux_path(value):
    p = Path(value).resolve()
    return '/mnt/' + p.drive[0].lower() + p.as_posix()[2:]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--manager-source', type=Path)
    parser.add_argument('--baseline-manager', type=Path)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--skip-regression', action='store_true')
    args = parser.parse_args()
    if os.name == 'nt':
        command = ['wsl.exe', '-d', 'Ubuntu', '--exec', 'python3', linux_path(__file__),
                   '--source-root', linux_path(args.source_root), '--build-dir', linux_path(args.build_dir)]
        for name in ('manager_source', 'baseline_manager'):
            if getattr(args, name):
                command += ['--' + name.replace('_', '-'), linux_path(getattr(args, name))]
        if args.skip_regression:
            command.append('--skip-regression')
        raise SystemExit(subprocess.call(command))
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    root = args.source_root.resolve(); build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    manager = (args.manager_source or root / 'src/platform/epd_memory_manager.c').resolve()
    tests = root / 'tests/unified_memory'; sdk = root / 'SiFli-SDK/rtos/rtthread'
    compute = root / 'src/platform/epd_compute.c'
    code = compute.read_text(encoding='utf8')
    start = code.index('static const uint8_t remote_heap_keys[2]')
    end = code.index('static void session_class(', start)
    extracted = build / 'remote_observe.inc'
    extracted.write_text(code[start:end], encoding='utf8')
    inputs = [manager, compute, HERE / 'backend_snapshot.c', HERE / 'backend_snapshot_run.py',
              tests / 'host.c', root / 'src/platform/epd_memory_manager.h',
              root / 'src/platform/epd_memory_sdk.c', root / 'src/platform/epd_resource_runtime.c',
              root / 'src/ui/platform/app_memory.c',
              *(sdk / 'src' / name for name in ('memheap.c', 'thread.c', 'idle.c'))]
    before_hashes = {str(p): digest(p) for p in inputs}
    includes = [build, tests, sdk / 'include', root / 'src/platform', root / 'src/ui/platform',
                root / 'components/compute/include']
    common = ['gcc', '-std=c11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
              '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize=alignment',
              '-fno-omit-frame-pointer', '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast',
              '-Werror=implicit-function-declaration', '-Werror=return-type',
              '-DEPD_MEMORY_HOST', '-DEPD_MEMORY_DIAGNOSTIC_HOST', '-DRT_USING_MUTEX',
              *(f'-I{p}' for p in includes)]
    sources = [HERE / 'backend_snapshot.c', root / 'src/platform/epd_memory_sdk.c',
               root / 'src/platform/epd_resource_runtime.c', root / 'src/ui/platform/app_memory.c',
               *(sdk / 'src' / name for name in ('memheap.c', 'thread.c', 'idle.c'))]
    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    report = {'inputs': before_hashes, 'extracted_observer_sha256': digest(extracted),
              'negative': [], 'runs': []}

    def compile_executable(path, source, rate, extra=()):
        command = [*common, f'-DRT_TICK_PER_SECOND={rate}', *extra, *map(str, sources),
                   str(source), '-Wl,--gc-sections', '-o', str(path)]
        result = subprocess.run(command, cwd=build, capture_output=True)
        (build / f'{path.name}-compile.log').write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError((result.stdout + result.stderr).decode(errors='replace'))

    def run_case(exe, case, expected_failure=False):
        result = subprocess.run([str(exe), case], cwd=build, env=env,
                                capture_output=True, timeout=60)
        output = result.stdout + result.stderr
        (build / f'{exe.name}-{case}.log').write_bytes(output)
        if expected_failure:
            if not result.returncode or b'Assertion' not in output:
                raise RuntimeError(f'Expected assertion: {case}: {output.decode(errors="replace")}')
        elif result.returncode:
            raise RuntimeError(f'{case}: {output.decode(errors="replace")}')
        return result.returncode

    if args.baseline_manager:
        baseline = args.baseline_manager.resolve(); report['baseline_sha256'] = digest(baseline)
        exe = build / 'baseline-1000'; compile_executable(exe, baseline, 1000)
        for case in ('snapshot_same', 'snapshot_pending_notify', 'snapshot_heap_ledger'):
            status = run_case(exe, case, True)
            report['negative'].append({'case': case, 'exit': status})
            print(f'EXPECTED FAIL unchanged production: {case}', flush=True)
    for rate in (1000, 100):
        exe = build / f'candidate-{rate}'; compile_executable(exe, manager, rate)
        new_cases = subprocess.check_output([str(exe), '--snapshots'], text=True).splitlines()
        existing = [] if args.skip_regression else subprocess.check_output([str(exe), '--list'], text=True).splitlines()
        for case in new_cases + existing:
            run_case(exe, case)
        report['runs'].append({'tick_rate': rate, 'snapshot_cases': new_cases, 'existing_cases': existing})
        print(f'PASS {len(new_cases)} snapshot + {len(existing)} existing cases: {rate}Hz ASAN/UBSAN', flush=True)
    if not args.skip_regression:
        exe = build / 'candidate-generations'
        compile_executable(exe, manager, 1000, ('-DEPD_MEMORY_GENERATION_MAX=3',
            '-DEPD_MEMORY_DESCRIPTOR_COUNT=8', '-DEPD_MEMORY_TEST_GENERATIONS'))
        run_case(exe, 'generation_retire')
        report['generation_retire'] = True
        print('PASS generation_retire ASAN/UBSAN', flush=True)
    after_hashes = {str(p): digest(p) for p in inputs}
    report['inputs_unchanged'] = before_hashes == after_hashes
    if not report['inputs_unchanged']:
        raise RuntimeError('Source input changed during validation; rerun against a stable snapshot')
    report['passed'] = True
    (build / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')


if __name__ == '__main__':
    main()
