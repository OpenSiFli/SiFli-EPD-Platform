"""Audit real SAP imports and compile the private/public ARM API boundary."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from check_app_imports import PRIVATE_MEMORY_IMPORTS, parse_firmware_exports, verify
from package_app import SAP_HEADER, SAP_RECORD, SAP_MAGIC, SAP_VERSION


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command):
    return subprocess.check_output(list(map(str, command)), text=True, stderr=subprocess.STDOUT)


def imports(nm, path, dynamic=True):
    return {line.split()[-1] for line in run([nm, *(['-D'] if dynamic else []),
                                             '--undefined-only', path]).splitlines() if line.split()}


def exports(nm, path):
    return {line.split()[-1].removeprefix('__rtmsym_') for line in run([nm, path]).splitlines()
            if line.split()[-1].startswith('__rtmsym_') and not line.split()[-1].endswith('_name')}


def executable_sections(path):
    data = path.read_bytes(); offset = struct.unpack_from('<I', data, 32)[0]
    size, count, names = struct.unpack_from('<HHH', data, 46)
    rows = [struct.unpack_from('<10I', data, offset + size * i) for i in range(count)]
    names = data[rows[names][4]:rows[names][4] + rows[names][5]]
    return {names[row[0]:].split(b'\0', 1)[0].decode(): data[row[4]:row[4] + row[5]]
            for row in rows if row[2] & 4}


def sap_entries(path):
    data = path.read_bytes()
    magic, version, count, length, reserved = SAP_HEADER.unpack_from(data)
    assert (magic, version, length, reserved) == (SAP_MAGIC, SAP_VERSION, len(data), 0)
    cursor = SAP_HEADER.size; entries = {}
    for _ in range(count):
        name_size, flags, size, checksum, reserved = SAP_RECORD.unpack_from(data, cursor)
        cursor += SAP_RECORD.size
        name = data[cursor:cursor + name_size].decode(); cursor += name_size
        payload = data[cursor:cursor + size]; cursor += size
        assert not flags and not reserved and name not in entries
        assert len(payload) == size and hashlib.sha256(payload).digest() == checksum
        entries[name] = payload
    assert cursor == length
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--packages', type=Path, required=True)
    parser.add_argument('--g2', type=Path, required=True)
    parser.add_argument('--before', type=Path, required=True, help='Snapshot root containing src/ and tools/')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    cc = args.cc.resolve(); nm = cc.with_name(cc.name.replace('gcc', 'nm'))
    sdk = ROOT / 'SiFli-SDK'
    includes = [args.firmware, sdk / 'rtos/rtthread/include', sdk / 'rtos/rtthread/components/finsh',
                sdk / 'middleware/include', sdk / 'drivers/cmsis/sf32lb57x', sdk / 'external/CMSIS/Include',
                sdk / 'drivers/Include', sdk / 'drivers/cmsis/Include', sdk / 'middleware/sifli_matrix',
                ROOT / 'src/platform']
    flags = ['-std=c11', '-mcpu=cortex-m33', '-mthumb', '-mfpu=fpv5-sp-d16', '-mfloat-abi=hard',
             '-fshort-enums', '-ffunction-sections', '-fdata-sections', '-Os', '-DSOC_BF0_HCPU',
             '-DSF32LB57X', '-DHAVE_CCONFIG_H', *['-I' + str(p) for p in includes]]
    core = out / 'manager.o'; old_core = out / 'manager-before.o'
    for source, obj in ((ROOT / 'src/platform/epd_memory_manager.c', core),
                        (args.before / 'src/platform/epd_memory_manager.c', old_core)):
        run([cc, *flags, '-c', source, '-o', obj])
    current, previous = exports(nm, core), exports(nm, old_core)
    assert len(previous - current) == 19 and (previous - current) <= PRIVATE_MEMORY_IMPORTS and not current - previous
    defined = {line.split()[-1] for line in run([nm, '--defined-only', core]).splitlines()}
    assert {n for n in PRIVATE_MEMORY_IMPORTS if n.startswith('epd_memory_') and n != 'epd_memory_block_transfer_try_trusted'} <= defined
    firmware = args.firmware / 'main.elf'
    legacy_exports = set(parse_firmware_exports(run([nm, firmware])))
    assert previous <= legacy_exports, 'The reference firmware must contain the pre-change manager exports'
    effective_exports = ((legacy_exports - previous) | current) - PRIVATE_MEMORY_IMPORTS
    record = {'reference_firmware': str(firmware), 'reference_sha256': digest(firmware),
              'manager_exports_removed': sorted(previous - current),
              'manager_exports_retained': sorted(current), 'trusted_entrypoints_unexported': True,
              'arm_objects': {str(p): digest(p) for p in (core, old_core)}, 'packages': [], 'common_objects': [],
              'header_rejections': [], 'import_rejections': []}

    spec = importlib.util.spec_from_file_location('before_imports', args.before / 'tools/check_app_imports.py')
    old_check = importlib.util.module_from_spec(spec); spec.loader.exec_module(old_check)
    for symbol in sorted(PRIVATE_MEMORY_IMPORTS):
        header = out / 'header.c'
        header.write_text('#include "epd_activity_work.h"\n'
                          f'void (*const probe)(void) = (void (*)(void)){symbol};\n')
        run([cc, *flags, '-Werror', '-fsyntax-only', header])
        for define in ('BUILD_DLMODULE', 'RTT_IN_MODULE'):
            result = subprocess.run([str(cc), *flags, '-Werror', '-fsyntax-only', '-D' + define, str(header)],
                                    capture_output=True, text=True)
            assert result.returncode and symbol in result.stderr and 'undeclared' in result.stderr, result.stderr
            record['header_rejections'].append({'symbol': symbol, 'define': define})
        probe = out / (symbol + '.c'); module = probe.with_suffix('.so')
        probe.write_text(f'extern void {symbol}(void);\nvoid *probe(void) {{ return (void *){symbol}; }}\n'
                         'int app_main(void) { return 0; }\nconst unsigned epd_app_info = 2;\n')
        run([cc, *flags, '-shared', '-fPIC', '-nostdlib', '-Wl,-e,0', probe, '-o', module])
        assert imports(nm, module) == {symbol}
        # These are real ARM ET_DYN modules, not mocked nm output. The old
        # checker accepts the old export table; the current checker must reject.
        if symbol in legacy_exports:
            old_check.verify(module, firmware, nm, application=True)
        try:
            verify(module, firmware, nm, application=True)
        except ValueError as error:
            assert str(error) == 'Platform-private module imports: ' + symbol
        else:
            raise AssertionError('Private import passed: ' + symbol)
        assert symbol not in effective_exports
        record['import_rejections'].append(symbol)

    packages = sorted(args.packages.glob('*.sap'))
    standard = sorted(args.g2.glob('*.sap'))
    assert len(packages) == 12 and len(standard) == 2
    for index, package in enumerate(packages + standard):
        entries = sap_entries(package); manifest = json.loads(entries['app.json'])
        if package in standard:
            assert manifest['version'] == 2
        row = {'path': str(package), 'sha256': digest(package), 'id': manifest['id'],
               'version': manifest['version'], 'modules': []}
        for n, (name, content) in enumerate((k, v) for k, v in entries.items() if k.endswith('.so')):
            module = out / f'package-{index}-{n}.so'; module.write_bytes(content)
            imported = imports(nm, module)
            assert not (imported & PRIVATE_MEMORY_IMPORTS)
            assert imported <= effective_exports, sorted(imported - effective_exports)
            verify(module, firmware, nm, application=name == manifest['id'] + '.so')
            row['modules'].append({'entry': name, 'sha256': digest(module), 'imports': sorted(imported)})
        assert row['modules']; record['packages'].append(row)
    # Shared components are linked into modules. Check their compiled object
    # imports as well as the final SAP imports, including dead-stripped objects.
    for obj in sorted((ROOT / 'modules').glob('*/output/**/*.o')):
        components = set(obj.relative_to(ROOT / 'modules').parts[2:-1])
        if not components.intersection({'reader', 'image', 'http', 'input'}):
            continue
        imported = imports(nm, obj, dynamic=False)
        assert not imported & PRIVATE_MEMORY_IMPORTS, str(obj)
        record['common_objects'].append({'path': str(obj), 'sha256': digest(obj),
                                        'memory_imports': sorted(s for s in imported if s.startswith('epd_memory_'))})
    assert record['common_objects']
    header = out / 'public-header.c'
    public = sorted(current | {'epd_resource_context_begin', 'epd_resource_context_update',
                              'epd_resource_context_enter', 'epd_resource_context_leave'})
    header.write_text('#include "epd_resource_runtime.h"\n' + '\n'.join(
        f'void (*const probe_{i})(void) = (void (*)(void)){symbol};' for i, symbol in enumerate(public)))
    run([cc, *flags, '-DBUILD_DLMODULE', '-Werror', '-fsyntax-only', header])
    record['public_header_symbols'] = public
    (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'PASS ARM: 19 grant exports remain removed; trusted entrypoints unexported; '
          f'{len(record["header_rejections"])} header rejections; '
          f'{len(record["import_rejections"])} actual private ET_DYN imports rejected.')
    print(f'PASS imports: {len(record["packages"])} SAPs; {len(record["common_objects"])} public component objects.')


if __name__ == '__main__':
    main()
