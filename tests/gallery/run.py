import importlib.util,re,subprocess,sys,tempfile
from pathlib import Path
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]; HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'tests/storage_perf'))
from owned_fixture import write_owned_fixture
sys.path.insert(0,str(ROOT/'tests/cache_budget'))
from fixture import prepare as prepare_budget
build = tempfile.TemporaryDirectory(prefix='epd-gallery-regression-')
OUT = Path(build.name)
write_owned_fixture(OUT)
prepare_budget(OUT)
spec=importlib.util.spec_from_file_location('compiler',ROOT/'tests/reader_prefetch/cache_run.py')
cc=importlib.util.module_from_spec(spec);spec.loader.exec_module(cc)
compiler,env=cc.compiler_environment(None)
header=(ROOT/'modules/gallery/gallery.h').read_text(encoding='utf-8-sig')
(OUT/'types.inc').write_text(header[header.index('#define GALLERY_ROWS'):header.index('typedef raster_decoder_t')],encoding='utf-8')
raster=(ROOT/'modules/common/image/raster.h').read_text(encoding='utf-8')
(OUT/'raster_types.inc').write_text('#include "epd_gray_quantize.h"\n'+raster[raster.index('typedef struct'):raster.index('typedef enum')]+'''\nint gallery_file_open(const char *, int, uint32_t);
bool gallery_file_close(int);
bool gallery_file_io(int,void*,size_t,bool,uint32_t,uint32_t);
uint32_t gallery_submit_page(gallery_job_kind_t,const char*,unsigned,bool,const char*);
''',encoding='utf-8')
for name in ['catalog','decode','store','service']:
 source=(ROOT/f'modules/gallery/gallery_{name}.c').read_text(encoding='utf-8-sig')
 (OUT/f'{name}.inc').write_text(re.sub(r'^#include[^\n]*\n','',source,flags=re.M),encoding='utf-8')
arguments=sys.argv[1:];checks=['/RTC1']
if '--asan' in arguments: arguments.remove('--asan');checks=['/Zi','/fsanitize=address']
for test in (arguments or ['catalog','faults','service','rebuild']):
 if test not in ['catalog','faults','service','rebuild']: raise SystemExit('unknown gallery test: '+test)
 subprocess.run([compiler,'/nologo','/std:c11','/utf-8','/Od',*checks,'/D_CRT_SECURE_NO_WARNINGS',f'/I{OUT}',f'/I{ROOT}/src/ui/platform',str(HERE/f'{test}.c'),f'/Fe:{OUT}/{test}.exe'],env=env,cwd=OUT,check=True)
 subprocess.run([str(OUT/f'{test}.exe')],env=env,check=True)
