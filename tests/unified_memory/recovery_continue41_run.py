"""Recovery continuation against the real manager, SDK bridge and RT memheap."""
from pathlib import Path
import argparse
import os
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

def main():
    if os.name == 'nt':
        script = '/mnt/' + HERE.drive[0].lower() + HERE.as_posix()[2:] + '/recovery_continue41_run.py'
        raise SystemExit(subprocess.call(['wsl.exe', '-d', 'Ubuntu', '--exec', 'python3', script, *sys.argv[1:]]))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve(); build.mkdir(parents=True, exist_ok=True)
    sdk = ROOT / 'SiFli-SDK/rtos/rtthread'
    sources = [HERE / 'recovery_continue41.c', *(ROOT / 'src/platform' / name for name in
        ('epd_memory_manager.c', 'epd_memory_sdk.c', 'epd_resource_runtime.c')),
        ROOT / 'src/ui/platform/app_memory.c', *(sdk / 'src' / n for n in ('memheap.c', 'thread.c', 'idle.c'))]
    includes = [HERE, sdk / 'include', ROOT / 'src/platform', ROOT / 'src/ui/platform']
    for rate in (1000, 100):
        exe = build / f'recovery-{rate}'
        command = ['gcc', '-std=c11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
            '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize=alignment',
            '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast', '-DEPD_MEMORY_HOST',
            '-DEPD_MEMORY_DIAGNOSTIC_HOST', '-DRT_USING_MUTEX', f'-DRT_TICK_PER_SECOND={rate}',
            *(f'-I{p}' for p in includes), *map(str, sources), '-Wl,--gc-sections', '-o', str(exe)]
        result = subprocess.run(command, capture_output=True)
        (build / f'compile-{rate}.log').write_bytes(result.stdout + result.stderr)
        if result.returncode: raise SystemExit((result.stdout + result.stderr).decode(errors='replace'))
        existing = [n for n in subprocess.check_output([str(exe), '--list'], text=True).splitlines() if n.startswith('recovery_')]
        for case in ['replace', 'deadline', 'cancel', 'closed', 'generation', 'budget', *existing]:
            result = subprocess.run([str(exe), case], capture_output=True,
                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'}, timeout=30)
            (build / f'{case}-{rate}.log').write_bytes(result.stdout + result.stderr)
            print((result.stdout + result.stderr).decode(errors='replace'), end='', flush=True)
            if result.returncode: raise SystemExit(result.returncode)

if __name__ == '__main__': main()
