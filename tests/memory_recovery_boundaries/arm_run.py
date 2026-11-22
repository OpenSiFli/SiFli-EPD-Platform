"""Replay the five isolated ARM compiles from a recorded command directory."""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FILES = ('src/platform/app_memory_pressure.c', 'src/platform/epd_memory_sdk.c',
         'modules/books/ui_bookshelf_data.c', 'modules/books/pages.c',
         'modules/common/reader/reader_view.c')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--command-dir', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--baseline-root', type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve(); build.mkdir(parents=True, exist_ok=True)
    records = {}
    variants = [('current', ROOT)]
    if args.baseline_root:
        variants.insert(0, ('baseline', args.baseline_root.resolve()))
    for variant, base in variants:
        out = build/variant; out.mkdir(exist_ok=True)
        for relative in FILES:
            name = Path(relative).stem
            command = json.loads((args.command_dir/(name+'.command.json')).read_text())
            # Recorded commands are from the current checkout. Prefer the
            # snapshot headers while retaining all original dependency paths.
            if variant == 'baseline':
                command[1:1] = ['-I'+str(base/'src/platform')]
                if relative.startswith('modules/'):
                    command[1:1] = ['-include', str(base/'modules/books/ui_bookshelf_data.h')]
                forced = ['-include', str(ROOT/'modules/books/ui_bookshelf_data.h')]
                for i in range(len(command)-1, 0, -1):
                    if command[i-1:i+1] == forced:
                        del command[i-1:i+1]
            command[command.index('-c')+1] = str(base/relative)
            command[command.index('-o')+1] = str(out/(name+'.o'))
            (out/(name+'.command.json')).write_text(json.dumps(command, indent=2))
            result = subprocess.run(command, cwd=ROOT/'project', capture_output=True)
            (out/(name+'.log')).write_bytes(result.stdout+result.stderr)
            if result.returncode:
                raise SystemExit((result.stdout+result.stderr).decode(errors='replace'))
            readelf = Path(command[0]).with_name('arm-none-eabi-readelf.exe')
            sections = subprocess.check_output([str(readelf), '-SW', str(out/(name+'.o'))], text=True)
            (out/(name+'.sections.txt')).write_text(sections)
            writable = {}
            for line in sections.splitlines():
                match = re.search(r'\[\s*\d+\]\s+(\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S+)', line)
                if match and 'W' in match[3] and 'A' in match[3]:
                    writable[match[1]] = int(match[2], 16)
            records[variant+'/'+name] = writable
            print(variant, name, sum(writable.values()), flush=True)
    (build/'writable.json').write_text(json.dumps(records, indent=2))
    if args.baseline_root:
        for relative in FILES:
            name = Path(relative).stem
            assert records['baseline/'+name] == records['current/'+name], name

if __name__ == '__main__':
    main()
