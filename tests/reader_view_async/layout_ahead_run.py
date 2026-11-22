"""Exercise bounded lookahead with production scheduling, layout and page cache."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

CASES = ['ahead_unknown_chapter', 'ahead_requires_three_frames', 'ahead_bound_and_window',
         'ahead_pending_source', 'ahead_reverse_three', 'ahead_cancel_late_layout',
         'ahead_budget_and_eof', 'ahead_pause_resume', 'ahead_reverse_uncached',
         'ahead_midpage_input_limit', 'ahead_font_error_releases', 'ahead_pending_timeout',
         'ahead_compute_timeout', 'ahead_glyph_timeout',
         'ahead_third_frame_transition', 'ahead_third_frame_reverse_preempts',
         'ahead_third_frame_budget_release', 'ahead_third_frame_timeout',
         'ahead_third_frame_reverse_direction']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append')
    parser.add_argument('--regress', action='store_true')
    parser.add_argument('--expect-old-miss', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    tests = root / 'tests/reader_prefetch'
    sys.path.insert(0, str(tests))
    from cache_run import compiler_environment
    import view_run
    source_path = args.source or root / 'modules/common/reader/reader_view.c'
    source = source_path.read_text(encoding='utf-8')
    build = args.output.resolve()
    build.mkdir(parents=True, exist_ok=True)
    # Generate the current value types and the established asynchronous doubles.
    view_run.generate(root, build)
    types = build / 'view_types.h'
    types.write_text('typedef struct bookshelf_item bookshelf_item_t;\n' + types.read_text(encoding='utf-8'), encoding='utf-8')
    functions = list(dict.fromkeys(view_run.FUNCTIONS + [
        'navigation_clear', 'link_clear', 'document_flow', 'frame_find', 'near_frames_pending']))
    for name in ('layout_ahead_ready', 'layout_ahead_expired', 'preview_ahead_pending', 'preview_ahead_retire'):
        if re.search(r'^static (?:bool|void) ' + name + r'\(', source, re.M):
            functions.append(name)
    extracted = sorted(view_run.extract(source, name) for name in functions)
    start, end = source.index('#define VIEW_TEXT'), source.index('static unsigned elapsed_ms')
    data = '\n'.join(re.findall(r'^static (?:epd_reading_data_t data_client[^;]*|bool data_retiring[^;]*);', source, re.M))
    unit = ['#include "view_host.h"', source[start:end], data, '#include "view_core_bridge.h"']
    unit += [signature for _, signature, _ in extracted]
    for offset, _, body in extracted:
        line = source.count('\n', 0, offset) + 1
        if 'static bool near_frames_pending(void)' in body:
            body = body.replace('static bool near_frames_pending(void)', 'static bool model_near_frames_pending(void)', 1)
        unit += [f'#line {line} "{source_path.as_posix()}"', body]
    # The existing core cases omit pixel frames. Only the frame-aware cases
    # enable the real near-frame gate; the pixel/worker suites cover it separately.
    unit += ['static bool near_frames_pending(void) { return host_frame_model && model_near_frames_pending(); }']
    unit += ['#include "view_host.c"']
    (build / 'view_unit.c').write_text('\n'.join(unit), encoding='utf-8')
    bridge = (tests / 'view_core_bridge.h').read_text(encoding='utf-8')
    bridge = bridge.replace('struct view_frame { epd_reader_page_t page; };',
                            'struct view_frame { struct view_frame *next; unsigned layout; epd_reader_page_t page; };')
    bridge = bridge.replace('static bool view_interrupted(void) { return false; }',
                            'static bool host_interrupted, host_frame_model;\nstatic bool view_interrupted(void) { return host_interrupted; }')
    bridge += '''
static bool host_pixels_blocked;
static bool optional_pixels_ready_for(unsigned number) { (void)number; return !host_pixels_blocked; }
static void document_navigation_release(document_t *d) { (void)d; }
static int document_target_require(document_t *d, bool required);
static int epd_app_font_set_purpose(const lv_font_t *f, unsigned purpose) { (void)f; (void)purpose; return 0; }
#ifndef EPD_COMPUTE_FOREGROUND
#define EPD_COMPUTE_FOREGROUND 1u
#define EPD_COMPUTE_BACKGROUND 2u
#endif
'''
    if 'layout_ahead_ready' not in functions:
        bridge += 'static bool layout_ahead_ready(void) { return false; }\n'
    if 'preview_ahead_pending' not in functions:
        bridge += 'static bool preview_ahead_pending(void) { return false; }\n'
    bridge += '#ifndef VIEW_AHEAD_WAIT_MS\n#define VIEW_AHEAD_WAIT_MS 2000u\n#endif\n'
    (build / 'view_core_bridge.h').write_text(bridge, encoding='utf-8')
    fixture = (tests / 'view_host.c').read_text(encoding='utf-8')
    # Consume every bounded layout batch at the published tail. Older fixtures
    # assumed one submission consumed the entire record.
    tail = '    CHECK(view.count == 11 && !view.complete && !view.index->done);'
    assert fixture.count(tail) == 1
    fixture = fixture.replace(tail, '''    for (unsigned i = 0; i < 20 && page_runnable(view.index); ++i) {
        host_async_work();
        if (view.compute_job) { worker_complete(view.compute_job); compute_one(true); }
        else CHECK(page_step(view.index, true, view.index_reader, &view.progress) >= 0);
    }
''' + tail)
    fixture = fixture.replace('static struct {\n    size_t free_bytes',
                              'static unsigned ahead_preview_calls;\nstatic bool ahead_eager, ahead_mixed_error;\nstatic struct {\n    size_t free_bytes', 1)
    marker = 'static const host_test_t tests[] = {'
    if marker not in fixture:
        match = re.search(r'static const struct[^;]*?tests\[\]\s*=\s*\{', fixture, re.S)
        assert match
        marker = match[0]
    fixture = fixture.replace(marker, '#include "layout_ahead_cases.inc"\n' + marker + '\n' +
                              '\n'.join(f'    {{"{case}", {case}}},' for case in CASES))
    (build / 'view_host.c').write_text(fixture, encoding='utf-8')
    async_text = (tests / 'view_async.c').read_text(encoding='utf-8')
    marker = '    CHECK(font && text && !async_host.worker);'
    assert async_text.count(marker) == 1
    async_text = async_text.replace(marker, marker + '''
    if (ahead_mixed_error && pixels) {
        reader_cached_page_t *p = reader_page_cache_find(&view.cache, view.preview_page + 4, view.layout);
        if (p && (uintptr_t)text >= (uintptr_t)reader_page_cache_text(p) &&
            (uintptr_t)text < (uintptr_t)reader_page_cache_text(p) + p->bytes) {
            unsigned offset = (unsigned)(text - reader_page_cache_text(p));
            return offset == p->runs[0].text ? 0 : -1;
        }
    }
''')
    async_text += '\nstatic int document_target_require(document_t *d, bool required) { (void)d; (void)required; return async_host.navigation_pending ? 0 : view.progress.complete ? 1 : 0; }\n'
    old = '{ return epd_app_font_prepare_text(font, text, length, false); }'
    assert async_text.count(old) == 1
    async_text = async_text.replace(old, '''{
    if (view.prefetch_number > view.preview_page && view.prefetch_number - view.preview_page == 4)
        ++ahead_preview_calls;
    int ready = epd_app_font_prepare_text(font, text, length, false);
    if (ahead_eager) for (unsigned at = 0; at < length;) {
        uint32_t letter; at += utf8_letter(text + at, (unsigned)length - at, &letter);
        (void)epd_app_font_glyph_ready(font, letter, 0, true);
    }
    return ready;
}''')
    (build / 'view_async.c').write_text(async_text, encoding='utf-8')
    (build / 'layout_ahead_cases.inc').write_bytes(Path(__file__).with_name('layout_ahead_cases.inc').read_bytes())
    cc, env = compiler_environment(None)
    reader = root / 'modules/common/reader'
    data_dir = root / 'components/compute_reading_data'
    sources = [build / 'view_unit.c', reader / 'reader_page_cache.c', reader / 'compute/reader_kernel.c',
               reader / 'compute/reader_page.c', tests / 'view_data_transport.c',
               tests.parent / 'compute_reading_data/protocol.c', data_dir / 'reading_data_client.c', data_dir / 'reading_data_remote.c']
    includes = [build, tests, reader, reader / 'compute/include', root / 'src/ui/platform',
                tests.parent / 'compute_reading_data', data_dir / 'include', root / 'src/platform', root / 'components/compute/include']
    exe = build / 'layout_ahead.exe'
    if Path(cc).stem.lower() == 'cl':
        command = [cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/TC', '/wd4127', '/wd4200',
                   '/wd4244', '/wd4701', '/wd4505', '/D_CRT_SECURE_NO_WARNINGS', '/fsanitize=address', '/Zi',
                   *(f'/I{p}' for p in includes), *map(str, sources), '/Fe' + str(exe)]
    else:
        command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O1', '-g',
                   '-fsanitize=address,undefined', *(f'-I{p}' for p in includes), *map(str, sources), '-o', str(exe)]
    result = subprocess.run(command, cwd=build, env=env, capture_output=True, timeout=60)
    (build / 'compile.log').write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit((result.stdout + result.stderr).decode('utf-8', errors='replace'))
    selected = args.case or (subprocess.check_output([str(exe), '--list'], env=env, text=True).splitlines()
                             if args.regress else CASES)
    results = []
    for case in selected:
        run = subprocess.run([str(exe), case], cwd=build, env=env, capture_output=True, timeout=30)
        output = run.stdout + run.stderr
        (build / (case + '.log')).write_bytes(output)
        passed = (run.returncode == 1 and b'cache_contains(base + 4)' in output) if args.expect_old_miss else run.returncode == 0
        results.append({'case': case, 'passed': passed, 'exit_code': run.returncode})
        print(output.decode('utf-8', errors='replace').strip(), flush=True)
    report = {'reader_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
              'expected_old_miss': args.expect_old_miss, 'results': results}
    (build / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    assert all(row['passed'] for row in results), report
    print(f'PASS {len(results)} ASan cases; expected_old_miss={args.expect_old_miss}')

if __name__ == '__main__':
    main()
