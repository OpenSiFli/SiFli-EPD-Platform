"""Package words.so, its independent icon and offline dictionary into words.sap."""
import argparse
import json
from pathlib import Path
import shutil
import sys
import tempfile

MODULE = Path(__file__).resolve().parents[1]
REPO = MODULE.parents[1]
sys.path.insert(0, str(REPO / 'tools'))
from package_app import build_sap
from check_app_imports import verify
from prebuilt_dictionary import unpack


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, default=REPO / 'project/build_sf32lb57-epd-gyd7n6_hcpu')
    parser.add_argument('--dictionary', type=Path, help='Override the repository dictionary with a WDB and adjacent report')
    parser.add_argument('--icon', type=Path)
    parser.add_argument('--output', type=Path, help='Output .sap file; defaults to firmware/app-packages/words.sap')
    args = parser.parse_args()
    icon = args.icon or args.firmware / 'words-images/icon.ezip'
    destination = args.output or args.firmware / 'app-packages/words.sap'
    try:
        if destination.suffix.lower() != '.sap':
            raise ValueError('Archive output must have a .sap extension')
        verify(MODULE / 'output/words.so', args.firmware / 'main.elf', 'arm-none-eabi-nm')
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='words-resources-', dir=destination.parent) as temporary:
            resources = Path(temporary) / 'res'
            resources.mkdir()
            for name in ('LICENSE.fsrs', 'LICENSE.ecdict', 'SOURCES.txt'):
                shutil.copyfile(MODULE / name, resources / name)
            if args.dictionary:
                report = args.dictionary.with_suffix('.wdb.json')
                shutil.copyfile(args.dictionary, resources / 'library.wdb')
            else:
                report = MODULE / 'assets/dictionary/library.wdb.json'
                unpack(MODULE / 'assets/dictionary/library.wdb.gz', report, resources / 'library.wdb')
            shutil.copyfile(report, resources / 'library.wdb.json')
            result = build_sap(MODULE / 'app.json', MODULE / 'output/words.so',
                               args.firmware / 'app-profile.json', destination, resources, icon)
        print(json.dumps(result, indent=2, ensure_ascii=False))
    except (OSError, EOFError, ValueError) as error:
        parser.exit(1, f'Package failed: {error}\n')
