"""Exercise live production recovery boundaries; --regression uses the old boundary."""
from pathlib import Path
import os,re,subprocess,sys,json,argparse
sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
if os.name=='nt':
    script='/mnt/'+HERE.drive[0].lower()+HERE.as_posix()[2:]+'/span_run.py'
    raise SystemExit(subprocess.call(['wsl.exe','-d','Ubuntu','--exec','python3',script,*[(('/mnt/'+a[0].lower()+a[2:]).replace('\\','/')) if len(a)>2 and a[1]==':' else a for a in sys.argv[1:]]]))
ROOT=HERE.parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir',type=Path,required=True)
parser.add_argument('--regression',action='store_true')
parser.add_argument('--reader',action='store_true',help='Execute real reader frame and compute allocation functions')
parser.add_argument('--reader-source',type=Path,help='Reader source snapshot for a regression control')
args=parser.parse_args()
variant='baseline' if args.regression else 'candidate'
build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
def read(rel):
    return (ROOT/rel).read_text(encoding='utf-8')
host=(ROOT/'tests/unified_memory/host.c').read_text(encoding='utf-8')
host=host.replace('unified_test_storage[3][32768]','unified_test_storage[3][4u*1024u*1024u]')
start=host.index('static uint32_t pressure_publish(');end=host.index('static rt_err_t recovery_sleep(',start)
host=host[:start]+host[end:]
host=host.replace('#include "../../',f'#include "{ROOT.as_posix()}/')
(build/'memory_host.inc').write_text(host)
(build/'pressure.inc').write_text(re.sub(r'^#include[^\n]*\n','',read('src/platform/app_memory_pressure.c'),flags=re.M))
for name in ['app_memory_pressure.h','epd_memory_manager.h','epd_memory_sdk.c']:
    (build/name).write_text(read('src/platform/'+name))
(build/'host.c').write_text((HERE/'span_host.c').read_text())
if args.reader:
    sys.path.insert(0,str(ROOT/'tests/reader_prefetch'))
    from view_run import extract
    source=(args.reader_source or ROOT/'modules/common/reader/reader_view.c').read_text(encoding='utf-8')
    compute=(ROOT/'src/platform/epd_compute.c').read_text(encoding='utf-8')
    constants='\n'.join(re.findall(r'^#define VIEW_(?:FRAME_BUDGET|RENDER_WORKSPACE|FRAME_WAIT_MS)\b[^\n]*',source,re.M))
    compute_header=(ROOT/'components/compute/include/epd_compute_protocol.h').read_text(encoding='utf-8')
    cache_line=re.search(r'^#define EPD_COMPUTE_CACHE_LINE\b[^\n]*',compute_header,re.M)[0]
    (build/'reader_compute.inc').write_text(cache_line+'\n'+'\n\n'.join(extract(compute,n)[2] for n in ['epd_compute_buffer_alloc','epd_compute_buffer_free']))
    names=['view_memory_room','view_memory_recover','frame_storage_minimum','frame_allocate_pixels','frame_storage']
    (build/'reader_storage.inc').write_text(constants+'\n'+'\n\n'.join(extract(source,n)[2] for n in names))
    prefix=(HERE/'span_host.c').read_text().split('int main(int argc,char **argv)')[0]
    (build/'host.c').write_text(prefix+(HERE/'reader_host.c').read_text())
sdk=ROOT/'SiFli-SDK/rtos/rtthread'
sources=[build/'host.c',ROOT/'src/platform/epd_memory_manager.c',build/'epd_memory_sdk.c',ROOT/'src/platform/epd_resource_runtime.c',ROOT/'src/ui/platform/app_memory.c',*[sdk/'src'/n for n in ['memheap.c','thread.c','idle.c']]]
includes=[build,ROOT/'tests/unified_memory',sdk/'include',ROOT/'src/platform',ROOT/'src/ui/platform']
results=[]
for rate in [1000,100]:
    exe=build/f'span-{rate}'
    cmd=['gcc','-std=c11','-O1','-g','-ffunction-sections','-fdata-sections','-fno-pie','-no-pie','-fsanitize=address,undefined','-fno-sanitize=alignment','-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast','-DEPD_MEMORY_HOST','-DEPD_MEMORY_DIAGNOSTIC_HOST','-DRT_USING_MUTEX',f'-DRT_TICK_PER_SECOND={rate}',f'-DCANDIDATE={int(variant=="candidate")}',*[f'-I{p}' for p in includes],*map(str,sources),'-Wl,--gc-sections','-o',str(exe)]
    r=subprocess.run(cmd,capture_output=True);(build/f'compile-{rate}.log').write_bytes(r.stdout+r.stderr)
    if r.returncode:print((r.stdout+r.stderr).decode(errors='replace'));sys.exit(r.returncode)
    cases=['exact','short','query','credit','merge','cancel','deadline','busy','initial','continue','invalid','legacy']
    if args.reader:
        cases=['initial','used','admission','success'] if args.regression else ['initial','used','admission','short','success','cancel','deadline']
    for case in cases:
        r=subprocess.run([str(exe),case],capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'},timeout=30)
        (build/f'{case}-{rate}.log').write_bytes(r.stdout+r.stderr)
        print(case,rate,r.returncode,(r.stdout+r.stderr).decode(errors='replace')[-1600:],flush=True)
        results.append({'case':case,'hz':rate,'exit_code':r.returncode})
(build/'result.json').write_text(json.dumps(results,indent=2))
if variant=='candidate' and not args.reader:
    exe=build/'span-solution'
    cmd[cmd.index('-o')+1]=str(exe)
    cmd.insert(1,'-DSOLUTION')
    r=subprocess.run(cmd,capture_output=True);(build/'compile-solution.log').write_bytes(r.stdout+r.stderr)
    if r.returncode:print((r.stdout+r.stderr).decode(errors='replace'));sys.exit(r.returncode)
    r=subprocess.run([str(exe),'query'],capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'},timeout=30)
    (build/'query-solution.log').write_bytes(r.stdout+r.stderr)
    print('solution-query',r.returncode,(r.stdout+r.stderr).decode(errors='replace'),flush=True)
    if r.returncode:sys.exit(r.returncode)

def expected(case):
    if not args.regression:return 0
    if args.reader:return 0 if case=='success' else 23
    if case in ('query','invalid'):return 77
    return 23 if case in ('exact','credit','busy','initial','continue') else 0
raise SystemExit(0 if all(r['exit_code']==expected(r['case']) for r in results) else 1)
