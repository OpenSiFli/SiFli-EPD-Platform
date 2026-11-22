from pathlib import Path
import sys

repo, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
source = (repo / 'modules/notes/notes_runtime.c').read_text(encoding='utf-8')
start = source.index('static void ai_job(')
end = source.index('static void transcribe_job(', start)
(output / 'ai_job_actual.inc').write_text(source[start:end], encoding='utf-8')
source = (repo / 'src/ui/apps/ui_ai_settings.c').read_text(encoding='utf-8')
start = source.index('epd_ai_error_t epd_ai_authorize_service(')
end = source.index('void ui_ai_settings_create(', start)
(output / 'authorize_actual.inc').write_text(source[start:end], encoding='utf-8')

source = (repo / 'src/ui/apps/app_files.c').read_text(encoding='utf-8')
def extract(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start)+2]
(output / 'picker_actual.inc').write_text("\n\n".join(extract(signature) for signature in (
    'static bool picker_image(', 'static bool picker_accepts(', 'bool epd_file_picker_open(')), encoding='utf-8')
