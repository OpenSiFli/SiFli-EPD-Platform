"""Compile the production Books entry adapter with deterministic LVGL/storage doubles."""
from pathlib import Path
import argparse
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment
from view_run import extract
sys.path.insert(0, str(ROOT / 'tests/storage_perf'))
from owned_fixture import write_owned_fixture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    cc, env = compiler_environment(None)
    source = (ROOT / 'modules/books/ui_reader.c').read_text(encoding='utf-8')
    source = source.replace('#include "reader_entry_store.inc"', (ROOT / 'modules/books/reader_entry_store.inc').read_text(encoding='utf-8-sig'))
    source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
    types = (ROOT / 'modules/common/reader/reader_types.h').read_text(encoding='utf-8')
    types = re.sub(r'^#include[^\n]*\n', '', types, flags=re.M)
    fixture = (HERE / 'host.c').read_text(encoding='utf-8')
    budget = (ROOT / 'src/platform/epd_cold_budget.c').read_text(encoding='utf-8')
    budget = re.sub(r'^#include[^\n]*\n', '', budget, flags=re.M)
    budget_port = (HERE / 'cold_budget_port.inc').read_text(encoding='utf-8').replace('/* BUDGET PRODUCTION */', budget)
    fixture = fixture.replace('/* COLD BUDGET */', budget_port)
    fixture = fixture.replace('/* TYPES */', types).replace('/* PRODUCTION */', source)
    worker = extract((ROOT / 'modules/books/app_main.c').read_text(encoding='utf-8'), 'books_worker')[2]
    fixture = fixture.replace('/* WORKER */', worker)
    with tempfile.TemporaryDirectory(prefix='books-open-') as directory:
        build = Path(directory)
        write_owned_fixture(build)
        fixture = fixture.replace('/* OWNERSHIP */', (ROOT / 'tests/storage_perf/owned_adapter.h').read_text(encoding='utf-8'))
        path = build / 'host.c'; path.write_text(fixture, encoding='utf-8')
        exe = build / 'host.exe'
        subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4505', '/wd4244', '/wd4267', '/wd4200',
                        '/D_CRT_SECURE_NO_WARNINGS', '/Od', *(['/fsanitize=address', '/Zi'] if args.asan else ['/RTC1']), f'/I{ROOT / "src/platform"}', f'/I{ROOT / "tests/unified_resource"}', f'/I{ROOT / "tests/unified_resource/stubs"}', str(path), str(ROOT / 'src/platform/epd_resource.c'), f'/Fe{exe}'],
                       cwd=build, env=env, check=True, timeout=60)
        subprocess.run([str(exe)], cwd=build, env=env, check=True, timeout=30)


if __name__ == '__main__':
    main()
