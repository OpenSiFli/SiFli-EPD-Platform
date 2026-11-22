"""Cross cached-frame trim, UI/pin ownership and automatic shared NAV resumption."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SUPPORT = ROOT / 'tests/reader_document_compute'


def once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f'Expected one fixture/production fragment: {old[:100]!r}')
    return text.replace(old, new, 1)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(build, reader, document, variant):
    runner = runpy.run_path(str(ROOT / 'tests/reader_global_window/run.py'))
    function = runner['function']
    selected = build / 'input_reader.c'
    selected.write_bytes(reader.read_bytes())
    doc = build / 'input_document.c'
    doc.write_bytes(document.read_bytes())
    if variant in ('no-trim', 'no-gate'):
        source = selected.read_text(encoding='utf-8')
        name = 'layout_accept' if variant == 'no-trim' else 'window_service'
        original = function(source, name)
        removed = ('    frame_trim();\n' if variant == 'no-trim' else
                   '    if (!render_frame && (!neighbor || !neighbor->rendering) &&\n'
                   '        window_conversion_ready(current)) document_flow();\n')
        selected.write_text(once(source, original, once(original, removed, '')), encoding='utf-8')
    if variant == 'no-shared-nav':
        source = doc.read_text(encoding='utf-8')
        original = function(source, 'navigation_allowed')
        # Disable only shared ACTIVE-NAV eligibility; retain other flow checks.
        changed = once(original, 'bool shared = d->nav_state == DOCUMENT_NAV_ACTIVE &&',
                       'bool shared = false && d->nav_state == DOCUMENT_NAV_ACTIVE &&')
        doc.write_text(once(source, original, changed), encoding='utf-8')
    runner['prepare'](build, selected, None)
    (build / 'document_under_test.c').write_bytes(doc.read_bytes())
    production = (build / 'reader_under_test.c').read_text(encoding='utf-8')

    host = (build / 'host.inc').read_text(encoding='utf-8')
    host = once(host, 'struct epd_compute_pipe_host {',
        'static void integration_wire(unsigned chapter, bool navigation, const uint8_t **data, size_t *length);\n'
        'struct epd_compute_pipe_host {')
    host = once(host, 'initial->values[4] == current_document->chapter',
        'initial->values[4] == (current_document->toc_context ? current_document->toc_context->nav_chapter : current_document->chapter)')
    host = once(host, '    return EPD_COMPUTE_OK;\n}\nint epd_compute_pipe_step',
        '    integration_wire(initial->values[4], (initial->values[6] & BOOK_DOCUMENT_NAV_ONLY) != 0, &(*out)->wire, &(*out)->wire_length);\n'
        '    if (initial->values[6] & BOOK_DOCUMENT_NAV_ONLY) for (unsigned i = 0; i < initial->size; ++i) {\n'
        '        book_document_event_t e; memcpy(&e, (*out)->wire + (*out)->at, sizeof(e));\n'
        '        (*out)->at += e.bytes; assert((*out)->at <= (*out)->wire_length);\n'
        '    }\n    return EPD_COMPUTE_OK;\n}\nint epd_compute_pipe_step')
    start = host.index('document_cache_t *document_cache_create(')
    end = host.index('bool document_cache_file_signature(', start)
    host = host[:start] + '#include "toc_context_cache.inc"\n' + host[end:]
    (build / 'host.inc').write_text(host, encoding='utf-8')

    flow = (build / 'flow.inc').read_text(encoding='utf-8')
    flow = once(flow, 'unsigned references,layout,',
                'unsigned cache_begin,cache_end;size_t blocked_available;\n    unsigned references,layout,')
    flow = once(flow, 'unsigned content_width, top, bottom, line_space;',
                'unsigned page,content_width, top, bottom, line_space;')
    (build / 'flow.inc').write_text(flow, encoding='utf-8')
    fixture = (build / 'global_test.c').read_text(encoding='utf-8')
    # Reuse providers and extracted production routines, without the old suite's cases.
    assert fixture.count('static void retention_cases(void)') == 1
    fixture = fixture.split('static void retention_cases(void)', 1)[0]
    fixture = once(fixture, function(fixture, 'layout_accept'), '\n\n'.join(
        function(production, name) for name in ('cache_in_window', 'cache_store', 'layout_accept')))
    fixture = once(fixture, 'static void epd_app_font_page_release(void *pins) { assert(!pins); }',
        'static unsigned released_pins;\nstatic void epd_app_font_page_release(void *pins) {\n'
        '    if (pins) { assert(*(unsigned *)pins == 0x1234abcd); ++released_pins; free(pins); }\n}\n')
    fixture = once(fixture, 'assert(milliseconds==2 && !commit);view.compute_foreground=foreground;',
        'assert(milliseconds==2 && !commit);document_flow();view.compute_foreground=foreground;')
    trim = (ROOT / 'tests/reader_global_window/trim_cases.c').read_text(encoding='utf-8')
    helpers = '\n\n'.join(function(trim, name) for name in ('check_trim', 'trim_page', 'trim_frame', 'trim_has'))
    fixture += '\n#include "integration_support.inc"\n' + helpers + '\n#include "combo_cases.inc"\n'
    fixture += '''int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    InitializeCriticalSection(&irq_lock);InitializeCriticalSection(&io_lock);
    assert(argc==2);snprintf(test_root,sizeof(test_root),"%s",argv[1]);
    snprintf(persistent_path,sizeof(persistent_path),"%s/persisted.bin",test_root);
    snprintf(index_file,sizeof(index_file),"%s/index.bin",test_root);
    integration_make_wires();pipe_events_per_step=1;pipe_step_ticks=60;
    combo_case();
    document_service_shutdown();document_worker_shutdown();document_service_process();
    assert_compute_released();
    assert(!documents && !allocated_bytes && !fd_live && !context_caches && !observers && created==disposed);
    for(unsigned i=0;i<20;++i){free(integration_body[i].data);free(integration_nav[i].data);}
    DeleteCriticalSection(&io_lock);DeleteCriticalSection(&irq_lock);
    printf("PASS combined trim-nav: created=%u disposed=%u frames=%zu layout=%zu bytes=%lld fd=%u\\n",
        created,disposed,frame_bytes,layout_bytes,allocated_bytes,fd_live);
    return 0;
}
'''
    # Compile next to fresh generated includes; never resolve a stale tmp snapshot.
    (build / 'global_test.c').write_text(fixture, encoding='utf-8')
    for name in ('toc_context_cache.inc', 'integration_support.inc', 'combo_cases.inc'):
        (build / name).write_bytes((HERE / name).read_bytes())
    return runner


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'modules/common/reader/reader_view.c')
    parser.add_argument('--document-source', type=Path, default=ROOT / 'modules/books/document/document.c')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--variant', choices=('candidate', 'no-gate', 'no-trim', 'no-shared-nav'), default='candidate')
    args = parser.parse_args()
    build = (args.build_dir or ROOT / 'tmp/reader-navigation-regression/integration' / args.variant).resolve()
    build.mkdir(parents=True, exist_ok=True)
    reader, document = args.source.resolve(), args.document_source.resolve()
    runner = prepare(build, reader, document, args.variant)
    cc, env = runner['compiler_environment'](None)
    if Path(cc).stem.lower() != 'cl':
        parser.error('The document host requires MSVC with AddressSanitizer.')
    includes = [build, SUPPORT, ROOT / 'modules/books/document', ROOT / 'components/compute/include',
        ROOT / 'src/platform', ROOT / 'components/compute_reader_render/include',
        ROOT / 'components/compute_reading_data/include', ROOT / 'modules/common/reader/compute/include',
        ROOT / 'modules/common/reader']
    units = [build / 'global_test.c', ROOT / 'components/compute_reader_render/render_kernel.c',
        ROOT / 'components/compute_reader_render/render_format.c', ROOT / 'modules/common/reader/compute/reader_kernel.c',
        ROOT / 'modules/common/reader/compute/reader_page.c', ROOT / 'modules/common/reader/reader_page_cache.c']
    exe = build / 'combo.exe'
    command = [cc, '/nologo', '/std:c11', '/utf-8', '/Od', '/Zi', '/Gy', '/W3', '/D_CRT_SECURE_NO_WARNINGS',
        '/DHCPU_DOCUMENT_SOURCE', '/fsanitize=address', *[f'/I{p}' for p in includes], *map(str, units),
        f'/Fe{exe}', '/link', '/OPT:REF']
    compiled = subprocess.run(command, cwd=build, env=env, capture_output=True, timeout=90)
    (build / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise SystemExit((compiled.stdout + compiled.stderr).decode(errors='replace'))
    run = subprocess.run([str(exe), str(build)], cwd=build, env=env, capture_output=True, timeout=60)
    output = run.stdout + run.stderr
    (build / 'result.log').write_bytes(output)
    print(output.decode(errors='replace'))
    marker = {'no-gate': 'TRIM_NAV_BACKGROUND_LIVENESS', 'no-shared-nav': 'TRIM_NAV_BACKGROUND_LIVENESS',
              'no-trim': 'CACHED_TURN_STALE_FRAME'}.get(args.variant)
    passed = (run.returncode == 1 and marker.encode() in output) if marker else run.returncode == 0
    passed = passed and b'AddressSanitizer' not in output
    result = dict(passed=passed, exit_code=run.returncode, asan=True, expected_failure=marker,
        input_reader_sha256=digest(reader), input_document_sha256=digest(document),
        reader_sha256=digest(build / 'reader_under_test.c'),
        document_sha256=digest(build / 'document_under_test.c'))
    (build / 'evidence.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
