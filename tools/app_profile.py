"""Generate the firmware configuration identity used by native app packages."""
import ast
import hashlib
import json
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace
import compute_build
from i18n import FORMAT_FLAGS

# SDK Windows parallel compile actions temporarily replace the process PATH.
GIT_EXECUTABLE = shutil.which('git') or 'git'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_changed(path, data):
    path = Path(path)
    if not path.exists() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def stabilize_module_defines(path):
    """Keep SDK-generated module compiler flags independent of Python hash order."""
    path = Path(path)
    text = path.read_text(encoding='utf-8')
    lines = text.splitlines(keepends=True)
    function = next(node for node in ast.parse(text).body
                    if isinstance(node, ast.FunctionDef) and node.name == 'GetCPPDEFINES')
    assignment = next(node for node in function.body if isinstance(node, ast.Assign) and
                      any(isinstance(target, ast.Name) and target.id == 'CPPDEFINES'
                          for target in node.targets))
    defines = sorted(ast.literal_eval(assignment.value), key=repr)
    indent = lines[assignment.lineno - 1][:assignment.col_offset]
    lines[assignment.lineno - 1:assignment.end_lineno] = [indent + 'CPPDEFINES=' + repr(defines) + '\n']
    write_changed(path, ''.join(lines).encode('utf-8'))


def git(root, *args):
    return subprocess.check_output([GIT_EXECUTABLE, '-C', str(root), *args])


def changes(root, *exclude):
    state = hashlib.sha256(git(root, 'diff', '--binary', 'HEAD', '--', '.', *exclude))
    for name in sorted(git(root, 'ls-files', '--others', '--exclude-standard', '-z').decode().split('\0')):
        if not name:
            continue
        state.update(name.encode())
        state.update((root / name).read_bytes())
    return state.hexdigest()


def generate(repo, build, config, compute_exports=None):
    repo, build = Path(repo).resolve(), Path(build).resolve()
    sdk = repo / 'SiFli-SDK'
    submodules = {}
    for line in git(sdk, 'submodule', 'status', '--recursive').decode().splitlines():
        path = line.strip().split()[1]
        root = sdk / path
        submodules[path] = {'commit': git(root, 'rev-parse', 'HEAD').decode().strip(),
                            'changes': changes(root)}
    public = [repo / 'src/ui/platform/epd_app.h', repo / 'src/ui/platform/epd_app.c',
              repo / 'src/ui/platform/app_runtime.c', repo / 'src/ui/platform/epd_input.h',
              repo / 'src/ui/platform/app_memory.h', repo / 'src/ui/platform/app_memory.c',
              repo / 'src/ui/platform/app_library.c', repo / 'src/ui/platform/app_package.h',
              repo / 'src/ui/platform/app_content.h',
              repo / 'src/ui/platform/epd_app_font.h',
              repo / 'src/ui/platform/epd_app_library.h',
              repo / 'src/ui/platform/epd_reader_model.h',
              repo / 'src/ui/platform/epd_system.h',
              repo / 'src/ui/platform/epd_sound.h',
              repo / 'src/ui/platform/epd_ui_scale.h',
              repo / 'src/ui/platform/epd_keyboard.h',
              repo / 'src/ui/platform/epd_lock.h',
              repo / 'src/ui/platform/epd_i18n.h', repo / 'src/ui/platform/epd_locale.h',
              repo / 'src/ui/platform/epd_locale.c',
              repo / 'src/platform/epd_memory.h', repo / 'src/platform/epd_memory.c',
              repo / 'src/platform/epd_ai.h',
              repo / 'src/platform/epd_speech.h',
              repo / 'src/platform/epd_tls_config.h',
              repo / 'src/platform/epd_audio.c',
              repo / 'src/platform/epd_pcm_output.h', repo / 'src/platform/epd_pcm_output.c',
              repo / 'src/platform/epd_compute.h', repo / 'src/platform/epd_compute.c',
              repo / 'components/compute/include/epd_compute_protocol.h',
              repo / 'src/platform/epd_image.h', repo / 'src/platform/epd_image.c',
              repo / 'src/platform/epd_svg.h', repo / 'src/platform/epd_svg.c',
              repo / 'src/ui/ui_settings.h',
              repo / 'src/services/storage.h', repo / 'src/services/storage_file.h',
              repo / 'src/services/system_time.h',
              repo / 'src/ui/platform/app_image.h', repo / 'src/services/app_service.h',
              repo / 'src/services/net/network.h']
    compiler = Path(config.EXEC_PATH) / config.CC
    profile = {
        'profile_format': 1,
        'board': build.name.removeprefix('build_'),
        'sdk_commit': git(sdk, 'rev-parse', 'HEAD').decode().strip(),
        # SDK regenerates certificate order from a Python set on each invocation.
        # Certificate data does not change the native app ABI.
        'sdk_changes': changes(sdk, ':(exclude)external/mbedtls_228/ports/src/tls_certificate.c'),
        'sdk_submodules': submodules,
        'compiler': subprocess.check_output([str(compiler), '--version']).decode().splitlines()[0],
        'module_cflags': config.M_CFLAGS + ' ' + ' '.join(FORMAT_FLAGS),
        'module_ldflags': config.M_LFLAGS,
        'configuration': {name: digest((build / name).read_bytes())
                          for name in ('rtconfig.h', 'cconfig.h')},
        'platform_interface': {path.name: digest(path.read_bytes()) for path in public},
        'component_interface': compute_build.interface_inputs(compute_build.read_json(compute_exports))
                               if compute_exports is not None else [],
    }
    profile['build_id'] = digest(json.dumps(profile, sort_keys=True, ensure_ascii=True).encode())
    write_changed(build / 'app-profile.json',
                  (json.dumps(profile, ensure_ascii=False, indent=2) + '\n').encode())
    write_changed(build / 'epd_app_profile.h',
                  ('#ifndef EPD_APP_PROFILE_H\n#define EPD_APP_PROFILE_H\n'
                   f'#define EPD_APP_BUILD_ID "{profile["build_id"]}"\n#endif\n').encode())
    return profile


def scons_generate(env, repo, build, config, compute_exports):
    """Generate the final HCPU profile after its component export node.

    Bind actual HCPU object nodes to these targets, not DefineGroup source nodes.
    Capture configuration before creating an ACPU project. Rechecking preserves
    the existing SDK Git-state input without an early profile generation pass.
    """
    repo, build = Path(repo).resolve(), Path(build).resolve()
    values = {name: getattr(config, name) for name in ('EXEC_PATH', 'CC', 'M_CFLAGS', 'M_LFLAGS')}
    captured = SimpleNamespace(**values)
    export_path = str(Path(str(compute_exports)).resolve())

    def action(target, source, env):
        generate(repo, build, captured, compute_exports=export_path)
        return 0

    nodes = env.Command([str(build / name) for name in ('app-profile.json', 'epd_app_profile.h')],
                        [compute_exports, str(Path(__file__).resolve()),
                         str(Path(compute_build.__file__).resolve()),
                         str(build / 'rtconfig.h'), str(build / 'cconfig.h'),
                         env.Value(json.dumps(values, sort_keys=True))], action)
    env.AlwaysBuild(nodes)
    return nodes
