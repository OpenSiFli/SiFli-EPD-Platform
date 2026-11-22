"""MSVC fault regression for all audio compute OPEN ownership adapters."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests/reader_render_compute'))
from run import compiler

def main():
    cc, env = compiler('cl')
    components = [ROOT / 'components' / ('compute_' + name) for name in ('mp3', 'flac', 'pcm', 'anyka', 'opus')]
    includes = [HERE, ROOT / 'src/platform', ROOT / 'components/compute/include',
                ROOT / 'SiFli-SDK/external/libhelix-mp3/pub', *(p / 'include' for p in components)]
    units = [HERE / 'host.c', *[p / (p.name.removeprefix('compute_') + '_client.c') for p in components],
             ROOT / 'components/compute_pcm/pcm_convert.c']
    with tempfile.TemporaryDirectory(prefix='build-', dir=HERE) as folder, \
         (HERE / 'last-run.stdout.txt').open('w', encoding='utf-8') as out, \
         (HERE / 'last-run.stderr.txt').open('w', encoding='utf-8') as err:
        exe = Path(folder) / 'cleanup.exe'
        command = [cc, '/nologo', '/std:c11', '/TC', '/utf-8', '/W4', '/WX', '/wd4267', '/wd4334', '/Od',
                   '/Zi', '/fsanitize=address', '/DBUILD_DLMODULE', '/D_CRT_SECURE_NO_WARNINGS',
                   *[f'/I{p}' for p in includes], *map(str, units), f'/Fe{exe}']
        jpeg = Path(folder) / 'jpeg.exe'
        jpeg_command = command[:command.index(str(HERE / 'host.c'))] + [
            f'/I{ROOT / "modules/common/image/compute/include"}', str(HERE / 'jpeg.c'),
            str(ROOT / 'modules/common/image/compute/jpeg_client.c'), f'/Fe{jpeg}']
        for args in (command, [str(exe)], jpeg_command, [str(jpeg)]):
            p = subprocess.run(args, cwd=folder, env=env, capture_output=True, timeout=60)
            stdout, stderr = (x.decode('utf-8', errors='replace') for x in (p.stdout, p.stderr))
            out.write(stdout); err.write(stderr)
            if p.returncode:
                err.write(stdout)
                print(stdout + stderr, file=sys.stderr)
                return p.returncode
            for line in stdout.splitlines():
                if line.startswith('PASS'): print(line)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
