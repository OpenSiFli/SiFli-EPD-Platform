"""Run the full production document component with two real application owners."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import runpy
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DOC = ROOT / 'tests/reader_document_compute'
RESOURCE = ROOT / 'tests/unified_resource'
extract = runpy.run_path(str(ROOT / 'tests/reader_view_async/run.py'))['extract']


def replace_once(source, old, new):
    assert source.count(old) == 1, old[:100]
    return source.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cc, env = runpy.run_path(str(ROOT / 'tests/reader_prefetch/cache_run.py'))['compiler_environment'](None)
    production = [ROOT / 'modules/books/document/document.c', ROOT / 'modules/books/document/document.h',
                  ROOT / 'modules/common/reader/reader_view.c', *(ROOT / 'src/platform' / name for name in
                  ('epd_memory_manager.c', 'epd_resource.c', 'epd_resource_runtime.c', 'epd_activity_work.c'))]
    before = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in production}
    port = output / 'document-port'
    port.mkdir(exist_ok=True)
    support = (ROOT / 'tests/business_memory/activity_host.c').read_text(encoding='utf-8')
    first = support.index('/* Synchronous activity tests')
    last = support.index('static size_t physical', first)
    support = support[:first] + (RESOURCE / 'queued_background.inc').read_text(encoding='utf-8') + support[last:]
    support = support[:support.index('int main(int argc, char **argv)')]
    support = replace_once(support, 'epd_memory_owner_create("activity-test")', 'epd_memory_owner_create(application_id)')
    (output / 'queued_support.inc').write_text(support, encoding='utf-8')
    dfs = ROOT / 'SiFli-SDK/rtos/rtthread/components/dfs'
    descriptor = (dfs / 'include/dfs_file.h').read_text(encoding='utf-8')
    first = descriptor.index('struct dfs_fd\n{')
    last = descriptor.index('\n};', first) + 3
    (output / 'dfs_file.h').write_text('#include <sys/types.h>\n' + descriptor[first:last], encoding='utf-8')
    posix = (dfs / 'include/dfs_posix.h').read_text(encoding='utf-8')
    directory = re.search(r'typedef struct\s*\{[^{}]*\} DIR;', posix)[0]
    (output / 'dfs_posix.h').write_text('#include "dfs_file.h"\n' + directory, encoding='utf-8')
    for name in ('ff.h', 'ffconf.h'):
        (output / name).write_bytes((dfs / 'filesystems/elmfat' / name).read_bytes())
    (output / 'rtdef.h').write_text('#include "rtthread.h"\ntypedef struct rt_mutex *rt_mutex_t;\n', encoding='utf-8')

    base = (ROOT / 'tests/books/document_host.h').read_text(encoding='utf-8')
    base = replace_once(base, '#define EPD_APP_PSRAM 0', '#define EPD_APP_PSRAM 1')
    base = replace_once(base, '#define RT_TICK_PER_SECOND 1000', '#ifndef RT_TICK_PER_SECOND\n#define RT_TICK_PER_SECOND 1000\n#endif')
    replacements = {
        'epd_app_alloc': '''static void *epd_app_alloc(size_t n, int kind) {
    (void)kind;
    if (allocation_probe) allocation_probe();
    if (allocation_failures) { --allocation_failures; return NULL; }
    void *p = identity_alloc(n);
    if (p) { allocated_bytes += n; ++allocations; }
    return p;
}''',
        'epd_app_free': '''static void epd_app_free(void *p) {
    if (p) { allocated_bytes -= identity_size(p); identity_free(p); }
}''',
        'epd_app_realloc': '''static void *epd_app_realloc(void *p, size_t n, int kind) {
    void *out = epd_app_alloc(n, kind);
    if (out && p) { size_t old = identity_size(p); memcpy(out, p, old < n ? old : n); epd_app_free(p); }
    return out;
}''',
        'epd_app_psram_available': 'static size_t epd_app_psram_available(void) { return identity_available(); }',
        'rt_tick_from_millisecond': 'static rt_tick_t rt_tick_from_millisecond(unsigned n) { return (rt_tick_t)(((uint64_t)n * RT_TICK_PER_SECOND + 999) / 1000); }',
    }
    for name, code in replacements.items():
        base = replace_once(base, extract(name, base), code)
    base = base.replace('#include <stdbool.h>', '#include "document_runtime.h"\n#include <stdbool.h>', 1)
    (port / 'document_host.h').write_text(base, encoding='utf-8')
    hcpu = (DOC / 'hcpu_host.h').read_text(encoding='utf-8').replace('../books/document_host.h', 'document_host.h')
    (port / 'hcpu_host.h').write_text(hcpu, encoding='utf-8')
    for name in ('app_service.h', 'rtthread.h', 'rthw.h', 'storage.h', 'storage_file.h',
                 'epd_memory.h', 'reader_compute_client.h', 'unistd.h', 'dirent.h',
                 'platform/epd_app.h', 'platform/epd_app_library.h'):
        path = port / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_text('#include "hcpu_host.h"\n', encoding='utf-8')
    stubs = (DOC / 'hcpu_compute_stubs.h').read_text(encoding='utf-8')
    stubs = replace_once(stubs, 'assert(r && !r->active && current_document->image_render == r);',
                        'assert(!"unexpected remote renderer in document identity fixture");')
    (port / 'hcpu_compute_stubs.h').write_text(stubs, encoding='utf-8')
    mocks = (DOC / 'hcpu_test.c').read_text(encoding='utf-8').split('static void pump(void)', 1)[0]
    mocks = replace_once(mocks, 'if (p->cancelled) return EPD_COMPUTE_CANCELLED;',
                         'if (p->cancelled) return EPD_COMPUTE_CANCELLED;\n    if (transport_blocked) return EPD_COMPUTE_MORE;')
    (port / 'document_ports.inc').write_text(mocks, encoding='utf-8')
    (port / 'document_under_test.c').write_bytes(production[0].read_bytes())
    wire = output / 'input.wire'
    events = bytearray()
    for value in ('First source paragraph.', 'Second source paragraph.'):
        text = value.encode()
        length = (32 + len(text) + 80 + 3) & ~3
        events += struct.pack('<8I', 1, length, 0, 0, 0, 0, len(text), 80)
        events += text + bytes(80) + bytes(length - 32 - len(text) - 80)
    wire.write_bytes(events)

    def execute(command, log, cwd, timeout=60):
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, timeout=timeout)
        log.write_bytes(result.stdout + result.stderr)
        if result.returncode:
            print(log.read_text(encoding='utf-8', errors='replace'))
            raise SystemExit(f'{log.name}: exit {result.returncode}')
        return result.stdout.decode('utf-8', errors='replace')

    results = []
    for rate in (1000, 100):
        build = output / str(rate)
        build.mkdir(exist_ok=True)
        flags = [cc, '/nologo', '/std:c11', '/utf-8', '/Od', '/Zi', '/Gy', '/W3', '/fsanitize=address',
                 '/D_CRT_SECURE_NO_WARNINGS', f'/DRT_TICK_PER_SECOND={rate}']
        runtime_includes = [output, HERE, ROOT / 'tests/business_memory/stubs', RESOURCE / 'stubs',
                            ROOT / 'src/platform', ROOT / 'src/ui/platform', ROOT / 'tests/business_memory']
        units = [HERE / 'document_runtime.c', *production[3:]]
        execute([*flags, '/c', '/DEPD_MEMORY_HOST', '/DRT_USING_MEMHEAP_OWNER',
                 '/DRT_USING_DFS_ELMFAT', '/DRT_DFS_ELM_MAX_SECTOR_SIZE=4096', '/DRT_DFS_ELM_USE_LFN=3',
                 '/DRT_DFS_ELM_MAX_LFN=255', '/DRT_DFS_ELM_DRIVES=2',
                 *(f'/I{p}' for p in runtime_includes), *map(str, units)], build / 'runtime-build.log', build)
        includes = [port, HERE, DOC, ROOT / 'modules/books/document', ROOT / 'components/compute/include',
                    ROOT / 'src/platform', ROOT / 'components/compute_reader_render/include',
                    ROOT / 'components/compute_reading_data/include']
        exe = build / 'identity-document.exe'
        kernels = [ROOT / 'components/compute_reader_render' / name for name in ('render_kernel.c', 'render_format.c')]
        execute([*flags, '/DHCPU_DOCUMENT_SOURCE', *(f'/I{p}' for p in includes), str(HERE / 'document_host.c'),
                 *map(str, kernels), *(str(build / (p.stem + '.obj')) for p in units), f'/Fe{exe}',
                 '/link', '/OPT:REF'], build / 'document-build.log', build)
        for case in ('normal', 'cancel_queued', 'cancel_active'):
            traces = []
            for app, peer in (('books', 'reader_probe731'), ('reader_probe731', 'books')):
                work = build / f'{app}-{case}'
                work.mkdir(exist_ok=True)
                text = execute([str(exe), app, case, str(work), str(wire), peer], work / 'run.log', build, 30)
                assert 'PASS identity document' in text and 'errors=0' in text
                traces.append(text.replace(f'app={app}', 'app=IDENTITY'))
                results.append({'app': app, 'peer': peer, 'case': case, 'tick_rate': rate, 'log': str(work / 'run.log')})
                print(text, end='', flush=True)
            assert traces[0] == traces[1], f'Identity-dependent behavior: {case}, {rate}'
    after = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in production}
    assert before == after, 'Production files changed during the test'
    (output / 'result.json').write_text(json.dumps({'passed': len(results), 'identity_pairs': 6,
        'sanitizer': 'MSVC AddressSanitizer', 'production_sha256': before, 'cases': results,
        'boundary': 'Full document.c and real memory/resource/activity; deterministic remote wire, storage and background-policy ports; production spool-reader hit, not cross-app disk-cache sharing.'}, indent=2), encoding='utf-8')
    print(f'PASS {len(results)} document cases, 6 equivalent identity pairs; production unchanged', flush=True)


if __name__ == '__main__':
    main()
