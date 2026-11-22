"""Exercise the seven-context reader window with real document/page supply under MSVC ASan.

Use --source to select another reader_view.c. Each --without-* negative control
must fail its corresponding assertion in the same C fixture.
"""
import argparse
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SUPPORT = ROOT / 'tests/reader_document_compute'
sys.path.insert(0, str(SUPPORT))
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from hcpu_reader_extract import extract
from cache_run import compiler_environment

spec = importlib.util.spec_from_file_location('view_extract', ROOT / 'tests/reader_view_async/run.py')
view_extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(view_extract)


def function(source, name):
    return view_extract.extract(name, source)


def prepare(build, reader, negative):
    snapshot = build / 'reader_under_test.c'
    snapshot.write_bytes(reader.read_bytes())
    extract(snapshot, build)
    source = snapshot.read_text(encoding='utf-8')
    activity = (ROOT / 'tests/unified_resource/activity_fixture.h').read_text(encoding='utf-8')
    activity = activity.replace(function(activity, 'epd_resource_context_reserve'),
        'int epd_resource_context_reserve(epd_memory_region_t region, size_t bytes);')
    (build / 'activity_fixture.h').write_text(activity, encoding='utf-8')
    pages = (build / 'reader_page_functions.inc').read_text(encoding='utf-8')
    extra = '\n'.join(function(source,n) for n in ('preview_visible','request_memory_failed',
        'page_required','page_work_allowed','page_memory_block','view_input_set'))
    pages = pages.replace(function(source,'page_error'), extra+'\n'+function(source,'page_error'))
    (build / 'reader_page_functions.inc').write_text(pages, encoding='utf-8')
    worker = function(source, 'view_worker')
    assert worker.index('command_apply(command, &current)') < worker.index('window_quiesce(current)') < worker.index('worker_quiesce(current,')
    for name in ('app_service.h', 'rtthread.h', 'rthw.h', 'storage.h',
                 'storage_file.h', 'epd_memory.h', 'reader_compute_client.h',
                 'unistd.h', 'dirent.h', 'platform/epd_app.h', 'platform/epd_app_library.h'):
        header = build / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "hcpu_host.h"\n', encoding='utf-8')

    host = (SUPPORT / 'hcpu_test.c').read_text(encoding='utf-8')
    host = '#include "epd_reader_render.h"\n' + host
    host = host.split('static void pump(void)', 1)[0] + '\n' + function(host, 'pump')
    picture = (SUPPORT / 'hcpu_picture_position.h').read_text(encoding='utf-8')
    event = function(picture, 'picture_event')
    event = event.replace('e.bytes = (sizeof(e) + length + 3u) & ~3u;',
                          'e.length2 = type == BOOK_DOCUMENT_TEXT || type == BOOK_DOCUMENT_PARAGRAPH ? sizeof(epd_paragraph_style_t) : 0;\n'
                          '    e.bytes = (sizeof(e) + length + e.length2 + 3u) & ~3u;')
    host += '\n' + event + '\n' + function(picture, 'picture_pixels')
    host = host.replace('    *out = calloc(1, sizeof(**out));', '''    for(size_t at=0;at<wire_bytes;) {
        book_document_event_t *e=(book_document_event_t*)(wire_data+at);
        if(e->type==BOOK_DOCUMENT_SOURCE_ANCHOR || e->type==BOOK_DOCUMENT_CHAPTER)e->a=initial->values[4];
        if(e->type==BOOK_DOCUMENT_SOURCE_ANCHOR) {
            /* Reused wire must publish the requested boundary on every open. */
            e->d&=~(BOOK_DOCUMENT_SOURCE_TARGET|BOOK_DOCUMENT_SOURCE_TARGET_BEGIN);
            if(initial->values[5]==UINT32_MAX ? (e->d&BOOK_DOCUMENT_SOURCE_END) :
                !e->b && !(e->d&(BOOK_DOCUMENT_SOURCE_BEGIN|BOOK_DOCUMENT_SOURCE_END)))
                e->d|=BOOK_DOCUMENT_SOURCE_TARGET;
        }
        if(e->type==BOOK_DOCUMENT_TEXT) {
            char text[15];snprintf(text,sizeof(text),"chapter-%06u",initial->values[4]);
            memcpy(e+1,text,14);
        }
        at+=e->bytes;
    }
    *out = calloc(1, sizeof(**out));''')
    host += '\nstatic void rt_enter_critical(void) { rt_hw_interrupt_disable(); }\n'
    host += 'static void rt_exit_critical(void) { rt_hw_interrupt_enable(0); }\n'
    (build / 'host.inc').write_text(host, encoding='utf-8')
    compute_stubs = (SUPPORT / 'hcpu_compute_stubs.h').read_text(encoding='utf-8')
    # Image rendering moved out of document. This text-only window fixture
    # must fail if that obsolete document-owned path is invoked.
    compute_stubs = compute_stubs.replace('current_document->image_render == r',
                                         '!"unexpected document-owned image render"')
    (build / 'hcpu_compute_stubs.h').write_text(compute_stubs, encoding='utf-8')
    (build / 'document_under_test.c').write_bytes((ROOT / 'modules/books/document/document.c').read_bytes())

    flow = (SUPPORT / 'hcpu_page_flow.h').read_text(encoding='utf-8').split('static view_page_t flow_pages[3];', 1)[0]
    client = (ROOT / 'modules/common/reader/compute/reader_client.c').read_text(encoding='utf-8')
    (build / 'compute_client_under_test.c').write_text(client, encoding='utf-8')
    job = re.search(r'struct reader_compute_job \{.*?\n\};', client, re.S).group()
    flow = re.sub(r'typedef struct reader_compute_job \{.*?\} reader_compute_job_t;',
                  'typedef struct reader_compute_job reader_compute_job_t;\n' + job, flow, count=1, flags=re.S)
    pool = re.search(r'^#define READER_JOBS .*$', client, re.M).group() + '''
#define RT_ASSERT assert
static bool pool_stopping;
static epd_service_t *pool_owner;
static unsigned pool_peak, pool_busy;
int epd_compute_query(const char *name, uint32_t version, epd_compute_capability_t *capability)
{
    assert(!strcmp(name, EPD_READER_CAPABILITY) && version == EPD_READER_VERSION);
    memset(capability, 0, sizeof(*capability));
    capability->ready = true; capability->registration.step_limit = 1;
    return EPD_COMPUTE_OK;
}
#define jobs flow_jobs
#define stopping pool_stopping
#define owner pool_owner
''' + function(client, 'reader_compute_release') + '\n' + function(client, 'reader_compute_create').replace(
        'reader_compute_create(', 'pool_create(', 1) + '''
#undef jobs
#undef stopping
#undef owner
static reader_compute_job_t *reader_compute_create(int *result)
{
    reader_compute_job_t *job = pool_create(result);
    if (*result == EPD_COMPUTE_BUSY) ++pool_busy;
    assert(flow_jobs <= READER_JOBS);
    if (flow_jobs > pool_peak) pool_peak = flow_jobs;
    return job;
}
'''
    flow = flow.replace(function(flow, 'reader_compute_create'), pool)
    # The production release routine above owns the job ledger and host allocation.
    release = function((SUPPORT / 'hcpu_page_flow.h').read_text(encoding='utf-8'), 'reader_compute_release')
    flow = flow.replace(release, '')
    flow = flow.replace('static struct {\n    document_t *document;', '''struct view_frame;
typedef struct view_state {
    epd_memory_recovery_t init_recovery;
    uint32_t init_blocked_epoch,window_attempt_epoch,neighbor_epoch,input_order;
    unsigned request_failure,request_failure_intent,font_blocked_intent;
    bool init_alloc_retried,input_enabled,font_memory_blocked,pixels_retry_ready;
    struct view_state *previous,*following,*next;
    struct view_frame *frames,*rendering;
    view_page_t *display;
    document_reader_t *page_reader;
    document_cursor_t *pages,initial_anchor;
    document_cursor_t counting_origin[1];char title[96];rt_tick_t turn_started;
    unsigned prefetch_record;
    document_t *prefetch_document;
    unsigned prefetch_layout,prefetch_session,sparse_next,sparse_goal;
    int deferred_turns;unsigned link_scan,glyph_steps;
    bool trace_images_ready,trace_font_wait,glyph_turn,trace_published;
    rt_tick_t glyph_ticks;
    document_cursor_t forward_from,numbered_cursor;
    unsigned references,layout,capacity,failure,segment_turns,cache_blocked,init_blocked_intent,toc_capacity;
    unsigned navigation_link,navigation_toc,published_intent;
    bool parked,counting_only,allow_glyphs,frame_published;
    bool boundary_target,notified;document_cursor_t boundary_anchor;
    unsigned chapter_percent;document_record_request_t *progress_request;
    struct {unsigned record,level;} *toc;
    void *images,*back_request;
    lv_font_t *fallbacks[3],*retired[6],*retired_oblique[3];
    size_t init_blocked_available;
    int direction;
    bool initialized,init_attempted,init_blocked,target_pending,display_pending,displayed,backward_target;
    bool preview_back_blocked,pixels_blocked,pixels_memory_blocked;
    unsigned pixels_blocked_intent,pixels_blocked_priority;
    size_t pixels_blocked_available,pixels_blocked_bytes;
    size_t window_state_charge,window_workspace_charge,window_attempt_available;
    unsigned window_attempts,window_attempt_intent;
    unsigned neighbor_attempts,neighbor_publication,neighbor_intent;
    uint32_t window_probe_revision;
    size_t neighbor_available;
    bool work_exhausted,initial_target_ready,sparse_seek,forward_step,preview_back_ready;
    bool near_tracing;
    rt_tick_t near_started,near_service_ticks;
    unsigned near_intent,near_number,near_steps;
    bool preview_back_seeking,preview_back_failed,trace_hit;
    unsigned sparse_stride,numbered_page,display_layout,back_request_offset;
    unsigned preview_back_scan,preview_back_low,preview_back_headers,preview_back_base;
    rt_tick_t preview_back_started,preview_back_seek_ticks;
    document_cursor_t anchor;
    struct { unsigned page; } saved_position;
    int turn_remaining;
    reader_page_cache_t cache;
    document_t *document;''')
    flow = flow.replace('} view;', '} view_state_t;\nstatic view_state_t *state,*primary_state;\n#define view (*state)')
    if 'VIEW_VISITED_PAGES' in source:
        visited = re.search(r'^#define VIEW_VISITED_PAGES .+$', source, re.M)[0]
        visited += '\n' + re.search(r'typedef struct \{\s*document_cursor_t start, end;\s*uint32_t layout;\s*bool eof;\s*\} view_visited_page_t;', source)[0]
        flow = flow.replace('typedef struct view_state {', visited + '\ntypedef struct view_state {\n'
                            '    view_visited_page_t visited[VIEW_VISITED_PAGES];\n'
                            '    unsigned visited_count,visited_next;\n'
                            '    bool preview_back_exact,preview_back_eof;')
    flow = flow.replace('bool ready; } status;', 'bool ready,indexing; const char *title,*text; } status;')
    flow = flow.replace('static unsigned flow_jobs, flow_eofs;', '''#include "reader_page_cache.h"
static unsigned flow_jobs, flow_eofs;
static bool hold_glyphs;
static int epd_app_font_set_purpose(const lv_font_t *font,unsigned purpose) { (void)font;(void)purpose;return 1; }''')
    flow = flow.replace('(void)font; (void)letter; (void)next; (void)pixels; return 1;',
                        '(void)font; (void)letter; (void)next; (void)pixels; return !hold_glyphs;')
    flow = flow.replace('static bool near_frames_pending(void) { return flow_near_pending; }',
                        'static bool near_frames_pending(void);')
    flow = flow.replace('#define compute_cancel flow_compute_cancel',
                        '#define VIEW_LAYOUT_AHEAD 4u\nstatic uint32_t input_sequence;\n'
                        'static bool view_interrupted(void) { return false; }\n'
                        'static reader_compute_job_t *compute_acquire(int *result);\n'
                        + function(source, 'near_trace_finish') + '\n'
                        '#define compute_cancel flow_compute_cancel')
    (build / 'flow.inc').write_text(flow, encoding='utf-8')

    groups = {
        'window_frames': ('frame_admission_error','frame_workspace_ready','frame_storage_minimum', 'frame_bitmap_less_useful',
                          'frame_trace_note', 'frame_drop_note', 'frame_handoff_note',
                          'frame_priority', 'frame_distance', 'frame_retention_distance', 'frame_reclaim_note', 'frame_ref', 'frame_release',
                          'frame_find', 'frame_window_contains', 'frame_trim', 'frame_create', 'frame_reclaim_layout',
                          'frame_reclaim_pages', 'frame_reserve', 'frame_pixel_victim', 'frame_reuse_pixels', 'frame_allocate_pixels', 'optional_pixels_ready',
                          'optional_pixels_ready_for', 'optional_pixels_block', 'frame_adopt'),
        'window_prepare_frames': ('frame_prepare', 'near_frames_pending'),
        'window_ownership': ('window_uncharge', 'window_clear', 'window_store', 'window_shift',
                             'window_work_allowed','window_pixels_pending_for','window_pixels_pending',
                             'window_runnable','window_conversion_document','window_conversion_ready', 'window_prepare'),
        'lifecycle': ('navigation_clear', 'link_clear', 'frame_cache_clear', 'core_release',
                      'state_retire', 'state_dispose', 'cluster_retire', 'chapter_select',
                      'worker_target_needed', 'core_background_pause', 'worker_context_pause',
                      'window_quiesce', 'worker_quiesce','worker_input_commit'),
        'reclaim': ('window_reclaim', 'window_admit'),
        'compute_acquire': ('compute_acquire',),
        'near_trace': ('near_page_step',),
        'backward': ('cursor_before', 'preview_start', 'preview_back_discard', 'preview_back_stop',
                     'preview_back_begin', 'preview_back_seek_step', 'preview_back_finish', 'target_resolve'),
        'idle': ('worker_idle_delay',),
        'initialize': ('view_allocate','view_resize','cache_allocate', 'cache_free', 'reserve_pages',
                       'core_initialize_release','core_initialize_recover','core_initialize_failed','core_initialize'),
        'neighbor_step': ('neighbor_step',),
        'window_service': ('neighbor_pending','neighbor_index_pending', 'neighbor_choose', 'neighbors_prepare', 'window_service'),
    }
    for name, functions in groups.items():
        if name == 'backward' and 'VIEW_VISITED_PAGES' in source:
            functions = (functions[0], 'visited_clear', 'visited_record', 'visited_find', 'visited_forget', *functions[1:])
        code = '\n\n'.join(function(source, entry) for entry in functions)
        if name == 'window_frames':
            code = '#define frame_pixels_free(pixels, reason, session, intent) epd_compute_buffer_free(pixels)\n' + code
        if name == 'neighbor_step':
            code = code.replace('neighbor_step(', 'production_neighbor_step(', 1)
        if name == 'window_ownership' and negative == 'window':
            code = code.replace(function(source, 'window_prepare'),
                                'static bool window_prepare(view_state_t *current) { (void)current;return false; }')
        if name == 'window_ownership' and negative == 'entry':
            code = code.replace('if (owner == current) continue;', 'if (!window_extra(owner)) continue;')
        if name == 'window_ownership' and negative == 'conversion':
            code = code.replace(function(source, 'window_conversion_ready'),
                                'static bool window_conversion_ready(view_state_t *current) { (void)current;return true; }')
        if name == 'window_ownership' and negative == 'resume':
            assert code.count('view_input_set(contexts[i], true);') == 1
            code = code.replace('view_input_set(contexts[i], true);', '(void)pending;')
        if name == 'window_service' and negative == 'success-resume':
            guarded = 'if (window_prepare(current)) {\n        window_conversion_ready(current);\n        return;\n    }'
            assert code.count(guarded) == 1, 'Missing the successful-window flow restoration branch'
            code = code.replace(guarded, 'if (window_prepare(current)) return;', 1)
        if name == 'compute_acquire' and negative == 'pool':
            code = 'static reader_compute_job_t *compute_acquire(int *result) { return reader_compute_create(result); }'
        if name == 'lifecycle' and negative == 'sweep':
            code = code.replace(function(source, 'window_quiesce'),
                                'static void window_quiesce(view_state_t *current) { (void)current; }')
        (build / (name + '.inc')).write_text(code, encoding='utf-8')
    helpers = source[source.index('/* Four off-screen slots'):source.index('static void core_seek(unsigned page);')]
    if negative == 'tail':
        helpers = helpers.replace(function(source, 'window_end'),
                                  'static bool window_end(const view_state_t *owner, document_cursor_t cursor, bool eof) '
                                  '{ (void)owner;(void)cursor;return eof; }')
    (build / 'window_helpers.inc').write_text(helpers, encoding='utf-8')

    # Quoted includes must resolve beside these freshly generated fragments.
    test = (HERE / 'global_test.c').read_text(encoding='utf-8')
    settings = (ROOT / 'src/ui/ui_settings.h').read_text(encoding='utf-8')
    test = re.search(r'^#define UI_READING_FONT_SIZES .+$', settings, re.M)[0] + '\n' + test
    font = (ROOT / 'src/ui/platform/epd_app_font.h').read_text(encoding='utf-8')
    test = re.search(r'^#define EPD_APP_FONT_NO_MEMORY .+$',font,re.M)[0]+'\n'+test
    test = re.search(r'^#define VIEW_RENDER_WORKSPACE .+$',source,re.M)[0]+'\n'+test
    (build/'memory_room.inc').write_text(function(source,'view_memory_room'),encoding='utf-8')
    reserve = re.search(r'^#define VIEW_RENDER_RESERVE .*$', source, re.M)
    if reserve: test = reserve.group() + '\n' + test
    (build / 'global_test.c').write_text(test, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'modules/common/reader/reader_view.c')
    controls = parser.add_mutually_exclusive_group()
    controls.add_argument('--without-window', action='store_true', help='Require the missing third frame to be detected')
    controls.add_argument('--without-pool-transfer', action='store_true', help='Require starvation at four live jobs to be detected')
    controls.add_argument('--without-tail-boundary', action='store_true', help='Require the clipped reverse tail to be rejected')
    controls.add_argument('--without-idle-sweep', action='store_true', help='Require retained extra-slot jobs to be detected')
    controls.add_argument('--without-direct-entry', action='store_true', help='Require a cancelled direct entry to be rebuilt before farther extras')
    controls.add_argument('--without-conversion-guard', action='store_true', help='Require an unfinished opposite conversion to survive admission')
    controls.add_argument('--without-conversion-resume', action='store_true', help='Require a paused converter to reach EOF while admission waits')
    controls.add_argument('--without-success-resume', action='store_true', help='Require successful window work to restore the active converter gate')
    parser.add_argument('--case', choices=('all', 'retention', 'window', 'waiting-readable', 'waiting-zero',
                                           'ready-neighbor', 'near-neighbor-priority'), default='all')
    parser.add_argument('--build-dir', type=Path, help='Keep generated fragments, logs and results in this directory')
    args = parser.parse_args()
    compiler, env = compiler_environment(None)
    if Path(compiler).stem.lower() != 'cl':
        parser.error('The Win32 document host requires MSVC with AddressSanitizer.')
    negative = ('window' if args.without_window else 'pool' if args.without_pool_transfer else
                'tail' if args.without_tail_boundary else 'sweep' if args.without_idle_sweep else
                'entry' if args.without_direct_entry else 'conversion' if args.without_conversion_guard else
                'resume' if args.without_conversion_resume else 'success-resume' if args.without_success_resume else None)
    build = (args.build_dir or ROOT / 'tmp/reader-global-window-regression' /
             (f'negative-{negative}-host' if negative else 'global-host')).resolve()
    build.mkdir(parents=True, exist_ok=True)
    reader = args.source.resolve()
    prepare(build, reader, negative)
    includes = [build, SUPPORT, ROOT / 'modules/books/document', ROOT / 'components/compute/include',
                ROOT / 'src/platform', ROOT / 'tests/unified_resource', ROOT / 'tests/unified_resource/stubs',
                ROOT / 'components/compute_reader_render/include',
                ROOT / 'components/compute_reading_data/include',
                ROOT / 'modules/common/reader/compute/include', ROOT / 'modules/common/reader']
    units = [build / 'global_test.c', ROOT / 'src/platform/epd_resource.c',
             ROOT / 'components/compute_reader_render/render_kernel.c',
             ROOT / 'components/compute_reader_render/render_format.c',
             ROOT / 'modules/common/reader/compute/reader_kernel.c',
             ROOT / 'modules/common/reader/compute/reader_page.c',
             ROOT / 'modules/common/reader/reader_page_cache.c']
    exe = build / 'test.exe'
    command = [compiler, '/nologo', '/std:c11', '/utf-8', '/Od', '/Zi', '/Gy', '/W3',
               '/we4013', '/we4047', '/we4024',
               '/D_CRT_SECURE_NO_WARNINGS', '/DHCPU_DOCUMENT_SOURCE', '/fsanitize=address',
               *[f'/I{path}' for path in includes], *map(str, units), f'/Fe{exe}', '/link', '/OPT:REF']
    compiled = subprocess.run(command, cwd=build, env=env, capture_output=True, timeout=60)
    (build / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise SystemExit((compiled.stdout + compiled.stderr).decode(errors='replace'))
    run = subprocess.run([str(exe), str(build), args.case], cwd=build, env=env, capture_output=True, timeout=60)
    output = run.stdout + run.stderr
    (build / 'result.log').write_bytes(output)
    print(output.decode(errors='replace'), end='', flush=True)
    result = dict(exit_code=run.returncode, test_case=args.case, reader_source=str(reader),
                  reader_sha256=hashlib.sha256((build / 'reader_under_test.c').read_bytes()).hexdigest(),
                  compute_client_sha256=hashlib.sha256((build / 'compute_client_under_test.c').read_bytes()).hexdigest(),
                  document_sha256=hashlib.sha256((build / 'document_under_test.c').read_bytes()).hexdigest())
    if negative:
        markers = {'window': (b'route.count==3',),
                   'pool': (b'POOL_STALL jobs=4', b'owner->frames && owner->frames->complete'),
                   'tail': (b'route.count==3 && created==7',),
                   'sweep': (b'IDLE_JOBS_RETAINED', b'flow_jobs==0'),
                   'entry': (b'DIRECT_ENTRY_MISSING', b'next->frames'),
                   'conversion': (b'!turn && !optional_counter_steps',),
                   'resume': (b'CONVERSION_STRANDED', b'progress.complete'),
                   'success-resume': (b'WINDOW_SUCCESS_STALL', b'b_progress.complete')}
        detected = run.returncode == 1 and all(marker in output for marker in markers[negative])
        result['negative_control'] = negative
        result['expected_failure_detected'] = detected
    else:
        detected = run.returncode == 0
    (build / 'results.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    if not detected:
        raise SystemExit('FAIL global reader window ' + (f'{negative} negative control' if negative else 'regression'))
    if negative:
        print(f'PASS {negative} negative control detected the required assertion failure')


if __name__ == '__main__':
    main()
