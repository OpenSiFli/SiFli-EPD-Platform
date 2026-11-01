"""Compute component manifests, isolated compile groups and paired registries.

compute.json (schema_version=1) contains name, version, public_includes,
public_headers, public_defines, requires and hcpu_app/hcpu_firmware/acpu groups.
Groups contain sources, includes, defines, cflags, archives and system_libraries. The ACPU group can also
provide a kconfig fragment. Paths are relative to the component directory.
Remote implementations also declare acpu_ops and purposes.

No SCons import is required. ComputeRegistry.scons_generate/scons_objects accept
an existing environment; RegisterComputeComponent accepts an explicit registry.
Board execution settings are supplied separately: validated, allow_unvalidated,
step_shared and step_limit (defaults False, False, False, 1).
"""
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re


SCHEMA = 1
TARGETS = ('hcpu_app', 'hcpu_firmware', 'acpu')
PURPOSES = {'continuous': 1, 'foreground': 2, 'background': 4}
NAME = re.compile(r'[a-z0-9_][a-z0-9_.-]*/[a-z0-9_][a-z0-9_./-]*\Z')
IDENT = re.compile(r'[A-Za-z_][A-Za-z_0-9]*\Z')
LIBRARY = re.compile(r'[A-Za-z0-9_][A-Za-z0-9_.+-]*\Z')
# Only implementation options are accepted here. Target, ABI, include and
# linker options have dedicated owners and cannot be smuggled in through flags.
LOCAL_FLAG = re.compile(r'(?:-O(?:[0-3sgz]|fast)|-g[0-3]?|-std=(?:gnu|c)(?:89|99|11|17|23)'
                        r'|-std=(?:gnu\+\+|c\+\+)(?:11|14|17|20|23)'
                        r'|-W(?:no-)?[A-Za-z0-9_=-]+|-f(?:no-)?(?:strict-aliasing|'
                        r'omit-frame-pointer|inline-functions|unroll-loops|function-sections|'
                        r'data-sections|exceptions|rtti)|-fvisibility=(?:hidden|default))\Z')
RESERVED_MACRO = re.compile(r'(?:__.*|BSP_.*|SOC_.*|SF32LB.*|CORE_.*|CPU_.*|ARCH_.*|'
                            r'RT_.*|CONFIG_.*|BUILD_DLMODULE|HAVE_CCONFIG_H|EPD_APP_.*)\Z')


def read_json(path):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError('Duplicate manifest field: ' + key)
            value[key] = item
        return value
    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique)


def fields(value, allowed, label):
    if not isinstance(value, dict) or set(value) - set(allowed):
        raise ValueError('Invalid fields in ' + label)


def strings(value, label):
    if not isinstance(value, list) or any(not isinstance(v, str) or not v for v in value):
        raise ValueError('Expected string list: ' + label)
    return value


def uint(value, label):
    if type(value) is not int or not 1 <= value <= 0xffffffff:
        raise ValueError('Expected positive uint32: ' + label)
    return value


def local_path(root, value, directory=False):
    if directory and value == '.' and root.is_dir():
        return root.resolve()
    if (not isinstance(value, str) or not value or '\\' in value or ':' in value
            or value.startswith('/') or any(p in ('', '.', '..') for p in value.split('/'))):
        raise ValueError('Invalid component relative path: ' + repr(value))
    path = (root / value).resolve()
    if not path.is_relative_to(root) or not (path.is_dir() if directory else path.is_file()):
        raise ValueError('Missing or external component path: ' + value)
    return path


def macros(value, label):
    if not isinstance(value, dict):
        raise ValueError('Expected macro object: ' + label)
    result = {}
    for name, item in value.items():
        if not IDENT.fullmatch(name) or RESERVED_MACRO.fullmatch(name):
            raise ValueError('Reserved or invalid component macro: ' + name)
        if item is not None and (type(item) not in (str, int) or any(c in str(item) for c in '\n\r\0')):
            raise ValueError('Invalid macro value: ' + name)
        result[name] = None if item is None else str(item)
    return result


def merge_macros(*values):
    result = {}
    for value in values:
        for name, item in value.items():
            if name in result and result[name] != item:
                raise ValueError('Conflicting interface macro: ' + name)
            result[name] = item
    return result


def define_map(items):
    """Normalize existing SDK/SCons definitions without changing their values."""
    result = {}
    for item in items:
        if isinstance(item, (list, tuple)):
            name, value = item
        else:
            name, separator, value = str(item).partition('=')
            if not separator:
                value = None
        result = merge_macros(result, {name: None if value is None else str(value)})
    return result


def define_args(value):
    return [name if item is None else name + '=' + item for name, item in sorted(value.items())]


@dataclass(frozen=True)
class BuildGroup:
    name: str
    sources: tuple
    includes: tuple
    defines: dict
    cflags: tuple
    kconfig: Path | None = None
    archives: tuple = ()
    system_libraries: tuple = ()


@dataclass(frozen=True)
class Component:
    manifest: Path
    name: str
    version: int
    public_includes: tuple
    public_headers: tuple
    public_defines: dict
    requires: tuple
    groups: dict
    acpu_ops: str | None
    purposes: int


def load_component(path):
    path = Path(path).resolve()
    if path.is_dir():
        path /= 'compute.json'
    root = path.parent
    data = read_json(path)
    fields(data, ('schema_version', 'name', 'version', 'public_includes', 'public_headers',
                  'public_defines', 'requires', 'acpu_ops', 'acpu_library', 'purposes', *TARGETS), str(path))
    if data.get('schema_version') != SCHEMA or not NAME.fullmatch(data.get('name', '')):
        raise ValueError('Invalid component schema/name: ' + str(path))
    version = uint(data.get('version'), 'version')
    public_includes = tuple(local_path(root, p, True) for p in strings(data.get('public_includes', []), 'public_includes'))
    public_headers = tuple(local_path(root, p) for p in strings(data.get('public_headers', []), 'public_headers'))
    public_defines = macros(data.get('public_defines', {}), 'public_defines')
    requires = strings(data.get('requires', []), 'requires')
    if len(set(requires)) != len(requires) or any(not NAME.fullmatch(n) for n in requires):
        raise ValueError('Invalid component requires')
    groups = {}
    for target in TARGETS:
        group = data.get(target, {})
        fields(group, ('sources', 'includes', 'defines', 'cflags', 'kconfig', 'archives', 'system_libraries'), target)
        kconfig = local_path(root, group['kconfig']) if 'kconfig' in group else None
        if kconfig and target != 'acpu':
            raise ValueError('Component Kconfig fragments belong to the ACPU target')
        sources = tuple(dict.fromkeys(local_path(root, p) for p in strings(group.get('sources', []), target + '.sources')))
        if any(p.suffix not in ('.c', '.cpp', '.cc', '.s', '.S') for p in sources):
            raise ValueError('Unsupported component source extension: ' + target)
        includes = tuple(local_path(root, p, True) for p in strings(group.get('includes', []), target + '.includes'))
        defines = macros(group.get('defines', {}), target + '.defines')
        merge_macros(public_defines, defines)
        flags = strings(group.get('cflags', []), target + '.cflags')
        if any(not LOCAL_FLAG.fullmatch(f) for f in flags):
            raise ValueError('Unsupported component option (target/ABI options are fixed): ' + repr(flags))
        archives = tuple(dict.fromkeys(local_path(root, p) for p in strings(group.get('archives', []), target + '.archives')))
        if any(p.suffix != '.a' for p in archives):
            raise ValueError('Component archives must be .a files')
        libraries = tuple(dict.fromkeys(strings(group.get('system_libraries', []), target + '.system_libraries')))
        if any(not LIBRARY.fullmatch(p) for p in libraries):
            raise ValueError('Invalid toolchain library name')
        groups[target] = BuildGroup(data['name'], sources, includes, defines, tuple(flags), kconfig, archives, libraries)
    ops = data.get('acpu_ops')
    uses = strings(data.get('purposes', []), 'purposes')
    if len(set(uses)) != len(uses) or any(p not in PURPOSES for p in uses):
        raise ValueError('Invalid component purposes')
    if ops is not None and (not isinstance(ops, str) or not IDENT.fullmatch(ops)
                            or not (groups['acpu'].sources or groups['acpu'].archives) or not uses):
        raise ValueError('Remote component requires acpu_ops, acpu sources and purposes')
    library = data.get('acpu_library', False)
    if type(library) is not bool or (library and (ops is not None or uses)):
        raise ValueError('ACPU library cannot declare an operation or purpose')
    if ops is None and not library and (groups['acpu'].sources or groups['acpu'].archives or uses):
        raise ValueError('Remote component is missing acpu_ops')
    return Component(path, data['name'], version, public_includes, public_headers,
                     public_defines, tuple(requires), groups, ops, sum(PURPOSES[p] for p in uses))


def public_header_inputs(component):
    paths = set(component.public_headers)
    for directory in component.public_includes:
        for path in directory.rglob('*.h'):
            path = path.resolve()
            if not path.is_relative_to(component.manifest.parent):
                raise ValueError('External component public header: ' + str(path))
            paths.add(path)
    return sorted(paths)


def interface_inputs(exports):
    """Normalize build-local exports for the existing firmware profile hash."""
    if exports.get('schema_version') != SCHEMA:
        raise ValueError('Unsupported compute build metadata')
    result = []
    for component in exports['components']:
        root = Path(component['root']).resolve()

        def relative(value):
            path = Path(value).resolve()
            if not path.is_relative_to(root):
                raise ValueError('External component public path: ' + str(path))
            return path.relative_to(root).as_posix()

        hashes = {}
        for path, expected in component['public_header_sha256'].items():
            if hashlib.sha256(Path(path).read_bytes()).hexdigest() != expected:
                raise ValueError('Component public header differs from build; rebuild platform: ' + path)
            hashes[relative(path)] = expected
        result.append({'name': component['name'], 'version': component['version'],
                       'requires': sorted(component['requires']),
                       'public_includes': [relative(p) for p in component['public_includes']],
                       'public_headers': [relative(p) for p in component['public_headers']],
                       'public_defines': macros(component['public_defines'], 'public_defines'),
                       'public_header_sha256': hashes})
    return sorted(result, key=lambda c: (c['name'], c['version']))


def execution_config(value=None):
    value = value or {}
    fields(value, ('validated', 'allow_unvalidated', 'step_shared', 'step_limit'), 'execution')
    for key in ('validated', 'allow_unvalidated', 'step_shared'):
        if type(value.get(key, False)) is not bool:
            raise ValueError('Expected boolean: ' + key)
    validated, shared = value.get('validated', False), value.get('step_shared', False)
    if shared and not (validated or value.get('allow_unvalidated', False)):
        raise ValueError('step_shared requires execution permission')
    return {'flags': int(validated) | (int(shared) << 1) | (int(value.get('allow_unvalidated', False)) << 2),
            'step_limit': uint(value.get('step_limit', 1), 'step_limit')}


class ComputeRegistry:
    def __init__(self):
        self.components = []
        self.execution = {}

    def register(self, path, execution=None):
        component = load_component(path)
        if any((c.name, c.version) == (component.name, component.version) for c in self.components):
            raise ValueError('Duplicate compute capability: ' + component.name)
        self.execution[(component.name, component.version)] = execution_config(execution)
        self.components.append(component)
        return component

    def ordered(self):
        return sorted(self.components, key=lambda c: (c.name, c.version))

    def dependencies(self, component, chain=()):
        if component.name in chain:
            raise ValueError('Cyclic component dependency: ' + component.name)
        result = []
        for name in component.requires:
            matches = [c for c in self.components if c.name == name]
            if len(matches) != 1:
                raise ValueError('Missing or ambiguous component dependency: ' + name)
            for dependency in [matches[0], *self.dependencies(matches[0], (*chain, component.name))]:
                if dependency not in result:
                    result.append(dependency)
        return result

    def groups(self, target, base_defines=()):
        if target not in TARGETS:
            raise ValueError('Unknown component target: ' + target)
        base = define_map(base_defines)
        result, claimed = [], set()
        for component in self.ordered():
            group = component.groups[target]
            public = [component, *self.dependencies(component)]
            defines = merge_macros(*(c.public_defines for c in public), group.defines)
            merge_macros(base, defines)
            includes = tuple(dict.fromkeys([*group.includes, *(p for c in public for p in c.public_includes)]))
            if claimed.intersection(group.sources):
                raise ValueError('Source belongs to multiple component groups: ' + component.name)
            if target == 'hcpu_app' and any(p.suffix != '.c' for p in group.sources):
                raise ValueError('Application component sources must be C: ' + component.name)
            claimed.update(group.sources)
            result.append(BuildGroup(component.name, group.sources, includes, defines, group.cflags, group.kconfig,
                                     group.archives, group.system_libraries))
        return result

    def link_inputs(self, target):
        """Put dependent archives before their providers; toolchain libraries last."""
        self.groups(target)  # Also validate missing/cyclic dependencies.
        visited, ordered = set(), []
        def visit(component):
            if component.name in visited:
                return
            for dependency in self.dependencies(component):
                visit(dependency)
            visited.add(component.name)
            ordered.append(component)
        for component in self.ordered():
            visit(component)
        groups = [c.groups[target] for c in reversed(ordered)]
        return (tuple(dict.fromkeys(p for g in groups for p in g.archives)),
                tuple(dict.fromkeys(p for g in groups for p in g.system_libraries)))

    def write_acpu_kconfig(self, path):
        """Compose selected ACPU fragments for the SDK's Kconfig resolver."""
        fragments = dict.fromkeys(c.groups['acpu'].kconfig for c in self.ordered()
                                  if c.groups['acpu'].kconfig)
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        write_changed(path, ''.join('source ' + json.dumps(p.as_posix(), ensure_ascii=False) + '\n'
                                    for p in fragments))
        return path

    def consumer_interface(self, names, base_defines=()):
        """Public options for a named consumer group, not the global SDK env."""
        public = []
        for name in names:
            matches = [c for c in self.ordered() if c.name == name]
            if len(matches) != 1:
                raise ValueError('Missing or ambiguous component dependency: ' + name)
            for component in [matches[0], *self.dependencies(matches[0])]:
                if component not in public:
                    public.append(component)
        defines = merge_macros(*(c.public_defines for c in public))
        merge_macros(define_map(base_defines), defines)
        includes = tuple(dict.fromkeys(p for c in public for p in c.public_includes))
        return includes, defines

    def export_inputs(self):
        """Build-local paths and public configuration; no execution policy."""
        return {'schema_version': SCHEMA, 'components': [
            {'name': c.name, 'version': c.version, 'root': str(c.manifest.parent),
             'requires': list(c.requires),
             'public_headers': [str(p) for p in c.public_headers],
             'public_includes': [str(p) for p in c.public_includes],
             'public_defines': c.public_defines,
             'public_header_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in public_header_inputs(c)}} for c in self.ordered()]}

    def capabilities(self):
        return {'schema_version': SCHEMA, 'capabilities': [
            dict(name=c.name, version=c.version, purposes=c.purposes,
                 **self.execution[(c.name, c.version)])
            for c in self.ordered() if c.acpu_ops]}

    def generate(self, output):
        """Write deterministic paired tables and build-only SDK export inputs."""
        output = Path(output)
        self.groups('hcpu_firmware')
        self.groups('acpu')
        output.mkdir(parents=True, exist_ok=True)
        capabilities = self.capabilities()
        remote = [c for c in self.ordered() if c.acpu_ops]
        rows = ['{ ' + json.dumps(c['name']) + ', ' + ', '.join(str(c[k]) + 'u' for k in
                ('version', 'purposes', 'flags', 'step_limit')) + ' }' for c in capabilities['capabilities']]
        prefix = '/* Generated by compute_build.py. */\n#include "epd_compute_protocol.h"\n\n'
        hcpu = prefix + 'const epd_compute_registration_t epd_compute_capabilities[] = {\n'
        hcpu += ''.join('    ' + row + ',\n' for row in rows) if rows else '    { 0 },\n'
        hcpu += '};\nconst uint32_t epd_compute_capability_count = ' + str(len(rows)) + 'u;\n'
        acpu = prefix + ''.join('extern const epd_compute_ops_t ' + c.acpu_ops + ';\n' for c in remote)
        acpu += '\nconst epd_compute_remote_registration_t epd_compute_remote_capabilities[] = {\n'
        acpu += ''.join('    { ' + row + ', &' + c.acpu_ops + ' },\n' for c, row in zip(remote, rows)) if rows else '    { { 0 }, 0 },\n'
        acpu += '};\nconst uint32_t epd_compute_remote_capability_count = ' + str(len(rows)) + 'u;\n'
        # Build-local paths are not copied to the portable capability manifest.
        exports = self.export_inputs()
        values = {'hcpu': ('epd_compute_registry_hcpu.c', hcpu),
                  'acpu': ('epd_compute_registry_acpu.c', acpu),
                  'capabilities': ('compute-capabilities.json', json_text(capabilities)),
                  'exports': ('compute-exports.json', json_text(exports))}
        for name, text in values.values():
            write_changed(output / name, text)
        return {key: output / value[0] for key, value in values.items()}

    def scons_generate(self, env, output):
        """Create explicit generation dependencies; return SCons target nodes."""
        output = Path(output).resolve()
        names = ['epd_compute_registry_hcpu.c', 'epd_compute_registry_acpu.c',
                 'compute-capabilities.json', 'compute-exports.json']
        manifests = [str(c.manifest) for c in self.ordered()]
        settings = [(c.name, c.version, self.execution[(c.name, c.version)]) for c in self.ordered()]

        def action(target, source, env):
            registry = ComputeRegistry()
            for manifest, (_, _, config) in zip(manifests, settings):
                registry.register(manifest, {'validated': bool(config['flags'] & 1),
                                            'allow_unvalidated': bool(config['flags'] & 4),
                                            'step_shared': bool(config['flags'] & 2),
                                            'step_limit': config['step_limit']})
            registry.generate(output)
            return 0

        # These headers are hashed as data, not preprocessed. Scanning their C
        # includes would pull in epd_app_profile.h, which depends on this export.
        metadata_env = env.Clone(SCANNERS=[])
        return metadata_env.Command([str(Path(output) / n) for n in names],
                           [*manifests, *(str(p) for c in self.ordered() for p in public_header_inputs(c)),
                            str(Path(__file__).resolve()), env.Value(json_text(settings))], action)

    def scons_objects(self, env, target, output, base_defines=()):
        """Compile component objects in cloned environments, without SDK globals."""
        output = Path(output).resolve()
        objects = []
        inherited = [*env.get('CPPDEFINES', []), *base_defines]
        for group_index, group in enumerate(self.groups(target, inherited)):
            local = env.Clone()
            local.Prepend(CPPPATH=[str(p) for p in group.includes])
            for flag_name in ('CCFLAGS', 'CFLAGS', 'CXXFLAGS'):
                if isinstance(local.get(flag_name, []), str):
                    local.Replace(**{flag_name: local.Split(local[flag_name])})
            exception_flags = [flag for flag in group.cflags if flag in ('-fexceptions', '-fno-exceptions')]
            cpp_flags = [flag for flag in group.cflags if '++' in flag or flag in ('-frtti', '-fno-rtti')]
            c_flags = [flag for flag in group.cflags if flag.startswith('-std=') and flag not in cpp_flags]
            cpp_flags += exception_flags
            c_flags += exception_flags
            shared_flags = [flag for flag in group.cflags if flag not in cpp_flags and flag not in c_flags]
            local.Append(CPPDEFINES=define_args(group.defines), CCFLAGS=shared_flags,
                         CFLAGS=c_flags, CXXFLAGS=cpp_flags)
            for index, source in enumerate(group.sources):
                dest = Path(output) / str(group_index) / group.name / (str(index) + '.o')
                objects += local.Object(str(dest), str(source))
        archives, libraries = self.link_inputs(target)
        # Archive nodes are explicit link inputs. The SDK's response-file path
        # conversion applies to SOURCES, but not to File nodes placed in LIBS.
        objects += [env.File(p.as_posix()) for p in archives]
        if libraries:
            env.Prepend(LIBS=list(libraries))
        return objects


def RegisterComputeComponent(registry, manifest, execution=None):
    return registry.register(manifest, execution)


def load_product_components(root, selection=None, execution_path=None):
    """Load product defaults or the complete explicit component selection."""
    root = Path(root).resolve()
    if selection is None:
        path = root / 'project/compute-components.json'
        data = read_json(path)
        fields(data, ('schema_version', 'components'), str(path))
        if data.get('schema_version') != SCHEMA:
            raise ValueError('Unsupported product components schema')
        selected = strings(data.get('components', []), 'components')
    else:
        selected = [p.strip() for p in selection.split(',') if p.strip()]
    execution = read_json(execution_path) if execution_path and Path(execution_path).is_file() else {}
    registry = ComputeRegistry()
    for value in selected:
        manifest = (root / value).resolve()
        if manifest.is_dir():
            manifest /= 'compute.json'
        name = read_json(manifest)['name']
        registry.register(manifest, execution.get(name))
    return registry


def json_text(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, indent=2) + '\n'


def write_changed(path, text):
    path = Path(path)
    if not path.exists() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')
