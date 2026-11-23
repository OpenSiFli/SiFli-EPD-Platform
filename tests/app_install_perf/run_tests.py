"""Build production package/module/installer code with deterministic host I/O."""
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main(test_source=None, arguments=(), *, activity=False, rate=1000, control=None, asan=False):
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip()
    with tempfile.TemporaryDirectory(prefix='app-install-perf-', ignore_cleanup_errors=True) as directory:
        build = Path(directory)
        for name in ('storage.h', 'storage_internal.h', 'storage_file.h', 'epd_memory.h',
                     'gui_app_fwk.h', 'rtthread.h', 'rthw.h', 'dlmodule.h', 'dlfcn.h',
                     'lv_ext_resource_manager.h', 'dfs_posix.h', 'dfs_fs.h', 'icons/ui_icons.h',
                     'reminder_internal.h', 'storage_file_internal.h', 'system_reset_guard.h',
                     'epd_i18n.h', 'app_service.h', 'ai_settings.h', 'sleep_guard.h', 'gui_app_int.h',
                     'platform/epd_app_library.h', 'ui_reader_font_cache.h', 'rtm.h', 'lvgl.h'):
            target = build / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('#include "host.h"\n')
        (build / 'sha_config.h').write_text('#define MBEDTLS_SHA256_C\n')
        (build / 'app_service_internal.h').write_text('#include "lifecycle_host.h"\n')
        memory = (ROOT / 'src/ui/platform/app_memory.c').read_text(encoding='utf-8')
        (build / 'load_probe.inc').write_text('#include "app_memory.h"\n' +
            memory[memory.index('static app_memory_load_watch_t *load_watch;'):memory.index('L2_RET_BSS_SECT_BEGIN')],
            encoding='utf-8')
        for source, name in [('src/platform/epd_memory_manager.c', 'memory_core.inc'),
                             ('src/services/app_background.c', 'background.inc')]:
            text = (ROOT / source).read_text(encoding='utf-8')
            if control == 'close_gate' and name == 'background.inc':
                old = '} else if (before.dirty_revision != before.saved_revision) {'
                assert text.count(old) == 1
                text = text.replace(old, '} else if (before.effective == EPD_APP_BG_STATE || before.dirty_revision != before.saved_revision) {')
            if control == 'retire_generation' and name == 'background.inc':
                old = 'app->schedule_generation = ++background_next_schedule;'
                assert text.count(old) == 1
                text = text.replace(old, '++app->schedule_generation;')
            if control == 'retire_pending' and name == 'background.inc':
                old = '!app->state.tasks && !app->scheduled && !app->schedule_dispatched'
                assert text.count(old) == 1
                text = text.replace(old, '!app->state.tasks')
            (build / name).write_text(re.sub(r'^#include[^\n]*\n', '', text, flags=re.M), encoding='utf-8')
        installer = (ROOT / 'src/ui/platform/app_installer.c').read_text(encoding='utf-8')
        if control == 'retire_missing':
            old = 'if (ok) app_background_forget(job_id);'
            assert installer.count(old) == 1
            installer = installer.replace(old, '(void)job_id;')
        (build / 'installer.inc').write_text(installer, encoding='utf-8')
        if activity:
            for source, name in [('src/platform/epd_resource.c', 'resource_core.inc'),
                                 ('src/platform/epd_resource_runtime.c', 'resource_runtime.inc'),
                                 ('src/platform/epd_activity_work.c', 'activity_work.inc')]:
                text = (ROOT / source).read_text(encoding='utf-8')
                if name == 'resource_runtime.inc':
                    # Preserve the original separate-TU name space in the combined fixture.
                    text = re.sub(r'(?<![.\w>])wake\b', 'resource_runtime_wake', text)
                (build / name).write_text(re.sub(r'^#include[^\n]*\n', '', text, flags=re.M), encoding='utf-8')
            text = (ROOT / 'src/ui/platform/app_module.c').read_text(encoding='utf-8')
            if control == 'selection':
                old = 'return state->dirty_revision == state->saved_revision;'
                assert text.count(old) == 1
                text = text.replace(old, 'return false;')
            (build / 'state_module.inc').write_text(text, encoding='utf-8')
        watch = (ROOT / 'src/services/storage_watch.c').read_text(encoding='utf-8')
        (build / 'watch_actual.inc').write_text('\n'.join(line for line in watch.splitlines()
                                                       if not line.startswith('#include')), encoding='utf-8')
        catalog = (ROOT / 'src/ui/platform/app_catalog.c').read_text(encoding='utf-8')
        begin = catalog.index('static bool module_open_requested(')
        end = catalog.index('\n}\n', catalog.index('static bool module_open(')) + 3
        read_begin = catalog.index('static app_package_status_t package_read_status(')
        read_end = catalog.index('\n}\n', read_begin) + 3
        (build / 'catalog_entry.inc').write_text(catalog[read_begin:read_end] + catalog[begin:end], encoding='utf-8')
        for source, signature, name in [
            ('src/services/app_service.c', 'epd_memory_result_t app_service_terminate(', 'terminate_entry.inc'),
            ('src/ui/apps/app_management.c', 'static app_job_message_t app_source_metadata(', 'management_metadata.inc')]:
            text = (ROOT / source).read_text(encoding='utf-8')
            if control == 'install_stop' and name == 'terminate_entry.inc':
                old = 'epd_memory_result_t result = app_module_request_stop(app_id);'
                assert text.count(old) == 1
                text = text.replace(old, 'epd_memory_result_t result = EPD_MEMORY_OK;')
            begin = text.index(signature)
            end = text.index('\n}\n', begin) + 3
            (build / name).write_text(text[begin:end], encoding='utf-8')
        sdk = ROOT / 'SiFli-SDK/external'
        args = [
            'cl', '/nologo', '/std:c11', '/utf-8', '/Od', '/Zi', '/Gy', '/W3', '/WX',
            '/D_CRT_SECURE_NO_WARNINGS', '/DMBEDTLS_CONFIG_FILE=\\"sha_config.h\\"',
            f'/I"{HERE}"', f'/I"{build}"', f'/I"{sdk / "cJSON-1.7.17"}"',
            f'/I"{ROOT / "src"}"', f'/I"{ROOT / "src/services"}"',
            f'/I"{ROOT / "src/platform"}"',
            f'/I"{ROOT / "src/ui/platform"}"', f'/DRT_TICK_PER_SECOND={rate}',
            f'/I"{sdk / "mbedtls_228/mbedtls/include"}"', f'"{test_source or HERE / "host_test.c"}"',
            f'"{HERE / "watch_bridge.c"}"',
            f'"{sdk / "cJSON-1.7.17/cJSON.c"}"', f'"{sdk / "mbedtls_228/mbedtls/library/sha256.c"}"',
            f'"{sdk / "mbedtls_228/mbedtls/library/platform_util.c"}"',
            f'/Fe:"{build / "host_test.exe"}"', '/link', '/OPT:REF',
        ]
        if asan:
            args.insert(1, '/fsanitize=address')
        script = build / 'build.cmd'
        script.write_text(f'@echo off\ncall "{vs}/VC/Auxiliary/Build/vcvars64.bat" >nul\n' + ' '.join(args) + '\n')
        subprocess.run(['cmd', '/d', '/c', str(script)], cwd=build, check=True)
        command = [str(build / 'host_test.exe'), *arguments]
        if asan:
            run_script = build / 'run.cmd'
            run_script.write_text(f'@echo off\ncall "{vs}/VC/Auxiliary/Build/vcvars64.bat" >nul\n' +
                                  subprocess.list2cmdline(command) + '\n')
            command = ['cmd', '/d', '/c', str(run_script)]
        result = subprocess.run(command, cwd=build,
                                capture_output=bool(control), text=True, timeout=60)
        if control:
            expected = ({'retire_missing': '!background_find_id(id)',
                         'retire_generation': '!app_background_schedule_claim("demo", old_generation, now)',
                         'retire_pending': '!app_background_forget("demo")'}[control] if control.startswith('retire_') else
                        'app_modules_reclaim("other")' if control == 'selection' else
                        'stopped.stopping && stopped.effective == EPD_APP_BG_RELEASE' if control == 'install_stop' else
                        '!instances && !dlmodule_find')
            assert result.returncode != 0 and expected in result.stderr, result.stdout + result.stderr
            print(f'PASS negative control {control}: ' + result.stderr.strip())
        else:
            result.check_returncode()


if __name__ == '__main__':
    main(arguments=sys.argv[1:])
