"""Run the production reader worker and frame retirement with real admission."""
from pathlib import Path
import importlib.util
import re
import subprocess
import sys
import tempfile
import argparse
from contextlib import nullcontext

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("reader_extract", HERE / "run.py")
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)
sys.path.insert(0, str(ROOT / "tests/reader_prefetch"))
from cache_run import compiler_environment
cc, env = compiler_environment(None)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT / 'modules/common/reader/reader_view.c')
parser.add_argument('--build-dir', type=Path)
parser.add_argument('--negative', action='store_true')
parser.add_argument('--negative-credit', action='store_true')
parser.add_argument('cases', nargs='*')
args = parser.parse_args()
source = args.source.read_text(encoding='utf-8')
negative = args.negative
functions = ['request_memory_failed', 'worker_retry_fonts', 'command_restores_origin', 'document_flow',
    'neighbor_pending', 'neighbor_index_pending', 'neighbor_choose', 'window_service', 'worker_target_needed',
    'worker_context_pause', 'worker_quiesce', 'worker_input_commit', 'worker_idle_delay', 'worker_request_reject',
    'worker_background_kind', 'worker_near_block', 'worker_near_retry', 'worker_near_resume', 'view_worker']
fixture = (HERE / 'worker.c').read_text(encoding='utf-8').split('int main(void)')[0]
fixture = fixture.replace('#include "activity_fixture.h"', '''#include "epd_activity_work.h"
#include "epd_resource_runtime.h"
#include <assert.h>
#define rt_sem_take fixture_sem_take
#define rt_sem_release fixture_sem_release
#define rt_thread_self fixture_thread_self
#define rt_thread_suspend fixture_thread_suspend
#define rt_schedule fixture_schedule
#define rt_tick_from_millisecond fixture_delay
#define epd_resource_cache_close fixture_cache_close
void retry_host_init(bool); epd_activity_work_t *retry_host_input(void (*)(void *));
void retry_host_poll(unsigned); unsigned retry_host_tasks(void);
void retry_host_pressure(void); void retry_host_release(void); void retry_host_invalidate(unsigned);
epd_memory_owner_t retry_host_owner(void *); bool retry_host_ended(epd_memory_owner_t); void retry_host_finish(void);
void retry_host_recovery(epd_memory_recovery_t *); bool retry_host_waiting(epd_activity_work_t *);
''')
fixture = fixture.replace('static void view_activity_wake(void *unused) { (void)unused; }', 'static void view_activity_wake(void *unused);')
fixture = fixture.replace(reader.extract('view_activity_new', fixture),
    'static epd_activity_work_t *view_activity_new(void) { return retry_host_input(view_activity_wake); }')
fixture = fixture.replace('typedef struct { view_state_t *owner; unsigned pin_run,pin_byte; } view_frame_t;', '''
typedef struct { void *data,*allocation; size_t capacity; } epd_compute_buffer_t;
typedef struct view_frame { struct view_frame *next,*backing; view_state_t *owner;
unsigned pin_run,pin_byte,references; void *renderer,*render_image,*pins; epd_compute_buffer_t pixels;
size_t window_charge; epd_memory_recovery_t admission_recovery;
const char *admission_stage; bool allocation_attempted;
bool memory_reclaimed,memory_waiting,cold_reusing,complete; } view_frame_t;''')
fixture = fixture.replace('view_page_t *display,*index,', 'view_frame_t *frames; unsigned references; view_page_t *display,*index,')
fixture = fixture.replace('view_frame_t *frames; unsigned references;',
    'epd_memory_recovery_t init_recovery,image_recovery; view_frame_t *frames; unsigned references;')
for name in ('rt_sem_take', 'frame_prepare', 'frame_release', 'render_cleanup', 'cluster_retire'):
    fixture = fixture.replace(reader.extract(name, fixture), '')
fixture = fixture.replace('static void rt_sem_release(int sem) { (void)sem;stopped=true; }',
    'static unsigned wake_releases; static void rt_sem_release(int sem) { if(sem==1)++wake_releases;else {assert(sem==2);stopped=true;} }')
fixture = fixture.replace('allocation_activity->kind', 'fixture_activity_kind(allocation_activity)')
fixture = fixture.replace('static reader_work_t core_step', '''static epd_resource_kind_t fixture_activity_kind(epd_activity_work_t *w) {
epd_resource_activity_snapshot_t s;assert(epd_resource_activity_get(epd_activity_work_activity(w),&s)==EPD_RESOURCE_OK);return s.requested.kind;}
static reader_work_t core_step''')
fixture = fixture.replace('/* PRODUCTION */', (HERE / 'near_retry_worker.inc').read_text(encoding='utf-8') + '\n/* PRODUCTION */')
fixture = fixture.replace('/* PRODUCTION */', (HERE / 'credit_worker41.inc').read_text(encoding='utf-8') + '\n/* PRODUCTION */')
fixture = fixture.replace('#define RT_WAITING_FOREVER ((unsigned)-1)', '#undef RT_WAITING_FOREVER\n#define RT_WAITING_FOREVER ((unsigned)-1)')
fixture = re.sub(r'\bretired\b', 'retired_count', fixture)
fixture = fixture.replace('static void state_retire(', 'static void frame_release(view_frame_t *);\nstatic void state_retire(')
fixture = fixture.replace('static unsigned rt_tick_from_millisecond',
    'static void rt_sem_take(int,unsigned);\nstatic void frame_prepare(void);\nstatic bool render_cleanup(void);\nstatic void cluster_retire(view_state_t *);\nstatic unsigned rt_tick_from_millisecond')
code = re.search(r'typedef struct \{\s*epd_resource_activity_t parent, retiring;[\s\S]*?\} view_near_retry_t;', source)[0] + '\n'
code += 'static uint32_t input_sequence;\n' + reader.extract('view_input_set', source) + '\n'
extra = ('worker_recovery_continue', 'worker_credit_changed') if 'static void worker_credit_changed(' in source else ()
code += '\n'.join(reader.extract(name, source) for name in ('wake_view', 'view_activity_wake', 'frame_release', 'render_cleanup', *extra, *functions))
if negative:
    code = code.replace('worker_near_retry(&near_retry, current, activity)', 'false')
fixture = fixture.replace('/* PRODUCTION */', code)
fixture += (HERE / 'near_retry_main.inc').read_text(encoding='utf-8')
if args.build_dir: args.build_dir.mkdir(parents=True, exist_ok=True)
with (nullcontext(str(args.build_dir.resolve())) if args.build_dir else tempfile.TemporaryDirectory(prefix='reader-retry-worker-')) as directory:
    build = Path(directory)
    support = (ROOT / 'tests/business_memory/activity_host.c').read_text(encoding='utf-8')
    start = support.index('/* Synchronous activity tests')
    end = support.index('static size_t physical', start)
    support = support[:start] + (ROOT / 'tests/unified_resource/queued_background.inc').read_text(encoding='utf-8') + support[end:]
    support = support.replace('int rt_sem_release(struct rt_semaphore *s) {', 'int rt_sem_release(struct rt_semaphore *s) { if(s==&sdk_heap.lock)return 0;')
    support = support.replace('int rt_sem_take(struct rt_semaphore *s, int timeout) {',
        'int rt_sem_take(struct rt_semaphore *s, int timeout) { if(s==&sdk_heap.lock)return 0;')
    support = support.replace('physical[region] += bytes;', 'physical[region] += bytes; sdk_heap.available_size=CAPACITY-physical[1];')
    support = support.replace('physical[region] -= blocks[i].bytes;', 'physical[region] -= blocks[i].bytes; sdk_heap.available_size=CAPACITY-physical[1];')
    (build / 'core_support.inc').write_text(support[:support.index('int main(int argc, char **argv)')], encoding='utf-8')
    (build / 'realloc.inc').write_text(reader.extract('epd_app_realloc', (HERE / 'near_retry_probe.c').read_text()), encoding='utf-8')
    (build / 'worker.c').write_text(fixture, encoding='utf-8')
    sdk = (ROOT / 'src/platform/epd_memory_sdk.c').read_text(encoding='utf-8')
    sdk_code = '\n'.join(reader.extract(n,sdk) for n in ('recovery_fragmented','recovery_step',
        'epd_memory_recovery_begin','epd_memory_recovery_poll','epd_memory_recovery_continue','epd_memory_recovery_failed'))
    (build / 'recovery_sdk.inc').write_text('#define app_image_psram_memheap sdk_heap\n#define rt_memheap_item sdk_item\n'+sdk_code+
        '\n#undef rt_memheap_item\n#undef app_image_psram_memheap\n',encoding='utf-8')
    includes = [build, ROOT / 'tests/business_memory/stubs', ROOT / 'tests/unified_resource/stubs', ROOT / 'src/platform']
    sources = [build / 'worker.c', HERE / 'near_retry_core.c', *(ROOT / 'src/platform' / name for name in (
        'epd_memory_manager.c', 'epd_resource.c', 'epd_resource_runtime.c', 'epd_activity_work.c'))]
    for rate in (1000, 100):
        exe = build / f'worker-{rate}.exe'
        subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/TC', '/wd4200', '/wd4389', '/wd4505', '/wd4459', '/Od', '/Zi', '/fsanitize=address',
            '/D_CRT_SECURE_NO_WARNINGS', '/DEPD_MEMORY_HOST', '/DRT_USING_MEMHEAP_OWNER', f'/DRT_TICK_PER_SECOND={rate}',
            *(f'/I{p}' for p in includes), *map(str, sources), f'/Fe{exe}'], cwd=build, env=env, check=True)
        for case in args.cases or ('recover', 'sram', 'steady', 'generation', 'close', 'cancel', 'font', 'raster', 'early_font', 'early_raster', 'credit', 'credit_deadline'):
            result = subprocess.run([str(exe), case], cwd=build, env=env, capture_output=True, timeout=15)
            output=result.stdout+result.stderr
            (build / f'{case}-{rate}.log').write_bytes(output)
            print(output.decode(errors='replace'),end='',flush=True)
            if args.negative_credit:
                assert result.returncode and b'CREDIT_REPLACEMENT_STRANDED' in output, 'negative control missed old credit'
            elif result.returncode: raise SystemExit(result.returncode)
