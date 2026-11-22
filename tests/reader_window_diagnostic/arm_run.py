"""Verify module diagnostic propagation and compile reader units for Cortex-M33."""
from pathlib import Path
import argparse
import ast
import difflib
import hashlib
import json
import os
import runpy
import shlex
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def sections(path):
    raw = path.read_bytes()
    assert raw[:6] == b'\x7fELF\x01\x01'
    offset = struct.unpack_from('<I', raw, 32)[0]
    stride, count, names_index = struct.unpack_from('<HHH', raw, 46)
    items = [struct.unpack_from('<10I', raw, offset + i * stride) for i in range(count)]
    names = items[names_index]
    strings = raw[names[4]:names[4] + names[5]]
    out = {}
    for name, kind, flags, addr, at, size, link, info, align, entry in items:
        if flags & 2:
            label = strings[name:strings.index(0, name)].decode()
            out[label] = {'size': size, 'flags': flags, 'type': kind, 'align': align,
                          'sha256': None if kind == 8 else hashlib.sha256(raw[at:at + size]).hexdigest()}
    return out


def functions(path, names, scope):
    tree = ast.parse(path.read_text(encoding='utf-8'))
    tree.body = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
    exec(compile(tree, str(path), 'exec'), scope)


def export(enabled, build, inherited):
    build.mkdir(parents=True, exist_ok=True)
    scope = {'os': os, 'Path': Path, 'ast': ast}
    functions(ROOT / 'SiFli-SDK/tools/build/ua.py', ['PrepareUA'], scope)
    functions(ROOT / 'tools/app_profile.py', ['write_changed', 'stabilize_module_defines'], scope)
    # INPUT_TEST is an environment definition in the firmware SConstruct;
    # SDK PrepareUA collects group definitions, which do not contain it.
    group_defines = [d for d in inherited if 'EPD_INPUT_TEST_ENABLED' not in str(d)]
    old = os.getcwd()
    os.chdir(build)
    try:
        scope['PrepareUA']([{'CPPDEFINES': group_defines}],
                           str(ROOT / 'SiFli-SDK/rtos/rtthread'), str(ROOT / 'project'))
        assert b'EPD_INPUT_TEST_ENABLED' not in Path('rtua.py').read_bytes()
        tree = ast.parse((ROOT / 'project/SConstruct').read_text())
        call = next(n for n in tree.body if isinstance(n, ast.Expr) and isinstance(n.value, ast.Call)
                    and isinstance(n.value.func, ast.Name) and n.value.func.id == 'stabilize_module_defines')
        scope['ARGUMENTS'] = {'INPUT_TEST': str(enabled)}
        code = compile(ast.Module(body=[call], type_ignores=[]), 'project/SConstruct', 'exec')
        exec(code, scope)
        first = Path('rtua.py').read_bytes()
        exec(code, scope)
        assert Path('rtua.py').read_bytes() == first
        defines = runpy.run_path(str(build / 'rtua.py'))['GetCPPDEFINES']()
        assert ('EPD_INPUT_TEST_ENABLED=1' in defines) == bool(enabled)
        assert set(defines) - set(group_defines) == ({'EPD_INPUT_TEST_ENABLED=1'} if enabled else set())
    finally:
        os.chdir(old)
    return defines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--firmware', type=Path, default=ROOT / 'project/build_sf32lb57-epd-gyd7n6_hcpu')
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--baseline-source', type=Path, help='Optional reader_view.c before the diagnostic patch')
    args = parser.parse_args()
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='reader-window-arm-'))
    output.mkdir(parents=True, exist_ok=True)
    firmware = args.firmware.resolve()
    compiler = args.compiler or shutil.which('arm-none-eabi-gcc')
    if not compiler:
        compiler = sorted((Path.home() / '.sifli/tools/arm-none-eabi-gcc').glob('*/bin/arm-none-eabi-gcc.exe'))[-1]
    compiler = str(compiler)
    profile = json.loads((firmware / 'app-profile.json').read_text())
    flags = shlex.split(profile['module_cflags'])
    rtua = runpy.run_path(str(firmware / 'rtua.py'))
    inherited = rtua['GetCPPDEFINES']()
    paths = [str(ROOT / 'modules/books/output/i18n'), str(firmware)]
    paths += rtua['GetCPPPATH'](str(ROOT / 'project'), str(ROOT / 'SiFli-SDK/rtos/rtthread'))
    paths += [str(ROOT / p) for p in ('modules/books', 'modules/common/reader',
              'modules/common/reader/compute/include', 'src/platform', 'components/compute/include',
              'components/compute_reader_render/include', 'components/compute_reading_data/include')]
    includes = ['-I' + p for p in paths]
    results = []
    for enabled in (0, 1):
        build = output / str(enabled)
        defines = export(enabled, build, inherited)
        macroargs = ['-D' + (d if isinstance(d, str) else '='.join(d)) for d in defines]
        macroargs += ['-DBUILD_DLMODULE', '-DBOOK_FORMAT_TARGET', '-DBOOKS_READER_UI', '-DHAVE_CCONFIG_H']
        variants = [('current', ROOT / 'modules/common/reader/reader_view.c')]
        if args.baseline_source:
            variants.insert(0, ('baseline', args.baseline_source.resolve()))
        compiled = {}
        for variant, source in variants:
            obj = build / (variant + '.o')
            cmd = [compiler, *flags, *includes, *macroargs, '-O2', '-Werror', '-fstack-usage',
                   '-c', str(source), '-o', str(obj)]
            result = subprocess.run(cmd, cwd=ROOT / 'project', capture_output=True, timeout=90)
            (build / (variant + '.log')).write_bytes(result.stdout + result.stderr)
            if result.returncode:
                print((result.stdout + result.stderr).decode(errors='replace'))
            result.check_returncode()
            raw = obj.read_bytes()
            assert (b'reader init_trace' in raw) == bool(enabled)
            assert (b'reader window_snapshot' in raw) == bool(enabled and variant == 'current')
            compiled[variant] = sections(obj)
            results.append({'enabled': enabled, 'variant': variant, 'command': cmd,
                            'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                            'sections': compiled[variant]})
        if args.baseline_source:
            before, after = compiled['baseline'], compiled['current']
            writable = lambda s: {n: v for n, v in s.items() if v['flags'] & 1}
            assert writable(before) == writable(after), 'snapshot added static storage'
            if not enabled:
                changed = [n for n in set(before) | set(after) if before.get(n) != after.get(n)]
                assert not changed or changed == ['.text.frame_prepare'], changed
                if changed:
                    tool = str(Path(compiler).with_name('arm-none-eabi-objdump' + Path(compiler).suffix))
                    dump = [subprocess.check_output([tool, '-dr', '-j', '.text.frame_prepare', str(build / f'{v}.o')],
                            text=True).splitlines(True)[4:] for v in ('baseline', 'current')]
                    delta = list(difflib.unified_diff(*dump))
                    (build / 'normal-diff.txt').write_text(''.join(delta))
                    lines = [s for s in delta if s.startswith(('-', '+')) and not s.startswith(('---', '+++'))]
                    assert len(lines) == 2 and all('movw' in s and 'r2, #' in s for s in lines), lines
        obj = build / 'command.o'
        cmd = [compiler, *flags, *includes, *macroargs, '-O2', '-Werror', '-Werror=implicit-function-declaration',
               '-c', str(ROOT / 'modules/books/command.c'), '-o', str(obj)]
        result = subprocess.run(cmd, cwd=ROOT / 'project', capture_output=True, timeout=90)
        (build / 'command.log').write_bytes(result.stdout + result.stderr)
        if result.returncode:
            print((result.stdout + result.stderr).decode(errors='replace'))
        result.check_returncode()
        assert (b'epd_reader_window_diagnostic' in obj.read_bytes()) == bool(enabled)
        print(f'PASS PrepareUA -> module definitions -> ARM reader/command INPUT_TEST={enabled}', flush=True)
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('Evidence:', output)


if __name__ == '__main__':
    main()
