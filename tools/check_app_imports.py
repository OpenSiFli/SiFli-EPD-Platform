"""Check native module format, firmware imports and application entry symbols."""
import subprocess
from pathlib import Path
from package_app import elf_load_bytes


ENTRY_SYMBOLS = {'app_main', 'epd_app_info'}

# Reject platform settlement authority even when checking against an older
# firmware that still exported it. Application identity does not affect this
# boundary; ordinary scopes and resource context APIs remain public.
PRIVATE_MEMORY_IMPORTS = frozenset({
    'epd_memory_init', 'epd_memory_owner_create',
    'epd_memory_credit_request', 'epd_memory_credit_get',
    'epd_memory_credit_end', 'epd_memory_credit_cancel',
    'epd_memory_credit_current', 'epd_memory_credit_trim',
    'epd_memory_credit_expand', 'epd_memory_credit_suspend',
    'epd_memory_credit_resume_commit', 'epd_memory_credit_replace_prepare',
    'epd_memory_credit_share_prepare', 'epd_memory_credit_commit',
    'epd_memory_association_change', 'epd_memory_association_batch',
    'epd_memory_credit_associate', 'epd_memory_credit_share_commit',
    'epd_memory_credit_abort',
    'epd_memory_scope_create_trusted', 'epd_memory_owner_retain_trusted',
    'epd_memory_owner_release_trusted', 'epd_memory_owner_enter_trusted',
    'epd_memory_owner_leave_trusted', 'epd_memory_owner_close_trusted',
    'epd_memory_owner_is_current_account',
    'epd_activity_work_enter_trusted', 'epd_activity_work_leave_trusted',
    'rt_thread_mem_context_set', 'rt_memheap_owner_transfer', 'rt_memheap_owner_transfer_try',
    'epd_memory_block_transfer_try_trusted',
})


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
    private = sorted(set(imports) & PRIVATE_MEMORY_IMPORTS)
    if private:
        raise ValueError('Platform-private module imports: ' + ', '.join(private))
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
