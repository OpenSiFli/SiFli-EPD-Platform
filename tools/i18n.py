"""Validate JSON translations and generate SiFli SDK language resources."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import tempfile


FORMAT_FLAGS = ['-Wformat', '-Werror=format']
GENERATED_HEADERS = {'lang_pack.h', 'epd_strings.h', 'module_name.h'}
LOCALE = re.compile(r'[a-z]{2,3}_[a-z]{2}\Z')
KEY = re.compile(r'[a-z][a-z0-9_]*\Z')
C_KEYWORDS = set(('auto break case char const continue default do double else enum extern float for '
                  'goto if inline int long register restrict return short signed sizeof static struct '
                  'switch typedef union unsigned void volatile while').split())


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique_object)
    except (ValueError, UnicodeError) as error:
        raise ValueError(f'{path}: {error}') from error


def validate_locales(locales, label='locales'):
    if not isinstance(locales, list) or not locales:
        raise ValueError(label + ' must be a nonempty array')
    for locale in locales:
        if not isinstance(locale, str) or not LOCALE.fullmatch(locale):
            raise ValueError(f'{label}: noncanonical locale {locale!r}; expected language_region')
    if len(set(locales)) != len(locales):
        raise ValueError(label + ': duplicate locale')
    for index, first in enumerate(locales):
        for second in locales[index + 1:]:
            if first.startswith(second) or second.startswith(first):
                raise ValueError(f'{label}: ambiguous locale prefix: {first}, {second}')
    return locales


def validate_metadata(metadata, locales=None, required=False):
    """Validate optional legacy metadata, or require explicit localized app metadata."""
    if not isinstance(metadata, dict):
        raise ValueError('Application manifest must be an object')
    if not required and 'default_locale' not in metadata and 'localized_names' not in metadata:
        return None
    default = metadata.get('default_locale')
    validate_locales([default], 'default_locale')
    names = metadata.get('localized_names')
    if not isinstance(names, dict):
        raise ValueError('localized_names must be an object')
    validate_locales(list(names), 'localized_names')
    if default not in names:
        raise ValueError('localized_names must include default_locale')
    for locale, name in names.items():
        if (not isinstance(name, str) or not name.strip() or '\0' in name
                or not 0 < len(name.encode('utf-8')) < 48):
            raise ValueError(f'localized_names[{locale}] must occupy 1-47 UTF-8 bytes')
    if locales is not None:
        if default not in locales:
            raise ValueError('default_locale is absent from translation locales')
        if set(names) != set(locales):
            raise ValueError('localized_names must cover exactly the translation locales')
    return default


def format_signature(text):
    """Accept printf syntax supported by the SDK's RT_PRINTF_PRECISION backend."""
    signature = []
    cursor = 0
    while cursor < len(text):
        if text[cursor] != '%':
            cursor += 1
            continue
        start = cursor
        cursor += 1
        if cursor < len(text) and text[cursor] == '%':
            cursor += 1
            continue
        match = re.match(r'([-+ 0#]*)(\*|[0-9]*)(?:\.(\*|[0-9]*))?(h|l)?([diuoxXcs])', text[cursor:])
        if not match:
            raise ValueError(f'unsupported rt_snprintf format at {text[start:]!r}; '
                             'll, hh, z, positional and floating formats are disabled')
        flags, width, precision, length, conversion = match.groups()
        length = length or ''
        if conversion in 'cs' and (length or set(flags) - {'-'}):
            raise ValueError('unsupported character/string modifier: ' + match.group())
        if conversion in 'uoxX' and set(flags) & {'+', ' '}:
            raise ValueError('signed flag on unsigned format: ' + match.group())
        if conversion == 'c' and precision is not None:
            raise ValueError('precision is invalid for character format: ' + match.group())
        # RT_PRINTF_SPECIAL is not enabled by the project. GCC also rejects # on d/u.
        if '#' in flags:
            raise ValueError('alternate form (#) requires RT_PRINTF_SPECIAL and is disabled')
        if width == '*':
            signature.append('width:int')
        elif width and int(width) > 2147483647:
            raise ValueError('format width exceeds int32')
        if precision == '*':
            signature.append('precision:int')
        elif precision and int(precision) > 2147483647:
            raise ValueError('format precision exceeds int32')
        signature.append(length + conversion)
        cursor += len(match.group())
    return tuple(signature)


def merge_tables(paths, default_locale, c_locales=None):
    validate_locales([default_locale], 'default_locale')
    locales, strings, origins, signatures = None, {}, {}, {}
    for path in paths:
        table = read_json(path)
        if not isinstance(table, dict) or set(table) != {'locales', 'strings'}:
            raise ValueError(f'{path}: expected locales and strings')
        columns = validate_locales(table['locales'], str(path))
        if locales is None:
            locales = columns
        elif set(columns) != set(locales):
            raise ValueError(f'{path}: merged tables must have the same complete locale columns')
        if not isinstance(table['strings'], dict) or not table['strings']:
            raise ValueError(f'{path}: strings must be a nonempty object')
        for key, translations in table['strings'].items():
            if not KEY.fullmatch(key) or key in C_KEYWORDS:
                raise ValueError(f'{path}: invalid semantic C key {key!r}')
            if key in strings:
                kind = 'duplicate' if translations == strings[key] else 'conflicting'
                raise ValueError(f'{path}: {kind} key {key!r}; first defined in {origins[key]}')
            if not isinstance(translations, dict) or set(translations) != set(columns):
                raise ValueError(f'{path}: {key}: translation columns are incomplete or unknown')
            signature = None
            for locale in columns:
                value = translations[locale]
                if not isinstance(value, str) or not value.strip() or '\0' in value:
                    raise ValueError(f'{path}: {key}/{locale}: empty or invalid translation')
                try:
                    value.encode('utf-8')
                    current = format_signature(value)
                except (ValueError, UnicodeError) as error:
                    raise ValueError(f'{path}: {key}/{locale}: {error}') from error
                if signature is not None and signature != current:
                    raise ValueError(f'{path}: {key}/{locale}: format signature {current} != {signature}')
                signature = current
            strings[key], origins[key], signatures[key] = translations, str(path), signature
    if not locales or default_locale not in locales:
        raise ValueError('default_locale is absent from translation locales')
    selected = locales if c_locales is None else validate_locales(list(c_locales), 'c_locales')
    if not set(selected) <= set(locales):
        raise ValueError('c_locales contains an unknown locale')
    if default_locale not in selected:
        raise ValueError('default_locale must be compiled into the language pack')
    return {'locales': locales, 'strings': strings, 'signatures': signatures,
            'default_locale': default_locale, 'c_locales': selected}


def sdk_generator_path():
    return Path(__file__).resolve().parents[1] / 'SiFli-SDK/tools/build/lang_excel_pack.py'


def load_generator(path=None):
    path = Path(path) if path else sdk_generator_path()
    spec = importlib.util.spec_from_file_location('epd_sdk_lang_excel_pack', path)
    module = importlib.util.module_from_spec(spec)
    # Loading the SDK generator must not leave bytecode in the SDK checkout.
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def write_changed(path, content):
    path = Path(path)
    data = content.encode('utf-8') if isinstance(content, str) else content
    if not path.exists() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def format_header(table, generator):
    lines = ['#ifndef EPD_STRINGS_H', '#define EPD_STRINGS_H', '', '#include <stdbool.h>',
             '#include <rtthread.h>', '', '#ifdef __cplusplus', 'extern "C" {', '#endif',
             'extern int epd_i18n_format_check(const char *format, ...)',
             '    __attribute__((format(printf, 1, 2)));', '#ifdef __cplusplus', '}', '#endif', '']
    for key, translations in table['strings'].items():
        if not table['signatures'][key]:
            continue
        literal = generator.c_string_literal(translations[table['default_locale']])
        lines += [f'#define EPD_FMT_{key}(buffer, capacity, ...) \\',
                  f'    (false ? epd_i18n_format_check({literal}, __VA_ARGS__) : \\',
                  f'     rt_snprintf((buffer), (capacity), epd_get_str({key}), __VA_ARGS__))', '']
    lines += ['#endif /* EPD_STRINGS_H */', '']
    return '\n'.join(lines)


def generate(paths, output, default_locale, c_locales=None, generator_path=None, module_name=None):
    table = merge_tables(paths, default_locale, c_locales)
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    generator = load_generator(generator_path)
    # The SDK owns all language table layouts and source generation.
    with tempfile.TemporaryDirectory(prefix='.i18n-', dir=output) as temporary:
        stage = Path(temporary)
        keys = list(table['strings'])
        generated = [generator.generate_lang_pack_h(stage, keys),
                     generator.generate_lang_pack_c(stage, table['c_locales'], default_locale)]
        for locale in table['c_locales']:
            translations = {key: table['strings'][key][locale] for key in keys}
            generated.append(generator.generate_lang_c(stage, locale, locale, keys, translations))
        for path in generated:
            write_changed(output / path.name, path.read_bytes())
    write_changed(output / 'epd_strings.h', format_header(table, generator))
    if module_name is not None:
        if not re.fullmatch('[a-z0-9_]{1,7}', module_name):
            raise ValueError('Invalid module name: ' + str(module_name))
        write_changed(output / 'module_name.h', '#ifndef EPD_MODULE_NAME_H\n#define EPD_MODULE_NAME_H\n'
                      f'#define MODULE_NAME {module_name}\n#endif\n')
    report = {'default_locale': default_locale, 'locales': table['c_locales'], 'keys': len(keys),
              'string_bytes': sum(len(table['strings'][key][locale].encode('utf-8')) + 1
                                  for key in keys for locale in table['c_locales'])}
    write_changed(output / 'i18n-report.json', json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    return report


def platform_sources(ui_root):
    ui_root = Path(ui_root).resolve()
    return sorted((ui_root / 'i18n').glob('*.json')) + [ui_root / 'platform/i18n/strings.json']


def module_sources(module):
    module = Path(module).resolve()
    sources = [module / 'i18n/strings.json']
    if module.name == 'books':
        sources.append(module.parent / 'common/reader/i18n/strings.json')
    return sources


def without_platform_tables(paths):
    """Remove generated platform directories obtained from the firmware's rtua."""
    return [str(path) for path in paths if not any((Path(path) / name).is_file()
                                                  for name in GENERATED_HEADERS)]


def configure_module(firmware_ua, repo, firmware, module):
    """Configure BuildLibrary before it captures the firmware's include paths."""
    repo, firmware, module = (Path(path).resolve() for path in (repo, firmware, module))
    original_paths = firmware_ua.GetCPPPATH
    original_defines = firmware_ua.GetCPPDEFINES
    generated = module / 'output/i18n'

    def paths(bsp_root, rtt_root):
        result = [str(firmware)] + original_paths(str(repo / 'project'), rtt_root)
        result += [str(repo / 'SiFli-SDK' / path.replace('\\', '/').split('/sifli_sdk/', 1)[1])
                   for path in result if '/sifli_sdk/' in path.replace('\\', '/')]
        return [str(generated)] + without_platform_tables(result)

    def defines():
        return [item for item in (original_defines() or [])
                if (item[0] if isinstance(item, (tuple, list)) else item.split('=')[0]) != 'DEFAULT_LANG']

    firmware_ua.GetCPPPATH = paths
    firmware_ua.GetCPPDEFINES = defines


def add_resources(env, sources, output, default_locale, module_name=None, manifest=None):
    """Create the SCons dependency graph before source/header scanning begins."""
    from SCons.Script import Action, Value
    from building import DefineGroup
    table = merge_tables(sources, default_locale)
    if manifest is not None:
        validate_metadata(read_json(manifest), table['locales'], required=True)
    output = Path(output).resolve()
    names = ['lang_pack.h', 'epd_strings.h', 'lang_pack.c', 'i18n-report.json']
    names += [locale + '.c' for locale in table['c_locales']]
    if module_name:
        names.append('module_name.h')
    inputs = [str(Path(source).resolve()) for source in sources]
    inputs += [str(Path(__file__).resolve()), str(sdk_generator_path()), Value(default_locale)]
    if manifest:
        inputs.append(str(manifest))

    def action(target, source, env):
        generate(sources, output, default_locale, module_name=module_name)
        return 0

    nodes = env.Command([str(output / name) for name in names], inputs, Action(action, 'I18N $TARGET'))
    env.PrependUnique(CPPPATH=[str(output)])
    if module_name:
        env.Append(CCFLAGS=' ' + ' '.join(FORMAT_FLAGS))
    env['DEFAULT_LANG'] = default_locale
    objects = DefineGroup('i18n_' + (module_name or 'platform'),
                          [node for node in nodes if str(node).endswith('.c')], depend=[''],
                          CPPPATH=[str(output)],
                          LOCAL_CCFLAGS=' '.join(FORMAT_FLAGS),
                          CPPDEFINES=['BUILD_DLMODULE'] if module_name else [])
    return objects, nodes


def add_module_resources(objects, module):
    from building import GetCurrentEnv
    env, module = GetCurrentEnv(), Path(module).resolve()
    manifest = module / 'app.json'
    metadata = read_json(manifest)
    default = validate_metadata(metadata, required=True)
    generated, nodes = add_resources(env, module_sources(module), module / 'output/i18n', default,
                                     module_name=metadata['id'], manifest=manifest)
    env.Depends(objects, nodes)
    env.Append(LINKFLAGS=' -Wl,-Bsymbolic -Wl,--version-script=' + str(module / 'exports.map'))
    return objects + generated


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', action='append', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--default-locale', required=True)
    parser.add_argument('--c-locales', help='Comma-separated compiled locales')
    parser.add_argument('--sdk-generator')
    parser.add_argument('--module-name')
    args = parser.parse_args(argv)
    try:
        report = generate(args.source, args.output, args.default_locale,
                          args.c_locales.split(',') if args.c_locales else None,
                          args.sdk_generator, args.module_name)
        print(json.dumps(report, ensure_ascii=False, indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f'I18N failed: {error}\n')


if __name__ == '__main__':
    main()
