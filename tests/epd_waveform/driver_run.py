import importlib.util,re,subprocess,sys,tempfile
from pathlib import Path
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2];HERE=Path(__file__).parent
build = tempfile.TemporaryDirectory(prefix='epd-driver-recovery-')
OUT = Path(build.name)
spec=importlib.util.spec_from_file_location('cc',ROOT/'tests/reader_prefetch/cache_run.py');cc=importlib.util.module_from_spec(spec);spec.loader.exec_module(cc)
compiler,env=cc.compiler_environment(None)
source=(ROOT/'src/boards/epd_e0470a03_57x/reference57/epd_e0470a03.c').read_text(encoding='utf-8')
# Compile the production request/interrupt/completion path; omit only registration
# and unrelated brightness/control entry points after LCD_ReadID.
source=source[:source.index('static uint32_t LCD_ReadID')]
(OUT/'driver.inc').write_text(re.sub(r'^#include[^\n]*\n','',source,flags=re.M),encoding='utf-8')
panel=ROOT/'SiFli-SDK/customer/peripherals/display/epd_e0470a03'
includes=[OUT,HERE,HERE/'stubs',panel,panel/'gyd7n6',panel/'waveform',ROOT/'src/boards/epd_e0470a03_57x']
subprocess.run([compiler,'/nologo','/std:c11','/utf-8','/Od','/RTC1','/D_CRT_SECURE_NO_WARNINGS','/DLCD_USING_EPD_E0470A03_V10_57','/DLCD_FB_USING_NONE',*[f'/I{p}' for p in includes],str(HERE/'driver_host.c'),str(panel/'gyd7n6/epd_policy.c'),str(panel/'gyd7n6/reference_waveform.c'),f'/Fe:{OUT}/driver.exe'],cwd=OUT,env=env,check=True)
subprocess.run([str(OUT/'driver.exe'),str(ROOT/'src/boards/epd_e0470a03_57x/waveform/epd_waveform.bin')],check=True)

pmic = ROOT / 'src/boards/epd_e0470a03_57x/reference57/epd_tps.c'
(OUT / 'tps.inc').write_text(re.sub(r'^#include[^\n]*\n', '', pmic.read_text(encoding='utf-8'), flags=re.M),
                             encoding='utf-8')
subprocess.run([compiler, '/nologo', '/std:c11', '/utf-8', '/Od', '/RTC1', f'/I{OUT}',
                str(HERE / 'pmic_host.c'), f'/Fe:{OUT}/pmic.exe'], cwd=OUT, env=env, check=True)
subprocess.run([str(OUT / 'pmic.exe')], check=True)
