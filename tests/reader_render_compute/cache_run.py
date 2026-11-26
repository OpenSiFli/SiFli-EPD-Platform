"""Run real dispatch/render with independent HCPU/RAM/ACPU cache images."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
sys.dont_write_bytecode=True
sys.path.insert(0,str(ROOT/'tests/compute_scheduler'))
from coexist_run import compiler

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log-dir', type=Path, default=HERE)
    logs=parser.parse_args().log_dir.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    cc,env=compiler()
    render=ROOT/'components/compute_reader_render'
    pcm=ROOT/'components/compute_pcm'
    with tempfile.TemporaryDirectory(prefix='cache-',dir=HERE) as tmp:
        build=Path(tmp); exe=build/'cache.exe'
        protocol=build/'protocol.c'
        protocol.write_text('#define epd_compute_signals real_compute_signals\n#include "'+(ROOT/'components/compute/epd_compute_protocol.c').as_posix()+'"\n')
        formats=build/'format.c'
        formats.write_text('#define rr_sync_windows real_rr_sync_windows\n#include "'+(render/'render_format.c').as_posix()+'"\n')
        # Board address checks use the guarded host ranges from compute_scheduler/host.c.
        (build/'rthw.h').write_text('#pragma once\n#include <rtthread.h>\ntypedef int rt_err_t;\n')
        (build/'board.h').write_text('#include <stdint.h>\nextern uintptr_t test_sram_start, test_sram_size;\n'
            '#define HCPU_RAM_DATA_START_ADDR test_sram_start\n#define HCPU_RAM_DATA_SIZE test_sram_size\n')
        (build/'acpu_ctrl.h').write_text('#define ACPU_ERR_OK 0\n')
        # This suite models cache ownership, not allocator admission or heap overhead.
        admission=build/'admission.c'
        admission.write_text('#include <assert.h>\n#include "epd_resource_runtime.h"\n'
            'size_t epd_app_allocation_peak(size_t n) { return n; }\n'
            'int epd_resource_context_reserve(epd_memory_region_t region, size_t bytes) {\n'
            '    assert(bytes && (region == EPD_MEMORY_REGION_SRAM || region == EPD_MEMORY_REGION_HCPU_PSRAM));\n'
            '    return EPD_RESOURCE_OK;\n}\n')
        includes=[build, ROOT/'tests/compute_scheduler',ROOT/'src/ui',ROOT/'src/platform',ROOT/'src/services',ROOT/'components/compute/include',render/'include',pcm/'include']
        units=[HERE/'cache_host.c',admission,protocol,formats,ROOT/'components/compute/epd_compute_remote.c',
               *[render/(n+'.c') for n in ('render_client','render_kernel','render_remote')],
               *[pcm/(n+'.c') for n in ('pcm_client','pcm_convert','pcm_remote')]]
        args=[cc,'/nologo','/std:c11','/TC','/utf-8','/W4','/WX','/wd4334','/wd4200','/Od','/Zi','/fsanitize=address',
              '/D_CRT_SECURE_NO_WARNINGS',*[f'/I{p}' for p in includes],*map(str,units),f'/Fe{exe}']
        with (logs/'last-cache.stdout.txt').open('w',encoding='utf-8') as out, (logs/'last-cache.stderr.txt').open('w',encoding='utf-8') as err:
            for cmd in (args,[str(exe)]):
                out.write('COMMAND '+subprocess.list2cmdline(cmd)+'\n')
                p=subprocess.run(cmd,cwd=build,env=env,capture_output=True,timeout=90)
                stdout,stderr=[x.decode('utf-8',errors='replace') for x in (p.stdout,p.stderr)]
                out.write(stdout); err.write(stderr)
                if p.returncode:
                    err.write(stdout); print(stdout+stderr); return p.returncode
                for line in stdout.splitlines():
                    if line.startswith(('PASS','BYTES')): print(line)
    return 0
if __name__=='__main__': raise SystemExit(main())
