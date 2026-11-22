"""Exercise the production cache ownership helpers without audio or firmware builds."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/reader_prefetch'))
from cache_run import compiler_environment
from view_run import extract


def main():
    cc, env = compiler_environment(None)
    sources = [
        ('music_library.c', ['music_library_free']),
        ('music_cover.c', ['music_cover_image_free', 'music_cover_trim', 'music_cover_copy']),
        ('music_service.c', ['music_ui_active', 'trim_caches', 'ui_process']),
    ]
    code = []
    for path, names in sources:
        text = (ROOT / 'modules/music' / path).read_text(encoding='utf-8')
        code.extend(extract(text, name)[2] for name in names)
    with tempfile.TemporaryDirectory(prefix='music-cache-') as folder:
        build = Path(folder)
        (build / "app_work.inc").write_text((ROOT / "modules/common/app_work.h").read_text(encoding="utf-8").replace('#include "app_service.h"', ''), encoding="utf-8")
        (build / 'actual.inc').write_text('\n'.join(code), encoding='utf-8')
        exe = build / 'cache.exe'
        subprocess.run([cc, '/nologo', '/std:c11', '/utf-8', '/W3', '/WX', '/Od', '/RTC1',
                        f'/I{build}', str(HERE / 'host.c'), f'/Fe{exe}'],
                       cwd=build, env=env, check=True, timeout=60)
        subprocess.run([str(exe)], cwd=build, env=env, check=True, timeout=15)


if __name__ == '__main__':
    main()
