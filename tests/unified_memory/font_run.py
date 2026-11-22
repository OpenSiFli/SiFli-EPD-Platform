"""Run the production font admission helper against the real core/SDK heaps."""
from pathlib import Path
import os, subprocess, sys
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
if os.name == 'nt':
    script = '/mnt/' + HERE.drive[0].lower() + HERE.as_posix()[2:] + '/font_run.py'
    raise SystemExit(subprocess.call(['wsl.exe','-d','Ubuntu','--exec','python3',script]))
SDK = ROOT / 'SiFli-SDK/rtos/rtthread'
build = HERE / '.build'
build.mkdir(exist_ok=True)
for rate in (1000,100):
    exe = build / f'font-recovery-{rate}'
    sources = [HERE/'font_recovery.c', ROOT/'src/platform/epd_memory_manager.c',
               ROOT/'src/platform/epd_memory_sdk.c', ROOT/'src/ui/platform/app_memory.c',
               *(SDK/'src'/name for name in ('memheap.c','thread.c','idle.c'))]
    subprocess.run(['gcc','-std=c11','-O1','-g','-ffunction-sections','-fdata-sections',
        '-fno-pie','-no-pie','-fsanitize=address,undefined','-fno-sanitize=alignment',
        '-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast','-DEPD_MEMORY_HOST',
        '-DEPD_MEMORY_DIAGNOSTIC_HOST',f'-DRT_TICK_PER_SECOND={rate}',
        *(f'-I{p}' for p in (HERE,SDK/'include',ROOT/'src/platform',ROOT/'src/ui/platform')),
        *map(str,sources),'-Wl,--gc-sections','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
