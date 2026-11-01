"""Import selected AOSP sounds, or embed the checked-in PCM WAVs offline."""
import argparse
import base64
import hashlib
import json
import math
import struct
from pathlib import Path
import urllib.request
import wave
import zlib

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets/sounds"
REVISION = "1cdfff555f4a21f71ccc978290e2e212e2f8b168"
BASE = f"https://android.googlesource.com/platform/frameworks/base/+/{REVISION}/data/sounds/"
SELECTION = [
    (1, "argon", "notifications/ogg/Argon.ogg"),
    (2, "beryllium", "notifications/ogg/Beryllium.ogg"),
    (3, "ceti", "notifications/ogg/CetiAlpha.ogg"),
    (4, "tick", "effects/ogg/Effect_Tick_48k.ogg"),
    (5, "keypress", "effects/ogg/KeypressStandard_120_48k.ogg"),
]
# Fixed lossless transform parameters for the checked-in PCM resources.
DIFFERENCE_ORDER = {"argon": 4, "beryllium": 3, "ceti": 2, "tick": 2, "keypress": 2}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url):
    with urllib.request.urlopen(url, timeout=30) as response:
        return response.read()


def import_aosp():
    import numpy as np
    import soundfile as sf
    from scipy.signal import resample_poly

    upstream = ASSETS / "upstream"
    upstream.mkdir(parents=True, exist_ok=True)
    declaration = base64.b64decode(fetch(BASE + "Android.bp?format=TEXT"))
    (upstream / "Android.bp").write_bytes(declaration)
    license_bytes = fetch("https://www.apache.org/licenses/LICENSE-2.0.txt")
    (ASSETS / "LICENSE-Apache-2.0.txt").write_bytes(license_bytes)
    entries = []
    for tone, name, path in SELECTION:
        if ('"' + path + '"').encode() not in declaration:
            raise ValueError(f"Asset absent from the licensed AOSP declaration: {path}")
        original = base64.b64decode(fetch(BASE + path + "?format=TEXT"))
        original_path = upstream / (name + ".ogg")
        original_path.write_bytes(original)
        samples, rate = sf.read(original_path, dtype="float64", always_2d=True)
        samples = samples.mean(axis=1)
        divisor = math.gcd(rate, 32000)
        if rate != 32000:
            samples = resample_poly(samples, 32000 // divisor, rate // divisor)
        gain = 4 if name == "tick" else 1
        pcm = np.clip(np.rint(samples * 32768) * gain, -32768, 32767).astype("<i2").tobytes()
        wav_path = ASSETS / (name + ".wav")
        with wave.open(str(wav_path), "wb") as output:
            output.setparams((1, 2, 32000, 0, "NONE", "not compressed"))
            output.writeframes(pcm)
        entries.append(dict(id=tone, name=name, source=BASE + path,
                            original=f"upstream/{name}.ogg", original_sha256=digest(original),
                            wav=wav_path.name, sha256=digest(wav_path.read_bytes()),
                            pcm_bytes=len(pcm), sample_rate=32000, gain=gain))
    manifest = dict(version=1, revision=REVISION, license="Apache-2.0",
                    declaration_sha256=digest(declaration),
                    conversion="Decode, average channels, resample to 32000 Hz, round to integer PCM, apply per-sound gain, clip to signed 16-bit; no trimming.",
                    sounds=entries)
    (ASSETS / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def load_assets():
    manifest = json.loads((ASSETS / "manifest.json").read_text(encoding="utf-8"))
    if manifest["version"] != 1 or manifest["revision"] != REVISION:
        raise ValueError("Unsupported sound manifest")
    if digest((ASSETS / "upstream/Android.bp").read_bytes()) != manifest["declaration_sha256"]:
        raise ValueError("AOSP declaration checksum mismatch")
    result = []
    for item, (tone, name, path) in zip(manifest["sounds"], SELECTION, strict=True):
        if item["id"] != tone or item["name"] != name or item["source"] != BASE + path:
            raise ValueError("Sound identity mismatch")
        for field, checksum in (("original", "original_sha256"), ("wav", "sha256")):
            file = (ASSETS / item[field]).resolve()
            if not file.is_relative_to(ASSETS.resolve()) or digest(file.read_bytes()) != item[checksum]:
                raise ValueError(f"Sound checksum mismatch: {file}")
        with wave.open(str(ASSETS / item["wav"]), "rb") as source:
            if (source.getnchannels(), source.getsampwidth(), source.getframerate(), source.getcomptype()) != (1, 2, 32000, "NONE"):
                raise ValueError("Expected mono 16-bit 32000 Hz PCM")
            pcm = source.readframes(source.getnframes())
            if not pcm or len(pcm) != source.getnframes() * 2 or len(pcm) != item["pcm_bytes"]:
                raise ValueError("PCM length mismatch")
        result.append((item, pcm))
    return result


def pack_pcm(pcm, order):
    if not pcm or len(pcm) % 2 or len(pcm) > 180 * 1024 or not 1 <= order <= 4:
        raise ValueError("Invalid bounded tone or difference order")
    values = list(struct.unpack(f"<{len(pcm) // 2}H", pcm))
    for _ in range(order):
        values = [(value - (values[i - 1] if i else 0)) & 65535
                  for i, value in enumerate(values)]
    signed = [value if value < 32768 else value - 65536 for value in values]
    zigzag = [((value << 1) ^ (value >> 15)) & 65535 for value in signed]
    planes = bytes(value & 255 for value in zigzag) + bytes(value >> 8 for value in zigzag)
    return zlib.compress(planes, 9)


def unpack_pcm(packed, order):
    planes = zlib.decompress(packed)
    count = len(planes) // 2
    codes = [planes[i] | (planes[count + i] << 8) for i in range(count)]
    values = [((value >> 1) ^ -(value & 1)) & 65535 for value in codes]
    for _ in range(order):
        previous = 0
        for i, value in enumerate(values):
            previous = (previous + value) & 65535
            values[i] = previous
    return struct.pack(f"<{count}H", *values)


def generate(output):
    lines = ['/* Generated by tools/sound_assets.py from checked-in AOSP PCM. */',
             '#include "epd_tone_assets.h"',
             '#define SOUND_DATA __attribute__((section(".EPD_SOUND"), aligned(4)))']
    assets = load_assets()
    for item, pcm in assets:
        order = DIFFERENCE_ORDER[item['name']]
        packed = pack_pcm(pcm, order)
        if unpack_pcm(packed, order) != pcm:
            raise ValueError("Tone compression round-trip mismatch")
        lines.append(f'static const unsigned char {item["name"]}[] SOUND_DATA = {{')
        for offset in range(0, len(packed), 24):
            lines.append(",".join(str(b) for b in packed[offset:offset + 24]) + ",")
        lines.append('};')
    lines.append('static const epd_tone_asset_t tones[] = {')
    for item, pcm in assets:
        name = item["name"]
        lines.append(f'    {{{name}, sizeof({name}), {len(pcm)}, {item["sample_rate"]}, '
                     f'EPD_TONE_DIFF_PLANES_ZLIB_V1, {DIFFERENCE_ORDER[name]}, 0x{zlib.crc32(pcm):08x}u}},')
    lines.extend(['};', 'const epd_tone_asset_t *epd_tone_asset(unsigned tone)', '{',
                  '    return tone && tone <= sizeof(tones) / sizeof(tones[0]) ? &tones[tone - 1] : 0;', '}'])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--import-aosp", action="store_true")
    parser.add_argument("--generate", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.import_aosp:
        import_aosp()
    if args.generate:
        generate(args.generate)
    assets = load_assets()
    for item, pcm in assets:
        order = DIFFERENCE_ORDER[item['name']]
        print(f'{item["name"]}: {len(pcm)} PCM bytes, order {order}, '
              f'{len(pack_pcm(pcm, order))} packed bytes, {len(pcm) / 64000:.3f} s')
    print(f'{len(assets)} sounds verified, {sum(len(pcm) for _, pcm in assets)} PCM bytes')
    print(f'{sum(len(pack_pcm(pcm, DIFFERENCE_ORDER[item["name"]])) for item, pcm in assets)} packed bytes')


if __name__ == "__main__":
    main()
