"""Run production storage signatures/imports against instrumented DFS callbacks."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment


def main():
    compiler, env = compiler_environment(None)
    with tempfile.TemporaryDirectory(prefix='storage_io_') as output:
        build = Path(output)
        unicode_source = (ROOT / 'SiFli-SDK/rtos/rtthread/components/dfs/filesystems/elmfat/ffunicode.c').read_text(encoding='utf-8')
        start = unicode_source.index('DWORD ff_wtoupper (')
        end = unicode_source.index('#endif /* #if FF_USE_LFN */', start)
        (build / 'fat_upper_actual.inc').write_text(
            'typedef uint32_t DWORD; typedef uint16_t WORD;\n' + unicode_source[start:end], encoding='utf-8')
        for name in ('storage_watch', 'storage_file'):
            source = (ROOT / f'src/services/{name}.c').read_text(encoding='utf-8')
            source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
            (build / f'{name}_actual.inc').write_text(source, encoding='utf-8')
        storage = (ROOT / 'src/services/storage.c').read_text(encoding='utf-8')
        start = storage.index('rt_err_t storage_release_complete(')
        end = storage.index('\n}', start) + 2
        (build / 'storage_release_actual.inc').write_text(storage[start:end], encoding='utf-8')
        exe = build / 'storage_io.exe'
        command = [compiler, '/nologo', '/std:c11', '/utf-8', '/W3', '/O2',
                   '/D_CRT_SECURE_NO_WARNINGS', f'/I{build}',
                   str(Path(__file__).with_name('io_host.c')), f'/Fe{exe}']
        subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
        cases = sys.argv[1:] or subprocess.check_output([str(exe), '--list'], text=True).splitlines()
        failed = []
        for case in cases:
            result = subprocess.run([str(exe), case], env=env, timeout=20)
            if result.returncode:
                failed.append(case)
        print(f'Storage I/O: {len(cases)-len(failed)}/{len(cases)} passed', flush=True)
        if failed:
            print('Failed: ' + ', '.join(failed), flush=True)
            raise SystemExit(1)


if __name__ == '__main__':
    main()
