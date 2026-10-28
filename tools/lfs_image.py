"""Build the NOR image using the matching SDK's mklfsimg executable."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile


def flash_region(ptab, name):
    for memory in json.loads(Path(ptab).read_text(encoding='utf-8')):
        for region in memory.get('regions', []):
            if region.get('name') == name:
                return int(memory['base'], 0) + int(region['offset'], 0), int(region['max_size'], 0)
    raise ValueError('Missing partition: ' + name)


def verify_import(directory):
    directory = Path(directory).resolve()
    manifest = json.loads((directory / 'migration.json').read_text(encoding='utf-8'))
    if manifest['format'] != 1:
        raise ValueError('Unsupported storage migration format')
    actual = {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (directory / 'flash').rglob('*') if p.is_file()}
    actual['ble.bin'] = hashlib.sha256((directory / 'ble.bin').read_bytes()).hexdigest()
    if actual != manifest['files'] or (directory / 'ble.bin').stat().st_size != 0x4000:
        raise ValueError('Storage backup file list/checksum mismatch')
    return directory / 'flash'


def build_image(directory, target, tool, ptab):
    directory, target = Path(directory).resolve(), Path(target).resolve()
    _, size = flash_region(ptab, 'fs_root')
    # This SDK's Windows tool uses the first '/' as the image-relative path.
    # Pass a backslash-only absolute source directory (never a POSIX/relative path).
    if os.name != 'nt':
        raise RuntimeError('The current SDK image tool requires Windows')
    if not directory.is_dir():
        raise ValueError('Missing image source directory: ' + str(directory))
    for file in directory.rglob('*'):
        if file.is_symlink():
            raise ValueError('Image source must not contain symlinks: ' + str(file))
        name = file.relative_to(directory).as_posix()
        # The SDK executable stores FindFirstFileA names without transcoding.
        if name.encode('mbcs', errors='replace') != name.encode('utf-8'):
            raise ValueError('SDK mklfsimg uses Windows ANSI filenames, which differ from device UTF-8: ' +
                             name + '; use ASCII image filenames or a UTF-8 Windows system locale')
        if len(str(file).encode('utf-8')) >= 256:
            raise ValueError('SDK image tool path exceeds 255 bytes: ' + str(file))
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(tool), '-c', str(directory), '-b', '4096', '-r', '32', '-p', '256',
                    '-s', str(size), '-i', str(target)], check=True)
    if target.stat().st_size != size or b'littlefs' not in target.read_bytes()[:8192]:
        raise ValueError('Invalid LittleFS image output')


def add_image(env, sdk, migration=None, packages=()):
    from SCons.Script import Action, Value
    directory = verify_import(migration) if migration else Path('../disk').resolve()
    ptab = env['PARTITION_TABLE']
    target = str(Path(env['build_dir']) / 'fs_root.bin')
    tool = Path(sdk).resolve() / 'tools/mklfsimg/mklfsimg.exe'
    files = sorted(p for p in directory.rglob('*') if p.is_file())

    def action(target, source, env):
        if migration:
            verify_import(migration)
        # Overlay selected app packages without changing disk/ or the old backup.
        with tempfile.TemporaryDirectory(prefix='lfs-', dir=Path(str(target[0])).parent) as staging:
            staging = Path(staging)
            if directory.exists():
                shutil.copytree(directory, staging, dirs_exist_ok=True)
            for package, nodes in packages:
                destination = staging / 'apps' / package.name
                if not destination.exists():
                    shutil.copytree(package, destination)
            build_image(staging, str(target[0]), tool, ptab)
        return 0

    dependencies = [str(p) for p in files] + [str(ptab), __file__,
                    str(tool),
                    Value(str(directory) + '\n' + '\n'.join(str(p) for p in files)),
                    Value([package.name for package, nodes in packages])]
    if migration:
        dependencies += [str(Path(migration) / 'migration.json'), str(Path(migration) / 'ble.bin')]
    for package, nodes in packages:
        dependencies += nodes
    env.Command(target, dependencies, Action(action, 'LittleFS $TARGET'))
    return target
