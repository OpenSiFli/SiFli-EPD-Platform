"""Exercise reader repaint, resize, and binding with offscreen LVGL on Windows."""
import argparse
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
READER = ROOT / 'modules/common/reader'
LVGL_HOST = ROOT / 'tests/notes/ui'
CASES = ('roundtrip', 'enqueue-failure', 'render-failure', 'widget-failure', 'settings-failure', 'resize')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lvgl-build', type=Path, default=Path(tempfile.gettempdir()) / 'epd-reader-appearance-lvgl',
                        help='Reusable x64 build directory for the existing lvgl_host library')
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    library = args.lvgl_build.resolve()
    cc, env = runpy.run_path(str(ROOT / 'tests/reader_prefetch/cache_run.py'))['compiler_environment']('cl')

    def run(command, cwd, timeout=60, quiet=False):
        command = list(map(str, command))
        print(subprocess.list2cmdline(command), flush=True)
        result = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE if quiet else None,
                                stderr=subprocess.STDOUT if quiet else None, timeout=timeout)
        if result.returncode:
            if quiet:
                print(result.stdout.decode(errors='backslashreplace')[-8000:])
            raise SystemExit(result.returncode)

    # Use just the existing host LVGL target; no application or device targets are linked.
    run(['cmake', '-S', LVGL_HOST, '-B', library, '-A', 'x64'], ROOT, quiet=True)
    run(['cmake', '--build', library, '--config', 'Release', '--target', 'lvgl_host', '--parallel', '8'],
        ROOT, timeout=600, quiet=True)
    extract = runpy.run_path(str(ROOT / 'tests/reader_view_async/run.py'))['extract']
    source = (READER / 'reader_view.c').read_text(encoding='utf-8')
    header = (READER / 'reader.h').read_text(encoding='utf-8')
    functions = '''cursor_equal frame_ref window_contexts frame_cache_clear visited_clear
        layout_for_area preview_start state_reset_layout preview_visible core_waiting command_restores_origin command_apply
        width_available frame_create frame_publish ui_configuration ui_root_deleted epd_reader_detach epd_reader_save
        font_request_cancel font_configuration_snapshot epd_reader_set_error_code epd_reader_settings_changed
        ui_configuration_rejected epd_reader_reflow font_request_process ui_command_at epd_reader_resize
        epd_reader_request_appearance epd_reader_waiting epd_reader_suspended epd_reader_step
        ui_link_clicked epd_reader_bind epd_reader_bound_image'''.split()
    # Keep the positional aggregate initializers in ui_command_at coupled to their real declarations.
    def through(first, last):
        start = source.index(first)
        return source[start:source.index(last, start) + len(last)]

    with tempfile.TemporaryDirectory(prefix='reader-appearance-') as tmp:
        build = Path(tmp)
        (build / 'channels.inc').write_text(through('typedef enum { INTENT_NONE', '} mailbox;') + '\n' +
            through('static struct {\n    view_state_t *session;', '} ui;') + '\n' +
            through('static struct {\n    epd_activity_work_t *activity;\n    view_state_t *context;', '} font_request;'),
            encoding='utf-8')
        settings = (ROOT / 'src/ui/ui_settings.c').read_text(encoding='utf-8')
        (build / 'settings_set.inc').write_text('\n\n'.join(extract(name, settings) for name in
                                                ('ui_settings_set', 'ui_settings_index')), encoding='utf-8')
        definitions = [re.search(r'^#define EPD_READER_LAYOUT_CACHE_VERSION .+$', header, re.M)[0],
                       *re.findall(r'^#define (?:VIEW_LAYOUT_VERSION|VIEW_PREVIEW_ORIGIN) .+$', source, re.M)]
        (build / 'appearance.inc').write_text('\n\n'.join([
            *definitions, *(extract(name, source) for name in functions),
        ]), encoding='utf-8')
        exe = build / 'appearance.exe'
        includes = [build, READER, READER / 'compute/include', ROOT / 'src/ui',
                    LVGL_HOST, ROOT / 'SiFli-SDK/external/lvgl_v9']
        flags = ['/nologo', '/std:c11', '/utf-8', '/W3', '/WX', '/Od', '/RTC1', '/MD', '/UNDEBUG',
                 '/D_CRT_SECURE_NO_WARNINGS', '/DLV_CONF_INCLUDE_SIMPLE', '/DLV_KCONFIG_IGNORE',
                 *(f'/I{p}' for p in includes)]
        kernel = build / 'reader_kernel.obj'
        # C4244 in the kernel's run placement; keep strict warnings in the fixture.
        run([cc, *flags, '/wd4244', '/c', READER / 'compute/reader_kernel.c', f'/Fo{kernel}'], build)
        run([cc, *flags, HERE / 'appearance.c', READER / 'compute/reader_page.c', kernel,
             f'/Fe{exe}', '/link', library / 'Release/lvgl_host.lib'], build)
        for case in args.case or CASES:
            run([exe, case], build, timeout=15)


if __name__ == '__main__':
    main()
