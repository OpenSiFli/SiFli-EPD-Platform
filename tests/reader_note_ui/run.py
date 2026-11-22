"""Test the production note popup using SDK LVGL, MiSans and physical-key routing."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare(output):
    generated = output / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    function = load('ui_extract', ROOT / 'tests/ai_demo_ui/run.py').function

    def write(name, text):
        path = generated / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text(encoding='utf-8') != text:
            path.write_text(text, encoding='utf-8')

    for name in ('epd_app.h', 'epd_ui_scale.h', 'epd_input.h'):
        write('platform/' + name, (ROOT / 'src/ui/platform' / name).read_text(encoding='utf-8'))
    reader = ROOT / 'modules/common/reader'
    for name in ('reader_note_ui.c', 'reader_note_ui.h'):
        write(name, (reader / name).read_text(encoding='utf-8'))
    document = (ROOT / 'modules/books/document/document.h').read_text(encoding='utf-8')
    declarations = '\n'.join(line for line in document.splitlines() if
        re.match(r'(typedef struct document(?:_note_request)? |#define DOCUMENT_NOTE_BYTES|(?:document_note_request_t \*document_note_request|int document_note_take|void document_note_cancel)\()', line))
    write('reader.h', '#pragma once\n#include "platform/epd_app.h"\n' + declarations +
          '\nbool epd_reader_note_visible(void);\nvoid epd_reader_note_close(void);\n')
    strings = json.loads((reader / 'i18n/strings.json').read_text(encoding='utf-8'))['strings']
    table = '#pragma once\n#define epd_get_str(s) (s)\n'
    table += '\n'.join('#define ' + key + ' ' + json.dumps(value['zh_cn'], ensure_ascii=False)
                       for key, value in strings.items())
    write('reader_i18n.h', table)
    launcher = (ROOT / 'src/ui/launcher.c').read_text(encoding='utf-8')
    app = (ROOT / 'src/ui/platform/epd_app.c').read_text(encoding='utf-8')
    write('view.inc', '\n'.join(function(launcher, name) for name in
          ('epd_obj_init', 'label_create', 'centered_label', 'panel_create')) + '\n' + function(app, 'epd_app_button'))
    scale = (ROOT / 'src/ui/platform/epd_ui_scale.c').read_text(encoding='utf-8')
    write('scale.inc', '\n'.join(function(scale, name) for name in ('epd_ui_px', 'epd_ui_touch')))
    source = (ROOT / 'src/ui/platform/epd_input.c').read_text(encoding='utf-8')
    declarations = source[source.index('#define KEY_QUEUE_CAPACITY'):source.index('static void pointer_read_muted')]
    names = ('epd_input_focusable', 'scope_for_owner', 'object_above', 'active_scope', 'publish_scope',
             'back_deleted', 'scope_deleted', 'epd_app_input_group', 'epd_app_input_listen',
             'epd_app_input_unlisten', 'epd_input_add_object', 'epd_app_input_back', 'epd_input_set_ops',
             'discard_queue', 'physical_post', 'remaining_ms', 'wake_release_process',
             'dispatch_application', 'dispatch_long', 'epd_input_clear', 'epd_input_process',
             'epd_input_back', 'epd_input_scope_id')
    write('input.inc', declarations + '\n'.join(function(source, name) for name in names))
    inputs = [reader / 'reader_note_ui.c', reader / 'reader_note_ui.h',
              ROOT / 'modules/books/document/document.h', ROOT / 'font/MiSans-Normal.ttf',
              ROOT / 'src/ui/platform/epd_input.c', ROOT / 'src/ui/platform/epd_app.c',
              ROOT / 'src/ui/platform/epd_ui_scale.c', ROOT / 'src/ui/launcher.c']
    (output / 'sources.json').write_text(json.dumps({str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in inputs}, indent=2), encoding='utf-8')
    return generated


def inspect_tail(image, background, geometry):
    """Extrapolate the rendered triangle apex from changed pixels in its upper/lower half."""
    left, top, right, bottom = geometry['tail']
    tip_y = bottom if geometry['above'] else top
    rows = range(top, bottom + 1)
    centers = []
    for y in rows:
        if not 0 <= y < image.height:
            continue
        if abs(y - tip_y) > max(4, (bottom - top) // 2):
            continue
        points = []
        for x in range(max(0, left), min(image.width - 1, right) + 1):
            rgb = image.getpixel((x, y))
            if rgb != background.getpixel((x, y)) and max(rgb) - min(rgb) <= 2 and 44 <= min(rgb) <= 100:
                points.append(x)
        if len(points) >= 2:
            centers.append((y, (min(points) + max(points)) / 2))
    if len(centers) < 3:
        return dict(tip=None, delta=None, passed=False)
    mean_y = sum(y for y, _ in centers) / len(centers)
    mean_x = sum(x for _, x in centers) / len(centers)
    slope = sum((y - mean_y) * (x - mean_x) for y, x in centers) / sum((y - mean_y) ** 2 for y, _ in centers)
    x = mean_x + slope * (tip_y - mean_y)
    center = (geometry['anchor'][0] + geometry['anchor'][2]) / 2
    return dict(tip=[round(x, 3), tip_y], delta=round(x - center, 3), passed=abs(x - center) <= 3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=HERE / 'output')
    parser.add_argument('--scales', nargs='+', type=int, default=[100])
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    generated = prepare(output)
    _, env = load('host_compiler', ROOT / 'tests/reader_prefetch/cache_run.py').compiler_environment(None)
    for command, name in ((['cmake', '-S', str(HERE), '-B', str(output / 'build'),
                            '-DGENERATED=' + generated.as_posix()], 'configure.log'),
                           (['cmake', '--build', str(output / 'build'), '--config', 'Release', '--parallel', '8'], 'build.log')):
        with (output / name).open('w', encoding='utf-8') as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            print((output / name).read_text(encoding='utf-8', errors='replace')[-12000:])
            return result.returncode
    from PIL import Image, ImageDraw, ImageFont
    records = []
    for scale in args.scales:
        folder = output / str(scale)
        folder.mkdir(exist_ok=True)
        (folder / 'background').mkdir(exist_ok=True)
        result = subprocess.run([str(output / 'build/Release/reader_note_ui.exe'), str(folder),
            str(ROOT / 'font/MiSans-Normal.ttf'), str(scale)], env=env, capture_output=True, timeout=90)
        (folder / 'run.log').write_bytes(result.stdout + result.stderr)
        transcript = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        print(transcript, flush=True)
        tally = re.search(r'Note UI: (\d+) checks, (\d+) failures', transcript)
        frames = []
        errors = []
        geometry_file = folder / 'geometry.json'
        geometry = {row['name']: row for row in json.loads(geometry_file.read_text())} if geometry_file.exists() else {}
        visual_checks = []
        for ppm in sorted(folder.glob('*.ppm')):
            with Image.open(ppm) as image:
                image.save(ppm.with_suffix('.png'))
                if image.size != (684, 1216) or image.convert('L').getextrema()[0] > 100:
                    errors.append('Blank or invalid capture: ' + ppm.name)
                if ppm.stem in geometry:
                    with Image.open(folder / 'background' / ppm.name) as background:
                        check = dict(name=ppm.stem, **inspect_tail(image.convert('RGB'), background.convert('RGB'), geometry[ppm.stem]))
                    visual_checks.append(check)
                    if not check['passed']:
                        errors.append('Rendered tail misses anchor: ' + ppm.name + ' delta=' + str(check['delta']))
                frames.append(ppm.with_suffix('.png'))
        (folder / 'visual-checks.json').write_text(json.dumps(visual_checks, indent=2), encoding='utf-8')
        sheet = Image.new('RGB', (4 * 342, ((len(frames) + 3) // 4) * 640), '#eeeeee')
        draw = ImageDraw.Draw(sheet)
        font = ImageFont.truetype(str(ROOT / 'font/MiSans-Normal.ttf'), 18)
        for i, path in enumerate(frames):
            x, y = (i % 4) * 342, (i // 4) * 640
            draw.text((x + 8, y + 6), path.stem, fill='black', font=font)
            with Image.open(path) as image:
                sheet.paste(image.resize((342, 608)), (x, y + 28))
        if frames:
            sheet.save(folder / 'contact-sheet.png')
        records.append(dict(scale=scale, returncode=result.returncode,
            checks=int(tally[1]) if tally else 0, failures=int(tally[2]) if tally else None,
            captures=len(frames), capture_errors=errors,
            passed=bool(not result.returncode and tally and int(tally[1]) and not int(tally[2]) and not errors)))
    (output / 'results.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    return int(not all(row['passed'] for row in records))


if __name__ == '__main__':
    raise SystemExit(main())
