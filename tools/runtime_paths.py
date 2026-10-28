"""Keep runtime source locations relative without changing debugger paths."""
from pathlib import Path


def macro_flags(*mappings):
    return ['-fmacro-prefix-map=' + Path(root).resolve().as_posix() + '/=' + prefix.rstrip('/') + '/'
            for root, prefix in mappings]


def configure(env):
    import rtconfig

    if rtconfig.PLATFORM != 'gcc' or env.get('EPD_RUNTIME_PATHS'):
        return
    root = Path(__file__).resolve().parents[1]
    # GCC on Windows accepts either separator in source file names.
    flags = ''.join(' "' + flag + '"' for flag in macro_flags((root, '.')))
    env.Append(CCFLAGS=flags, ASFLAGS=flags)
    env['EPD_RUNTIME_PATHS'] = True
