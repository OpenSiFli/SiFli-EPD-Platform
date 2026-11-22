# Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0
"""Check exact-fit allocation with diagnostic hooks and owner hooks on/off."""
from pathlib import Path
import argparse
import os
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SDK = ROOT / 'SiFli-SDK/rtos/rtthread'


def main():
    if os.name == 'nt':
        script = '/mnt/' + HERE.drive[0].lower() + HERE.as_posix()[2:] + '/exact_fit_run.py'
        raise SystemExit(subprocess.call(['wsl.exe', '-d', 'Ubuntu', '--exec', 'python3', script, *sys.argv[1:]]))
    parser = argparse.ArgumentParser()
    parser.add_argument('--case')
    parser.add_argument('--hook', type=int, choices=(0, 1))
    parser.add_argument('--owner', type=int, choices=(0, 1))
    args = parser.parse_args()
    build = HERE / '.build'
    build.mkdir(exist_ok=True)
    includes = [HERE, SDK / 'include', ROOT / 'src/platform', ROOT / 'src/ui/platform',
                ROOT / 'SiFli-SDK/middleware/app_fwk']
    total = 0
    for rate in (1000, 100):
        for owner in ((False, True) if args.owner is None else (bool(args.owner),)):
            for hooks in ((False, True) if args.hook is None else (bool(args.hook),)):
                defs = ['-DEPD_MEMORY_HOST', '-DEPD_MEMORY_DIAGNOSTIC_HOST', f'-DRT_TICK_PER_SECOND={rate}']
                if not owner:
                    defs += ['-DEPD_MEMORY_TEST_OWNER_DISABLED']
                if not hooks:
                    defs += ['-DEPD_MEMORY_TEST_HOOK_DISABLED']
                sources = [HERE / 'exact_fit.c', SDK / 'src/memheap.c']
                if owner:
                    defs += ['-DRT_USING_MUTEX']
                    sources += [ROOT / 'src/platform/epd_memory_manager.c', ROOT / 'src/platform/epd_memory_sdk.c',
                                ROOT / 'src/platform/epd_resource_runtime.c',
                                ROOT / 'src/ui/platform/app_memory.c', SDK / 'src/thread.c', SDK / 'src/idle.c']
                output = build / f'exact-fit-{rate}-owner{int(owner)}-hook{int(hooks)}'
                subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
                                '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize=alignment',
                                '-fno-omit-frame-pointer', '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast',
                                *defs, *['-I' + str(p) for p in includes], *map(str, sources),
                                '-Wl,--gc-sections', '-o', str(output)], check=True)
                cases = ['whole', 'unsplit', 'fragmented']
                if owner:
                    cases += ['credit_enough', 'credit_short', 'credit_unsplit_enough', 'credit_unsplit_short']
                if args.case:
                    if args.case not in cases:
                        parser.error('case is not available in this owner configuration')
                    cases = [args.case]
                print(f'CONFIG exact_fit ticks={rate} owner={int(owner)} hook={int(hooks)}', flush=True)
                for name in cases:
                    subprocess.run([str(output), name], check=True, cwd=build,
                                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'})
                    total += 1
    print(f'PASS exact-fit matrix: {total} executions', flush=True)


if __name__ == '__main__':
    main()
