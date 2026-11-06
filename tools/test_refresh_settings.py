"""Exercise the production settings codec and deferred refresh-mode functions."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'outputs/host-tests/refresh-settings'
OUT.mkdir(parents=True, exist_ok=True)
compiler = shutil.which('gcc')
assert compiler, 'Host GCC is required'


def function(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth = 1
    at = begin + 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[start:at] + '\n'


store_path = ROOT / 'src/ui/ui_settings_store.c'
settings_path = ROOT / 'src/ui/ui_settings.c'
driver_path = ROOT / 'src/boards/epd_e0470a03_57x/reference57/epd_e0470a03.c'
store = store_path.read_text(encoding='utf-8')
settings = settings_path.read_text(encoding='utf-8')
driver = driver_path.read_text(encoding='utf-8')
old_record = bytes.fromhex('32535045020000003c0000001413e011010001000200050003000a0004000100050001000700010008000000090020000a0000000b0078000c0014000d0000000e00640010000f0011000100')
defines = store[store.index('#define SETTINGS_MAGIC'):store.index('static struct rt_mutex')]
codec = store[store.index('typedef struct\n{\n    ui_setting_id_t id;'):store.index('/* Called with storage_lock held.')]
defaults = settings[settings.index('#define SETTINGS_DEFAULTS'):settings.index('static const uint8_t defaults[')]
crc = function((ROOT / 'src/services/storage_file.c').read_text(encoding='utf-8'), 'uint32_t storage_crc32(')

codec_test = r'''
int main(void)
{
    assert(storage_crc32("123456789", 9) == 0xcbf43926u);
    assert(V2_FIELDS == 16 && V2_SIZE == 64 && V2_SIZE <= V2_MAX_SIZE);
    assert(UI_SETTING_NAVIGATION == 15 && UI_SETTING_REFRESH_METHOD == 16);
    assert(ui_settings_default(UI_SETTING_REFRESH_METHOD) == 0);
    uint8_t values[UI_SETTING_COUNT], loaded[UI_SETTING_COUNT], record[HEADER_SIZE + V2_SIZE];
    for (unsigned method = 0; method < 3; ++method)
    {
        default_values(values);
        values[UI_SETTING_REFRESH_METHOD] = method;
        values[UI_SETTING_NAVIGATION] = 1;
        values[UI_SETTING_MARGIN] = 4;
        assert(encode_record(values, record));
        assert(get_u32(record) == SETTINGS_MAGIC && get_u32(record + 4) == 2);
        assert(get_u32(record + 8) == 64);
        assert(get_u32(record + 12) == storage_crc32(record + HEADER_SIZE, 64));
        assert(decode_v2(record + HEADER_SIZE, 64, loaded));
        assert(memcmp(values, loaded, sizeof(values)) == 0);
    }
    /* A frozen old-format file exercises compatibility independently of the encoder. */
    const uint8_t *old = old_record + HEADER_SIZE;
    assert(get_u32(old_record + 4) == 2 && get_u32(old_record + 8) == 60);
    assert(get_u32(old_record + 12) == storage_crc32(old, 60));
    default_values(values);
    values[UI_SETTING_NAVIGATION] = 1;
    values[UI_SETTING_MARGIN] = 4;
    assert(decode_v2(old, 60, loaded));
    assert(loaded[UI_SETTING_REFRESH_METHOD] == 0 && loaded[UI_SETTING_NAVIGATION] == 1);
    assert(loaded[UI_SETTING_MARGIN] == 4);
    assert(decode_v2(old, 56, loaded));
    assert(loaded[UI_SETTING_REFRESH_METHOD] == 0 && loaded[UI_SETTING_NAVIGATION] == 0);
    uint8_t v1[V1_SIZE] = {1,2,3,1,1,0,1,2,0,1,1,0,0};
    assert(decode_v1(v1, loaded) && loaded[UI_SETTING_REFRESH_METHOD] == 0);
    values[UI_SETTING_REFRESH_METHOD] = 3;
    assert(!encode_record(values, record));
    values[UI_SETTING_REFRESH_METHOD] = 2;
    assert(encode_record(values, record));
    put_u16(record + HEADER_SIZE + 62, 3);
    assert(!decode_v2(record + HEADER_SIZE, 64, loaded));
    assert(encode_record(values, record));
    put_u16(record + HEADER_SIZE + 60, 17);
    assert(!decode_v2(record + HEADER_SIZE, 64, loaded));
    assert(encode_record(values, record));
    record[HEADER_SIZE] ^= 1;
    assert(get_u32(record + 12) != storage_crc32(record + HEADER_SIZE, 64));
    puts("PASS: all three refresh preferences round-trip; old 56/60-byte V2 and V1 retain normal default; existing IDs/values preserved; invalid and duplicate fields rejected; CRC checked");
}
'''
codec_source = '#include <assert.h>\n#include <stdio.h>\n#include <stdint.h>\n#include <string.h>\n#include "ui_settings.h"\n'
codec_source += defines + defaults + '\nstatic const uint8_t defaults[UI_SETTING_COUNT] = SETTINGS_DEFAULTS;\n'
codec_source += 'unsigned ui_settings_default(ui_setting_id_t id) { return (unsigned)id < UI_SETTING_COUNT ? defaults[id] : 0; }\n'
codec_source += 'static const uint8_t old_record[] = {' + ','.join(str(b) for b in old_record) + '};\n'
codec_source += crc + codec + function(store, 'static bool encode_record(') + codec_test

policy_path = ROOT / 'src/boards/epd_e0470a03_57x/reference57/epd_policy.c'
policy = policy_path.read_text(encoding='utf-8')
mode_source = r'''
#include <assert.h>
#include <stdio.h>
#include "epd_waveform.h"
#define EPD_WAVE_TEXT 16
#define RT_ASSERT(value) assert(value)
typedef int rt_base_t;
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
static int prepared_mode;
void ref57_wave_init(void) { prepared_mode = 3; }
int ref57_wave_prepare_mode(int mode) { prepared_mode = mode; return 0; }
uint32_t ref57_wave_get_frames(void) { return prepared_mode == EPD_WAVE_TEXT ? 44u : 60u; }
void ref57_wave_fill_lut(uint32_t *lut, uint32_t frame) { (void)lut; (void)frame; }
static volatile EpdRefreshMethod refresh_method_requested;
static EpdRefreshMethod refresh_method_applied;
'''
mode_source += policy[policy.index('#define MODE_QUEUE_SIZE'):]
for signature in ['bool epd_wave_set_refresh_method(', 'EpdRefreshMethod epd_wave_get_refresh_method(', 'static void apply_refresh_method(']:
    mode_source += function(driver, signature)
mode_source += r'''
static void expect_scan(EpdDrawMode request, int selected)
{
    epd_wave_submit_frame(request);
    assert(epd_wave_table_get_frames(28, EPD_DRAW_MODE_AUTO) != 0);
    assert(prepared_mode == selected);
}

int main(void)
{
    epd_wave_table();
    expect_scan(EPD_DRAW_MODE_AUTO, 0);
    assert(epd_wave_set_refresh_method(EPD_REFRESH_NORMAL));
    apply_refresh_method();
    expect_scan(EPD_DRAW_MODE_PARTIAL, EPD_WAVE_TEXT);

    /* The UI can queue a single-chunk frame before the LCD applies a mode. */
    epd_wave_submit_frame(EPD_DRAW_MODE_PARTIAL);
    assert(epd_wave_set_refresh_method(EPD_REFRESH_RIGHT_TO_LEFT));
    assert(epd_wave_get_refresh_method() == EPD_REFRESH_RIGHT_TO_LEFT);
    assert(refresh_method_applied == EPD_REFRESH_NORMAL);
    apply_refresh_method();
    assert(refresh_method_applied == EPD_REFRESH_RIGHT_TO_LEFT);
    assert(!epd_wave_skip_unchanged_frame());
    assert(epd_wave_table_get_frames(28, EPD_DRAW_MODE_AUTO) != 0 && prepared_mode == 0);
    expect_scan(EPD_DRAW_MODE_PARTIAL, EPD_WAVE_TEXT);

    /* Repeated preferences and cancelled queued changes add no full scan. */
    assert(epd_wave_set_refresh_method(EPD_REFRESH_RIGHT_TO_LEFT));
    apply_refresh_method();
    expect_scan(EPD_DRAW_MODE_PARTIAL, EPD_WAVE_TEXT);
    assert(epd_wave_set_refresh_method(EPD_REFRESH_LEFT_TO_RIGHT));
    assert(refresh_method_applied == EPD_REFRESH_RIGHT_TO_LEFT);
    assert(epd_wave_set_refresh_method(EPD_REFRESH_RIGHT_TO_LEFT));
    apply_refresh_method();
    expect_scan(EPD_DRAW_MODE_PARTIAL, EPD_WAVE_TEXT);

    /* With no queued frame, a mode change also primes the next submission. */
    assert(epd_wave_set_refresh_method(EPD_REFRESH_LEFT_TO_RIGHT));
    apply_refresh_method();
    assert(refresh_method_applied == EPD_REFRESH_LEFT_TO_RIGHT);
    expect_scan(EPD_DRAW_MODE_AUTO, 0);
    expect_scan(EPD_DRAW_MODE_PARTIAL, EPD_WAVE_TEXT);

    epd_wave_submit_frame(EPD_DRAW_MODE_AUTO);
    assert(epd_wave_set_refresh_method(EPD_REFRESH_NORMAL));
    assert(refresh_method_applied == EPD_REFRESH_LEFT_TO_RIGHT);
    apply_refresh_method();
    assert(refresh_method_applied == EPD_REFRESH_NORMAL);
    assert(epd_wave_table_get_frames(28, EPD_DRAW_MODE_AUTO) != 0 && prepared_mode == 0);
    expect_scan(EPD_DRAW_MODE_PARTIAL, EPD_WAVE_TEXT);
    assert(!epd_wave_set_refresh_method((EpdRefreshMethod)3));
    assert(!epd_wave_set_refresh_method((EpdRefreshMethod)-1));
    assert(epd_wave_get_refresh_method() == EPD_REFRESH_NORMAL);
    puts("PASS: deferred RTL/LTR/normal selection; queued and unqueued changes prime exactly one full scan; unchanged and cancelled preferences add no full scan; invalid values rejected");
}
'''

results = []
for name, source, includes in [('codec', codec_source, ROOT / 'src/ui'),
                               ('mode', mode_source, ROOT / 'src/boards/epd_e0470a03_57x')]:
    path = OUT / (name + '.c')
    path.write_text(source, encoding='utf-8')
    binary = OUT / (name + '.exe')
    command = [compiler, '-std=c99', '-fshort-enums', '-O2', '-Wall', '-Wextra', '-Werror', '-Wpedantic',
               '-I', str(includes), str(path), '-o', str(binary)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(result.stdout, end='')
    if result.stderr: print(result.stderr, end='', file=sys.stderr)
    result.check_returncode()
    results.append(dict(name=name, command=command, output=result.stdout))
record = dict(passed=True, production_functions_extracted=True, tests=results,
    sources=[dict(path=str(p.relative_to(ROOT)), sha256=hashlib.sha256(p.read_bytes()).hexdigest())
             for p in [store_path, settings_path, driver_path, policy_path]])
(OUT / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
