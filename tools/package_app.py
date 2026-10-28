"""Build a single-file .sap application archive without executing its module."""
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import tempfile
from pathlib import Path
from i18n import merge_tables, read_json, validate_metadata


SAP_MAGIC = b'SIFLSAP\0'
SAP_VERSION = 1
SAP_HEADER = struct.Struct('<8sIIQQ')
SAP_RECORD = struct.Struct('<HHQ32sI')
SAP_MAX_FILES = 8192
SAP_MAX_BYTES = 1073741824
SAP_MAX_MANIFEST = 65536
SAP_MAX_PATH = 159
SAP_CHUNK_SIZE = 1024 * 1024
SAP_METADATA = ('app.json', 'files.sha256')


def sha256(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4096), b''):
            result.update(block)
    return result.hexdigest()


def elf_load_bytes(data):
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Module must be a 32-bit little-endian ELF')
    fields = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    kind, machine, phoff, phsize, phcount = fields[0], fields[1], fields[4], fields[8], fields[9]
    if kind != 3 or machine != 40 or phsize != 32 or phoff + phcount * phsize > len(data):
        raise ValueError('Module must be an ARM ET_DYN image with valid program headers')
    start = end = None
    for i in range(phcount):
        ptype, offset, vaddr, _, filesz, memsz, _, _ = struct.unpack_from('<8I', data, phoff + i * phsize)
        if ptype != 1:
            continue
        if filesz > memsz or offset + filesz > len(data) or vaddr + memsz > 0xffffffff:
            raise ValueError('Invalid ELF load segment')
        if end is not None and vaddr < end:
            raise ValueError('ELF load segments must be sorted and not overlap')
        if start is None:
            start = vaddr
        end = vaddr + memsz
    if start is None or end <= start:
        raise ValueError('Module has no loadable bytes')
    return end - start


def relative_name(name):
    if not isinstance(name, str):
        raise ValueError(f'Unsupported package path: {name!r}')
    try:
        encoded = name.encode('utf-8')
    except UnicodeEncodeError as error:
        raise ValueError(f'Unsupported package path: {name!r}') from error
    parts = name.split('/')
    if (not name or len(encoded) > SAP_MAX_PATH or len(parts) > 8 or
            any(part in ('', '.', '..') for part in parts) or
            any(part.endswith((' ', '.')) for part in parts) or
            any(ord(char) < 32 or ord(char) == 127 or char in '\\:<>"|?*' for char in name)):
        raise ValueError(f'Unsupported package path: {name!r}')
    return name


def package_entries(directory, prefix=''):
    """Enumerate regular files without following links or Windows junctions."""
    with os.scandir(directory) as entries:
        for entry in entries:
            name = relative_name(prefix + entry.name)
            status = entry.stat(follow_symlinks=False)
            if (stat.S_ISLNK(status.st_mode) or
                    getattr(status, 'st_file_attributes', 0) & 0x400):
                raise ValueError('Package cannot contain links: ' + entry.path)
            if stat.S_ISDIR(status.st_mode):
                yield from package_entries(entry.path, name + '/')
            elif stat.S_ISREG(status.st_mode):
                yield Path(entry.path), status.st_size
            else:
                raise ValueError('Package requires regular files: ' + entry.path)


def write_sap(package_directory, output):
    """Validate staged application files and publish a SAP v1 archive atomically."""
    root, output = Path(package_directory).resolve(), Path(output).resolve()
    if output.suffix.lower() != '.sap':
        raise ValueError('Archive output must have a .sap extension')
    if output.is_relative_to(root):
        raise ValueError('Archive output must be outside the installation directory')
    files = {}
    total = SAP_HEADER.size
    for path, size in package_entries(root):
        name = relative_name(path.relative_to(root).as_posix())
        files[name] = (path, size)
        total += SAP_RECORD.size + len(name.encode('utf-8')) + size
        if len(files) > SAP_MAX_FILES or total > SAP_MAX_BYTES:
            raise ValueError('SAP exceeds the 8192-file or 1 GiB archive limit')
    if not set(SAP_METADATA) <= files.keys():
        raise ValueError('Package requires app.json and files.sha256')
    if files['app.json'][1] > SAP_MAX_MANIFEST:
        raise ValueError('app.json exceeds 65536 bytes')
    with files['app.json'][0].open('rb') as stream:
        manifest = stream.read(SAP_MAX_MANIFEST + 1)
    if len(manifest) > SAP_MAX_MANIFEST:
        raise ValueError('app.json exceeds 65536 bytes')
    metadata = json.loads(manifest.decode('utf-8'))
    if not isinstance(metadata, dict):
        raise ValueError('app.json must contain an object')
    expected = {}
    index_hash = hashlib.sha256()
    previous = b''
    with files['files.sha256'][0].open('rb') as stream:
        while True:
            line = stream.readline(64 + 2 + SAP_MAX_PATH + 2)
            if not line:
                break
            index_hash.update(line)
            if not re.fullmatch(rb'[0-9a-f]{64}  [^\r\n]+\n', line):
                raise ValueError('Invalid files.sha256 line')
            encoded_name = line[66:-1]
            name = relative_name(encoded_name.decode('utf-8'))
            if name in SAP_METADATA or encoded_name <= previous:
                raise ValueError('File index paths must be unique, sorted and exclude metadata')
            previous = encoded_name
            expected[name] = bytes.fromhex(line[:64].decode('ascii'))
            if len(expected) > SAP_MAX_FILES - 2:
                raise ValueError('File index exceeds the SAP file limit')
    if set(expected) != files.keys() - set(SAP_METADATA):
        raise ValueError('Package contains missing or unindexed files')
    if index_hash.hexdigest() != metadata.get('files_sha256'):
        raise ValueError('files.sha256 digest does not match app.json')
    expected['app.json'] = hashlib.sha256(manifest).digest()
    expected['files.sha256'] = index_hash.digest()
    names = [*SAP_METADATA, *sorted(files.keys() - set(SAP_METADATA), key=lambda n: n.encode('utf-8'))]
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    archive_hash = hashlib.sha256()
    try:
        with tempfile.NamedTemporaryFile(mode='wb', prefix=output.name + '.', suffix='.tmp',
                                         dir=output.parent, delete=False) as archive:
            temporary = Path(archive.name)

            def write(data):
                archive.write(data)
                archive_hash.update(data)

            write(SAP_HEADER.pack(SAP_MAGIC, SAP_VERSION, len(names), total, 0))
            for name in names:
                path, size = files[name]
                encoded_name = name.encode('utf-8')
                write(SAP_RECORD.pack(len(encoded_name), 0, size, expected[name], 0))
                write(encoded_name)
                checksum = hashlib.sha256()
                remaining = size
                with path.open('rb') as stream:
                    while remaining:
                        block = stream.read(min(remaining, SAP_CHUNK_SIZE))
                        if not block:
                            raise ValueError('Package file shrank while archiving: ' + name)
                        checksum.update(block)
                        write(block)
                        remaining -= len(block)
                    if stream.read(1):
                        raise ValueError('Package file grew while archiving: ' + name)
                if checksum.digest() != expected[name]:
                    raise ValueError('Package file digest mismatch: ' + name)
            if archive.tell() != total:
                raise ValueError('SAP archive length mismatch')
        temporary.replace(output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return {'package': str(output), 'archive_bytes': total, 'archive_files': len(names),
            'archive_sha256': archive_hash.hexdigest()}


def content_extensions(metadata):
    extensions = metadata.get('file_extensions', [])
    if (not isinstance(extensions, list) or
            any(not isinstance(extension, str) or not re.fullmatch('[a-z0-9]+', extension)
                for extension in extensions)):
        raise ValueError('file_extensions must be an array of lowercase alphanumeric extensions without dots')
    if len(set(extensions)) != len(extensions):
        raise ValueError('file_extensions must not contain duplicates')
    if metadata.get('kind') == 'resources' and extensions:
        raise ValueError('Resource packages cannot handle files')
    return extensions.copy()


def build_package(manifest_path, module_path, profile_path, destination, resources=None, icon=None,
                  libraries=None):
    """Build the installation directory used by preinstalled filesystem images."""
    metadata = read_json(manifest_path)
    tables = sorted((Path(manifest_path).resolve().parent / 'i18n').rglob('*.json'))
    default_locale = validate_metadata(metadata, required=bool(tables))
    if tables:
        table = merge_tables(tables, default_locale)
        validate_metadata(metadata, table['locales'], required=True)
    extensions = content_extensions(metadata)
    app_id = metadata.get('id', '')
    if not isinstance(app_id, str) or not re.fullmatch('[a-z0-9_]{1,7}', app_id):
        raise ValueError('Application id must contain 1-7 lowercase letters, digits or underscores')
    if not isinstance(metadata.get('name'), str) or not 0 < len(metadata['name'].encode()) < 48:
        raise ValueError('Application name must occupy 1-47 UTF-8 bytes')
    for field in ('version', 'data_version'):
        if type(metadata.get(field)) is not int or not 0 < metadata[field] <= 0xffffffff:
            raise ValueError(f'{field} must be a positive uint32')
    if metadata.get('abi') != 1 or type(metadata.get('background', False)) is not bool:
        raise ValueError('ABI must be 1 and background must be boolean')
    if type(metadata.get('settings', False)) is not bool:
        raise ValueError('settings must be boolean')
    if type(metadata.get('lockscreen', False)) is not bool:
        raise ValueError('lockscreen must be boolean')
    if metadata.get('kind') == 'resources' and metadata.get('lockscreen', False):
        raise ValueError('Resource packages cannot provide a lock screen')
    profile = json.loads(Path(profile_path).read_text(encoding='utf-8'))
    data = Path(module_path).read_bytes()
    load_bytes = elf_load_bytes(data)
    files = {app_id + '.so': Path(module_path)}
    library_spans = {}
    library_images = {}
    for library in libraries or ():
        library = Path(library)
        name = library.stem
        if (library.suffix != '.so' or not re.fullmatch('[a-z0-9_]{1,7}', name) or
                name == app_id or name in library_spans):
            raise ValueError('Invalid or duplicate private library: ' + str(library))
        image = library.read_bytes()
        library_spans[name] = elf_load_bytes(image)
        library_images[name] = image
        files['codecs/' + library.name] = library
    if resources:
        resource_root = Path(resources).resolve()
        if not resource_root.is_dir():
            raise ValueError('Resource directory does not exist')
        for path in sorted(resource_root.rglob('*')):
            if path.is_symlink() or not path.resolve().is_relative_to(resource_root):
                raise ValueError('Package resources cannot contain links')
            if path.is_file():
                files[relative_name('res/' + path.relative_to(resource_root).as_posix())] = path
    if icon:
        files['icon.ezip'] = Path(icon)
    # Each invocation creates a new generation; older packages are never recursively removed.
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=False)
    lines = []
    for name, source in sorted(files.items(), key=lambda item: item[0].encode('utf-8')):
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        if name == app_id + '.so' and target.read_bytes() != data:
            raise ValueError('Module changed while packaging; rebuild the package')
        if name.startswith('codecs/') and target.read_bytes() != library_images[Path(name).stem]:
            raise ValueError('Private library changed while packaging; rebuild the package')
        lines.append(sha256(target) + '  ' + name + '\n')
    index = destination / 'files.sha256'
    index.write_text(''.join(lines), encoding='utf-8', newline='\n')
    package = {key: metadata[key] for key in ('abi', 'id', 'version', 'name', 'data_version')}
    if 'default_locale' in metadata:
        package.update(default_locale=metadata['default_locale'], localized_names=metadata['localized_names'])
    package.update(package_format=1, build_id=profile['build_id'], files_sha256=sha256(index),
                   settings=metadata.get('settings', False), background=metadata.get('background', False), load_bytes=load_bytes)
    if 'file_extensions' in metadata:
        package['file_extensions'] = extensions
    if 'lockscreen' in metadata:
        package['lockscreen'] = metadata['lockscreen']
    if icon:
        package['icon'] = 'icon.ezip'
    if library_spans:
        package['libraries'] = library_spans
    encoded = (json.dumps(package, ensure_ascii=False, indent=2) + '\n').encode()
    (destination / 'app.json').write_bytes(encoded)
    return {'id': app_id, 'files': len(files), 'module_bytes': len(data), 'load_bytes': load_bytes,
            'minimum_load_peak': len(data) + load_bytes,
            'libraries': {name: {'file_bytes': len(library_images[name]), 'load_bytes': span,
                                 'minimum_load_peak': len(library_images[name]) + span}
                          for name, span in library_spans.items()},
            'package': str(destination)}


def build_sap(manifest_path, module_path, profile_path, output=None, resources=None, icon=None,
              libraries=None):
    output = Path(output).resolve() if output is not None else None
    if output is not None and output.suffix.lower() != '.sap':
        raise ValueError('Archive output must have a .sap extension')
    parent = output.parent if output is not None else Path.cwd()
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sap-package-', dir=parent) as temporary:
        staged = Path(temporary) / 'package'
        result = build_package(manifest_path, module_path, profile_path, staged,
                               resources, icon, libraries)
        result.update(write_sap(staged, output or parent / (result['id'] + '.sap')))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--module', required=True)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--output', help='Output .sap file (default: <id>.sap in the current directory)')
    parser.add_argument('--resources')
    parser.add_argument('--icon')
    parser.add_argument('--library', action='append', default=[])
    args = parser.parse_args()
    try:
        print(json.dumps(build_sap(args.manifest, args.module, args.profile, args.output,
                                   args.resources, args.icon, args.library), ensure_ascii=False, indent=2))
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(1, f'Package failed: {error}\n')
