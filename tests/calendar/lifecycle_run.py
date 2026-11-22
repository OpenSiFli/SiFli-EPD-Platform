"""Run calendar runtime ownership and independent reminder persistence on the host."""
from pathlib import Path
import subprocess
import sys
import tempfile
import re

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment
from view_run import extract


def main():
    cc, env = compiler_environment(None)
    with tempfile.TemporaryDirectory(prefix='calendar-lifecycle-') as folder:
        build = Path(folder)
        memory = (ROOT / 'src/platform/epd_memory_manager.c').read_text(encoding='utf-8')
        (build / 'memory_core.inc').write_text(re.sub(r'^#include[^\n]*\n', '', memory, flags=re.M), encoding='utf-8')
        reminder = (ROOT / 'src/services/reminder.c').read_text(encoding='utf-8')
        old_reminder = reminder.replace('epd_memory_owner_enter(EPD_MEMORY_OWNER_SYSTEM)',
                                        'epd_memory_owner_enter(epd_memory_owner_current())')
        assert old_reminder != reminder
        (build / 'memory_retry.inc').write_text(re.sub(r'^#include[^\n]*\n', '', (ROOT / 'modules/common/memory_retry.h').read_text(encoding='utf-8'), flags=re.M), encoding='utf-8')
        ui = (ROOT / 'modules/calendr/calendar_ui.c').read_text(encoding='utf-8')
        (build / 'entry.inc').write_text('\n'.join(extract(ui, name)[2] for name in
            ('field_apply', 'field_close', 'cal_ui_flush', 'message', 'app_main')), encoding='utf-8')
        common = [HERE / 'host_fs.c', ROOT / 'src/platform/epd_calendar.c']
        units = [
            ('owner_before', [HERE / 'owner_test.c', *common]),
            ('owner', [HERE / 'owner_test.c', *common]),
            ('runtime', [HERE / 'runtime_test.c', *common,
                         ROOT / 'modules/calendr/calendar_model.c',
                         ROOT / 'modules/calendr/calendar_store.c',
                         ROOT / 'modules/calendr/calendar_exchange.c']),
            ('reminder', [HERE / 'reminder_test.c', *common]),
            ('entry', [HERE / 'entry_test.c', ROOT / 'modules/calendr/calendar_model.c',
                       ROOT / 'src/platform/epd_calendar.c']),
        ]
        includes = [build, HERE / 'reminder_host', HERE / 'host', ROOT / 'modules/calendr',
                    ROOT / 'src/platform', ROOT / 'src/services', ROOT / 'src/ui']
        for name, sources in units:
            (build / 'reminder_actual.inc').write_text(old_reminder if name == 'owner_before' else reminder, encoding='utf-8')
            exe = build / (name + '.exe')
            args = [cc, '/nologo', '/std:c11', '/utf-8', '/W3', '/Od', '/RTC1',
                    '/D_CRT_SECURE_NO_WARNINGS', '/DCAL_HOST', '/DEPD_CALENDAR_HOST',
                    *(f'/I{p}' for p in includes), *map(str, sources), f'/Fe{exe}']
            if name == 'owner_before':
                args.append('/DREMINDER_REPRODUCE_RETENTION')
            subprocess.run(args, cwd=build, env=env, check=True, timeout=60)
            subprocess.run([str(exe)], cwd=build, env=env, check=True, timeout=15)


if __name__ == '__main__':
    main()
