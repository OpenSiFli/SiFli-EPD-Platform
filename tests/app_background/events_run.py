"""Exercise real background policy, subscriptions, operation transport and accounting."""
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

CASES = ('initial', 'worker', 'coalesce', 'duplicate', 'self_remove', 'publish_in_callback',
         'unsubscribe_queued', 'unsubscribe_ready', 'close_queued', 'close_ready',
         'close_in_callback', 'root_close', 'foreign', 'bad_lane', 'managed_worker',
         'rebind', 'old_handle', 'quota', 'operation_capacity', 'scope_capacity',
         'sequence', 'generation_overflow', 'registration', 'tasks', 'persist', 'rejected',
         'stale_report', 'idle', 'partial_cancel', 'runtime_not_ready', 'close_after_submit',
         'race_publish', 'race_close', 'cold_optional', 'cold_required', 'cold_stale',
         'cold_durable', 'cold_coalesced', 'cold_recovery', 'transport_rejected', 'operation_finished_capacity',
         'empty_slots', 'empty_then_subscribe')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capacity-wake-probe', action='store_true',
                        help='Verify a temporary operation scope-release wake candidate.')
    parser.add_argument('cases', nargs='*', choices=CASES)
    args = parser.parse_args()
    cc, env = compiler_environment(None)
    with tempfile.TemporaryDirectory(prefix='background-events-integration-') as temporary:
        build = Path(temporary)
        support = (ROOT / 'tests/business_memory/activity_host.c').read_text(encoding='utf-8')
        first = support.index('/* Synchronous activity tests')
        last = support.index('static size_t physical', first)
        support = support[:first] + support[last:]
        support = support[:support.index('int main(int argc, char **argv)')]
        support = re.sub(r'\bapp\b', 'test_app', support)
        support = ('#define WIN32_LEAN_AND_MEAN\n#include <windows.h>\n'
                   'static CRITICAL_SECTION host_irq_lock;\nstatic unsigned irq_entries;\n') + support
        support = support.replace('static rt_thread_t actor =', 'static __declspec(thread) rt_thread_t actor =')
        support = support.replace('static unsigned irq, mutexes,',
                                  'static __declspec(thread) unsigned irq;\nstatic unsigned mutexes,')
        support = support.replace('return (rt_base_t)irq++;',
                                  'EnterCriticalSection(&host_irq_lock); ++irq_entries; return (rt_base_t)irq++;')
        support = support.replace('irq = (unsigned)level; }', 'irq = (unsigned)level; LeaveCriticalSection(&host_irq_lock); }')
        support = support.replace('void epd_operation_runtime_wake(void) { ++wakes; }',
                                  'void epd_operation_runtime_wake(void) { InterlockedIncrement((volatile LONG *)&wakes); }')
        marker = 'bool epd_operation_runtime_executors('
        support = support.replace(marker, 'static bool executors_ready = true;\n' + marker)
        support = support.replace('*e = (epd_operation_executor_t)',
                                  'if (!executors_ready) return false;\n    *e = (epd_operation_executor_t)')
        (build / 'events_support.inc').write_text(support, encoding='utf-8')
        background = (ROOT / 'src/services/app_background.c').read_text(encoding='utf-8')
        background = background.replace('#include "app_service_internal.h"',
            'void app_service_background_changed(epd_memory_owner_t);\n'
            'epd_memory_result_t app_service_quiesce(const char *);')
        (build / 'background.inc').write_text(background, encoding='utf-8')
        ports = build / 'ports.h'
        ports.write_text('#include <rtthread.h>\n'
            '#define INIT_COMPONENT_EXPORT(f) int test_init_##f(void) { return f(); }\n'
            'int rt_event_send(struct rt_event *, unsigned);\n', encoding='utf-8')
        includes = [build, ROOT / 'tests/business_memory/stubs', ROOT / 'tests/unified_resource/stubs',
                    ROOT / 'src/services', ROOT / 'src/platform', ROOT / 'src/ui', ROOT / 'src']
        sources = [HERE / 'events_host.c', *(ROOT / 'src/platform' / name for name in
                   ('epd_memory_manager.c', 'epd_resource.c', 'epd_resource_runtime.c',
                    'epd_activity_work.c', 'epd_operation.c'))]
        if args.capacity_wake_probe:
            operation = sources[-1].read_text(encoding='utf-8')
            needle = 'if (ok) epd_memory_owner_release_trusted(owner);\n    return ok;'
            assert operation.count(needle) == 1
            operation = operation.replace(needle,
                'if (ok) { epd_memory_owner_release_trusted(owner); epd_operation_runtime_wake(); }\n    return ok;')
            start = operation.index('bool epd_operation_release(epd_operation_t id)')
            end = operation.index('void epd_operation_poll(void)', start)
            release = operation[start:end]
            release = release.replace('if (ok) {', 'bool freed = ok && op->view.state == EPD_OPERATION_FINISHED;\n    if (ok) {', 1)
            release = release.replace('    return ok;', '    if (freed) epd_operation_runtime_wake();\n    return ok;', 1)
            operation = operation[:start] + release + operation[end:]
            sources[-1] = build / 'operation.c'
            sources[-1].write_text(operation, encoding='utf-8')
            print('Using temporary scope/operation-release capacity-wake candidate.', flush=True)
        count = 0
        failed = []
        for rate in (1000, 100):
            exe = build / f'events-{rate}.exe'
            subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4505', '/TC',
                '/Od', '/Zi', '/fsanitize=address', '/D_CRT_SECURE_NO_WARNINGS', '/DEPD_MEMORY_HOST',
                '/DRT_USING_MEMHEAP_OWNER', f'/DRT_TICK_PER_SECOND={rate}', f'/FI{ports}',
                *(f'/I{p}' for p in includes), *map(str, sources), f'/Fe{exe}'],
                cwd=build, env=env, check=True, timeout=60)
            for case in args.cases or CASES:
                result = subprocess.run([str(exe), case], cwd=build, env=env, timeout=20)
                if result.returncode:
                    failed.append(f'{case}@{rate}')
                count += 1
        exports = set(re.findall(r'RTM_EXPORT\((\w+)\)',
            (ROOT / 'src/services/app_background_events.c').read_text(encoding='utf-8')))
        assert exports == {'epd_app_background_subscribe', 'epd_app_background_unsubscribe'}
        print(f'Background events: {count - len(failed)}/{count} ASAN cases passed; export surface PASS.', flush=True)
        if failed:
            print('Failed cases: ' + ', '.join(failed), flush=True)
            sys.exit(1)

if __name__ == '__main__':
    main()
