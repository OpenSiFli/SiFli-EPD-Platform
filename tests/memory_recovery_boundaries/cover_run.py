"""Exercise live production recovery boundaries; --regression uses the old boundary."""
from pathlib import Path
import re, subprocess, sys, json, argparse
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path[:0] = [str(ROOT/'tests/reader_prefetch'), str(ROOT/'tests/storage_perf'), str(ROOT/'tests/cache_budget')]
from cache_run import compiler_environment
from view_run import extract
from owned_fixture import write_owned_fixture
from fixture import prepare as prepare_budget

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir',type=Path,required=True)
parser.add_argument('--regression',action='store_true')
args=parser.parse_args()
variant='baseline' if args.regression else 'candidate'
folder=args.build_dir.resolve()
folder.mkdir(parents=True, exist_ok=True)
write_owned_fixture(folder); prepare_budget(folder)
for h in ('rtthread.h','dfs_posix.h','storage_file.h'): (folder/h).write_text('')
def source(rel):
    s=(ROOT/rel).read_text(encoding='utf-8')
    if args.regression and rel=='modules/books/pages.c':
        old='    if (idle) ui_bookshelf_resources_refresh();\n'
        assert s.count(old)==1
        s=s.replace(old,'')
    return s
doc=source('modules/books/document/document.h')
anchor=re.search(r'typedef struct\s*\{[^{}]*\}\s*document_anchor_t;',doc)[0]
constants='\n'.join(re.findall(r'^#define DOCUMENT_(?:CONVERSION_VERSION|IMAGE_WIDTH|IMAGE_HEIGHT|SOURCE_SPOOL|TEXT_LIMIT)\b[^\n]*',doc,re.M))
(folder/'document_types.h').write_text(anchor+'\n'+constants+'\n'+extract(doc,'document_history_version')[2],encoding='utf-8')
for output,name in [('bookshelf.inc','bookshelf.c'),('shelf_data.inc','ui_bookshelf_data.c'),('cover_cache.inc','cover_cache.c')]:
    (folder/output).write_text(re.sub(r'^#include[^\n]*\n','',source('modules/books/'+name),flags=re.M),encoding='utf-8')
(folder/'books_process.inc').write_text(extract(source('modules/books/pages.c'),'books_process')[2],encoding='utf-8')
prefix='#include <windows.h>\n#include <assert.h>\n#include "epd_resource_runtime.h"\n#include "activity_fixture.h"\n#define main legacy_cover_main\n'
prefix+=f'#include "{(ROOT/"tests/books_cover_cache/host.c").as_posix()}"\n#undef main\n#include "cache_bridge.h"\n'
(folder/'host.c').write_text(prefix+(HERE/'cover_host.c').read_text(encoding='utf-8'),encoding='utf-8')
cc,env=compiler_environment(None)
includes=[folder,ROOT/'tests/unified_resource',ROOT/'tests/books_scan_perf',ROOT/'tests/compute_reading_data',ROOT/'components/compute_reading_data/include',ROOT/'src/platform',ROOT/'components/compute/include',ROOT/'tests/unified_resource/stubs']
sources=[folder/'host.c',ROOT/'src/platform/epd_resource.c',ROOT/'tests/books_scan_perf/data_transport.c',ROOT/'tests/compute_reading_data/protocol.c',ROOT/'components/compute_reading_data/reading_data_client.c',ROOT/'components/compute_reading_data/reading_data_remote.c']
exe=folder/'cover.exe'
command=[cc,'/nologo','/std:c11','/utf-8','/W3','/Od','/Zi','/fsanitize=address','/Gy','/D_CRT_SECURE_NO_WARNINGS',*[f'/I{p}' for p in includes],*map(str,sources),f'/Fe{exe}','/link','/OPT:REF']
r=subprocess.run(command,cwd=folder,env=env,capture_output=True)
(folder/'compile.log').write_bytes(r.stdout+r.stderr)
if r.returncode:
    print((r.stdout+r.stderr).decode(errors='replace'));sys.exit(r.returncode)
results=[]
for case in ['busy_reader','idle_keep','busy_protect','repeat','legacy']:
    r=subprocess.run([str(exe),case],cwd=folder,env=env,capture_output=True,timeout=60)
    (folder/(case+'.log')).write_bytes(r.stdout+r.stderr)
    print(case,r.returncode,(r.stdout+r.stderr).decode(errors='replace')[-1800:])
    results.append({'case':case,'exit_code':r.returncode})
(folder/'result.json').write_text(json.dumps(results,indent=2))

expected=lambda case: 23 if args.regression and case in ('busy_reader','repeat') else 0
raise SystemExit(0 if all(r['exit_code']==expected(r['case']) for r in results) else 1)
