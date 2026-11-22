"""Run production service/pressure code with deterministic heap and clock inputs."""
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('service_tests', ROOT / 'tests/app_service_ids/run.py')
services = importlib.util.module_from_spec(spec)
spec.loader.exec_module(services)
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from view_run import extract


def without_includes(path):
    return re.sub(r'^#include[^\n]*\n', '', path.read_text(encoding='utf-8'), flags=re.M)


def main():
    cc, env = services.compiler.compiler_environment(None)
    with tempfile.TemporaryDirectory(prefix='memory-pressure-') as folder:
        build = Path(folder)
        services.owner_sources(build)
        (build / 'rtthread.h').write_text(services.RTTHREAD_HEADER, encoding='utf-8')
        (build / 'service.inc').write_text(without_includes(ROOT / 'src/services/app_service.c'), encoding='utf-8')
        (build / 'pressure.inc').write_text(without_includes(ROOT / 'src/platform/app_memory_pressure.c'), encoding='utf-8')
        memory = (ROOT / 'src/platform/epd_memory.c').read_text(encoding='utf-8')
        (build / 'memory_capacity.inc').write_text(extract(memory, 'epd_app_psram_capacity')[2], encoding='utf-8')
        host = (ROOT / 'tests/app_service_ids/host.c').read_text(encoding='utf-8')
        host = host.replace('return 1000000;', 'return available_bytes;')
        host = host.replace('static unsigned checks,', 'static size_t available_bytes = 1000000;\nstatic unsigned checks,')
        host = host.replace('static unsigned interrupt_depth;',
            'static unsigned interrupt_depth;\nstatic bool pressure_heap_locked;')
        host = host.replace('return (rt_base_t)interrupt_depth++;',
            'CHECK(!pressure_heap_locked); return (rt_base_t)interrupt_depth++;')
        host = host.replace('static void speech_owner_stop(epd_service_t *s) { (void)s; }',
            'static unsigned speech_stops, system_stops;\nstatic void speech_owner_stop(epd_service_t *s) { (void)s; ++speech_stops; }')
        host = host.replace('static void epd_system_service_stopped(epd_service_t *s) { (void)s; }',
            'static void epd_system_service_stopped(epd_service_t *s) { (void)s; ++system_stops; }')
        host = host.replace('!strcmp(app, "music")', '!strcmp(app, "music") || !strcmp(app, "weather")')
        (build / 'service_host.inc').write_text(host, encoding='utf-8')
        catalog = (ROOT / 'src/ui/platform/app_catalog.c').read_text(encoding='utf-8')
        (build / 'catalog.inc').write_text('\n'.join(extract(catalog, name)[2] for name in
            ['app_catalog_background_resume', 'app_catalog_process']), encoding='utf-8')
        load_globals = catalog[catalog.index('typedef struct\n'):catalog.index('static void *array_grow')]
        (build / 'load_estimate.inc').write_text(load_globals + '\n' + '\n'.join(
            extract(catalog, name)[2] for name in
            ['valid_id', 'remember_load_required', 'app_catalog_load_required']), encoding='utf-8')
        weather = (ROOT / 'modules/weather/weather.c').read_text(encoding='utf-8')
        (build / 'weather.inc').write_text('\n'.join(extract(weather, name)[2] for name in
            ['weather_request_job', 'schedule_next_refresh', 'worker_entry']), encoding='utf-8')
        for rate in (1000, 100):
            exe = build / f'pressure_{rate}.exe'
            subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W3', '/WX', '/TC', '/Od', '/RTC1',
                '/D_CRT_SECURE_NO_WARNINGS', f'/DRT_TICK_PER_SECOND={rate}', f'/I{build}',
                f'/I{ROOT / "src/services"}', f'/I{ROOT / "src/platform"}',
                f'/I{ROOT / "modules/weather"}',
                str(HERE / 'host.c'), f'/Fe{exe}'], cwd=build, env=env, check=True, timeout=60)
            names = subprocess.check_output([str(exe), '--list'], text=True, env=env, timeout=15).splitlines()
            for name in names:
                subprocess.run([str(exe), name], cwd=build, env=env, check=True, timeout=15)
    print(f'Memory pressure: {len(names) * 2} cases passed at 1000/100 ticks per second.')


if __name__ == '__main__':
    main()
