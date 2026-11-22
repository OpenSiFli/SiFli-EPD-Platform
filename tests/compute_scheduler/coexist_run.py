"""Real codec/client interleaving through the production scheduler and dispatcher."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from compute_build import load_product_components


def compiler():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars32.bat'
    # The production IPC wire holds 32-bit pointers; compile an actual x86 host.
    output = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True, errors='replace')
    env = os.environ.copy()
    for line in output.splitlines():
        key, sep, value = line.partition('=')
        if sep and key:
            env[key.upper()] = value
    return shutil.which('cl', path=env['PATH']), env


def crc(data, bits, polynomial):
    value = 0
    for byte in data:
        value ^= byte << (bits - 8)
        for _ in range(8):
            value = ((value << 1) ^ (polynomial if value & (1 << (bits - 1)) else 0)) & ((1 << bits) - 1)
    return value


def fixtures(folder):
    from PIL import Image
    for number in (0, 1):
        channels, bits, rate = (1, 16, 8000) if not number else (2, 24, 16000)
        streaminfo = struct.pack('>HH', 16, 16) + bytes(6)
        streaminfo += ((rate << 44) | ((channels - 1) << 41) | ((bits - 1) << 36) | 128).to_bytes(8, 'big') + bytes(16)
        content = bytearray(streaminfo)
        for frame in range(8):
            header = bytes([255, 249, 0x64 + number, ((channels - 1) << 4) | (8 if bits == 16 else 12), frame * 16, 15])
            payload = bytearray(header + bytes([crc(header, 8, 7)]))
            values = [[((i - 8) * 100 + frame * 13 + channel * 64) * (256 if bits == 24 else 1)
                       for i in range(16)] for channel in range(channels)]
            for samples in values:
                payload.append(2)  # Verbatim subframe, no wasted bits.
                for value in samples:
                    payload += value.to_bytes(bits // 8, 'big', signed=True)
            payload += crc(payload, 16, 0x8005).to_bytes(2, 'big')
            content += struct.pack('<I', len(payload)) + payload
            for i in range(16):
                content += struct.pack('<h', (i - 8) * 100 + frame * 13 + (32 if channels == 2 else 0))
        (folder / f'flac{number}.bin').write_bytes(content)
        im = Image.new('L', (48, 40))
        im.putdata([(x * (3 + number) + y * 2 + number * 41) % 256 for y in range(40) for x in range(48)])
        im.save(folder / f'image{number}.jpg', quality=95)


def main():
    cc, env = compiler()
    output = ROOT / 'tmp/compute_scheduler'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='coexist-', dir=output) as directory:
        out = Path(directory)
        registry = load_product_components(ROOT, execution_path=ROOT / 'project/sf32lb57-epd-gyd7n6_hcpu/compute.json')
        generated = registry.generate(out)
        caps = {c['name']: c for c in registry.capabilities()['capabilities']}
        for name in ('epd/helix-mp3', 'epd/flac', 'epd/pcm', 'epd/tjpgd'):
            assert caps[name]['flags'] == 6, (name, caps[name])
        assert caps['epd/anyka-3a']['flags'] == 0
        fixtures(out)
        # Reading algorithms have separate tests. Here their parked session
        # stand-ins exercise admission/priority while real codecs execute.
        real = {'epd_mp3_remote_ops', 'epd_flac_remote_ops', 'epd_pcm_remote_ops', 'epd_jpeg_remote_ops'}
        standins = '\n'.join(f'const epd_compute_ops_t {c.acpu_ops} = {{park_open,park_step,park_reset,park_close}};'
                             for c in registry.ordered() if c.acpu_ops and c.acpu_ops not in real)
        (out / 'parked_ops.h').write_text(standins)
        helix = ROOT / 'SiFli-SDK/external/libhelix-mp3'
        flac = ROOT / 'SiFli-SDK/external/flac-1.5.0'
        jpeg = ROOT / 'modules/common/image/third_party/tjpgd'
        includes = [out, HERE, ROOT / 'src/platform', ROOT / 'src/services', ROOT / 'components/compute/include',
            ROOT / 'components/compute_mp3/include', ROOT / 'components/compute_flac/include',
            ROOT / 'components/compute_pcm/include', ROOT / 'modules/common/image/compute/include',
            helix, helix / 'pub', helix / 'real', flac, flac / 'include', flac / 'src/libFLAC/include', jpeg]
        units = [HERE / 'coexist.c', generated['hcpu'], generated['acpu'],
            ROOT / 'components/compute/epd_compute_protocol.c', ROOT / 'components/compute/epd_compute_remote.c',
            ROOT / 'components/compute/epd_compute_memory.c',
            ROOT / 'components/compute_mp3/mp3_remote.c', ROOT / 'components/compute_flac/flac_remote.c',
            ROOT / 'components/compute_pcm/pcm_remote.c', ROOT / 'components/compute_pcm/pcm_convert.c',
            helix / 'mp3dec.c', helix / 'mp3tabs.c', *sorted((helix / 'real').glob('*.c'))]
        units += [flac / f'src/libFLAC/{name}.c' for name in
            ('bitmath', 'bitreader', 'cpu', 'crc', 'fixed', 'float', 'format', 'lpc', 'md5', 'memory', 'stream_decoder')]
        # TJPGD's Windows typedefs predate stdint; select its standard-C typedefs
        # after the Windows/CRT headers, without changing the codec source.
        for index, source in enumerate((ROOT / 'modules/common/image/compute/jpeg_remote.c', jpeg / 'tjpgd.c')):
            wrapper = out / f'jpeg{index}.c'
            wrapper.write_text('#include <rtthread.h>\n#include <stdint.h>\n#include <string.h>\n#undef _WIN32\n#include "' + source.as_posix() + '"\n')
            units.append(wrapper)
        objects = []
        flags = ['/nologo', '/std:c11', '/utf-8', '/TC', '/Od', '/Zi', '/fsanitize=address',
            '/DWIN32', '/DHAVE_CONFIG_H', '/DFLAC__NO_DLL', '/D_CRT_SECURE_NO_WARNINGS',
            '/FIintrin.h', '/D__builtin_bswap32=_byteswap_ulong', '/D__builtin_bswap16=_byteswap_ushort',
            *(f'/I{p}' for p in includes)]
        for index, source in enumerate(units):
            obj = out / f'{index}.obj'
            command = [cc, *flags, '/W3', '/c', str(source), f'/Fo{obj}']
            result = subprocess.run(command, cwd=out, env=env, text=True, encoding='utf-8', errors='replace', capture_output=True, timeout=60)
            if result.returncode:
                print(result.stdout, result.stderr)
                raise SystemExit(result.returncode)
            objects.append(str(obj))
        exe = out / 'coexist.exe'
        subprocess.run([cc, '/nologo', '/fsanitize=address', *objects, f'/Fe{exe}', '/link', '/DEBUG'],
                       cwd=out, env=env, check=True, timeout=60)
        subprocess.run([str(exe), str(out), str(ROOT / 'SiFli-SDK/example/bt/music_source/disk/test.mp3')],
                       cwd=out, env=env, check=True, timeout=45)
        print('PASS board-generated flags=6 for MP3/FLAC/PCM/TJPGD; anyka remains disabled')


if __name__ == '__main__':
    main()
