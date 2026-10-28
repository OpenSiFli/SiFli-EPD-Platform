"""Check native module format, firmware imports and application entry symbols."""
import subprocess
from pathlib import Path
from package_app import elf_load_bytes


ENTRY_SYMBOLS = {'app_main', 'epd_app_info'}


def parse_firmware_exports(symbol_table):
    table = {}
    for line in symbol_table.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            table[fields[-1]] = int(fields[0], 16)
    if '__rtmsymtab_start' not in table or '__rtmsymtab_end' not in table:
        raise ValueError('Firmware has no RT-Thread export table boundaries')
    start, end = table['__rtmsymtab_start'], table['__rtmsymtab_end']
    # Name strings live outside this range. An exported API can itself end in _name.
    symbols = {name[len('__rtmsym_'):] for name, address in table.items()
               if name.startswith('__rtmsym_') and start <= address < end}
    if not symbols:
        raise ValueError('Firmware has no RT-Thread module exports')
    return sorted(symbols)


def verify(module, firmware, nm, application=False):
    elf_load_bytes(Path(module).read_bytes())
    exports = set(parse_firmware_exports(
        subprocess.check_output([str(nm), str(firmware)], text=True)))
    imports = [line.split()[-1] for line in subprocess.check_output(
        [str(nm), '-D', '--undefined-only', str(module)], text=True).splitlines() if line.split()]
    missing = sorted(set(imports) - exports)
    if missing:
        raise ValueError('Unexported module imports: ' + ', '.join(missing))
    if application:
        defined = {line.split()[-1] for line in subprocess.check_output(
            [str(nm), '-D', '--defined-only', str(module)], text=True).splitlines() if line.split()}
        missing_entries = sorted(ENTRY_SYMBOLS - defined)
        if missing_entries:
            raise ValueError('Missing application entry symbols: ' + ', '.join(missing_entries))
    print(f'{Path(module).name}: {len(imports)} imports resolved in firmware RTM table')
