import sys,re
from pathlib import Path
repo,out=map(Path,sys.argv[1:]);out.mkdir(parents=True,exist_ok=True)
s=(repo/'modules/calendr/calendar_ui.c').read_text(encoding='utf-8')
s=s[s.index('typedef enum { HOME'):s.index('static void message(')]
(out/'ui.inc').write_text(s,encoding='utf-8')
s=(repo/'modules/calendr/calendar_runtime.h').read_text();s=s.replace('#include "app_service.h"','typedef int epd_background_t;')
(out/'runtime.h').write_text(s)

for src,name in [('src/platform/epd_date_picker.c','picker.inc'),('src/ui/apps/app_time_settings.c','time_settings.inc')]:
 s=(repo/src).read_text(encoding='utf-8')
 s=re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
 s=re.sub(r'^RTM_EXPORT[^\n]*\n','',s,flags=re.M)
 (out/name).write_text(s,encoding='utf-8')
