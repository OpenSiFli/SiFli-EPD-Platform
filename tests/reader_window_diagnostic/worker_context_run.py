"""Check every reached production worker wait preserves its current context."""
from pathlib import Path
import argparse
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    build = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='reader-worker-context-'))
    build.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'modules/common/reader/reader_view.c').read_text(encoding='utf-8')
    macro = re.search(r'^#define worker_wait\([^\n]+', source, re.M)[0]
    checked = ('#define worker_wait(current, timeout) (assert(state == (current)), '
               'assert(primary_state == (current)), rt_sem_take(mailbox.wake, timeout))')
    source = source.replace(macro, checked)
    alternate = build / 'reader_view.c'
    alternate.write_text(source, encoding='utf-8')
    command = [sys.executable, str(ROOT / 'tests/reader_view_async/run.py'), '--sanitize',
               '--case', 'worker', '--source', str(alternate)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=90)
    (build / 'worker.log').write_bytes(result.stdout + result.stderr)
    print((result.stdout + result.stderr).decode(errors='replace'))
    result.check_returncode()


if __name__ == '__main__':
    main()
