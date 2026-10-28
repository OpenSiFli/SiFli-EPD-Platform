import os
import sys
from contextlib import contextmanager
from pathlib import Path
from tempfile import TemporaryDirectory
import rtconfig
from building import PrepareEnv, PrepareBuilding, GetCurrentEnv, GetBoardName, SifliEnv, DoBuilding
from SCons.Script import ARGUMENTS, Export

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from compute_build import load_product_components


@contextmanager
def component_config(registry, directory):
    fragment = registry.write_acpu_kconfig(Path(directory).resolve() / 'compute.Kconfig')
    previous = os.environ.get('EPD_COMPUTE_KCONFIG')
    os.environ['EPD_COMPUTE_KCONFIG'] = fragment.as_posix()
    try:
        yield
    finally:
        if previous is None:
            os.environ.pop('EPD_COMPUTE_KCONFIG', None)
        else:
            os.environ['EPD_COMPUTE_KCONFIG'] = previous


def prepare_env():
    # PrepareEnv discovers the board/output directory and immediately reads
    # proj.conf. Make component definitions available during that first parse.
    registry = load_product_components(ROOT, ARGUMENTS.get('COMPUTE_COMPONENTS'))
    with TemporaryDirectory(prefix='epd-compute-config-') as temporary:
        with component_config(registry, temporary):
            PrepareEnv()


def create_env(project=None):
    global compute_registry
    if project is not None:
        compute_registry = GetCurrentEnv()['EPD_COMPUTE_REGISTRY']
    else:
        profile = ROOT / 'project' / (GetBoardName('HCPU') or '') / 'compute.json'
        compute_registry = load_product_components(ROOT, ARGUMENTS.get('COMPUTE_COMPONENTS'), profile)
    with component_config(compute_registry, rtconfig.OUTPUT_DIR):
        # Re-resolve using persistent inputs in the final build directory.
        SifliEnv(project if project is not None else str(ROOT / 'project/acpu'))
    Export('compute_registry')


def build(env=None):
    # SDK configuration tools run inside PrepareBuilding and parse Kconfig again.
    with component_config(compute_registry, rtconfig.OUTPUT_DIR):
        objects = PrepareBuilding(env)
    env = GetCurrentEnv()
    import building
    allocator = (ROOT / 'project/acpu/compute_alloc.h').as_posix()
    for group in building.Projects:
        if group['name'] in ('libflac', 'libhelix'):
            group['LOCAL_CCFLAGS'] = group.get('LOCAL_CCFLAGS', '') + ' -include "' + allocator + '"'
    objects += env['EPD_COMPUTE_REGISTRY'].scons_objects(
        env, 'acpu', os.path.join(env['BUILD_DIR_FULL_PATH'], 'compute-components'))
    env.Depends(objects, env.get('PTAB_HEADER', []))
    DoBuilding(os.path.join(rtconfig.OUTPUT_DIR, rtconfig.TARGET_NAME + '.' + rtconfig.TARGET_EXT), objects)
    return env
