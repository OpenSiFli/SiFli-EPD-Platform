"""Execute artifact ABI rejection and identity-equivalent production runtime cases.

SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def digest(data):
    return hashlib.sha256(data).hexdigest()


def elf_descriptor(data):
    assert data[:7] == b'\x7fELF\x01\x01\x01'
    header = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    assert header[1] == 40
    sections = [struct.unpack_from('<10I', data, header[5] + i * header[10])
                for i in range(header[11])]

    def offset(address):
        for section in sections:
            if section[2] & 2 and section[1] != 8 and section[3] <= address < section[3] + section[5]:
                return section[4] + address - section[3]
        raise AssertionError(f'Unmapped address {address:x}')

    for section in sections:
        if section[1] not in (2, 11):
            continue
        names = sections[section[6]]
        for at in range(section[4], section[4] + section[5], section[9]):
            name, value, size, _, _, index = struct.unpack_from('<IIIBBH', data, at)
            start = names[4] + name
            if data[start:data.index(0, start)] == b'epd_app_info' and index:
                assert size == 8
                desc = offset(value)
                return desc, offset(struct.unpack_from('<I', data, desc + 4)[0])
    raise AssertionError('No exported epd_app_info')


def prepare_cases(artifacts, output):
    original = artifacts / 'extracted-sap/hello'
    payloads = {p.relative_to(original).as_posix(): p.read_bytes()
                for p in original.rglob('*') if p.is_file()}
    desc, id_at = elf_descriptor(payloads['hello.so'])
    assert payloads['hello.so'][id_at:id_at + 6] == b'hello\0'
    cases = [('manifest_old', 'probe', 'sap', 1, 2), ('manifest_future', 'probe', 'sap', 3, 2),
             ('module_old', 'probe', 'sap', 2, 1), ('module_future', 'probe', 'sap', 2, 3)]
    for identity in ('hello', 'probe', 'books', 'music', 'notes', 'board'):
        for origin in ('sap', 'preinstalled'):
            cases.append((f'{identity}_{origin}', identity, origin, 2, 2))
    generated = output / 'cases'
    generated.mkdir(parents=True, exist_ok=True)
    evidence = []
    for label, identity, origin, manifest_abi, module_abi in cases:
        files = {k: v for k, v in payloads.items() if k not in ('app.json', 'files.sha256', 'hello.so')}
        elf = bytearray(payloads['hello.so'])
        struct.pack_into('<I', elf, desc, module_abi)
        elf[id_at:id_at + 5] = identity.encode('ascii')
        # Equal-size identity edits leave instructions, relocations and layout intact.
        allowed = set(range(desc, desc + 4)) | set(range(id_at, id_at + 5))
        assert len(elf) == len(payloads['hello.so'])
        assert all(a == b or i in allowed for i, (a, b) in enumerate(zip(elf, payloads['hello.so'])))
        files[identity + '.so'] = bytes(elf)
        index = ''.join(f'{digest(files[k])}  {k}\n' for k in sorted(files)).encode()
        files['files.sha256'] = index
        manifest = json.loads(payloads['app.json'])
        manifest.update(id=identity, name=f'Counter {identity}', abi=manifest_abi, files_sha256=digest(index))
        manifest['localized_names'] = {'zh_cn': f'Counter {identity}', 'en_us': f'Counter {identity}'}
        files['app.json'] = json.dumps(manifest, ensure_ascii=False, separators=(',', ':')).encode()
        folder = generated / label
        for name, data in files.items():
            target = folder / 'tree' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (folder / 'files.txt').write_text('\n'.join(files) + '\n', encoding='ascii')
        body = bytearray()
        for name in ['app.json', 'files.sha256', *sorted(k for k in files if k not in ('app.json', 'files.sha256'))]:
            encoded, data = name.encode(), files[name]
            body += struct.pack('<HHQ32sI', len(encoded), 0, len(data), hashlib.sha256(data).digest(), 0)
            body += encoded + data
        sap = struct.pack('<8sIIQ8s', b'SIFLSAP\0', 1, len(files), 32 + len(body), b'\0' * 8) + body
        (folder / 'package.sap').write_bytes(sap)
        evidence.append(dict(label=label, id=identity, origin=origin, manifest_abi=manifest_abi,
                             module_abi=module_abi, sap_sha256=digest(sap), elf_sha256=digest(elf)))
    (generated / 'cases.tsv').write_text('\n'.join(' '.join(map(str, c)) for c in cases) + '\n')
    return generated, evidence, digest(payloads['hello.so'])


def run_runtime(artifacts, output):
    compiler = module('host_compiler', ROOT / 'tests/reader_prefetch/cache_run.py')
    cc, env = compiler.compiler_environment(os.environ.get('CC'))
    build = output / 'runtime-build'
    build.mkdir(exist_ok=True)
    exe = build / ('identity_host.exe' if os.name == 'nt' else 'identity_host')
    reader = ROOT / 'modules/common/reader'
    sources = [HERE / 'runtime_host.c', ROOT / 'src/platform/epd_resource.c',
               ROOT / 'src/platform/epd_resource_runtime.c', ROOT / 'src/platform/epd_memory_manager.c',
               reader / 'reader_page_cache.c', reader / 'reader_page_resource.c',
               ROOT / 'modules/hello/hello_state.c']
    includes = [HERE / 'stubs', ROOT / 'tests/unified_resource/stubs', ROOT / 'src/platform', ROOT / 'src',
                reader, reader / 'compute/include']
    if Path(cc).stem.lower() == 'cl':
        command = [cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/TC', '/Od', '/RTC1',
                   '/wd4200', '/D_CRT_SECURE_NO_WARNINGS', '/DEPD_MEMORY_HOST', '/DRT_TICK_PER_SECOND=1000',
                   *(f'/I{p}' for p in includes), *map(str, sources), f'/Fe{exe}']
    else:
        command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DEPD_MEMORY_HOST',
                   '-DRT_TICK_PER_SECOND=1000', *(f'-I{p}' for p in includes), *map(str, sources), '-o', str(exe)]
    log = output / 'runtime.log'
    compiled = subprocess.run(command, cwd=build, env=env, capture_output=True, text=True, errors='replace')
    log.write_text(compiled.stdout + compiled.stderr, encoding='utf-8')
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr); raise SystemExit(compiled.returncode)
    ids = sorted(p.name for p in (artifacts / 'extracted-sap').iterdir() if p.is_dir())
    assert len(ids) == 12
    traces, reference = [], None
    for identity in [*ids, 'probe']:
        for path in ('component', 'direct'):
            run = subprocess.run([str(exe), identity, path], cwd=build, env=env,
                                 capture_output=True, text=True, errors='replace', timeout=30)
            with log.open('a', encoding='utf-8') as stream:
                stream.write(f'CASE id={identity} path={path}\n' + run.stdout + run.stderr)
            if run.returncode:
                print(run.stdout + run.stderr); raise SystemExit(run.returncode)
            lines = [line for line in run.stdout.splitlines() if line.startswith('TRACE ')]
            assert len(lines) == 1
            trace = json.loads(lines[0][6:])
            if reference is None:
                reference = trace
            assert trace == reference, (identity, path, trace, reference)
            traces.append({'id': identity, 'path': path, 'trace': trace})
            print(f'PASS runtime id={identity} path={path} policy/accounting/reclaim equal')
    return {'passed': True, 'cases': traces, 'host_ports': 'RTOS, physical heap, cold-storage callback port; no external services',
            'production': ['memory_manager', 'resource', 'resource_runtime', 'app_background', 'hello_state',
                           'reader_page_cache', 'reader_page_resource'],
            'source_sha256': {str(p.relative_to(ROOT)): digest(p.read_bytes()) for p in sources}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--part', choices=('all', 'package', 'runtime'), default='all')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {'passed': False, 'device_test': False}
    result_path = args.output / 'result.json'
    if result_path.exists():
        result.update(json.loads(result_path.read_text(encoding='utf-8')))
        result['passed'] = False
    result_path.write_text(json.dumps(result, indent=2), encoding='utf-8')
    if args.part in ('all', 'package'):
        cases, evidence, source_hash = prepare_cases(args.artifacts, args.output)
        code = ('import importlib.util; '
                f's=importlib.util.spec_from_file_location("runner",{str(ROOT / "tests/app_install_perf/run_tests.py")!r}); '
                'm=importlib.util.module_from_spec(s); s.loader.exec_module(m); '
                f'm.main({str(HERE / "package_host.c")!r}, [{str(cases)!r}])')
        run = subprocess.run([sys.executable, '-c', code], capture_output=True, text=True, encoding='utf-8', errors='replace')
        (args.output / 'package.log').write_text(run.stdout + run.stderr, encoding='utf-8')
        print(run.stdout, end='')
        if run.returncode:
            print(run.stderr, end=''); raise SystemExit(run.returncode)
        assert f'PACKAGE_CASES={len(evidence)}' in run.stdout
        result['package'] = {'passed': True, 'source_elf_sha256': source_hash, 'cases': evidence,
                             'arm_execution': 'host loader port; descriptor read from real ELF'}
    if args.part in ('all', 'runtime'):
        result['runtime'] = run_runtime(args.artifacts, args.output)
    result['passed'] = bool(result.get('package', {}).get('passed') and result.get('runtime', {}).get('passed'))
    result_path.write_text(json.dumps(result, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
