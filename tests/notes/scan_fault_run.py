"""ASan tests of production notes storage, scan callbacks, and the I/O worker."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from notebook_run import compiler_environment


def mask(source):
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: re.sub(r'[^\n]', ' ', m[0]), source, flags=re.S)


def function(source, name):
    clean = mask(source)
    found = re.search(r'(?m)^[\w \t*]+\b' + name + r'\([^;{}]*\)\s*\{', clean)
    if not found:
        raise ValueError(name)
    start = clean.index('{', found.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (clean[end] == '{') - (clean[end] == '}')
        end += 1
    return found.start(), end


def generate(root, runtime, out):
    source = runtime.read_text(encoding='utf-8')
    header = (root / 'modules/notes/notes_runtime.h').read_text(encoding='utf-8')
    types = header[header.index('typedef enum {'):header.index('void notes_runtime_init(')]
    for begin, end in [('typedef struct { char name[256];', 'size_t notes_files('),
                       ('typedef struct { uint32_t note, block;', 'size_t notes_tasks(')]:
        types += header[header.index(begin):header.index(end)]
    types += source[source.index('typedef struct job {'):source.index('static bool book_name_valid(')]
    if 'scan_context_t' in source:
        types += source[source.index('typedef struct { bool started;'):source.index('static bool scan_row(')]
    (out / 'scan_types.inc').write_text(types, encoding='utf-8')
    pieces = []

    def add(begin, end):
        line = source.count('\n', 0, begin) + 1
        pieces.append(f'#line {line} "{runtime.as_posix()}"\n' + source[begin:end] + '\n')

    for name in ['lock', 'unlock', 'now', 'changed', 'catalog_changed', 'wake_io', 'stopping',
                 'status_error', 'scan_row', 'scan_catalog', 'book_name_valid', 'book_add',
                 'books_load', 'job_free', 'notes_command', 'notes_status', 'idle_hold']:
        if name == 'scan_catalog' and 'scan_catalog(' not in source:
            continue
        add(*function(source, name))
    start, end = function(source, 'io_command')
    # Keep the production prologue, N_SCAN arm, default and epilogue unchanged;
    # unrelated command arms are omitted from this scan-specific translation unit.
    add(start, source.index('    case N_PREFERENCES:', start))
    scan = source.index('    case N_SCAN:', start)
    add(scan, source.index('    case N_NEW:', scan))
    add(source.index('    default: e = NOTES_INVALID;', scan), end)
    add(*function(source, 'io_run'))
    (out / 'scan_runtime.inc').write_text('\n'.join(pieces), encoding='utf-8')
    posix = root / 'SiFli-SDK/rtos/rtthread/components/dfs/src/dfs_posix.c'
    text = posix.read_text(encoding='utf-8')
    a, b = function(text, 'readdir')
    (out / 'scan_readdir.inc').write_text(
        f'#line {text.count(chr(10), 0, a)+1} "{posix.as_posix()}"\n' + text[a:b], encoding='utf-8')
    return posix


CASES = ['missing_summary', 'corrupt_summary', 'stale_errno', 'body_io_partial',
         'body_invalid_partial', 'directory_io_first', 'directory_io_partial', 'directory_close_io',
         'directory_open_io', 'all_body_io_keep_old', 'empty_success_clears_old', 'callback_oom',
         'boot_error_visible', 'refresh_body_error', 'refresh_directory_error', 'retry_after_partial',
         'repeated_scan_no_duplicates', 'cancel_keep_old']


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runtime = args.baseline / 'baseline_notes_runtime.c' if args.baseline else root / 'modules/notes/notes_runtime.c'
    store = args.baseline / 'baseline_notes_store.c' if args.baseline else root / 'modules/notes/notes_store.c'
    posix = generate(root, runtime, out)
    compiler, environment = compiler_environment()
    environment['VSLANG'] = '1033'
    environment['ASAN_OPTIONS'] = 'halt_on_error=1:allocator_may_return_null=1'
    includes = [out, root / 'tests/notes/scan_host', root / 'tests/notes/host', root / 'modules/notes']
    sources = [root / 'tests/notes/scan_fault_test.c', store, root / 'modules/notes/notes_archive.c']
    exe = out / ('scan_tests.exe' if os.name == 'nt' else 'scan_tests')
    if os.name == 'nt':
        command = [compiler, '/nologo', '/std:c11', '/utf-8', '/W3', '/Od', '/Zi', '/fsanitize=address',
                   '/DNOTES_HOST', '/D_CRT_SECURE_NO_WARNINGS', *['/I'+str(p) for p in includes],
                   *map(str, sources), '/Fe:'+str(exe)]
    else:
        command = [compiler, '-std=c11', '-g', '-O1', '-fsanitize=address,undefined',
                   '-fno-omit-frame-pointer', '-DNOTES_HOST', *['-I'+str(p) for p in includes],
                   *map(str, sources), '-o', str(exe)]
    build = subprocess.run(command, cwd=out, env=environment, capture_output=True, text=True, errors='replace')
    (out / 'build.log').write_text(build.stdout+build.stderr, encoding='utf-8')
    if build.returncode:
        print(build.stdout+build.stderr)
        raise SystemExit(build.returncode)
    results = []
    for case in args.case or CASES:
        result = subprocess.run([str(exe), case], cwd=out, env=environment, capture_output=True, text=True, timeout=15)
        log = result.stdout+result.stderr
        (out / (case+'.log')).write_text(log, encoding='utf-8')
        results.append({'case':case, 'exit_code':result.returncode,
                        'asan_error':'ERROR: AddressSanitizer' in log})
        print(('PASS ' if result.returncode == 0 else 'FAIL ')+case)
    paths = [runtime, store, root / 'modules/notes/notes_model.c', root / 'modules/notes/notes_model.h',
             root / 'modules/notes/notes_runtime.h', posix, *sources, Path(__file__),
             root / 'tests/notes/host_fs.c', root / 'tests/notes/scan_host/dirent.h']
    report = {'cases':results,'sanitizer':'AddressSanitizer','command':command,
              'sha256':{str(p.resolve()):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
    (out / 'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    raise SystemExit(int(any(r['exit_code'] for r in results)))


if __name__ == '__main__':
    main()
