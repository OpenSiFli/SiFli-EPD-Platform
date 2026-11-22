"""Run the production reader block codec and extracted frame ownership paths."""
import argparse
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
reader = ROOT / 'modules/common/reader'
support = ROOT / 'tests/reader_view_async'
source = (reader / 'reader_view.c').read_text(encoding='utf-8')
extract = runpy.run_path(str(support / 'run.py'))['extract']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--source', type=Path, help='Alternate production source for negative controls')
args = parser.parse_args()
if args.source:
    source = args.source.read_text(encoding='utf-8')
cc, env = runpy.run_path(str(ROOT / 'tests/reader_prefetch/cache_run.py'))['compiler_environment'](None)
fixture = (support / 'frame_memory.c').read_text().split('/* PRODUCTION */')[0]
fixture = '#include "reader_frame_store.h"\n#define VIEW_RENDER_WORKSPACE 65536u\n#define VIEW_RENDER_SLICE_MS 4u\n' + fixture
fixture = fixture.replace('#define PIXELS 699592u', '#define PIXELS 690680u').replace(
    '#define CAPACITY 699616u', '#define CAPACITY 690688u')
fixture = fixture.replace('static size_t pressure_reclaim_bytes;',
    'static size_t pressure_reclaim_bytes;\nstatic bool fixture_pressure_pending;')
fixture = fixture.replace('available+=pressure_reclaim_bytes; return 0;',
    'available+=pressure_reclaim_bytes; return fixture_pressure_pending ? 42 : 0;')
fixture = fixture.replace('(app_memory_pressure_status_t){0,false}',
    '(app_memory_pressure_status_t){fixture_pressure_pending ? 42u : 0u,fixture_pressure_pending}')
fixture = fixture.replace('struct view_frame {', '''struct view_frame {
 view_page_t page; unsigned generation;
 reader_frame_store_t cold; reader_frame_block_t *thaw_next; size_t thaw_offset;
 uint32_t cold_intent,thaw_intent,completed_at; bool cold_attempted,cold_reusing,cancelled;
 epd_memory_recovery_t admission_recovery;
 const char *admission_stage; int admission_result; size_t admission_need; bool allocation_attempted;''')
fixture = fixture.replace('unsigned wanted,preview_page', 'unsigned intent,wanted,preview_page')
fixture = fixture.replace('typedef struct cached_page', '''
typedef struct { unsigned record,byte; } document_cursor_t;
typedef struct { document_cursor_t start,cursor; bool eof; unsigned generation; } view_page_t;
typedef view_page_t epd_reader_page_t;
typedef struct cached_page''', 1)
fixture = fixture.replace('struct cached_page *next;',
    'document_cursor_t start,end; bool eof; struct cached_page *next;')
fixture = fixture.replace('reader_page_cache_t cache;', '''reader_page_cache_t cache;
 struct { bool ready; } status;
 view_page_t *display,*building,*compute_page,*prefetch;
 unsigned failure,prefetch_number,cache_blocked,preview_back_base;
 bool initialized,target_pending,display_pending,selected,background_paused,preview_back_scanning,preview_back_blocked;
 int segment_turns;
 bool pixels_blocked,pixels_memory_blocked,pixels_retry_ready;
 unsigned pixels_blocked_intent,pixels_blocked_priority;
 size_t pixels_blocked_bytes,pixels_blocked_available;''')
fixture = fixture.replace('context,*primary_state;', 'context,*primary_state,*state;')
fixture = fixture.replace('#define view (*primary_state)', '#define view (*state)')
fixture = fixture.replace(extract('window_priority', fixture), '')
fixture = fixture.replace('bool stop,suspended,replace; unsigned session;', 'bool stop,suspended,replace,paused; unsigned session,intent;')
fixture = fixture.replace('static unsigned view_resource;', 'static struct { unsigned provider; } view_resource;')
fixture = fixture.replace('static void epd_app_free(void *p) { assert(!locks && p); ++frames_freed;free(p); }',
                          'static void epd_app_free(void *p);\nstatic void *epd_app_alloc(size_t bytes, int region);')
fixture = fixture.replace('assert(frame && (!strcmp(stage,"pixels_alloc") || !strcmp(stage,"glyph_pending")))', 'assert(frame && stage)')
fixture = fixture.replace('static rt_tick_t rt_tick_get(void) { return now_tick; }',
                          'static rt_tick_t rt_tick_get(void) { return now_tick; }')
helpers = ['frame_spare_clear', 'frame_spare_matches', 'frame_spare_available', 'frame_spare_trim',
           'frame_spare_keep', 'frame_spare_take', 'frame_cold_allocate', 'frame_cold_free',
           'frame_cold_clear', 'frame_release', 'frame_cache_clear', 'frame_trim', 'frame_priority',
           'frame_distance', 'frame_retention_distance', 'frame_bitmap_less_useful',
           'frame_storage_minimum', 'frame_nearby_growth', 'frame_admission_error', 'frame_workspace_ready',
           'frame_cold_cancelled', 'frame_cold_maintain', 'frame_cold_candidate', 'frame_cold_reuse',
           'frame_reclaim_pages', 'frame_reserve', 'frame_pixel_victim', 'frame_reuse_pixels',
           'frame_allocate_pixels', 'frame_storage', 'frame_materialize', 'frame_find', 'frame_pixels_ready',
           'optional_pixels_ready', 'optional_pixels_ready_for', 'optional_pixels_block', 'frame_prewarm',
           'window_pixels_pending_for', 'window_pixels_pending', 'near_direction_pending', 'near_frames_pending', 'frame_prepare']
declarations = '''
typedef int epd_app_memory_t;
static view_frame_t **frame_pixel_victim(const view_frame_t *, bool);
static void frame_drop_note(view_frame_t *f) {(void)f;}
static void frame_admission_report(const view_frame_t *f,int result) {(void)f;(void)result;}
static void frame_reclaim_note(const view_frame_t *f,const view_state_t *s,unsigned n,unsigned p,const char *r)
{(void)f;(void)s;(void)n;(void)p;(void)r;}
'''
spare = re.search(r'static struct \{\s*epd_compute_buffer_t pixels;\s*uint32_t session;\s*\} frame_spare;', source)[0]
allocator = re.search(r'static const reader_frame_allocator_t frame_cold_allocator = \{[^;]+;', source)[0]
code = extract('view_allocate', source) + '\n' + '\n'.join(extract(f, source) for f in helpers)
cache = (reader / 'reader_page_cache.c').read_text()
code = extract('remove_page', cache) + '\n' + extract('reader_page_cache_remove', cache) + '\n' + code
code = code.replace(extract('frame_cold_free', source), extract('frame_cold_free', source) + '\n' + allocator)
host = (HERE / 'host.c').read_text()
prelude, cases = host.split('/* CASES */')
warm_support, warm_cases = (HERE / 'prewarm.inc').read_text().split('/* CASES */')
cases = cases.replace('int main(void)', warm_cases + '\nint main(void)')
cases = cases.replace('    return 0;\n}', '    prewarm_cases();\n    return 0;\n}')
program = fixture + '\n' + prelude + '\n' + declarations + spare + '\n' + warm_support + '\n' + \
    extract('window_route', source) + '\n' + extract('window_priority', source) + \
    '\n#include "recovery_fixture.h"\n' + code + '\n' + cases
with tempfile.TemporaryDirectory(prefix='reader-frame-store-') as folder:
    build = Path(folder)
    unit = build / 'host.c'; unit.write_text(program, encoding='utf-8')
    exe = build / ('host.exe' if os.name == 'nt' else 'host')
    includes = [reader, support, ROOT / 'tests/unified_resource', ROOT / 'src/platform']
    if Path(cc).stem.lower() == 'cl':
        command = [cc, '/nologo', '/TC', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4505', '/wd4200', '/wd4459',
                   '/D_CRT_SECURE_NO_WARNINGS', *['/I' + str(p) for p in includes], str(unit), '/Fe' + str(exe)]
        command += ['/fsanitize=address', '/Zi'] if args.sanitize else ['/O2']
    else:
        command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O2',
                   *['-I' + str(p) for p in includes], str(unit), '-o', str(exe)]
        if args.sanitize: command += ['-fsanitize=address,undefined']
    subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
    subprocess.run([str(exe)], cwd=build, env=env, check=True, timeout=60)
    # Use the shared host's allocator observation hooks, then link the complete
    # production manager/resource/runtime; no reservation or transfer mocks.
    runtime = (ROOT / 'tests/unified_resource/runtime_host.c').read_text()
    runtime = runtime[:runtime.index('static const struct { const char *name;')]
    runtime = runtime.replace('blocks[128]', 'blocks[512]').replace('i < 128', 'i < 512')
    runtime = runtime.replace('#define CAPACITY ((size_t)1048576)', '#define CAPACITY ((size_t)10 * 1048576)')
    real = (HERE / 'accounting.c').read_text()
    supply = (HERE / 'supply_accounting.inc').read_text().replace('/* SUPPLY PRODUCTION */',
        '\n'.join(extract(f, source) for f in ('optional_pixels_ready', 'optional_pixels_ready_for', 'optional_pixels_block')))
    real = real.replace('#include "supply_accounting.inc"', supply)
    sdk = (ROOT / 'src/platform/epd_memory_sdk.c').read_text()
    real = real.replace('/* PEAK */', extract('epd_app_allocation_peak', sdk))
    real = real.replace('/* PRODUCTION */', '\n'.join(extract(f, source) for f in
        ('view_allocate', 'frame_cold_allocate', 'frame_cold_free')) + '\n' + allocator)
    unit.write_text(runtime + '\n' + real, encoding='utf-8')
    units = [unit, *[ROOT / 'src/platform' / f for f in
             ('epd_memory_manager.c', 'epd_resource.c', 'epd_resource_runtime.c')],
             reader / 'reader_page_cache.c', reader / 'reader_page_resource.c']
    includes = [ROOT / 'tests/unified_resource/stubs', ROOT / 'src/platform', reader, reader / 'compute/include']
    if Path(cc).stem.lower() == 'cl':
        command = [cc, '/nologo', '/TC', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4505', '/wd4200',
                   '/wd4459', '/DEPD_MEMORY_HOST', *['/I' + str(p) for p in includes], *map(str, units), '/Fe' + str(exe)]
        command += ['/fsanitize=address', '/Zi'] if args.sanitize else ['/O2']
    else:
        command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O2',
                   '-DEPD_MEMORY_HOST', *['-I' + str(p) for p in includes], *map(str, units), '-o', str(exe)]
        if args.sanitize: command += ['-fsanitize=address,undefined']
    subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
    for case in ('complete', 'denied', 'supply'):
        subprocess.run([str(exe), case], cwd=build, env=env, check=True, timeout=60)
