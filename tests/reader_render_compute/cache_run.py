"""Run real dispatch/render with independent HCPU/RAM/ACPU cache images."""
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
    cc,env=compiler()
    render=ROOT/'components/compute_reader_render'
    pcm=ROOT/'components/compute_pcm'
    with tempfile.TemporaryDirectory(prefix='cache-',dir=HERE) as tmp:
        build=Path(tmp); exe=build/'cache.exe'
        protocol=build/'protocol.c'
        protocol.write_text('#define epd_compute_signals real_compute_signals\n#include "'+(ROOT/'components/compute/epd_compute_protocol.c').as_posix()+'"\n')
        formats=build/'format.c'
        formats.write_text('#define rr_sync_windows real_rr_sync_windows\n#include "'+(render/'render_format.c').as_posix()+'"\n')
        includes=[HERE / 'admission', ROOT/'tests/compute_scheduler',ROOT/'src/platform',ROOT/'src/services',ROOT/'components/compute/include',render/'include',pcm/'include']
        units=[HERE/'cache_host.c',protocol,formats,ROOT/'components/compute/epd_compute_remote.c',
               *[render/(n+'.c') for n in ('render_client','render_kernel','render_remote')],
               *[pcm/(n+'.c') for n in ('pcm_client','pcm_convert','pcm_remote')]]
        args=[cc,'/nologo','/std:c11','/TC','/utf-8','/W4','/WX','/wd4334','/wd4200','/Od','/Zi','/fsanitize=address',
              '/D_CRT_SECURE_NO_WARNINGS',*[f'/I{p}' for p in includes],*map(str,units),f'/Fe{exe}']
        with (HERE/'last-cache.stdout.txt').open('w',encoding='utf-8') as out, (HERE/'last-cache.stderr.txt').open('w',encoding='utf-8') as err:
            for cmd in (args,[str(exe)]):
                p=subprocess.run(cmd,cwd=build,env=env,capture_output=True,timeout=90)
                stdout,stderr=[x.decode('utf-8',errors='replace') for x in (p.stdout,p.stderr)]
                out.write(stdout); err.write(stderr)
                if p.returncode:
                    err.write(stdout); print(stdout+stderr); return p.returncode
                for line in stdout.splitlines():
                    if line.startswith(('PASS','BYTES')): print(line)
    return 0
if __name__=='__main__': raise SystemExit(main())
