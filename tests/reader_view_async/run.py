"""Exercise extracted production ownership and worker transitions on the host."""
import argparse
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = (ROOT / 'modules/common/reader/reader_view.c').read_text(encoding='utf-8')

def extract(name, production=None):
    if production is None:
        production = SOURCE
    clean = re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                   lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), production)
    match = re.search(r'^[A-Za-z_][\w* \t]*\b' + name + r'\s*\([^;{}]*\)\s*\{', clean, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (clean[end] == '{') - (clean[end] == '}')
        end += 1
    result = production[match.start():end]
    if name == 'view_worker' and 'worker_wait(' in result:
        # These worker fixtures exercise the ordinary, non-diagnostic build.
        # Carry its production wait macro with the extracted worker body.
        wait = re.search(r'^#define worker_wait\([^\n]+', production, re.M)
        assert wait, 'worker_wait ordinary-build definition'
        result = wait[0] + '\n' + result
    return result

def main():
    global SOURCE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cc', default=os.environ.get('CC'))
    parser.add_argument('--case', action='append')
    parser.add_argument('--source', type=Path, help='Read an alternate reader_view.c without editing production')
    parser.add_argument('--entry-source', type=Path, help='Read alternate document entry-cache code')
    args = parser.parse_args()
    if args.source:
        SOURCE = args.source.read_text(encoding='utf-8')
    cc, env = runpy.run_path(str(ROOT / 'tests/reader_prefetch/cache_run.py'))['compiler_environment'](args.cc)
    cases = {
        'ownership': ['frame_ref', 'frame_release', 'source_percent', 'render_cancelled', 'frame_publish'],
        'backward': ['page_required', 'page_work_allowed', 'page_memory_block', 'preview_back_stop', 'preview_back_seek_step'],
        'backward_refill': ['cache_sync', 'preview_back_discard', 'preview_back_stop', 'preview_back_begin', 'preview_back_finish'],
        'visited_pages': ['page_required', 'page_work_allowed', 'page_memory_block', 'page_error', 'cursor_before', 'cursor_equal', 'compute_cancel', 'page_reset', 'visited_clear', 'visited_record', 'visited_find', 'visited_forget', 'frame_remember', 'cache_sync', 'preview_back_discard', 'preview_back_stop', 'preview_back_begin', 'preview_back_finish', 'preview_cached_target', 'preview_back', 'restore_target', 'page_step'],
        'worker': ['request_memory_failed', 'worker_retry_fonts', 'command_restores_origin', 'document_flow', 'neighbor_pending', 'neighbor_index_pending', 'neighbor_choose', 'window_service', 'worker_target_needed', 'worker_context_pause', 'worker_quiesce', 'worker_input_commit', 'worker_idle_delay', 'worker_request_reject', 'worker_background_kind', 'view_worker'],
        'cancel_error': ['view_interrupted', 'fail', 'core_background_pause', 'core_step', 'epd_reader_suspend'],
        'navigation': ['window_shift', 'state_retire', 'cluster_retire', 'chapter_select', 'target_select', 'progress_seek', 'follow_link', 'follow_toc', 'command_restores_origin', 'command_apply'],
        'traversal': ['cursor_at_segment_start', 'target_finish', 'core_turn', 'target_resolve', 'turn_continue', 'target_select', 'follow_link'],
        'links': ['document_link_target', 'document_link_open', 'adjacent_anchor', 'adjacent_link_locked', 'document_adjacent_internal'],
        'budget': ['window_charge', 'window_uncharge_bytes', 'reserve_pages', 'index_size', 'cache_allocate', 'cache_free', 'frame_priority', 'frame_distance', 'frame_reclaim_layout', 'frame_reclaim_pages', 'frame_reserve'],
        'frame_priority': ['page_required', 'request_memory_failed', 'page_work_allowed', 'page_memory_block', 'frame_priority', 'frame_distance', 'frame_release', 'frame_find', 'frame_trim', 'frame_reclaim_pages', 'frame_reserve', 'optional_pixels_ready', 'optional_pixels_ready_for', 'optional_pixels_block', 'frame_adopt', 'frame_window_contains', 'frame_prepare', 'cursor_at_segment_start', 'near_frames_pending', 'document_flow', 'neighbor_step'],
        'initialization': ['window_charge', 'window_uncharge_bytes', 'reserve_pages', 'core_initialize_release', 'core_initialize_recover', 'core_initialize_failed', 'core_initialize'],
        'reopen': ['position_compatible', 'reopen_book', 'document_view_open', 'document_view_save'],
        'font': ['layout_for_area', 'prepare_fonts', 'state_reset_layout', 'state_reflow', 'ui_configuration', 'epd_reader_settings_changed', 'ui_configuration_rejected'],
        'index': ['index_cancel', 'index_client_cleanup', 'index_client_begin', 'index_client_end', 'index_checksum', 'index_validate'],
        'render': ['frame_turn_matches', 'render_cancel', 'render_cancelled', 'render_admit', 'render_submit', 'render_flush', 'render_command', 'frame_storage', 'frame_glyph_progress', 'frame_render', 'frame_release', 'render_cleanup'],
        'render_batch': ['render_cancelled', 'render_submit', 'render_flush'],
        'frame_memory': ['frame_spare_clear', 'frame_spare_matches', 'frame_spare_available', 'frame_spare_trim', 'frame_spare_keep', 'frame_spare_take', 'frame_priority', 'frame_distance', 'frame_release', 'frame_cache_clear', 'render_cleanup', 'frame_reclaim_pages', 'frame_reserve', 'frame_pixel_victim', 'frame_reuse_pixels', 'frame_allocate_pixels', 'frame_storage', 'frame_glyph_progress'],
        'images': ['images_free', 'image_matches', 'page_uses_image', 'image_page_nearby', 'images_prune_keep', 'images_prune', 'image_find', 'images_cancel_pending', 'image_memory_room', 'images_prepare_runs'],
        'neighbors': ['request_memory_failed', 'page_work_allowed', 'window_work_allowed', 'window_conversion_document', 'window_conversion_ready', 'neighbor_pending', 'neighbor_index_pending', 'neighbor_choose', 'neighbors_prepare'],
        'maintenance': ['indexed_pages', 'index_size', 'count_checksum', 'index_load', 'index_save', 'count_cache_load', 'count_cache_save', 'optional_pixels_ready', 'optional_pixels_block', 'layout_accept', 'neighbor_step'],
        'sparse': ['indexed_pages', 'preview_start', 'sparse_compact', 'sparse_seek_page', 'index_finish', 'target_finish'],
        'pagination': ['indexed_pages', 'pagination_reset', 'pagination_chapter', 'pagination_matches', 'pagination_add', 'pagination_snapshot', 'pagination_locate', 'pagination_prepare', 'pagination_step', 'pagination_seek', 'epd_reader_save'],
        'count_source': ['document_pagination_open', 'document_can_seek_progress', 'document_progress_open', 'document_progress_cursor'],
        'chapter_title': ['navigation_append', 'toc_complete', 'navigation_is_toc', 'document_anchor_find', 'toc_current_title', 'document_current_title'],
        'count_paths': ['index_activity_begin', 'index_path', 'index_header_valid', 'index_save_kind', 'document_index_save', 'document_count_save', 'index_load_async_kind', 'document_index_load_async', 'document_count_load_async', 'index_work'],
    }
    cases['page_resources'] = ['cursor_equal', 'cursor_before', 'page_required', 'request_memory_failed', 'page_work_allowed', 'page_memory_block', 'page_error', 'decode_at', 'compute_cancel', 'page_reset', 'page_target', 'page_runnable', 'page_step', 'compute_one', 'fonts_ready']
    cases['frame_memory'].insert(cases['frame_memory'].index('frame_cache_clear'), 'frame_trim')
    cases['entry_metadata'] = ['entry_table_checksum', 'entry_table_valid', 'entry_cache_accept']
    cases['admission_diagnostic'] = ['view_memory_room', 'frame_admission_error', 'frame_admission_report', 'frame_workspace_ready']
    cases['window_reclaim'] = ['frame_reclaim_layout', 'view_resource_bytes', 'view_resource_publish', 'view_resource_step']
    cases['initialization_worker'] = ['view_memory_recover', *cases['initialization'],
        'command_apply', *cases['worker'], 'epd_reader_step', 'ui_command', 'epd_reader_turn']
    cases['window_scheduler'] = ['page_work_allowed', 'request_memory_failed', 'window_work_allowed',
        'window_pixels_pending', 'window_runnable', 'window_prepare', 'neighbor_pending',
        'neighbor_index_pending', 'neighbor_choose', 'window_service', 'neighbor_step', 'worker_background_kind']
    cases['input_gate'] = ['document_flow', 'window_conversion_document', 'window_conversion_ready',
        'window_service', 'worker_context_pause', 'window_quiesce', 'worker_quiesce', 'worker_input_commit']
    cases['target_input'] = ['window_contexts', 'navigation_clear', 'link_clear', 'document_flow',
        'window_conversion_document', 'worker_context_pause', 'worker_quiesce',
        'worker_input_commit', 'target_resolve']
    # command_apply uses this predicate before the worker's extracted definition.
    cases['initialization_worker'].remove('command_restores_origin')
    cases['initialization_worker'].insert(0, 'command_restores_origin')
    with tempfile.TemporaryDirectory(prefix='reader-view-host-') as folder:
        build = Path(folder)
        (build / 'recovery_fixture.h').write_bytes((HERE / 'recovery_fixture.h').read_bytes())
        (build / 'initialization_activity_fixture.h').write_bytes((HERE / 'initialization_activity_fixture.h').read_bytes())
        if args.case and set(args.case)-set(cases): parser.error("Unknown case")
        for case, functions in cases.items():
            if args.case and case not in args.case: continue
            if case in ('cancel_error', 'navigation', 'traversal', 'font'):
                functions = ['navigation_clear', 'link_clear', *functions]
            fixture = (HERE / (case + '.c')).read_text()
            if case == 'target_input':
                document = (ROOT / 'modules/books/document/document.c').read_text(encoding='utf-8')
                functions_document = ('flow_owner', 'flow_allows', 'chapter_restore_required',
                    'document_work_required', 'conversion_wanted', 'chapter_restore_runnable',
                    'content_pending', 'document_target_cursor', 'document_target_require',
                    'document_prefetch_enable')
                fixture = fixture.replace('/* DOCUMENT PRODUCTION */',
                    '\n\n'.join(extract(f, document) for f in functions_document))
            if case == 'initialization_worker':
                fixture = fixture.replace('/* INITIALIZATION SUPPORT */', (HERE / 'initialization_worker_support.inc').read_text())
                fixture = fixture.replace('/* REQUEST UI SUPPORT */', (HERE / 'request_ui_fixture.inc').read_text())
                fixture = fixture.replace('/* COMBINED CASES */', (HERE / 'initialization_worker_cases.inc').read_text())
            if case == 'images':
                functions = ['image_release', *functions]
                document = (ROOT / 'modules/books/document/document.h').read_text()
                for name in ('DOCUMENT_IMAGE_WIDTH', 'DOCUMENT_IMAGE_HEIGHT', 'DOCUMENT_IMAGE_CACHE_BYTES'):
                    fixture = re.search(r'^#define '+name+r' .+$', document, re.M)[0] + '\n' + fixture
                fixture = fixture.replace('/* PRODUCTION */', extract('document_picture_charge',document)+'\n/* PRODUCTION */')
            if case in ('budget', 'frame_memory'):
                cache = (ROOT / 'modules/common/reader/reader_page_cache.c').read_text()
                functions_cache = extract('remove_page', cache) + '\n' + extract('reader_page_cache_remove', cache)
                fixture = fixture.replace('/* PRODUCTION */', functions_cache + '\n/* PRODUCTION */')
            if case == 'reopen':
                document = (ROOT / 'modules/books/document/document.h').read_text()
                fixture = re.search(r'^#define DOCUMENT_CONVERSION_VERSION .+$', document, re.M)[0] + '\n' + fixture
                fixture = fixture.replace('/* PRODUCTION */', extract('document_history_version', document) + '\n/* PRODUCTION */')
            if '/* VISITED TYPES */' in fixture:
                visited = re.search(r'^#define VIEW_VISITED_PAGES .+$', SOURCE, re.M)[0]
                visited += '\n' + re.search(r'typedef struct \{\s*document_cursor_t start, end;\s*uint32_t layout;\s*bool eof;\s*\} view_visited_page_t;', SOURCE)[0]
                fixture = fixture.replace('/* VISITED TYPES */', visited)
            fixture = re.sub(r'^#define VIEW_CACHE_RESERVE .+$', '#define VIEW_CACHE_RESERVE 0u', fixture, flags=re.M)
            fixture = re.search(r'^#define VIEW_RENDER_WORKSPACE .+$', SOURCE, re.M)[0] + '\n' + fixture
            fixture = '#define VIEW_RENDER_RESERVE VIEW_RENDER_WORKSPACE\n' + fixture
            if case == 'frame_memory':
                font_source = (ROOT / 'src/ui/ui_reader_font_remote.c').read_text(encoding='utf-8')
                fixture = '#define SHARED_RESERVE 0u\n' + fixture
            if case in ('visited_pages', 'page_resources'):
                settings = (ROOT / 'src/ui/ui_settings.h').read_text(encoding='utf-8')
                fixture = re.search(r'^#define UI_READING_FONT_SIZES .+$', settings, re.M)[0] + '\n' + fixture
            if case == 'navigation':
                reflow_kind = re.search(r'ui\.bound\s*\?\s*(INTENT_\w+)', extract('epd_reader_reflow'))[1]
                fixture = '#define REFLOW_COMMAND ' + reflow_kind + '\n' + fixture
            source = build / (case + '.c')
            production = (ROOT / 'modules/books/document/document_view.c').read_text() if case == 'reopen' else SOURCE
            if case == 'entry_metadata':
                production = (args.entry_source or ROOT / 'modules/books/document/document_entry.inc').read_text(encoding='utf-8')
                fixture = fixture.replace('/* ENTRY TYPES */', production[:production.index('static uint32_t entry_table_checksum')])
            if case in ('links', 'count_source', 'count_paths', 'chapter_title'): production = (ROOT / 'modules/books/document/document.c').read_text(encoding='utf-8')
            if any(f in functions for f in ('frame_reclaim_layout','frame_reserve','frame_pixel_victim','frame_storage')):
                functions = ['frame_retention_distance', *functions]
                if '#define VIEW_NEIGHBOR_PRIORITY' not in fixture:
                    fixture = re.search(r'^#define VIEW_NEIGHBOR_PRIORITY .+$', SOURCE, re.M)[0] + '\n' + fixture
            if any(f in functions for f in ('frame_reserve', 'frame_pixel_victim')):
                functions.insert(1, 'frame_bitmap_less_useful')
            if any(f in functions for f in ('frame_reserve', 'frame_storage')):
                functions.insert(0, 'frame_storage_minimum')
            if any(f in functions for f in ('frame_reserve', 'frame_allocate_pixels', 'frame_storage')):
                functions.insert(0, 'frame_workspace_ready')
            if 'window_pixels_pending' in functions:
                functions.insert(functions.index('window_pixels_pending'), 'window_pixels_pending_for')
            if case == 'frame_memory' and 'static bool frame_nearby_growth(' in SOURCE:
                functions.insert(functions.index('frame_reserve'), 'frame_nearby_growth')
            code = '\n\n'.join(extract(f, production) for f in functions)
            if 'frame_admission_error(' in code and 'frame_admission_error' not in functions:
                code = extract('frame_admission_error') + '\n' + code
            if 'frame_admission_report(' in code and 'frame_admission_report' not in functions:
                code = 'static void frame_admission_report(const view_frame_t *f,int r) {(void)f;(void)r;}\n' + code
            if 'view_reserve_result' in code and 'view_reserve_result' not in fixture:
                code = 'static int view_reserve_result;\n' + code
            # These transition fixtures use hot frames. Cold block ownership,
            # cancellation and admission run against the real implementations
            # in reader_frame_store/run.py.
            if 'struct view_frame {' in fixture:
                fixture = '#include "reader_frame_store.h"\n' + fixture
                fixture = fixture.replace('struct view_frame {',
                    'struct view_frame { reader_frame_store_t cold; bool cold_reusing; '
                    'const char *admission_stage; int admission_result; size_t admission_need; bool allocation_attempted;')
            for name, stub in {
                'frame_cold_clear': 'static void frame_cold_clear(view_frame_t *f) {assert(!f->cold.first);}',
                'frame_cold_reuse': 'static int frame_cold_reuse(view_frame_t *f) {(void)f;return -1;}',
                'frame_cold_candidate': 'static view_frame_t *frame_cold_candidate(const view_frame_t *f) {(void)f;return NULL;}',
                'frame_cold_maintain': 'static void frame_cold_maintain(void) {}',
                'frame_materialize': 'static int frame_materialize(view_frame_t *f,bool required) {(void)required;assert(!f->cold.first);return 1;}',
                'frame_pixels_ready': 'static bool frame_pixels_ready(const view_frame_t *f) {return f != NULL;}',
                'frame_prewarm': 'static int frame_prewarm(view_frame_t *f) {assert(!f->cold.first);return 1;}',
                'frame_reuse_pixels': 'static bool frame_reuse_pixels(view_frame_t *f) {(void)f;return false;}',
            }.items():
                if name + '(' in code and name not in functions and name + '(' not in fixture:
                    code = stub + '\n' + code
            if 'frame_nearby_growth(' in code and 'frame_nearby_growth' not in functions:
                code = 'static bool frame_nearby_growth(const view_frame_t *f) {(void)f;return false;}\n' + code
            if 'EPD_APP_FONT_NO_MEMORY' in code and '#define EPD_APP_FONT_NO_MEMORY' not in fixture:
                font_api = (ROOT / 'src/ui/platform/epd_app_font.h').read_text()
                fixture = re.search(r'^#define EPD_APP_FONT_NO_MEMORY .+$', font_api, re.M)[0] + '\n' + fixture
            if 'view_input_set(' in code:
                if case in ('worker', 'initialization_worker', 'input_gate', 'target_input'):
                    code = 'static uint32_t input_sequence;\n' + extract('view_input_set') + '\n' + code
                else:
                    code = '#define view_input_set(owner, enabled) document_prefetch_enable((owner)->document, (enabled))\n' + code
            if 'view_worker' in functions:
                # Retry admission and cleanup run with the real ledger/activity
                # core in near_retry_worker.py; these older cases isolate UI work.
                code = '''typedef struct { struct {size_t psram,sram; epd_memory_credit_t credit;} demand; } view_near_retry_t;
static bool worker_near_retry(view_near_retry_t *r,view_state_t *v,epd_activity_work_t *p)
{ (void)r;(void)v;(void)p;return false; }
static void worker_near_block(view_near_retry_t *r,view_state_t *v,epd_activity_work_t *p,epd_activity_work_t *w)
{ (void)r;(void)v;(void)p;epd_activity_work_close(w); }
static void worker_near_resume(view_state_t *v,epd_memory_credit_t c) { (void)v;(void)c; }
typedef struct {size_t psram,sram; epd_memory_credit_t credit;} epd_activity_demand_t;
static bool epd_activity_work_demand(epd_activity_work_t *w,epd_activity_demand_t *d)
{ if(!w)return false;*d=(epd_activity_demand_t){w->peak,w->sram,1};return true; }
static void worker_credit_changed(view_state_t *v,epd_memory_credit_t c) { (void)v;(void)c;assert(false); }
''' + code
            if case == 'initialization_worker':
                sdk = (ROOT / 'src/platform/epd_memory_sdk.c').read_text()
                recovery = '\n\n'.join(extract(f, sdk) for f in ('recovery_fragmented', 'recovery_step',
                    'recovery_begin', 'epd_memory_recovery_begin', 'epd_memory_recovery_poll',
                    'recovery_failed', 'epd_memory_recovery_failed'))
                code = '#define rt_sem_take fixture_heap_take\n#define rt_sem_release fixture_heap_release\n' + recovery + \
                    '\n#undef rt_sem_take\n#undef rt_sem_release\n' + code
            for name in ('blocked_available', 'init_blocked_available', 'pixels_blocked_available', 'window_attempt_available', 'neighbor_available'):
                if name == 'pixels_blocked_available' and name in code: continue
                fixture = re.sub(r'\b' + name + r'\b', name.replace('available', 'epoch'), fixture)
            if 'epd_memory_capacity_epoch(' in code and 'epd_memory_capacity_epoch(' not in fixture:
                fixture = '#include <stdint.h>\n' + fixture
                code = 'uint32_t epd_memory_capacity_epoch(void) {return (uint32_t)epd_app_psram_available();}\n' + code
            if 'view_memory_room(' in code and 'view_memory_room(' not in fixture:
                code = 'static bool view_memory_room(size_t bytes) {return bytes <= epd_app_psram_available();}\n' + code
            if 'view_memory_recover(' in code:
                if 'activity_fixture.h' not in fixture:
                    fixture = '#include "activity_fixture.h"\n' + fixture
                if case in ('render', 'frame_memory', 'images'):
                    fixture = fixture.replace('struct view_frame {', 'struct view_frame { epd_memory_recovery_t admission_recovery, allocation_recovery;')
                    fixture = fixture.replace('rt_tick_t image_memory_started;', 'rt_tick_t image_memory_started; epd_memory_recovery_t image_recovery;')
                    code = '#include "recovery_fixture.h"\n' + code
            if 'view_resize(' in code:
                code = '#include <string.h>\n' + extract('view_resize') + '\n' + code
            if 'view_allocate(' in code:
                code = '#define view_allocate epd_app_alloc\n' + code
            if ('epd_app_allocation_peak(' in code or 'epd_resource_context_reserve(' in code) and 'activity_fixture.h' not in fixture:
                fixture = '#include "activity_fixture.h"\n' + fixture
            if 'frame_pixel_victim(' in code:
                if 'frame_pixel_victim' in functions:
                    code = 'static view_frame_t **frame_pixel_victim(const view_frame_t *, bool);\n' + code
                elif 'frame_pixel_victim(' not in fixture:
                    code = 'static view_frame_t **frame_pixel_victim(const view_frame_t *f, bool m) {(void)f;(void)m;return NULL;}\n' + code
            if case == 'frame_memory':
                code = re.search(r'static struct \{\s*epd_compute_buffer_t pixels;\s*uint32_t session;\s*\} frame_spare;', SOURCE)[0] + '\n' + code
            else:
                for name, stub in {
                    'frame_spare_clear': 'static void frame_spare_clear(void) {}',
                    'frame_spare_trim': 'static void frame_spare_trim(const view_frame_t *f) {(void)f;}',
                    'frame_spare_available': 'static bool frame_spare_available(const view_frame_t *f) {(void)f;return false;}',
                    'frame_spare_keep': 'static bool frame_spare_keep(view_frame_t *f) {(void)f;return false;}',
                    'frame_spare_take': 'static bool frame_spare_take(view_frame_t *f) {(void)f;return false;}',
                }.items():
                    if name + '(' in code and name + '(' not in fixture:
                        code = stub + '\n' + code
            if 'frame_reclaim_note(' in code and 'frame_reclaim_note(' not in fixture:
                code = 'static void frame_reclaim_note(const view_frame_t *v,const view_state_t *a,unsigned n,unsigned p,const char *r) {(void)v;(void)a;(void)n;(void)p;(void)r;}\n' + code
            if 'window_number(state)' in code and 'window_number(' not in fixture:
                code = 'static unsigned window_number(const view_state_t *o) {return o->preview_cache_active?o->preview_page:o->wanted;}\n' + code
            # Timing is checked separately; transition fixtures keep their small providers.
            if 'frame_turn_candidate(' in code and 'frame_turn_candidate' not in functions:
                code = '#define frame_turn_candidate frame_turn_matches\n' + code
            notes = {
                'near_direction_pending': 'static bool near_direction_pending(void) {return near_frames_pending();}',
                'visited_clear': 'static void visited_clear(void *owner) {(void)owner;}',
                'frame_remember': 'static void frame_remember(const view_frame_t *frame) {(void)frame;}',
                'frame_turn_cached': 'static bool frame_turn_cached(const view_frame_t *f,const void *c) {(void)f;(void)c;return false;}',
                'prefetch_turn_target': 'static bool prefetch_turn_target(int d) {(void)d;return false;}',
                'prefetch_take_target': 'static void prefetch_take_target(void) {assert(0);}',
                'near_trace_finish': 'static void near_trace_finish(const char *reason) { (void)reason; }',
                'frame_drop_note': 'static void frame_drop_note(view_frame_t *frame) { (void)frame; }',
                'frame_handoff_note': 'static void frame_handoff_note(view_frame_t *frame, unsigned intent) { (void)frame;(void)intent; }',
            }
            for name, stub in notes.items():
                if name + '(' in code and name not in functions and name + '(' not in fixture:
                    code = stub + '\n' + code
            source.write_text(fixture.replace('/* PRODUCTION */', code), encoding='utf-8')
            exe = build / (case + ('.exe' if os.name == 'nt' else ''))
            units, includes = [source], []
            if case == 'font': includes = [ROOT / 'src/ui']
            if case == 'entry_metadata': includes = [ROOT / 'modules/books/compute']
            if case == 'render':
                includes = [ROOT / 'modules/common/reader/compute/include', ROOT / 'components/compute_reader_render/include']
            if case == 'cancel_error':
                includes = [ROOT / 'modules/common/reader', ROOT / 'src/ui/platform']
            if case in ('frame_priority', 'backward_refill', 'visited_pages', 'page_resources'):
                reader = ROOT / 'modules/common/reader'
                units += [reader / 'reader_page_cache.c', reader / 'compute/reader_page.c']
                includes = [reader, reader / 'compute/include']
                if case in ('visited_pages', 'page_resources'): units += [reader / 'compute/reader_kernel.c']
            if case == 'render_batch':
                component = ROOT / 'components/compute_reader_render'
                support = ROOT / 'tests/reader_render_compute'
                units += [component / (name + '.c') for name in
                          ('render_client', 'render_format', 'render_kernel', 'render_remote')]
                units += [support / 'support.c']
                includes = [support / 'admission', component / 'include', ROOT / 'components/compute/include',
                            ROOT / 'src/platform', support / 'stubs', support]
            includes += [ROOT / 'src/platform', ROOT / 'tests/unified_resource', ROOT / 'modules/common/reader']
            if case in ('visited_pages', 'page_resources', 'images'): includes += [ROOT / 'modules/books/formats']
            if Path(cc).stem.lower() == 'cl':
                command = [cc, '/nologo', '/TC', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4505', '/wd4459', '/wd4200',
                           '/D_CRT_SECURE_NO_WARNINGS', *['/I' + str(p) for p in includes],
                           *map(str, units), '/Fe' + str(exe)]
                command += ['/fsanitize=address', '/Zi'] if args.sanitize else ['/RTC1', '/Od']
                if case in ('visited_pages', 'page_resources'): command += ['/wd4244', '/wd4701']
            else:
                command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                           *['-I' + str(p) for p in includes], *map(str, units), '-o', str(exe)]
                if args.sanitize: command += ['-fsanitize=address,undefined']
            subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
            subprocess.run([str(exe)], cwd=build, env=env, check=True, timeout=20)
    # The production UI entry performs no pipeline advancement or component work.
    step = extract('epd_reader_step')
    for forbidden in ('core_step(', 'page_step(', 'frame_render(', 'document_read(', 'epd_compute_'):
        assert forbidden not in step, forbidden
    worker_prefix = SOURCE[:SOURCE.index('/* The worker owns mutable layout state.')]
    for forbidden in ('lv_font_get_glyph_dsc(', 'lv_draw_wait_for_finish(', 'lv_label_', 'lv_obj_create('):
        assert forbidden not in worker_prefix, forbidden
    assert 'if (ready < 0)' in extract('preview_back_seek_step')
    assert 'for (' not in extract('index_checksum')
    assert 'for (' not in extract('index_load')
    assert 'epd_reader_render_open(' not in SOURCE
    for name in ('state_reflow', 'state_retire'):
        assert 'index_save(' not in extract(name), name
    assert 'ui_command_at(' in extract('ui_command')
    for name in ('ui_command_at', 'epd_reader_reflow', 'epd_reader_close', 'epd_reader_view_worker_shutdown'):
        assert 'view_cancel()' in extract(name), name
    assert 'chapter_convert' not in (ROOT / 'modules/common/reader/reader_source.c').read_text()
    print('PASS UI/pipeline separation and removal of HCPU chapter conversion')

if __name__ == '__main__':
    main()
