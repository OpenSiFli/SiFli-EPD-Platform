"""Compile the resident ownership implementation with module filesystem doubles."""
from pathlib import Path


def write_owned_fixture(directory):
    root = Path(__file__).resolve().parents[2]
    source = (root / 'src/services/storage_file.c').read_text(encoding='utf-8-sig')
    source = source[source.index('#define STORAGE_CLOSE_SLOTS'):source.index('const char *storage_app_directory')]
    prefix = '''
#ifndef RT_EOK
#define RT_EOK 0
#endif
#define L2_RET_BSS_SECT_BEGIN(x)
#define L2_RET_BSS_SECT_END
#define L2_RET_BSS_SECT(x)
#define INIT_COMPONENT_EXPORT(x)
#ifndef RTM_EXPORT
#define RTM_EXPORT(x)
#endif
'''
    (directory / 'owned_actual.inc').write_text(prefix + source, encoding='utf-8')
