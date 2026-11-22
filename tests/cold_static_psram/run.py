"""Check retained cold tables using production startup, access and wake functions."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment
from view_run import extract


def functions(source, names):
    return '\n'.join(extract(source, name)[2] for name in names.split())


def main():
    cc, env = compiler_environment(None)
    portal = (ROOT / 'src/services/portal/portal_package.c').read_text(encoding='utf-8')
    storage = (ROOT / 'src/services/storage_file.c').read_text(encoding='utf-8')
    catalog = (ROOT / 'src/ui/platform/app_catalog.c').read_text(encoding='utf-8')
    pm = (ROOT / 'src/boards/board_pm.c').read_text(encoding='utf-8')
    # A boot initializer must not acquire a runtime call site in these units.
    for source, name in [(portal, 'portal_package_history_init'),
                         (storage, 'storage_owned_files_init'),
                         (catalog, 'app_load_estimates_init')]:
        assert len(re.findall(r'\b' + name + r'\b', source)) == 2, name
        assert 'INIT_COMPONENT_EXPORT(' + name + ');' in source, name
    components = (ROOT / 'SiFli-SDK/rtos/rtthread/src/components.c').read_text(encoding='utf-8')
    entry = extract(components, 'main_thread_entry')[2]
    assert entry.index('rt_components_init();') < entry.index('main();')
    assert 'rt_components_init' not in pm
    with tempfile.TemporaryDirectory(prefix='cold-static-psram-') as folder:
        build = Path(folder)
        (build / 'portal.inc').write_text(
            portal[portal.index('typedef enum { EMPTY'):portal.index('bool portal_package_init(')] +
            functions(portal, 'portal_package_init valid_id active available fail portal_package_begin'),
            encoding='utf-8')
        (build / 'storage.inc').write_text(
            storage[storage.index('/* Reserve ownership'):storage.index('static bool managed_path(')],
            encoding='utf-8')
        (build / 'catalog.inc').write_text(
            catalog[catalog.index('typedef struct\n'):catalog.index('static void *array_grow')] +
            functions(catalog, 'valid_id remember_load_required app_catalog_load_required app_catalog_background_resume'),
            encoding='utf-8')
        (build / 'wake.inc').write_text(functions(pm, 'capture_key_wakeup board_pm_notify'), encoding='utf-8')
        exe = build / 'cold_static.exe'
        subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W3', '/WX', '/Od', '/RTC1',
            '/D_CRT_SECURE_NO_WARNINGS', f'/I{HERE}', f'/I{build}',
            str(HERE / 'host.c'), f'/Fe{exe}'], cwd=build, env=env, check=True, timeout=60)
        cases = ['boot', 'first_use', 'sleep_resume', 'capacity', 'reboot', 'revision_after_wake']
        for name in cases:
            subprocess.run([str(exe), name], cwd=build, env=env, check=True, timeout=15)
    print(f'Cold static PSRAM: {len(cases)} behavior cases plus startup wiring checks passed.')


if __name__ == '__main__':
    main()
