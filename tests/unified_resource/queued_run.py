"""Run the real queued activity helper, resource runtime and core ledger."""
from pathlib import Path
import subprocess, sys, tempfile, re
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment
sys.path.insert(0, str(ROOT / 'tests/reader_view_async'))
from run import extract
cc, env = compiler_environment(None)
cases = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='queued-', dir=HERE) as folder:
    folder = Path(folder)
    support = (ROOT / 'tests/business_memory/activity_host.c').read_text(encoding='utf-8')
    first = support.index('/* Synchronous activity tests')
    last = support.index('static size_t physical', first)
    support = support[:first] + (HERE / 'queued_background.inc').read_text(encoding='utf-8') + support[last:]
    support = support[:support.index('int main(int argc, char **argv)')]
    (folder / 'queued_support.inc').write_text(support, encoding='utf-8')
    sdk=(ROOT / 'src/platform/epd_memory_sdk.c').read_text(encoding='utf-8')
    (folder / 'memory_peak.inc').write_text(extract('epd_app_allocation_peak',sdk),encoding='utf-8')
    doc=(ROOT / 'modules/books/document/document.h').read_text(encoding='utf-8')
    view=(ROOT / 'modules/common/reader/reader_view.c').read_text(encoding='utf-8')
    types=re.search(r'^#define DOCUMENT_TEXT_LIMIT .+$',doc,re.M)[0]+'\n'
    for name in ('document_record_t','document_image_t'):
        types+=re.search(r'typedef struct \{[^{}]*\} '+name+r';',doc)[0]+'\n'
    types+='typedef struct document_record_request document_record_request_t;\n'
    types+=re.search(r'typedef struct \{[^{}]*\} view_page_t;',view)[0]
    (folder / 'reader_peak_types.inc').write_text(types,encoding='utf-8')
    (folder / 'reader_peak_functions.inc').write_text('\n'.join(extract(n,view) for n in
        ('view_allocate','view_resize','core_initialize_release','core_initialize_recover',
         'core_initialize_failed','core_initialize')),encoding='utf-8')
    (folder / 'reader_near_functions.inc').write_text('\n'.join(extract(n,view) for n in
        ('optional_pixels_ready','optional_pixels_ready_for','optional_pixels_block')),encoding='utf-8')
    document_source = (ROOT / 'modules/books/document/document.c').read_text(encoding='utf-8')
    (folder / 'document_reject_functions.inc').write_text('\n'.join(extract(n, document_source) for n in
        ('chapter_restore_required', 'document_work_rejected')), encoding='utf-8')
    (folder / 'document_open_functions.inc').write_text('\n'.join(extract(n, document_source) for n in
        ('open_pressure', 'reader_file_reserve', 'reader_file_open_attempt',
         'reader_file_open_status', 'reader_file_open')), encoding='utf-8')
    directory = ('\n'.join(extract(n, document_source) for n in
        ('reader_directory_attempt', 'reader_directory_create')) if 'static bool reader_directory_create(' in document_source
        else '#define reader_directory_create(d,p) storage_mkdirs(p)')
    (folder / 'document_directory_functions.inc').write_text(directory, encoding='utf-8')
    record_types = '\n'.join(re.search(r'typedef struct \{[^{}]*\} '+n+r';', doc)[0]
        for n in ('document_record_data_t', 'document_progress_t'))
    record_types += '\n' + re.search(r'struct document_record_request \{[^{}]*\};', document_source)[0]
    (folder / 'record_queue_types.inc').write_text(record_types, encoding='utf-8')
    (folder / 'record_queue_functions.inc').write_text('\n'.join(extract(n, document_source) for n in
        ('document_activity_wake', 'document_activity_queue', 'document_activity_new', 'record_request', 'document_record_request',
         'document_record_take', 'document_record_result', 'document_record_cancel', 'record_collect', 'record_seek_step',
         'record_work_priority', 'record_work')), encoding='utf-8')
    phases = []
    for name, filename, function, jobtype in (
        ('cover', 'cover_cache.c', 'books_cover_cache_worker_step', 'cover_job_t'),
        ('entry', 'reader_entry_store.inc', 'entry_worker_step', 'entry_store_job_t')):
        source = (ROOT / 'modules/books' / filename).read_text(encoding='utf-8')
        worker = extract(function, source)
        # Keep the production admission and entire OPEN-through-cleanup state
        # machine; queue/UI selection is covered by the component fixtures.
        phases.append(f'static bool {name}_phase_step({jobtype} *job) {{\n'
            'epd_activity_guard_t guard; bool entered = false; rt_base_t level; epd_service_t *service = NULL;\n' +
            worker[worker.index('    int admission ='):])
    (folder / 'io_phase_functions.inc').write_text('\n'.join(phases), encoding='utf-8')
    dfs = ROOT / 'SiFli-SDK/rtos/rtthread/components/dfs'
    source = (dfs / 'src/dfs.c').read_text(encoding='utf-8')
    start = source.index('static int fd_alloc(')
    end = source.index('\n/**', start)
    descriptor = (dfs / 'include/dfs_file.h').read_text(encoding='utf-8')
    first = descriptor.index('struct dfs_fd\n{')
    last = descriptor.index('\n};', first) + 3
    (folder / 'dfs_alloc.inc').write_text(descriptor[first:last] + '\n' + source[start:end], encoding='utf-8')
    (folder / 'dfs_file.h').write_text('#include <sys/types.h>\n'+descriptor[first:last],encoding='utf-8')
    posix=(dfs / 'include/dfs_posix.h').read_text(encoding='utf-8')
    directory=re.search(r'typedef struct\s*\{[^{}]*\} DIR;',posix)[0]
    (folder / 'dfs_posix.h').write_text('#include "dfs_file.h"\n'+directory,encoding='utf-8')
    for name in ('ff.h','ffconf.h'):
        (folder/name).write_bytes((dfs/'filesystems/elmfat'/name).read_bytes())
    (folder/'rtdef.h').write_text('#include "rtthread.h"\ntypedef struct rt_mutex *rt_mutex_t;\n',encoding='utf-8')
    includes = [folder, ROOT / 'tests/business_memory/stubs', HERE / 'stubs', ROOT / 'src/platform']
    sources = [HERE / 'queued_host.c', *(ROOT / 'src/platform' / s for s in (
        'epd_memory_manager.c', 'epd_resource.c', 'epd_resource_runtime.c', 'epd_activity_work.c'))]
    for rate in (1000, 100):
        exe = folder / f'queued-{rate}.exe'
        subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/TC', '/wd4200', '/wd4389',
            '/wd4505', '/Od', '/Zi', '/fsanitize=address', '/D_CRT_SECURE_NO_WARNINGS', '/DEPD_MEMORY_HOST', '/DRT_USING_MEMHEAP_OWNER',
            '/DRT_USING_DFS_ELMFAT', '/DRT_DFS_ELM_MAX_SECTOR_SIZE=4096', '/DRT_DFS_ELM_USE_LFN=3',
            '/DRT_DFS_ELM_MAX_LFN=255', '/DRT_DFS_ELM_DRIVES=2',
            f'/DRT_TICK_PER_SECOND={rate}', *(f'/I{p}' for p in includes), *map(str, sources), f'/Fe{exe}'],
            cwd=folder, env=env, check=True, timeout=60)
        for case in cases or ('ui', 'nested', 'capacity', 'sram', 'joint', 'page', 'proof', 'references', 'policy', 'allocation',
                             'dfs_zero', 'dfs_budget', 'dfs_closed_table', 'dfs_full', 'dfs_fil_peak',
                             'reader_start', 'reader_resize', 'io_cover', 'io_entry', 'io_document', 'io_document_multiple',
                             'io_directory', 'record_background', 'record_foreground', 'record_cancel',
                             'record_denied', 'record_error', 'record_sram_reuse', 'record_open_recover',
                             'record_open_exhausted', 'record_open_error', 'record_reader_oom', 'record_seek_oom',
                             'record_competing_release', 'record_competing_cancel',
                             'record_competing_timeout_retry', 'record_seek_competing_release',
                             'near_epoch', 'retire_auto',
                             'retire_pending', 'retire_notice', 'retire_reuse', 'retire_payload',
                             'prepare_psram', 'prepare_sram', 'prepare_timeout', 'prepare_close',
                             'prepare_optional', 'prepare_optional_initial',
                             'document_optional', 'document_initial', 'document_navigation', 'document_restore'):
            subprocess.run([str(exe), case], cwd=folder, env=env, check=True, timeout=20)
