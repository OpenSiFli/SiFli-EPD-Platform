"""Exercise the production navigation state machine and scoped save barrier."""
from pathlib import Path
import argparse
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment
from view_run import extract


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unit', choices=('switch', 'boundaries', 'entries'), action='append')
    parser.add_argument('--asan', action='store_true')
    parser.add_argument('--control', choices=('skip_recovery', 'skip_retirement'))
    options = parser.parse_args()
    cc, env = compiler_environment(None)
    with tempfile.TemporaryDirectory(prefix='navigation-switch-') as directory:
        build = Path(directory)
        source = (ROOT / 'src/ui/ui_navigation_switch.c').read_text(encoding='utf-8')
        if options.control:
            old = ('(load.busy || load.contiguous)' if options.control == 'skip_recovery' else
                   'if (nav.load_retry == NAV_RETRY_BLOCK && app_modules_retiring()) return false;')
            assert source.count(old) == 1
            source = source.replace(old, ('false' if options.control == 'skip_recovery' else
                                         '(void)app_modules_retiring();'))
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        (build / 'switch.inc').write_text(source, encoding='utf-8')
        memory = (ROOT / 'src/ui/platform/app_memory.c').read_text(encoding='utf-8')
        header = (ROOT / 'src/ui/platform/app_memory.h').read_text(encoding='utf-8')
        (build / 'load_probe.inc').write_text(
            header[header.index('typedef struct app_memory_load_watch'):header.rindex('#endif')] +
            memory[memory.index('static app_memory_load_watch_t *load_watch;'):memory.index('L2_RET_BSS_SECT_BEGIN')],
            encoding='utf-8')
        catalog = (ROOT / 'src/ui/platform/app_catalog.c').read_text(encoding='utf-8')
        (build / 'builtin_registry.inc').write_text(
            '\n'.join(extract(catalog, name)[2] for name in ('builtin_id', 'app_catalog_is_builtin')),
            encoding='utf-8')
        cover = (ROOT / 'modules/books/cover_cache.c').read_text(encoding='utf-8')
        declarations = re.sub(r'^#include[^\n]*\n', '', cover[:cover.index('static void *cover_allocate(')], flags=re.M)
        (build / 'cover.inc').write_text(declarations + '\n' + '\n'.join(
            extract(cover, name)[2] for name in (
                'cover_resource_publish', 'cover_pixels_release', 'cover_free',
                'books_cover_cache_cancel', 'books_cover_cache_busy', 'cover_cancelled', 'cover_cleanup',
            )), encoding='utf-8')
        (build / 'books.inc').write_text('\n'.join(
            extract((ROOT / path).read_text(encoding='utf-8'), name)[2]
            for path, name in (
                ('modules/common/reader/reader_view.c', 'epd_reader_save'),
                ('modules/books/document/document_view.c', 'document_view_save'),
                ('modules/books/ui_reader.c', 'ui_reader_view'),
                ('modules/books/ui_reader.c', 'ui_reader_save'),
                ('modules/books/ui_reader.c', 'entry_pixels_release'),
                ('modules/books/reader_entry_store.inc', 'entry_job_free'),
                ('modules/books/reader_entry_store.inc', 'entry_store_cancel_queue'),
                ('modules/books/reader_entry_store.inc', 'ui_reader_cache_quiesce'),
                ('modules/books/reader_entry_store.inc', 'ui_reader_cache_busy'),
                ('modules/books/reader_entry_store.inc', 'entry_read_current'),
                ('modules/books/reader_entry_store.inc', 'entry_job_cancelled'),
                ('modules/books/reader_entry_store.inc', 'entry_job_cleanup'),
                ('modules/books/app_main.c', 'books_flush'),
            )), encoding='utf-8')
        for output, path, names in (
            ('service.inc', 'src/services/app_service.c', ['service_page_task_retained', 'service_flush', 'app_service_flush_app']),
            ('draft.inc', 'modules/ai_demo/src/app_main.c', ['save_input', 'demo_ui_flush']),
        ):
            text = (ROOT / path).read_text(encoding='utf-8')
            (build / output).write_text('\n'.join(extract(text, name)[2] for name in names), encoding='utf-8')
        groups = [
            ('src/ui/platform/app_content.c', ['app_content_open']),
            ('src/ui/apps/app_home.c', ['home_open_settings']),
            ('src/ui/apps/app_management.c', ['open_dynamic_settings']),
            ('src/ui/ui_control_center.c', ['ui_control_center_close', 'open_notification']),
            ('src/ui/ui_navigation.c', ['ui_navigation_open']),
        ]
        (build / 'entries.inc').write_text('\n'.join(
            extract((ROOT / path).read_text(encoding='utf-8'), name)[2]
            for path, names in groups for name in names), encoding='utf-8')
        units = ['switch'] if options.control else options.unit or ('switch', 'boundaries', 'entries')
        for unit in units:
            print(f'Running navigation {unit}', flush=True)
            exe = build / f'{unit}.exe'
            subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/Od',
                            *(['/fsanitize=address', '/Zi'] if options.asan else ['/RTC1']),
                            '/D_CRT_SECURE_NO_WARNINGS', f'/I{build}', f'/I{ROOT / "src/platform"}', str(HERE / f'{unit}.c'),
                            f'/Fe{exe}'], cwd=build, env=env, check=True, timeout=60)
            result = subprocess.run([str(exe)], cwd=build, env=env, timeout=15,
                                    capture_output=bool(options.control), text=True)
            if options.control:
                expected = ('starts==1&&!errors&&nav.pending&&nav.load_retry==NAV_RETRY_BLOCK&&nav.hold.held'
                            if options.control == 'skip_recovery' else 'starts==1&&!errors&&nav.pending&&nav.started==started')
                assert result.returncode != 0 and expected in result.stderr, result.stdout + result.stderr
                print(f'PASS negative control {options.control}: expected recovery assertion failed', flush=True)
            else:
                result.check_returncode()
        print(f'Navigation: {len(units)} groups passed', flush=True)


if __name__ == '__main__':
    main()
