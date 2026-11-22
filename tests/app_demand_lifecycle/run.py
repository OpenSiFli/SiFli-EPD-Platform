"""Compile lifecycle functions extracted from the current production applications."""
import importlib.util
import json
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


def functions(path, names):
    source = (ROOT / path).read_text(encoding='utf-8')
    return '\n'.join(extract(source, name)[2] for name in names)


def main():
    cc, env = compiler_environment(None)
    for app, minimum in [('books', 49), ('ai_demo', 19), ('notes', 15)]:
        value = json.loads((ROOT / f'modules/{app}/app.json').read_text(encoding='utf-8'))
        assert value['background'] is False and value['version'] >= minimum, app
    for app in ('weather',):
        assert json.loads((ROOT / f'modules/{app}/app.json').read_text(encoding='utf-8'))['background']
    calendar = json.loads((ROOT / 'modules/calendr/app.json').read_text(encoding='utf-8'))
    assert calendar['background'] is False and calendar['version'] >= 6
    with tempfile.TemporaryDirectory(prefix='app-demand-') as folder:
        build = Path(folder)
        units = [
            ('ai', 'runtime.c', '/DAPP_AI', 'modules/ai_demo/src/demo.c',
             ['ui_process', 'demo_start', 'demo_ui_closed', 'demo_snapshot']),
            ('notes', 'runtime.c', '', 'modules/notes/notes_runtime.c',
             ['ui_process', 'notes_runtime_start', 'notes_runtime_ui_closed', 'notes_status']),
            ('books', 'books.c', '', 'modules/books/app_main.c',
             ['books_flush', 'service_process', 'books_restart', 'books_service_start', 'books_page_resume', 'page_message', 'app_main']),
            ('books_ai', 'books_ai.c', '', 'modules/books/reader_ai.c', ['books_ai_idle']),
            ('ai_entry', 'entry.c', '/DAPP_AI', 'modules/ai_demo/src/app_main.c', ['app_main']),
            ('notes_entry', 'entry.c', '', 'modules/notes/notes_ui.c', ['app_main']),
        ]
        for name, host, define, source, names in units:
            (build / 'actual.inc').write_text(functions(source, names), encoding='utf-8')
            exe = build / (name + '.exe')
            args = [cc, '/nologo', '/std:c11', '/utf-8', '/W3', '/WX', '/TC', '/Od', '/RTC1',
                    '/D_CRT_SECURE_NO_WARNINGS', f'/I{build}', str(HERE / host), f'/Fe{exe}']
            if define:
                args.append(define)
            subprocess.run(args, cwd=build, env=env, check=True, timeout=60)
            subprocess.run([str(exe)], cwd=build, env=env, check=True, timeout=15)
    print('Demand lifecycle: all production extraction groups passed; manifests verified.')


if __name__ == '__main__':
    main()
