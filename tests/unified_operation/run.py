# SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0
"""Compile actual operation/cold-state/runtime translation units with host ports."""
import argparse
import importlib.util
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CASES = ('cancel_queued', 'cancel_running', 'queue_full', 'deadline', 'scope_generation',
         'reentrant', 'capacity', 'context_failure', 'cold_roundtrip', 'cold_failure',
         'cold_revision', 'cold_capacity', 'cold_corrupt', 'runtime', 'owner_close',
         'settings_save', 'settings_failure', 'settings_reset', 'settings_during_write',
         'settings_backpressure', 'runtime_deadlines', 'runtime_clients', 'runtime_cold_burst', 'runtime_ui_wake',
         'task_shared_owner', 'task_yield_and_return', 'task_start_failure', 'public_example', 'runtime_global_objects',
         'checkpoint_cycle', 'checkpoint_save_failure', 'checkpoint_reopen', 'checkpoint_edit_before_restore',
         'checkpoint_reentrant_capture', 'checkpoint_restore_retry', 'checkpoint_format_and_duplicate',
         'checkpoint_init_abort', 'checkpoint_notify_saving', 'checkpoint_notify_retry',
         'checkpoint_notify_progress', 'checkpoint_notify_revision',
         'budget_metadata', 'budget_slots', 'budget_order', 'budget_required', 'budget_leases',
         'budget_reboot', 'budget_index_failure', 'budget_cold_cross_class', 'budget_deferred', 'budget_media',
         'budget_reboot_pending', 'budget_corrupt_index')


def check_exports():
    expected = {
        'epd_operation.c': {f'epd_operation_{s}' for s in (
            'scope_open', 'scope_get', 'scope_close', 'scope_release', 'owner_drained',
            'submit', 'cancel', 'yield', 'complete', 'get', 'retry_delivery', 'release')},
        'epd_operation_runtime.c': {'epd_operation_runtime_executors'},
        'epd_cold_state.c': {f'epd_cold_state_{s}' for s in (
            'put', 'load', 'get', 'copy', 'retry', 'release', 'abandon', 'take', 'blob_release')},
        'epd_cold_runtime.c': {f'epd_checkpoint_{s}' for s in (
            'register', 'register_edits', 'changed', 'get', 'restore', 'retry', 'unregister')},
    }
    for name, public in expected.items():
        source = (ROOT / 'src/platform' / name).read_text(encoding='utf-8')
        exported = set(re.findall(r'RTM_EXPORT\((\w+)\)', source))
        if exported != public:
            raise AssertionError(f'{name}: missing {public - exported}, unexpected {exported - public}')
    print('PASS public export surface (29 business interfaces; resident scheduling excluded)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc')
    parser.add_argument('--asan', action='store_true')
    parser.add_argument('--case', choices=CASES, action='append')
    args = parser.parse_args()
    check_exports()
    spec = importlib.util.spec_from_file_location('host_compiler', ROOT / 'tests/reader_prefetch/cache_run.py')
    compiler = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(compiler)
    cc, env = compiler.compiler_environment(args.cc)
    sources = [HERE / 'host.c', *(ROOT / 'src/platform' / name for name in
               ('epd_operation.c', 'epd_cold_state.c', 'epd_cold_budget.c', 'epd_operation_runtime.c', 'epd_cold_runtime.c')),
               ROOT / 'src/services/app_background.c',
               ROOT / 'src/services/app_background_events.c',
               ROOT / 'modules/hello/hello_state.c',
               ROOT / 'src/ui/ui_settings_store.c',
               ROOT / 'docs/examples/managed_operation/operation_example.c']
    includes = [HERE / 'stubs', ROOT / 'src', ROOT / 'src/platform', ROOT / 'src/ui',
                ROOT / 'docs/examples/managed_operation', ROOT / 'modules/hello', ROOT / 'src/services']
    with tempfile.TemporaryDirectory(prefix='build-', dir=HERE) as directory:
        build = Path(directory)
        exe = build / ('unified_operation.exe' if os.name == 'nt' else 'unified_operation')
        if Path(cc).stem.lower() == 'cl':
            command = [cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/TC', '/Od',
                       *(['/fsanitize=address', '/Zi'] if args.asan else ['/RTC1']),
                       '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_WARNINGS', '/DEPD_COLD_STATE_ROOT="."',
                       *(f'/I{p}' for p in includes), *(str(p) for p in sources), f'/Fe{exe}']
        else:
            command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-DEPD_COLD_STATE_ROOT="."', *(f'-I{p}' for p in includes),
                       *(str(p) for p in sources), '-o', str(exe)]
        subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
        selected = args.case or CASES
        for case in selected:
            folder = build / case
            folder.mkdir()
            (folder / 'system').mkdir()
            (folder / 'sd').mkdir()
            subprocess.run([str(exe), case], cwd=folder, env=env, check=True, timeout=20)
        print(f'Unified operation: {len(selected)}/{len(selected)} cases passed against production sources.')


if __name__ == '__main__':
    main()
