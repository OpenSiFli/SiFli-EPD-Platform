"""Exercise phased HCPU sessions, layout, navigation interruption and recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
from epub_body_nav_tools import HERE, ROOT, mirror, package
from epub_toc_budget_run import compiler_environment
from hcpu_reader_extract import extract, function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=ROOT)
    parser.add_argument('--remote', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append')
    parser.add_argument('--cache-spool', action='store_true', help='Check real spool ownership and cleanup')
    parser.add_argument('--document-source', type=Path)
    parser.add_argument('--expect-failure', choices=('NAV_FLOW_GATE',))
    args = parser.parse_args()
    candidate = args.candidate.resolve(); build = args.output.resolve(); build.mkdir(parents=True, exist_ok=True)
    overrides = mirror(candidate, build/'source')
    for header in ('app_service.h','rtthread.h','rthw.h','storage.h','storage_file.h','epd_memory.h',
                   'reader_compute_client.h','unistd.h','dirent.h','platform/epd_app.h','platform/epd_app_library.h'):
        p=build/header;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "hcpu_host.h"\n',encoding='utf-8')
    document_source = args.document_source.resolve() if args.document_source else candidate/'modules/books/document/document.c'
    document = document_source.read_bytes()
    (build/'document_under_test.c').write_bytes(document if args.document_source else overrides['document.c'].read_bytes())
    mock_path=candidate/'tests/reader_document_compute/hcpu_test.c'
    mocks=(mock_path if mock_path.exists() else HERE/'hcpu_test.c').read_text(encoding='utf-8')
    mocks=mocks.replace('#include "hcpu_fd_host.h"', '''#include "hcpu_fd_host.h"
static unsigned phased_syncs;
static int phased_sync(int fd) { ++phased_syncs; return _commit(fd); }
#undef fsync
#define fsync phased_sync
static long warm_read_failure_at = -1;
static unsigned warm_read_failures;
static int warm_read(int fd, void *out, size_t size) {
    long at = _tell(fd);
    if (warm_read_failure_at >= 0 && at >= 0 && (size_t)at + size > (size_t)warm_read_failure_at) {
        ++warm_read_failures; errno = EIO; return -1;
    }
    return fd_read(fd, out, size);
}
#undef read
#define read warm_read''')
    begin=mocks.index('int epd_compute_pipe_open(')
    end=mocks.index('int epd_compute_pipe_step(',begin)
    mocks=mocks[:begin]+'#include "epub_body_nav_pipe.inc"\n'+mocks[end:]
    mocks=mocks.replace('    if (p->at == p->wire_length) {', '''    if (p->at == p->wire_length) {
        if (!(p->message.values[6] & (BOOK_DOCUMENT_NAV_ONLY | BOOK_DOCUMENT_NOTE)))
            document_prefetch_enable(p->document, false);''')
    mocks=mocks.replace('i < 10000;', 'i < 40000;')
    mocks=mocks.replace('assert(!observers); ++observers;', '++observers;')
    mocks=mocks.replace('assert(observers == 1); --observers;', 'assert(observers); --observers;')
    (build/'body_nav_host.inc').write_text(mocks,encoding='utf-8')
    flow=(HERE/'hcpu_page_flow.h').read_text(encoding='utf-8').replace('    char link_target[2];',
        '    uint32_t navigation_link, navigation_toc;\n    char link_target[2];')
    flow=flow.replace('    int turn_pending;', '    int turn_pending, direction;')
    # This page fixture has one view and no other context from which to transfer a spare.
    flow=flow.replace('#include "reader_page_functions.inc"',
        'static bool view_interrupted(void) { return false; }\n'
        'static reader_compute_job_t *compute_acquire(int *result) { return reader_compute_create(result); }\n'
        '#include "reader_page_functions.inc"')
    if 'epd_app_font_set_purpose' not in flow:
        flow=flow.replace('static int epd_app_font_prepare_text(',
            'static int epd_app_font_set_purpose(const lv_font_t *font, unsigned purpose)\n'
            '{ (void)font; (void)purpose; return 0; }\nstatic int epd_app_font_prepare_text(',1)
    extract(candidate/'modules/common/reader/reader_view.c',build)
    reader=(candidate/'modules/common/reader/reader_view.c').read_text(encoding='utf-8')
    ahead=re.search(r'^#define VIEW_LAYOUT_AHEAD\s+(\S+)',reader,re.M);assert ahead
    flow='#define VIEW_LAYOUT_AHEAD '+ahead[1]+'\n'+flow
    (build/'hcpu_page_flow.h').write_text(flow,encoding='utf-8')
    (build/'navigation_functions.inc').write_text('\n'.join(function(reader,n) for n in
        ('navigation_clear','link_clear','follow_link','follow_toc','navigation_resolve','epd_reader_toc_prepare','epd_reader_toc_count',
         'epd_reader_toc_seek')),encoding='utf-8')
    position=(candidate/'modules/books/document/document_view.c').read_text(encoding='utf-8')
    (build/'position_compatible.inc').write_text(function(position,'position_compatible'),encoding='utf-8')
    includes=[build,HERE,ROOT/'modules/books/document',ROOT/'components/compute/include',ROOT/'src/platform',
              ROOT/'components/compute_reader_render/include',ROOT/'components/compute_reading_data/include',
              ROOT/'modules/common/reader/compute/include']
    kernels=[ROOT/'components/compute_reader_render'/s for s in ('render_kernel.c','render_format.c')]
    kernels += [ROOT/'modules/common/reader/compute'/s for s in ('reader_kernel.c','reader_page.c')]
    compiler, env=compiler_environment('x86'); exe=build/'body-nav.exe'
    fixture=build/'epub_body_nav_hcpu.c'
    fixture_source=candidate/'tests/reader_document_compute'/fixture.name
    fixture.write_bytes((fixture_source if fixture_source.exists() else HERE/fixture.name).read_bytes())
    window=re.search(r'#define VIEW_METRICS_WINDOW (\d+)u?',reader);assert window
    run=subprocess.run([compiler,'/nologo','/std:c11','/utf-8','/Od','/Zi','/Gy','/W3','/D_CRT_SECURE_NO_WARNINGS',
        '/DHCPU_DOCUMENT_SOURCE',f'/DVIEW_METRICS_WINDOW={window[1]}','/fsanitize=address',*[f'/I{i}' for i in includes],
        str(fixture),*map(str,kernels),f'/Fe{exe}','/link','/OPT:REF'],cwd=build,env=env,capture_output=True,timeout=120)
    (build/'compile.log').write_bytes(run.stdout+run.stderr)
    assert not run.returncode,(run.stdout+run.stderr).decode(errors='replace')
    position_exe=build/'position.exe'
    run=subprocess.run([compiler,'/nologo','/std:c11','/utf-8','/fsanitize=address',f'/I{build}',
        str(HERE/'epub_body_nav_position.c'),f'/Fe{position_exe}'],cwd=build,env=env,capture_output=True,timeout=120)
    assert not run.returncode,(run.stdout+run.stderr).decode(errors='replace')
    run=subprocess.run([str(position_exe)],cwd=build,env=env,capture_output=True,timeout=30)
    (build/'position.log').write_bytes(run.stdout+run.stderr)
    assert not run.returncode,(run.stdout+run.stderr).decode(errors='replace')
    if args.cache_spool:
        source=candidate/'tests/reader_document_compute/cache_buckets_cleanup.c'
        if not source.exists(): source=HERE/'cache_buckets_cleanup.c'
        spool_exe=build/'spool-cleanup.exe'
        run=subprocess.run([compiler,'/nologo','/std:c11','/utf-8','/Od','/Zi','/Gy','/W3','/D_CRT_SECURE_NO_WARNINGS',
            '/DHCPU_DOCUMENT_SOURCE',f'/DVIEW_METRICS_WINDOW={window[1]}','/fsanitize=address',*[f'/I{i}' for i in includes],
            str(source),*map(str,kernels),f'/Fe{spool_exe}','/link','/OPT:REF'],cwd=build,env=env,capture_output=True,timeout=120)
        (build/'spool-compile.log').write_bytes(run.stdout+run.stderr)
        assert not run.returncode,(run.stdout+run.stderr).decode(errors='replace')
        spool_directory=build/'spool-checks';spool_directory.mkdir(exist_ok=True)
        run=subprocess.run([str(spool_exe),str(spool_directory)],cwd=build,env=env,capture_output=True,timeout=30)
        (build/'spool-cleanup.log').write_bytes(run.stdout+run.stderr)
        assert not run.returncode,(run.stdout+run.stderr).decode(errors='replace')
        print((run.stdout+run.stderr).decode(errors='replace'),flush=True)
    cases=args.case or ['normal','warm','restore','nav-error','preempt','cancel','media','note','link','toc',
                       'link-error','toc-error','short','query','flow','nav-preempt-body','split-unchanged',
                       'restore-start','image-restore','target-drain','navigation-release',
                       'warm-nav-read','warm-nav-record','warm-nav-alloc','warm-nav-reader-alloc',
                       'warm-body-read','warm-body-alloc','warm-cancel','warm-media','warm-nav-short','warm-whole-truncated',
                       'admission-body','admission-nav','admission-body-cancel','admission-nav-cancel','admission-queued-nav',
                       'admission-busy','admission-busy-partial','admission-busy-cancel','admission-busy-body',
                       'admission-busy-limit','admission-busy-auto-limit','admission-busy-body-limit',
                       'body-cache-pending','body-cache-active','body-cache-failed','restore-cache']
    results=[]
    for case in cases:
        directory=build/case;directory.mkdir(exist_ok=True)
        wire=args.remote.resolve()/('fixture/image-restore' if case == 'image-restore' else 'fixture/short' if case == 'short' else
            'fixture' if case in ('note','link','toc','link-error','toc-error') else '')
        run=subprocess.run([str(exe),str(directory),str(wire),case],cwd=build,env=env,capture_output=True,timeout=120)
        (directory/'result.log').write_bytes(run.stdout+run.stderr)
        print(case,(run.stdout+run.stderr).decode(errors='replace'),flush=True)
        log = run.stdout + run.stderr
        passed = (run.returncode == 1 and args.expect_failure.encode() in log) if args.expect_failure else run.returncode == 0
        results.append(dict(case=case, passed=passed, expected_failure=args.expect_failure,
            exit_code=run.returncode, document_sha256=hashlib.sha256(document).hexdigest(),
            reader_sha256=hashlib.sha256((candidate/'modules/common/reader/reader_view.c').read_bytes()).hexdigest(),
            log=str(directory/'result.log')))
    (build/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    package(candidate.parent)
    assert all(r['passed'] for r in results)
    print('Report:', build/'results.json', flush=True)


if __name__=='__main__': main()
