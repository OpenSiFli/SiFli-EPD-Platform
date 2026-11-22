"""Build isolated copies of the example against an existing platform firmware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from check_app_imports import verify
from package_app import build_sap


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, default=ROOT / 'project/build_sf32lb57-epd-gyd7n6_hcpu')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--id', action='append', dest='ids')
    args = parser.parse_args()
    ids = args.ids or ['memdemo']
    if any(not re.fullmatch('[a-z0-9_]{1,7}', value) for value in ids) or len(set(ids)) != len(ids):
        parser.error('Each --id must be a distinct 1-7 character lowercase application ID')
    sdk = os.environ.get('SIFLI_SDK')
    nm = shutil.which('arm-none-eabi-nm')
    if not sdk or not nm:
        parser.error('Activate SiFli-SDK/export.ps1 first')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    firmware = output / 'firmware'
    firmware.mkdir()
    for name in ('main.elf', 'rtconfig.h', 'cconfig.h', 'rtua.py', 'app-profile.json'):
        shutil.copyfile(args.firmware / name, firmware / name)
    record = {'firmware': str(args.firmware.resolve()), 'applications': [], 'sources': {}}
    for path in sorted(HERE.rglob('*')):
        if path.is_file() and not any(part in ('output', '__pycache__') for part in path.relative_to(HERE).parts):
            record['sources'][path.relative_to(HERE).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    for app_id in ids:
        project = output / app_id
        shutil.copytree(HERE, project, ignore=shutil.ignore_patterns('output', '__pycache__', '.sconsign*'))
        manifest = json.loads((project / 'app.json').read_text(encoding='utf-8'))
        manifest['id'] = app_id
        (project / 'app.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        (project / 'app_id.h').write_text('#define DEMO_APP_ID "' + app_id + '"\n', encoding='utf-8')
        subprocess.run([sys.executable, '-m', 'SCons', '-C', str(project), 'PLATFORM_ROOT=' + str(ROOT),
                        'FIRMWARE=' + str(firmware), '-j4'], check=True)
        module = project / 'output' / (app_id + '.so')
        verify(module, firmware / 'main.elf', nm, application=True)
        images = project / 'images'
        images.mkdir()
        subprocess.run([str(Path(sdk) / 'tools/png2ezip/ezip.exe'), '-convert', str(project / 'assets/icon.png'),
                        '-rgb565', '-lvgl_version', '9', '-binfile', '2', '-binext', '.ezip',
                        '-dpt', '1', '-chip', 'sf57x', '-outdir', str(images)], check=True)
        resources = project / 'resources'
        resources.mkdir()
        for name in ('icon.LICENSE.txt', 'icon.source.json'):
            shutil.copyfile(project / 'assets' / name, resources / name)
        package = output / (app_id + '.sap')
        result = build_sap(project / 'app.json', module, firmware / 'app-profile.json', package,
                           resources=resources, icon=images / 'icon.ezip')
        result.update(imports='PASS', module=str(module), sha256=hashlib.sha256(package.read_bytes()).hexdigest())
        record['applications'].append(result)
    (output / 'result.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(record['applications'], ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
