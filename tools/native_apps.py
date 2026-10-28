"""Build native modules, installation directories and distributable .sap archives."""
import json
import re
from pathlib import Path
import struct
import subprocess
import sys
import shutil
import tempfile
from package_app import build_package, package_entries, relative_name, write_sap
from check_app_imports import verify
from i18n import read_json
from prebuilt_dictionary import unpack


SYMBOLS = ('location', 'humidity', 'wind', 'visibility', 'cloud',
           'sunrise', 'sunset', 'pressure', 'air')


def application_directory(repo, name):
    """Resolve a native application whose directory and manifest use the same id."""
    if (not isinstance(name, str) or not re.fullmatch(r'[a-z0-9_]{1,7}', name) or
            name in ('all', 'none')):
        raise ValueError('Invalid APPS/PREINSTALL application id: ' + repr(name) +
                         '; expected 1-7 lowercase letters, digits or underscores')
    modules = Path(repo).resolve() / 'modules'
    directory = modules / name
    if directory.resolve().parent != modules.resolve():
        raise ValueError('Application must be directly inside modules: ' + str(directory))
    if not directory.is_dir():
        raise ValueError('Unknown APPS/PREINSTALL application: ' + name +
                         '; expected ' + str(directory))
    for source in ('app.json', 'SConstruct', 'SConscript'):
        path = directory / source
        if not path.is_file():
            raise ValueError('Selected application source is missing: ' + str(path))
        if not path.resolve().is_relative_to(directory.resolve()):
            raise ValueError('Application source must stay inside its directory: ' + str(path))
    metadata = read_json(directory / 'app.json')
    if not isinstance(metadata, dict) or metadata.get('id') != name:
        raise ValueError('Application manifest id must match directory name: ' + str(directory))
    return directory


def select_apps(value, repo=None):
    """Select bundled apps or explicit native applications in the repository."""
    names = [name.strip() for name in value.split(',') if name.strip()]
    if not names or names == ['none']:
        return []
    if names == ['all']:
        return list(PACKAGE_BUILDERS) + ["calendr"]
    names = list(dict.fromkeys(names))
    repo = Path(repo) if repo is not None else Path(__file__).resolve().parents[1]
    for name in names:
        application_directory(repo, name)
    return names


def add_selected_packages(env, sdk, repo, names, dictionary=None):
    if not names:
        return {}
    repo = Path(repo).resolve()
    for name in names:
        application_directory(repo, name)
    config = (Path(env['build_dir']) / 'rtconfig.h').read_text(encoding='utf-8')
    defines = dict(re.findall(r'^#define[ \t]+(\w+)[ \t]*([^\r\n]*)', config, re.MULTILINE))
    rgb565 = defines.get('LV_COLOR_DEPTH', '').strip() == '16'
    palette = (1 if 'EZIP_PAL_SUPPORT_1' in defines else 0) if 'EZIP_PAL_SUPPORT' in defines else None
    packages = {}
    for name in names:
        builder = PACKAGE_BUILDERS.get(name, add_generic_package)
        options = {'dictionary': dictionary} if name == 'words' else {}
        if name not in PACKAGE_BUILDERS:
            options['app_id'] = name
        packages[name] = builder(env, sdk, repo, rgb565, palette, **options)
    return packages


def merge_resource(resources, name, source):
    relative_name('res/' + name)
    if name in resources:
        previous, source = Path(resources[name]), Path(source)
        if previous.resolve() == source.resolve() or previous.read_bytes() == source.read_bytes():
            return
        raise ValueError('Conflicting application resource: ' + name +
                         ' (' + str(previous) + ', ' + str(source) + ')')
    resources[name] = source


def validate_asset_names(names):
    """Reject ambiguous file destinations before adding SCons copy/conversion nodes."""
    destinations = {}
    for name in names:
        relative_name(name)
        folded = name.casefold()
        if folded in destinations:
            raise ValueError('Conflicting application resource paths: ' +
                             destinations[folded] + ', ' + name)
        destinations[folded] = name
    for folded, name in destinations.items():
        parts = folded.split('/')
        for end in range(1, len(parts)):
            parent = '/'.join(parts[:end])
            if parent in destinations:
                raise ValueError('Conflicting application file/directory resources: ' +
                                 destinations[parent] + ', ' + name)


def add_generic_package(env, sdk, repo, rgb565=True, palette=None, app_id=None):
    module_dir = application_directory(repo, app_id)
    resources = {}
    for name in ('assets', 'res'):
        directory = module_dir / name
        if not directory.exists() and not directory.is_symlink():
            continue
        if (directory.is_symlink() or
                getattr(directory.lstat(), 'st_file_attributes', 0) & 0x400):
            raise ValueError('Package resources cannot contain links: ' + str(directory))
        if not directory.is_dir():
            raise ValueError('Application resources must be a directory: ' + str(directory))
        for path, _ in sorted(package_entries(directory)):
            relative = path.relative_to(directory).as_posix()
            if name == 'assets' and relative == 'icon.png':
                continue
            merge_resource(resources, relative, path)
    return add_package(env, sdk, repo, app_id, {}, rgb565, palette, resources)


def add_package(env, sdk, repo, app_id, inputs, rgb565=True, palette=None, resources=None,
                libraries=None):
    from SCons.Script import Action, Copy, Value
    repo, sdk = Path(repo).resolve(), Path(sdk).resolve()
    build = Path(env['build_dir']).resolve()
    package_directory = env.get('APP_PACKAGE_DIR', 'app-resources')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', package_directory):
        raise ValueError('APP_PACKAGE_DIR must name a directory directly inside the firmware build')
    root = build / package_directory / app_id
    archive_directory = 'app-packages' if package_directory == 'app-resources' else package_directory + '-packages'
    archive = build / archive_directory / (app_id + '.sap')
    image_root = build / (app_id + '-images')
    module_dir = application_directory(repo, app_id)
    inputs = dict(inputs)
    resources = dict(resources or {})
    icon = module_dir / 'assets/icon.png'
    if icon.is_file():
        if 'icon.ezip' in inputs:
            raise ValueError('Conflicting application image: icon.ezip')
        inputs['icon.ezip'] = icon
        for name in ('icon.LICENSE.txt', 'icon.source.json'):
            source = module_dir / 'assets' / name
            if not source.is_file():
                raise ValueError('Application icon provenance is missing: ' + str(source))
            merge_resource(resources, name, source)
    asset_names = sorted(list(inputs) + ['res/' + name for name in resources])
    validate_asset_names(asset_names)
    module = module_dir / ('output/' + app_id + '.so')
    library_projects = [(module_dir / directory, name) for directory, name in (libraries or ())]
    library_modules = [directory / ('output/' + name + '.so') for directory, name in library_projects]
    tool = sdk / 'tools/png2ezip/ezip.exe'
    flags = ['-rgb565' if rgb565 else '-rgb888', '-lvgl_version', '9',
             '-binfile', '2', '-binext', '.ezip', '-dpt', '1', '-chip', 'sf57x']
    if palette is not None:
        flags += ['-pal_support'] + (['1'] if palette == 1 else [])

    def convert(target, source, env):
        source, target = Path(str(source[0])).resolve(), Path(str(target[0])).resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([str(tool), '-convert', str(source), *flags,
                                 '-outdir', str(target.parent)], capture_output=True, text=True)
        generated = target.parent / (source.stem + '.ezip')
        if result.returncode or not generated.is_file():
            raise RuntimeError(result.stdout + result.stderr)
        if generated != target:
            generated.replace(target)
        data = target.read_bytes()
        png = source.read_bytes()
        width, height = struct.unpack_from('>II', png, 16)
        magic, cf, image_flags, w, h, stride, reserved = struct.unpack_from('<BB5H', data)
        if len(data) <= 12 or magic != 0x19 or not image_flags & 0x100 or (w, h) != (width, height):
            raise ValueError('Invalid SDK LVGL V9 EZIP file: ' + str(target))
        return 0

    images = []
    for name, source in inputs.items():
        node = env.Command(str(image_root / name), [str(source), str(tool), __file__, Value(flags)],
                           Action(convert, 'EZIP $TARGET'))
        images.extend(node)
    for name, source in (resources or {}).items():
        name = 'res/' + name
        images.extend(env.Command(str(image_root / name), str(source), Copy('$TARGET', '$SOURCE')))
    profile = build / 'app-profile.json'

    def compile_module(target, source, env):
        for directory, name in library_projects:
            subprocess.run([sys.executable, '-m', 'SCons', '-C', str(directory),
                            'FIRMWARE=' + str(build), '-j' + str(env.get('APP_JOBS', 8))], check=True)
        subprocess.run([sys.executable, '-m', 'SCons', '-C', str(module_dir),
                        'FIRMWARE=' + str(build), '-j' + str(env.get('APP_JOBS', 8))], check=True)
        return 0

    module_inputs = [str(p) for p in sorted(module_dir.rglob('*')) if p.is_file() and
                     not any(part in ('output', 'packages', 'tests', 'node_modules', '__pycache__') or
                             part.endswith('_tests')
                             for part in p.relative_to(module_dir).parts) and
                     (p.suffix in ('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.inc', '.py', '.map', '.json') or
                      p.name in ('SConstruct', 'SConscript'))]
    module_inputs += [str(build / 'rtconfig.h'),
                      str(build / 'cconfig.h'), str(build / 'rtua.py'), __file__,
                      str(repo / 'tools/runtime_paths.py'), str(repo / 'tools/native_module.py')]
    shared = []
    if app_id in ('books', 'gallery', 'music'):
        shared += ['modules/common/image']
    if app_id == 'books':
        shared += ['modules/common/reader']
    if app_id == 'weather':
        shared += ['modules/common/http']
    if shared:
        module_inputs += [str(p) for directory in shared
                          for p in sorted((repo / directory).rglob('*')) if p.is_file() and
                          not any(part == 'tests' or part.endswith('_tests')
                                  for part in p.relative_to(repo / directory).parts) and
                          (p.suffix in ('.c', '.h', '.py', '.json') or p.name == 'SConscript')]
    module_inputs += [str(sdk / 'tools/build/ua.py'), str(sdk / 'tools/build/building.py')]
    module_inputs += [str(Path(__file__).with_name('i18n.py')),
                      str(sdk / 'tools/build/lang_excel_pack.py')]
    compiled_outputs = [path for image in [module] + library_modules
                        for path in (str(image), str(image) + '.nostrip')]
    module_node = env.Command(compiled_outputs, module_inputs,
                              Action(compile_module, 'MODULE ' + app_id + '.so'))
    # The child SCons owns these outputs. Keep them for its incremental checks.
    env.Precious(module_node)
    # Let the child inspect its transitive public/SDK headers on every invocation;
    # unchanged compiler inputs leave its objects and module intact.
    env.AlwaysBuild(module_node)

    def manifest(target, source, env):
        for compiled in [module] + library_modules:
            verify(compiled, build / 'main.elf', env['APP_NM'], application=(compiled == module))
        root.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=app_id + '-', dir=root.parent) as temporary:
            staged = Path(temporary) / app_id
            resources = Path(temporary) / 'res'
            for name in asset_names:
                if name.startswith('res/'):
                    destination = resources / name[4:]
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(image_root / name, destination)
            result = build_package(module_dir / 'app.json', module, profile, staged,
                                   resources if resources.exists() else None,
                                   image_root / 'icon.ezip' if 'icon.ezip' in inputs else None,
                                   library_modules)
            staged_archive = Path(temporary) / (app_id + '.sap')
            result.update(write_sap(staged, staged_archive))
            archive.parent.mkdir(parents=True, exist_ok=True)
            # Replace this generated package, so removed codecs/assets cannot survive.
            previous = Path(temporary) / 'previous'
            if root.exists():
                root.rename(previous)
            try:
                staged.rename(root)
                staged_archive.replace(archive)
            except OSError:
                if root.exists():
                    root.rename(staged)
                if previous.exists():
                    previous.rename(root)
                raise
        result['package'] = str(archive)
        result['package_directory'] = str(root)
        print(app_id + ' application: ' + json.dumps(result, ensure_ascii=False))
        return 0

    outputs = [str(archive), str(root / 'app.json'), str(root / 'files.sha256'), str(root / (app_id + '.so'))]
    outputs += [str(root / name) for name in asset_names]
    outputs += [str(root / 'codecs' / path.name) for path in library_modules]
    package = env.Command(outputs, images + module_node +
                          [str(profile), str(build / 'main.elf'), str(module_dir / 'app.json'), __file__,
                           str(Path(__file__).with_name('package_app.py')),
                           str(Path(__file__).with_name('check_app_imports.py')), Value(asset_names)],
                          Action(manifest, app_id + ' application package'))
    # The action publishes a complete directory; retain the previous package on failure.
    env.Precious(package)
    return root, package


def add_weather_package(env, sdk, repo, rgb565=True, palette=None):
    repo = Path(repo).resolve()
    assets = repo / 'modules/weather/assets'
    inputs = {f'res/{p.stem}.ezip': p for p in sorted((assets / 'ezip/weather').glob('w*.png'))}
    if len(inputs) != 140 or 'res/w999_160.ezip' not in inputs or 'res/w999_48.ezip' not in inputs:
        raise ValueError('The weather library must contain 70 codes at 160/48 pixels')
    inputs.update({f'res/ui_icon_{name}.ezip': assets / f'ezip/icons/ui_icon_{name}.png'
                   for name in SYMBOLS})

    return add_package(env, sdk, repo, 'weather', inputs, rgb565, palette, resources={
        'weather-icons.json': assets / 'weather/manifest.json',
        'moon-phases.source.json': assets / 'weather/phases/source.json',
        'LICENSE.weather-icons': assets / 'icons/LICENSE.MingCute',
        'licenses/cJSON.txt': repo / 'SiFli-SDK/external/cJSON-1.7.17/LICENSE',
    })


def add_books_package(env, sdk, repo, rgb565=True, palette=None):
    repo = Path(repo).resolve()
    licenses = {
        'licenses/libmobi.txt': 'modules/books/third_party/libmobi/COPYING',
        'licenses/libmobi-gpl3.txt': 'modules/books/third_party/libmobi/COPYING.GPL3',
        'licenses/libxml2.txt': 'modules/books/third_party/libxml2/Copyright',
        'licenses/miniz.txt': 'modules/books/LICENSE.miniz',
        'licenses/libpng.txt': 'modules/common/image/third_party/libpng/LICENSE',
        'licenses/zlib.txt': 'modules/common/image/third_party/zlib/LICENSE',
        'licenses/tjpgd.txt': 'modules/common/image/LICENSE.tjpgd',
    }
    return add_package(env, sdk, repo, 'books', {}, rgb565, palette,
                       resources={name: repo / path for name, path in licenses.items()},
                       libraries=[('codecs/mobi', 'bk_mobi')])


def add_gallery_package(env, sdk, repo, rgb565=True, palette=None):
    repo = Path(repo).resolve()
    return add_package(env, sdk, repo, 'gallery', {}, rgb565, palette)


def add_words_package(env, sdk, repo, rgb565=True, palette=None, dictionary=None):
    from SCons.Script import Action
    repo = Path(repo).resolve()
    module_dir = repo / 'modules/words'
    archive = None
    if dictionary:
        dictionary = Path(dictionary).resolve()
        report = dictionary.with_suffix('.wdb.json')
        sources = (dictionary, report)
    else:
        archive = module_dir / 'assets/dictionary/library.wdb.gz'
        report = module_dir / 'assets/dictionary/library.wdb.json'
        dictionary = Path(env['build_dir']).resolve() / 'words-dictionary/library.wdb'
        sources = (archive, report)
    for source in sources:
        if not source.is_file():
            raise ValueError('words requires dictionary and report: ' + str(source) +
                             '; set WORDS_DICTIONARY=<file.wdb> with its matching <file.wdb.json> report')
    if archive is not None:
        def restore_dictionary(target, source, env):
            unpack(str(source[0]), str(source[1]), str(target[0]))
            return 0

        restored = env.Command(str(dictionary), [str(archive), str(report),
                                                str(Path(__file__).with_name('prebuilt_dictionary.py'))],
                               Action(restore_dictionary, 'Restore words dictionary'))
        env.Precious(restored)
    resources = {'library.wdb': dictionary,
                 'library.wdb.json': report,
                 'LICENSE.ecdict': module_dir / 'LICENSE.ecdict',
                 'LICENSE.fsrs': module_dir / 'LICENSE.fsrs',
                 'SOURCES.txt': module_dir / 'SOURCES.txt'}
    return add_package(env, sdk, repo, 'words', {}, rgb565, palette, resources)


def add_board_package(env, sdk, repo, rgb565=True, palette=None):
    repo = Path(repo).resolve()
    root = repo / 'modules/board/res'
    resources = {path.relative_to(root).as_posix(): path
                 for path in sorted(root.rglob('*')) if path.is_file()}
    return add_package(env, sdk, repo, 'board', {}, rgb565, palette, resources)


def add_hello_package(env, sdk, repo, rgb565=True, palette=None):
    return add_package(env, sdk, repo, 'hello', {}, rgb565, palette)


def add_ai_demo_package(env, sdk, repo, rgb565=True, palette=None):
    root = Path(repo).resolve() / 'modules/ai_demo/assets'
    resources = {'markdown/LICENSE.md': root / 'markdown/LICENSE.md'}
    return add_package(env, sdk, repo, 'ai_demo', {}, rgb565, palette, resources)


def add_music_package(env, sdk, repo, rgb565=True, palette=None):
    sdk = Path(sdk).resolve()
    repo = Path(repo).resolve()
    return add_package(env, sdk, repo, 'music', {}, rgb565, palette, resources={
        'licenses/Helix.txt': sdk / 'external/libhelix-mp3/LICENSE.txt',
        'licenses/libFLAC.txt': sdk / 'external/flac-1.5.0/COPYING.Xiph',
        'licenses/WebRTC.txt': repo / 'modules/music/assets/webrtc.LICENSE.txt',
        'licenses/libpng.txt': repo / 'modules/common/image/third_party/libpng/LICENSE',
        'licenses/zlib.txt': repo / 'modules/common/image/third_party/zlib/LICENSE',
        'licenses/tjpgd.txt': repo / 'modules/common/image/LICENSE.tjpgd',
    })


PACKAGE_BUILDERS = {'weather': add_weather_package, 'books': add_books_package,
                    'gallery': add_gallery_package, 'words': add_words_package,
                    'board': add_board_package, 'hello': add_hello_package,
                    'music': add_music_package, 'ai_demo': add_ai_demo_package}
