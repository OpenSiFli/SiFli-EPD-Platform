"""Exercise TURN handoff against extracted view code and the real render component."""
import argparse
import os
from pathlib import Path
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FUNCTIONS = ['frame_turn_matches', 'frame_turn_candidate', 'frame_turn_cached', 'render_cancel', 'turn_cancel', 'frame_handoff_note', 'render_cancelled',
             'render_submit', 'render_flush', 'frame_adopt', 'command_apply', 'ui_command']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'modules/common/reader/reader_view.c')
    parser.add_argument('--cc', default=os.environ.get('CC'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--case', action='append')
    parser.add_argument('--negative', choices=('pins', 'command', 'target'),
                        help='Remove one handoff guard and require its regression to fail')
    args = parser.parse_args()
    if args.negative and args.case:
        parser.error('--negative selects its own case')
    extract = runpy.run_path(str(HERE / 'run.py'))['extract']
    production = args.source.read_text(encoding='utf-8')
    # Older sources have no handoff helpers; their original callers remain intact.
    functions = [extract(name, production) for name in FUNCTIONS
                 if name not in ('frame_turn_matches', 'turn_cancel', 'frame_handoff_note') or name + '(' in production]
    code = '\n\n'.join(functions)
    negatives = {
        'pins': ('(!frame->pins && !frame->renderer && !frame->render_finished)',
                 '(!frame->renderer && !frame->render_finished)',
                 'pins-forward', 'FAIL pins-only exact TURN cancelled before command consumption'),
        'command': ('if (frame_turn_matches(view.rendering, &command) || frame_turn_cached(view.rendering, &command)) {',
                    'if (command.kind == INTENT_TURN && view.rendering && !view.rendering->cancelled && '
                    '(!view.rendering->renderer || frame_turn_matches(view.rendering, &command))) {',
                    'pins-reject-6', 'FAIL mismatched pins-only frame retained at command consumption'),
        'target': (' ||\n        !cursor_equal((document_cursor_t){frame->page.cursor.record, frame->page.cursor.byte}, page->cursor) ||\n'
                   '        frame->page.eof != page->eof', '',
                   'target-end', 'FAIL pins-only frame adopted after target end changed'),
    }
    if args.negative:
        old, new, negative_case, expected_failure = negatives[args.negative]
        assert code.count(old) == (2 if args.negative == "pins" else 1), args.negative
        code = code.replace(old, new)
    fixture = (HERE / 'handoff.c').read_text(encoding='utf-8')
    fixture = fixture.replace('/* PRODUCTION */', code)
    cc, env = runpy.run_path(str(ROOT / 'tests/reader_prefetch/cache_run.py'))['compiler_environment'](args.cc)
    component = ROOT / 'components/compute_reader_render'
    support = ROOT / 'tests/reader_render_compute'
    includes = [component / 'include', ROOT / 'components/compute/include',
                ROOT / 'src/platform', support / 'stubs', support]
    with tempfile.TemporaryDirectory(prefix='reader-handoff-') as folder:
        build = Path(folder)
        source = build / 'handoff.c'
        source.write_text(fixture, encoding='utf-8')
        units = [source, support / 'support.c'] + [component / (name + '.c') for name in
                 ('render_client', 'render_format', 'render_kernel', 'render_remote')]
        exe = build / ('handoff.exe' if os.name == 'nt' else 'handoff')
        if Path(cc).stem.lower() == 'cl':
            command = [cc, '/nologo', '/TC', '/std:c11', '/utf-8', '/W4', '/WX', '/wd4505',
                       '/D_CRT_SECURE_NO_WARNINGS', *['/I' + str(p) for p in includes],
                       *map(str, units), '/Fe' + str(exe)]
            command += ['/fsanitize=address', '/Zi'] if args.sanitize else ['/RTC1', '/Od']
        else:
            command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                       *['-I' + str(p) for p in includes], *map(str, units), '-o', str(exe)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined']
        subprocess.run(command, cwd=build, env=env, check=True, timeout=60)
        if args.negative:
            result = subprocess.run([str(exe), negative_case], cwd=build, env=env,
                                    capture_output=True, text=True, timeout=20)
            if result.returncode != 1 or expected_failure not in result.stdout + result.stderr:
                raise SystemExit(f'FAIL {args.negative} negative control: {result.returncode}\n'
                                 + result.stdout + result.stderr)
            print(f'PASS {args.negative} negative control detected: {expected_failure}')
            return
        cases = args.case or ['forward', 'reverse', 'inflight', 'preempted', 'superseded', *map(str, range(12)),
                             'pins-forward', 'pins-reverse', 'pins-resources', 'target-end', 'target-eof',
                             *[f'pins-reject-{i}' for i in range(12)],
                             *[f'pins-terminal-{i}' for i in range(5)]]
        for case in cases:
            subprocess.run([str(exe), case], cwd=build, env=env, check=True, timeout=20)
    print(f'PASS {len(cases)}/{len(cases)} handoff cases')


if __name__ == '__main__':
    main()
