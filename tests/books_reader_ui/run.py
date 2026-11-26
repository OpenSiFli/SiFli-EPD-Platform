"""Exercise the device reader menus with SDK LVGL and the platform keyboard."""
import importlib.util
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('output', nargs='?', type=Path, default=Path(tempfile.gettempdir()) / 'books-reader-ui')
parser.add_argument('--font-only', action='store_true')
parser.add_argument('--navigation-only', action='store_true')
parser.add_argument('--controls-source', type=Path)
args = parser.parse_args()
OUT = args.output.resolve()
OUT.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('ai_ui', HERE.parent / 'ai_demo_ui/run.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
generated = base.prepare(OUT)

def write(name, text):
    (generated / name).write_text(text, encoding='utf-8')

strings = json.loads((ROOT / 'modules/books/i18n/strings.json').read_text(encoding='utf-8'))['strings']
table = (generated / 'platform/epd_i18n.h').read_text(encoding='utf-8')
table += '\n'.join('#define ' + key + ' ' + json.dumps(value['zh_cn'], ensure_ascii=False) for key, value in strings.items()) + '\n'
write('platform/epd_i18n.h', table)
write('epd_i18n.h', table)
for name in ('reader_controls.c', 'reader_ai_ui.c'):
    path = args.controls_source if name == 'reader_controls.c' and args.controls_source else ROOT/'modules/books'/name
    source = path.read_text(encoding='utf-8')
    write(name, '#include "host_api.h"\n' + re.sub(r'^#include[^\n]*\n', '', source, flags=re.M))
for name in ('reader_ai.h', 'reader_icons.h'):
    write(name, (ROOT / 'modules/books' / name).read_text(encoding='utf-8'))
write('reader_controls.h', (ROOT / 'modules/books/reader_controls.h').read_text(encoding='utf-8').replace('#include "books_ui.h"', '#include "host_api.h"'))
write('reading_font.h', (ROOT / 'src/services/reading_font.h').read_text(encoding='utf-8').replace('#include "storage.h"', ''))
launcher = (ROOT / 'src/ui/launcher.c').read_text(encoding='utf-8')
platform = (ROOT / 'src/ui/platform/epd_app.c').read_text(encoding='utf-8')
write('view.inc', '\n'.join(base.function(launcher, name) for name in ('epd_obj_init', 'label_create', 'centered_label', 'panel_create', 'icon_create')) + '\n' + base.function(platform, 'epd_app_button'))
write('clicked.inc', base.function((ROOT / 'modules/books/app_main.c').read_text(encoding='utf-8'), 'clicked'))
source = (ROOT / 'src/ui/platform/epd_input.c').read_text(encoding='utf-8')
scope = source[source.index('typedef struct input_scope'):source.index('} input_guard_t;') + len('} input_guard_t;')]
write('input.inc', 'typedef void epd_input_ops_t;\n' + scope +
      '\nstatic input_scope_t *scopes, *press_scope;\nstatic unsigned next_identity;\n'
      'static void scope_deleted(lv_event_t *event);\nvoid epd_input_cleanup_closed(void);\n'
      'static void publish_scope(void) { epd_input_cleanup_closed(); }\n' +
      '\n'.join(base.function(source, name) for name in ('context_open', 'context_enter', 'context_leave',
                'scope_for_owner', 'back_deleted', 'scope_destroy', 'epd_input_cleanup_closed', 'scope_deleted',
                'epd_app_input_group', 'epd_input_add_object', 'epd_app_input_back')))
source = (ROOT / 'modules/books/pages.c').read_text(encoding='utf-8')
write('resume.inc', base.function(source, 'reader_resume_prepare'))
# Exercise the production order and gates for reflow, navigation, target work
# and completion without bringing storage/status-bar services into the host.
process = base.function(source, 'books_process')
start = process.index('        bool leaving_page =')
end = process.index('        if (idle && !reader_page_live', start)
write('reader_process.inc', process[start:end])
# MSVC can omit include dependency tracking inside the temporary directory.
# Regenerate the translation unit so every run compiles the extracted sources.
write('host.c', (HERE / 'host.c').read_text(encoding='utf-8'))
reader = (ROOT / 'modules/common/reader/reader_view.c').read_text(encoding='utf-8')
write('binding.inc', '\n'.join(base.function(reader, name) for name in (
    'cursor_equal', 'source_percent', 'ui_configuration', 'ui_root_deleted', 'epd_reader_detach',
    'font_request_cancel', 'font_configuration_snapshot', 'epd_reader_reflow', 'font_request_process',
    'ui_command', 'epd_reader_turn', 'epd_reader_cancel_pending',
    'frame_publish', 'epd_reader_step', 'epd_reader_bind', 'epd_reader_bound_image')))
write('binding.c', (HERE / 'binding.c').read_text(encoding='utf-8'))
font_fixture = (ROOT / 'tests/reader_view_async/font.c').read_text(encoding='utf-8')
font_production = '\n'.join(base.function(reader, name) for name in (
    'navigation_clear', 'link_clear', 'prepare_fonts', 'state_reflow', 'ui_configuration',
    'epd_reader_settings_changed', 'ui_configuration_rejected'))
write('font_preparation.c', font_fixture.replace('/* PRODUCTION */', font_production))
spec = importlib.util.spec_from_file_location('compiler', HERE.parent / 'reader_prefetch/cache_run.py')
compiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compiler)
cc, env = compiler.compiler_environment(None)
for command, name in ((['cmake', '-S', str(HERE), '-B', str(OUT / 'build'), '-DGENERATED=' + generated.as_posix()], 'configure.log'),
                      (['cmake', '--build', str(OUT / 'build'), '--config', 'Release', '--parallel', '8'], 'build.log')):
    with (OUT / name).open('w', encoding='utf-8') as log:
        result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((OUT / name).read_text(encoding='utf-8', errors='replace')[-15000:])
        raise SystemExit(result.returncode)
result = subprocess.run([str(OUT / 'build/Release/books_reader_ui.exe'), str(OUT), str(ROOT / 'font/MiSans-Normal.ttf'),
                         *(['--navigation-only'] if args.navigation_only else ['--font-only'] if args.font_only else [])], env=env, capture_output=True, timeout=90)
(OUT / 'run.log').write_bytes(result.stdout + result.stderr)
print((result.stdout + result.stderr).decode('utf-8', errors='replace'))
binding = subprocess.run([str(OUT / 'build/Release/reader_binding.exe')], env=env, capture_output=True, timeout=20)
(OUT / 'binding.log').write_bytes(binding.stdout + binding.stderr)
print((binding.stdout + binding.stderr).decode('utf-8', errors='replace'))
font_exe = OUT / 'font_preparation.exe'
subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4505', '/wd4459',
                '/D_CRT_SECURE_NO_WARNINGS', '/Od', '/RTC1', f'/I{ROOT / "src/ui"}', str(generated / 'font_preparation.c'),
                '/Fe' + str(font_exe)], cwd=OUT, env=env, check=True, timeout=60)
font = subprocess.run([str(font_exe)], env=env, capture_output=True, timeout=20)
(OUT / 'font_preparation.log').write_bytes(font.stdout + font.stderr)
print((font.stdout + font.stderr).decode('utf-8', errors='replace'))
from PIL import Image
for source in OUT.glob('*.ppm'):
    with Image.open(source) as im:
        im.save(source.with_suffix('.png'))
raise SystemExit(result.returncode or binding.returncode or font.returncode)
