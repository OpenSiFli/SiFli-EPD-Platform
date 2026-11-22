"""Compile complete production home/image code with host platform boundaries."""
import re
import sys
from pathlib import Path

repo, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)


def without_includes(path):
    return re.sub(r'^\s*#include[^\n]*', '', (repo / path).read_text(encoding='utf-8'), flags=re.M)


for name, path in (
    ('home_actual.inc', 'src/ui/apps/app_home.c'),
    ('image_actual.inc', 'src/ui/platform/app_image.c'),
):
    (output / name).write_text(without_includes(path), encoding='utf-8')

# Exercise the SDK NOR path (both mapped flash and RAM-backed TF) on the host.
# Only hardware feature guards change; decoder functions are compiled intact.
decoder = without_includes('SiFli-SDK/middleware/lvgl/lv_drivers_v9/lvsf_img_decoder.c')
decoder = decoder.replace('#if (1 == LV_USE_DRAW_EPIC) && defined(LV_USE_EZIP)', '#if 1')
decoder = decoder.replace('#if (1 == LV_USE_DRAW_EPIC)', '#if 1')
(output / 'decoder_actual.inc').write_text(decoder, encoding='utf-8')
types = without_includes('src/ui/platform/app_catalog.h')
config = (repo / 'SiFli-SDK/middleware/lvgl/lv_conf_sifli.h').read_text(encoding='utf-8')
aliases = re.findall(r'^\s*#define (?:LV_IMAGE_FLAGS_EZIP|LV_IMG_CF_TRUE_COLOR(?:_ALPHA)?|LV_IMG_CF_RAW(?:_ALPHA)?)\s+[^\n]+', config, flags=re.M)
types = '\n'.join(line.strip() for line in aliases) + '\n' + types
types += without_includes('src/ui/platform/app_installer.h')
internal = (repo / 'src/ui/ui_internal.h').read_text(encoding='utf-8')
types += internal[internal.index('typedef struct'):internal.index('extern page_context_t *current;')]
(output / 'types_actual.h').write_text(types, encoding='utf-8')
