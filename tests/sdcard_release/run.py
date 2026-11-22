from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root/'tests/reader_prefetch'))
from cache_run import compiler_environment
cc, env = compiler_environment(None)
source = (root/'components/sdcard/src/sdcard.c').read_text(encoding='utf-8')
def function(name):
    begin = source.rfind('\nstatic ', 0, source.index(name + '(')) + 1
    brace = source.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]
with tempfile.TemporaryDirectory(prefix='sdcard-release-') as temp:
    build = Path(temp)
    (build/'production.inc').write_text('\n'.join(function(x) for x in
        ('_next_id', '_notify_release_locked', '_pin_irq', '_drain')), encoding='utf-8')
    exe = build/'host.exe'
    subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W4', '/WX', '/Od', '/Zi',
        '/fsanitize=address', '/D_CRT_SECURE_NO_WARNINGS', f'/I{build}',
        str(root/'tests/sdcard_release/host.c'), f'/Fe{exe}'], cwd=build, env=env, check=True)
    for case in ('success', 'quiet', 'insert', 'bounce', 'ack-race', 'sample-race', 'cancel'):
        subprocess.run([str(exe), case], cwd=build, env=env, check=True, timeout=10)
