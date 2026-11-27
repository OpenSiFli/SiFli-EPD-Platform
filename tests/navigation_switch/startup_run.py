"""Check application page startup errors and retirement with the production hooks."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment
from view_run import extract


def main():
    cc, env = compiler_environment(None)
    context = (ROOT / 'src/ui/platform/app_memory_context.c').read_text(encoding='utf-8')
    memory = (ROOT / 'src/ui/platform/app_memory.c').read_text(encoding='utf-8')
    header = (ROOT / 'src/ui/platform/app_memory.h').read_text(encoding='utf-8')
    switch = (ROOT / 'src/ui/ui_navigation_switch.c').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='navigation-startup-') as folder:
        build = Path(folder)
        (build / 'startup_context.inc').write_text('\n'.join([
            header[header.index('typedef struct app_memory_load_watch'):header.rindex('#endif')],
            memory[memory.index('static app_memory_load_watch_t *load_watch;'):
                   memory.index('#ifdef TINY_TTF_CACHE_IN_APP_PSRAM')],
            context[context.index('#define PAGE_CONTEXT_COUNT'):context.index('void epd_app_page_start_failed')],
            *(extract(context, name)[2] for name in ('epd_app_page_start_failed', 'gui_app_context_enter')),
        ]), encoding='utf-8')
        marker = '} startup_retirement_t;'
        end = switch.index(marker) + len(marker)
        start = switch.rindex('typedef struct', 0, end)
        (build / 'startup_retirement.inc').write_text('\n'.join([
            switch[start:end],
            'static void startup_retirement_end(startup_retirement_t *retirement, bool replaced);',
            *(extract(switch, name)[2] for name in ('startup_retirement_arm', 'startup_retirement_matches',
                                                  'startup_retirement_process')),
        ]), encoding='utf-8')
        exe = build / 'startup.exe'
        subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/RTC1',
                        f'/I{build}', str(HERE / 'startup.c'), f'/Fe{exe}'],
                       cwd=build, env=env, check=True, timeout=60)
        subprocess.run([str(exe)], cwd=build, env=env, check=True, timeout=15)


if __name__ == '__main__':
    main()
