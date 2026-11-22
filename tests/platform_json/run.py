"""Exercise production PSRAM JSON with real heaps and concurrent thread errno."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SDK = ROOT / 'SiFli-SDK/rtos/rtthread'
CJSON = ROOT / 'SiFli-SDK/external/cJSON-1.7.17'


def main():
    if os.name == 'nt':
        script = '/mnt/' + HERE.drive[0].lower() + HERE.as_posix()[2:] + '/run.py'
        raise SystemExit(subprocess.call(['wsl.exe', '-d', 'Ubuntu', '--exec', 'python3', script]))
    includes = [ROOT / 'tests/unified_memory', SDK / 'include', ROOT / 'src/platform',
                ROOT / 'src/ui/platform', CJSON]
    common = ['gcc', '-std=c11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
              '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize=alignment',
              '-fno-omit-frame-pointer', '-DEPD_MEMORY_HOST', '-DEPD_MEMORY_DIAGNOSTIC_HOST',
              '-DEPD_JSON_REAL', *['-I' + str(p) for p in includes]]
    json_sources = [ROOT / 'src/platform/epd_json.c', CJSON / 'cJSON.c']
    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'}
    with tempfile.TemporaryDirectory(prefix='platform-json-') as directory:
        build = Path(directory)
        for rate in (1000, 100):
            exe = build / f'core-{rate}'
            sources = [HERE / 'core.c', *json_sources, ROOT / 'src/platform/epd_memory_manager.c',
                       ROOT / 'src/platform/epd_memory_sdk.c', ROOT / 'src/ui/platform/app_memory.c',
                       *(SDK / 'src' / n for n in ('memheap.c', 'thread.c', 'idle.c'))]
            subprocess.run([*common, '-D__RTTHREAD__', f'-DRT_TICK_PER_SECOND={rate}',
                            '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast',
                            *map(str, sources), '-Wl,--gc-sections', '-lm', '-o', str(exe)], check=True)
            for case in ('sram_reserved', 'psram_denied', 'psram_exhausted', 'closed_owner', 'backend_init'):
                subprocess.run([str(exe), case], env=env, check=True, timeout=20)
        for control in (False, True):
            exe = build / ('threads-control' if control else 'threads')
            subprocess.run([*common, '-Wall', '-Wextra', '-Werror',
                            *(['-DJSON_SHARED_ERRNO_CONTROL'] if control else []),
                            str(HERE / 'threads.c'), *map(str, json_sources), '-pthread',
                            '-Wl,--gc-sections', '-lm', '-o', str(exe)], check=True)
            run = subprocess.run([str(exe), *(['--interleave'] if control else [])], env=env,
                                 text=True, capture_output=control, timeout=20)
            if control:
                assert run.returncode != 0 and 'EPD_JSON_NO_MEMORY' in run.stderr, run.stdout + run.stderr
                print('PASS negative control: shared errno loses the failing thread OOM', flush=True)
            else:
                run.check_returncode()
    print('PASS platform JSON: 10 real-heap cases, bounded/Nth/concurrency cases, negative control', flush=True)


if __name__ == '__main__':
    main()
