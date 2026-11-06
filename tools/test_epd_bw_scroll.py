"""Compile and run the actual portable EPD scroll helpers on the host."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, default=ROOT / 'outputs/host-tests/epd-band-refresh')
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
compiler = shutil.which('gcc')
if not compiler:
    raise SystemExit('A host GCC compiler is required')
header = ROOT / 'src/boards/epd_e0470a03_57x/epd_bw_scroll.h'
source = ROOT / 'tools/tests/epd_bw_scroll_test.c'
binary = args.out / 'epd_bw_scroll_test.exe'
command = [compiler, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-Wpedantic',
           '-I', str(header.parent), str(source), '-o', str(binary)]
subprocess.run(command, check=True)
result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
(args.out / 'result.txt').write_text(result.stdout, encoding='utf-8')
(args.out / 'result.json').write_text(json.dumps(dict(
    passed=True, compiler=compiler, command=command,
    header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
    source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
    output=result.stdout), indent=2) + '\n', encoding='utf-8')
print(result.stdout, end='')
