"""Test actual scope/activity eligibility and the now-private grant counterexample."""
from pathlib import Path
import argparse
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    (out / 'boundary_ports.h').write_text(
        '#include <rtthread.h>\n'
        '#define INIT_COMPONENT_EXPORT(f) int test_init_##f(void) { return f(); }\n'
        'int rt_event_send(struct rt_event *, unsigned);\n')
    cc, env = compiler_environment('cl')
    reader = ROOT / 'modules/common/reader'
    includes = [ROOT / 'tests/unified_resource/stubs', ROOT / 'tests/unified_identity/stubs', ROOT / 'src/platform', reader, reader / 'compute/include']
    sources = [ROOT / 'tests/unified_memory/public_api_host.c', reader / 'reader_page_cache.c',
               reader / 'reader_page_resource.c'] + [ROOT / 'src/platform' / name for name in
               ('epd_resource.c', 'epd_resource_runtime.c', 'epd_memory_manager.c', 'epd_operation.c')]
    count = 0
    for rate in (1000, 100):
        exe = out / f'boundary-{rate}.exe'
        subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4200', '/TC',
                        '/Od', '/Zi', '/fsanitize=address', '/DEPD_MEMORY_HOST', f'/DRT_TICK_PER_SECOND={rate}',
                        f'/FI{out / "boundary_ports.h"}',
                        *(f'/I{p}' for p in includes), *map(str, sources), f'/Fe{exe}'],
                       cwd=out, env=env, check=True, timeout=60)
        for app_id in ('memdemo', 'mx731'):
            for case in ('scope', 'activity', 'normal', 'private', 'dispatch'):
                subprocess.run([str(exe), app_id, case], cwd=out, env=env, check=True, timeout=20)
                count += 1
    print(f'Public boundary: {count}/{count} ASAN cases passed.')


if __name__ == '__main__':
    main()
