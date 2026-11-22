"""Observe actual operation/runtime guards, retries and close races; optionally compile ARM objects."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment

CASES = ('empty', 'failure_then_resubmit', 'context_retry', 'pending_cancel', 'fact_without_task',
         'matched_prefix', 'concurrent_close', 'post_dispatch', 'terminal_failure',
         *(f'scope_{status}_{scope}_{result}' for status in ('ok', 'failed')
           for scope in ('open', 'closed') for result in ('null', 'result')),
         'diag_samples', 'diag_wrap', 'diag_guards', 'diag_invalid', 'diag_before_metadata')
PRODUCTION = ('src/services/app_background_events.c', 'src/platform/epd_operation.c',
              'src/platform/epd_operation_runtime.c')


def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


def host(build, selected, run):
    cc, env = compiler_environment(None)
    support = (HERE / 'host.c').read_text(encoding='utf-8').split('int main(int argc, char **argv)')[0]
    support = replace_once(support, 'static rt_thread_t current =', 'static __declspec(thread) rt_thread_t current =')
    support = replace_once(support, 'static unsigned irq_depth, critical_depth,',
                           'static __declspec(thread) unsigned irq_depth;\nstatic unsigned critical_depth,')
    support = replace_once(support, 'static epd_memory_owner_t context;',
                           'static __declspec(thread) epd_memory_owner_t context;')
    support = replace_once(support, 'return (rt_base_t)irq_depth++;',
                           'EnterCriticalSection(&sync_irq); ++irq_entries; return (rt_base_t)irq_depth++;')
    support = replace_once(support, 'irq_depth = (unsigned)previous; }',
                           'irq_depth = (unsigned)previous; LeaveCriticalSection(&sync_irq); }')
    support = replace_once(support, 'if (fail_enter) return EPD_MEMORY_OWNER_INVALID;',
                           'if (!owner) before_system_enter();\n    if (fail_enter) return EPD_MEMORY_OWNER_INVALID;')
    support = replace_once(support, 'if(task_hook_error) return task_hook_error;',
                           '++task_publications; if(task_hook_error) return task_hook_error;')
    (build / 'sync_support.inc').write_text(support, encoding='utf-8')
    operation = (ROOT / PRODUCTION[1]).read_text(encoding='utf-8')
    operation = replace_once(operation, 'static bool scope_closed(operation_t *op)\n{',
                              'static bool scope_closed(operation_t *op)\n{\n    ++closed_queries;')
    (build / 'sync_operation.inc').write_text(operation, encoding='utf-8')
    runtime = (ROOT / PRODUCTION[2]).read_text(encoding='utf-8')
    runtime = replace_once(runtime, 'if (fact->owner && fact->owner == active[j].owner)',
                            'if ((++match_checks, fact->owner && fact->owner == active[j].owner))')
    (build / 'sync_runtime.inc').write_text(runtime, encoding='utf-8')
    (build / 'bf0_hal.h').write_text('#include <stdint.h>\n#define CORE_ID_HCPU 0\n'
        '#define __STATIC_FORCEINLINE static __forceinline\n'
        'uint32_t HAL_DBG_DWT_GetCycles(void);\nint HAL_DBG_DWT_IsInit(void);\n'
        'void HAL_DBG_DWT_Init(void);\nuint32_t HAL_RCC_GetHCLKFreq(int);\n', encoding='utf-8')
    (build / 'finsh.h').write_text('#define MSH_CMD_EXPORT(f, d)\n', encoding='utf-8')
    (build / 'ports.h').write_text('#define RT_EINVAL 10\n#define RT_EBUSY 7\n#define RT_ERROR 1\n'
                                  'void rt_kprintf(const char *, ...);\n', encoding='utf-8')
    sources = [HERE / 'sync_host.c', *(ROOT / 'src/platform' / name for name in
        ('epd_cold_state.c', 'epd_cold_budget.c', 'epd_cold_runtime.c')),
        ROOT / 'src/services/app_background.c', ROOT / PRODUCTION[0], ROOT / 'modules/hello/hello_state.c',
        ROOT / 'src/ui/ui_settings_store.c', ROOT / 'docs/examples/managed_operation/operation_example.c']
    includes = [build, HERE / 'stubs', HERE, ROOT / 'src', ROOT / 'src/platform', ROOT / 'src/ui',
                ROOT / 'docs/examples/managed_operation', ROOT / 'modules/hello', ROOT / 'src/services']
    passed = []
    for rate in (1000, 100):
        exe = build / f'sync-{rate}.exe'
        run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4505', '/TC', '/Od', '/Zi',
             '/fsanitize=address', '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_WARNINGS',
             '/DEPD_COLD_STATE_ROOT="."', '/DEPD_OPERATION_SYNC_DIAGNOSTIC=1',
             f'/DRT_TICK_PER_SECOND={rate}', f'/FI{build / "ports.h"}', *(f'/I{p}' for p in includes),
             *map(str, sources), f'/Fe{exe}'], build, env)
        for case in selected:
            work = build / f'{case}-{rate}'; work.mkdir(); (work / 'system').mkdir(); (work / 'sd').mkdir()
            run([str(exe), case], work, env)
            passed.append(f'{case}@{rate}')
    return passed


def arm(build, run):
    cc = Path(os.environ['USERPROFILE']) / '.sifli/tools/arm-none-eabi-gcc/14.2.1/bin/arm-none-eabi-gcc.exe'
    sdk = ROOT / 'SiFli-SDK'
    includes = [ROOT / 'project/build_sf32lb57-epd-gyd7n6_hcpu', sdk / 'rtos/rtthread/include',
                sdk / 'rtos/rtthread/components/finsh', sdk / 'middleware/include',
                sdk / 'drivers/cmsis/sf32lb57x', sdk / 'external/CMSIS/Include', sdk / 'drivers/Include',
                sdk / 'drivers/cmsis/Include', sdk / 'middleware/sifli_matrix',
                ROOT / 'src', ROOT / 'src/platform', ROOT / 'src/services', ROOT / 'src/ui']
    flags = ['-std=c11', '-mcpu=cortex-m33', '-mthumb', '-mfpu=fpv5-sp-d16', '-mfloat-abi=hard',
             '-fshort-enums', '-ffunction-sections', '-fdata-sections', '-Os', '-fstack-usage',
             # SDK section pragmas target Clang; FinSH uses its legacy syscall cast.
             '-Wall', '-Wextra', '-Werror', '-Wno-unknown-pragmas', '-Wno-cast-function-type',
             '-DSOC_BF0_HCPU', '-DSF32LB57X', '-DHAVE_CCONFIG_H']
    nm = cc.with_name(cc.name.replace('gcc', 'nm'))
    rows = []
    for relative in PRODUCTION:
        source = ROOT / relative
        for enabled in ((0, 1) if source.stem == 'epd_operation_runtime' else (None,)):
            obj = build / f'{source.stem}-{enabled}.o'
            defines = [f'-DEPD_OPERATION_SYNC_DIAGNOSTIC={enabled}'] if enabled is not None else []
            run([str(cc), '-c', *flags, *defines, *(f'-I{p}' for p in includes), str(source), '-o', str(obj)], build)
            symbols = run([str(nm), '-S', str(obj)], build, quiet=True)
            row = {'source': relative, 'diagnostic': enabled, 'stack': obj.with_suffix('.su').read_text()}
            for symbol in ('sync_diagnostic', 'sync_diagnostic_state'):
                values = [line.split() for line in symbols.splitlines() if line.endswith(' ' + symbol)]
                if values:
                    row[symbol] = int(values[0][1], 16)
            if enabled == 0:
                assert 'HAL_DBG_DWT' not in symbols and 'sync_diagnostic' not in symbols
                assert '__fsym___cmd_op_sync_diag' not in symbols
            if enabled == 1:
                assert row['sync_diagnostic_state'] == 1
                assert '__fsym___cmd_op_sync_diag' in symbols
                assert not any(line.endswith((' sync_lock', ' sync_unlock')) for line in symbols.splitlines())
                table = run([str(cc.with_name(cc.name.replace('gcc', 'objdump'))), '-t', str(obj)], build, quiet=True)
                assert any('.bss.l2_ret_bss_epd_operation_runtime' in line and line.endswith(' sync_diagnostic')
                           for line in table.splitlines())
            rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arm', action='store_true')
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--output', type=Path, default=HERE / '.build/sync-validation')
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    logs = []
    def run(command, cwd, env=None, quiet=False):
        process = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=60)
        try:
            output = process.stdout.decode('utf-8')
        except UnicodeDecodeError:
            output = process.stdout.decode('mbcs' if os.name == 'nt' else 'utf-8', errors='replace')
        logs.append('$ ' + subprocess.list2cmdline(list(map(str, command))) + '\n' + output)
        (args.output / 'validation.log').write_text('\n'.join(logs), encoding='utf-8')
        if not quiet: print(output, end='', flush=True)
        if process.returncode: raise subprocess.CalledProcessError(process.returncode, command, output)
        return output
    with tempfile.TemporaryDirectory(prefix='sync-guards-', dir=HERE) as directory:
        build = Path(directory)
        result = {'production_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                                        for name in PRODUCTION},
                  'host_instrumentation': 'IRQ entries, scope_closed calls and match comparisons only',
                  'host_passed': host(build, args.case or CASES, run)}
        if args.arm: result['arm'] = arm(build, run)
        (args.output / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(f'Sync guards: {len(result["host_passed"])} ASAN cases PASS; ARM={args.arm}', flush=True)


if __name__ == '__main__':
    main()
