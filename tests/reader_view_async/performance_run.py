"""Run production reader preparation and frame-adoption regressions on the host."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CASES = {
    'page_warm': ['foreground_pending', 'foreground_failure', 'background_pending',
                  'background_failure', 'background_slice', 'background_pending_slice', 'background_selection'],
    'frame_adopt': ['adopt_same', 'adopt_reject', 'adopt_live', 'prepare_promote', 'prepare_replace',
                    'prepare_optional', 'prepare_cached_target', 'near_forward', 'near_boundaries',
                    'near_preview_boundaries', 'near_third_gate', 'near_idle_delay'],
}
FUNCTIONS = {
    'page_warm': ['page_required', 'page_work_allowed', 'page_memory_block', 'page_fonts_ready', 'cache_warm_candidate', 'cache_warm'],
    'frame_adopt': ['page_required', 'request_memory_failed', 'page_work_allowed', 'page_memory_block', 'frame_priority', 'frame_find', 'frame_adopt', 'frame_window_contains', 'frame_prepare', 'cursor_at_segment_start', 'near_frames_pending', 'worker_idle_delay'],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC'))
    parser.add_argument('--case', action='append', choices=[c for cs in CASES.values() for c in cs])
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--source', type=Path, default=ROOT / 'modules/common/reader/reader_view.c')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    extract = runpy.run_path(str(HERE / 'run.py'))['extract']
    constants = []
    for name in ['VIEW_GLYPH_BURST', 'VIEW_GLYPH_BURST_MS', 'VIEW_BACKGROUND_TRACE', 'VIEW_NEIGHBOR_PRIORITY']:
        match = re.search(r'^#define ' + name + r'[^\n]*', source, re.M)
        assert match, name
        constants.append(match[0])
    font_header = (ROOT / 'src/ui/platform/epd_app_font.h').read_text(encoding='utf-8')
    constants.append(re.search(r'^#define EPD_APP_FONT_NO_MEMORY[^\n]*', font_header, re.M)[0])
    cc, env = runpy.run_path(str(ROOT / 'tests/reader_prefetch/cache_run.py'))['compiler_environment'](args.cc)
    reader = ROOT / 'modules/common/reader'
    failures = []
    total = 0
    print('reader_view.c sha256=' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='reader-performance-') as folder:
        build = Path(folder)
        for fixture, cases in CASES.items():
            selected = [c for c in cases if not args.case or c in args.case]
            if not selected:
                continue
            text = (HERE / (fixture + '.c')).read_text(encoding='utf-8')
            text = '\n'.join(constants) + '\n' + text.replace('/* CONSTANTS */', '')
            text = text.replace('/* PRODUCTION */', '\n\n'.join(extract(n, source) for n in FUNCTIONS[fixture]))
            unit = build / (fixture + '.c')
            unit.write_text(text, encoding='utf-8')
            exe = build / (fixture + ('.exe' if os.name == 'nt' else ''))
            sources = [unit, reader / 'reader_page_cache.c', reader / 'compute/reader_page.c']
            includes = [reader, reader / 'compute/include']
            if Path(cc).stem.lower() == 'cl':
                command = [cc, '/nologo', '/TC', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4200',
                           '/wd4505', '/wd4459', '/D_CRT_SECURE_NO_WARNINGS', '/Od',
                           *(f'/I{p}' for p in includes), *map(str, sources), '/Fe' + str(exe)]
                command += ['/fsanitize=address', '/Zi'] if args.sanitize else ['/RTC1']
            else:
                command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                           '-O1', '-g', *(f'-I{p}' for p in includes), *map(str, sources), '-o', str(exe)]
                if args.sanitize:
                    command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
            for case in selected:
                total += 1
                result = subprocess.run([str(exe), case], cwd=build, env=env, timeout=20)
                if result.returncode:
                    failures.append(case)
    print(f'Performance regression: {total - len(failures)}/{total} passed.', flush=True)
    if failures:
        raise SystemExit('Failed: ' + ', '.join(failures))


if __name__ == '__main__':
    main()
