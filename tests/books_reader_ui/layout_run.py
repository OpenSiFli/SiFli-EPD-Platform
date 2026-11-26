"""Run the Books typography and wire-format regression with the host C compiler."""
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TESTS = HERE.parent
UI = ROOT / 'src/ui'
READER = ROOT / 'modules/common/reader'
FONT = ROOT / 'components/compute_reader_font'


def main():
    compiler = runpy.run_path(str(TESTS / 'reader_prefetch/cache_run.py'))['compiler_environment']
    extract = runpy.run_path(str(TESTS / 'reader_document_compute/hcpu_reader_extract.py'))['function']
    cc, env = compiler(os.environ.get('CC'))
    settings = (UI / 'ui_settings.c').read_text(encoding='utf-8')
    store = (UI / 'ui_settings_store.c').read_text(encoding='utf-8')
    view = (READER / 'reader_view.c').read_text(encoding='utf-8')
    controls = (ROOT / 'modules/books/reader_controls.c').read_text(encoding='utf-8')
    books_ui = (ROOT / 'modules/books/books_ui.h').read_text(encoding='utf-8')
    header = (READER / 'reader.h').read_text(encoding='utf-8')
    disk_host = (TESTS / 'settings_async/host_test.c').read_text(encoding='utf-8')
    disk_api = (TESTS / 'settings_async/host.h').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='books-layout-') as directory:
        build = Path(directory)
        # Synchronous record I/O uses the settings_async suite's in-memory disk.
        (build / 'settings.inc').write_text('\n'.join([
            disk_api[disk_api.index('#define O_RDONLY'):disk_api.index('int host_open(')],
            *(extract(disk_host, n) for n in ('host_open', 'host_fstat', 'host_read', 'host_close', 'storage_crc32',
                'storage_path_available', 'storage_file_recover', 'storage_path_revision', 'storage_volume_revision',
                'storage_file_writers')),
            settings[settings.index('static const unsigned timeout_minutes'):settings.index('unsigned ui_settings_default(')],
            *(extract(settings, n) for n in ('ui_settings_default', 'ui_settings_item', 'refresh_text',
                                             'ui_settings_option', 'ui_settings_value', 'ui_settings_index')),
            store[store.index('#define SETTINGS_PATH'):store.index('static struct rt_mutex')],
            store[store.index('typedef struct\n{\n    ui_setting_id_t id;'):store.index('static uint32_t get_u32(')],
            *(extract(store, n) for n in ('get_u32', 'get_u16', 'put_u32', 'put_u16', 'default_values',
                                          'decode_value', 'decode_payload', 'read_exact', 'read_record', 'encode_record')),
        ]), encoding='utf-8')
        (build / 'reader.inc').write_text('\n'.join([
            re.search(r'^#define EPD_READER_LAYOUT_CACHE_VERSION .+$', header, re.M)[0],
            re.search(r'^#define VIEW_LAYOUT_VERSION .+$', view, re.M)[0],
            *(extract(view, n) for n in ('layout_for_area', 'prepare_fonts', 'fonts_ready', 'utf8_letter')),
            *re.findall(r'^#define (?:WIDTH|HEIGHT|MARGIN|CONTENT_WIDTH) .+$', books_ui, re.M),
            re.search(r'^enum \{\s*WIDE_HEADER_HEIGHT=.*?^\};', controls, re.M | re.S)[0],
            extract(controls, 'build_type'),
        ]), encoding='utf-8')
        exe = build / ('layout.exe' if os.name == 'nt' else 'layout')
        includes = [build, UI, READER / 'compute/include', FONT / 'include']
        sources = [HERE / 'layout.c', READER / 'compute/reader_kernel.c', FONT / 'compute_reader_font.c']
        if Path(cc).stem.lower() == 'cl':
            flags = ['/nologo', '/std:c11', '/utf-8', '/Od', '/RTC1', '/D_CRT_SECURE_NO_WARNINGS',
                     *(f'/I{p}' for p in includes), *sources, f'/Fe{exe}']
        else:
            flags = ['-std=c11', '-O1', '-g', *(f'-I{p}' for p in includes), *sources, '-lm', '-o', exe]
        subprocess.run([cc, *flags], cwd=build, env=env, check=True, timeout=60)
        subprocess.run([exe, ROOT / 'font/MiSans-Normal.ttf'], cwd=build, env=env, check=True, timeout=30)


if __name__ == '__main__':
    main()
