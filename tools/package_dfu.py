# SPDX-License-Identifier: Apache-2.0
"""Build a TF-card DFU ZIP from selected SF32LB57 EPD firmware images."""

import argparse
import json
import os
from pathlib import Path
import tempfile
import zipfile

BOARD = "sf32lb57-epd-gyd7n6"
LAYOUT = "epd57-tf-dfu-font-v2"
ROOT = Path(__file__).resolve().parents[1]
BOARD_PROJECT = ROOT / "project" / f"{BOARD}_hcpu"
DEFAULT_BUILD = BOARD_PROJECT.with_name(f"build_{BOARD_PROJECT.name}")
DEFAULT_WAVEFORM = ROOT / "src/boards/epd_e0470a03_57x/waveform/epd_waveform.bin"
ERASE_SIZE = 4096
ZIP_LIMIT = 16 * 1024 * 1024

# The insertion order is also the Flash write order.
TARGETS = {
    "main": ("HCPU_FLASH_CODE", "ER_IROM1.bin", "main.bin/ER_IROM1.bin"),
    "resources": ("BUILTIN_RESOURCE", "ER_IROM2.bin", "main.bin/ER_IROM2.bin"),
    "acpu": ("DFU_ACPU_FLASH", "acpu.bin", "acpu/acpu.bin"),
    "waveform": ("CUSTOM_EPD_WAVE_TABLE", "waveform.bin", DEFAULT_WAVEFORM),
}


def ptab_number(value):
    if isinstance(value, str):
        return int(value, 0)
    if type(value) is int:
        return value
    raise ValueError(f"Invalid partition number {value!r}")


def read_targets(ptab):
    regions = json.loads(ptab.read_text(encoding="utf-8"))
    if (not isinstance(regions, list) or {"version": "2"} not in regions
            or any(not isinstance(bank, dict) for bank in regions)):
        raise ValueError("Expected a version 2 partition table")
    flash, = [bank for bank in regions if bank.get("mem") == "flash2"]
    base = ptab_number(flash["base"])
    partitions = {}
    for target, (tag, _, _) in TARGETS.items():
        matching = [region for region in flash["regions"] if tag in region.get("tags", [])]
        if len(matching) != 1:
            raise ValueError(f"Expected one flash2 partition tagged {tag}")
        offset = ptab_number(matching[0]["offset"])
        size = ptab_number(matching[0]["max_size"])
        address = base + offset
        if (base < 0 or offset < 0 or size <= 0 or address + size > 2**32
                or address % ERASE_SIZE or size % ERASE_SIZE):
            raise ValueError(f"Invalid erase-aligned partition for {target}")
        for prior_address, prior_size in partitions.values():
            if address < prior_address + prior_size and prior_address < address + size:
                raise ValueError(f"Overlapping partition for {target}")
        partitions[target] = (address, size)
    return partitions


def add_image(archive, target, path, partition):
    address, capacity = partition
    name = f"images/{TARGETS[target][1]}"
    with path.open("rb") as source:
        initial = os.fstat(source.fileno())
        if not 0 < initial.st_size <= min(capacity, ZIP_LIMIT):
            raise ValueError(f"{target} image has {initial.st_size} bytes; capacity is {capacity}")
        remaining = initial.st_size
        with archive.open(name, "w") as entry:
            while remaining:
                block = source.read(min(remaining, 64 * 1024))
                if not block:
                    raise ValueError(f"Image changed while packaging: {path}")
                entry.write(block)
                remaining -= len(block)
        if source.read(1) or os.fstat(source.fileno()).st_mtime_ns != initial.st_mtime_ns:
            raise ValueError(f"Image changed while packaging: {path}")

    info = archive.getinfo(name)
    return {"target": target, "file": name, "address": address,
            "size": info.file_size, "crc32": info.CRC}


def package_dfu(output, ptab, images, *, store=False):
    if not images or images.keys() - TARGETS.keys():
        raise ValueError("Select at least one supported upgrade target")
    if output.resolve() in {path.resolve() for path in (ptab, *images.values())}:
        raise ValueError("The output path must differ from the partition table and inputs")

    partitions = read_targets(ptab)
    manifest = {"format_version": 1, "board": BOARD, "layout": LAYOUT}
    compression = zipfile.ZIP_STORED if store else zipfile.ZIP_DEFLATED
    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=f".{output.name}.",
                                         suffix=".tmp", delete=False) as temp:
            temp_path = Path(temp.name)
            with zipfile.ZipFile(temp, "w", compression=compression, allowZip64=False) as archive:
                manifest["images"] = [
                    add_image(archive, target, images[target], partitions[target])
                    for target in TARGETS if target in images
                ]
                archive.writestr("manifest.json", json.dumps(manifest, separators=(",", ":")))
            if temp.tell() > ZIP_LIMIT:
                raise ValueError(f"ZIP exceeds {ZIP_LIMIT} bytes")
        temp_path.replace(output)
    finally:
        if temp_path is not None:
            temp_path.unlink(missing_ok=True)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output ZIP path")
    parser.add_argument("--ptab", type=Path, default=BOARD_PROJECT / "ptab.json",
                        help="Project partition table")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD,
                        help="Directory containing the main, resources and ACPU binaries")
    parser.add_argument("--store", action="store_true", help="Store entries without compression")
    parser.add_argument("--all", action="store_true", help="Include all four targets")
    for target, (_, filename, _) in TARGETS.items():
        parser.add_argument(f"--{target}", type=Path, metavar="BIN", nargs="?", const=True,
                            help=f"Include {filename}; omit BIN to use the project artifact")
    args = parser.parse_args()

    images = {}
    for target, (_, _, default) in TARGETS.items():
        value = getattr(args, target)
        if value is True or (args.all and value is None):
            value = args.build_dir / default
        if value is not None:
            images[target] = value

    try:
        manifest = package_dfu(args.output, args.ptab, images, store=args.store)
    except (OSError, TypeError, ValueError, KeyError, zipfile.LargeZipFile) as exc:
        parser.error(str(exc))
    for image in manifest["images"]:
        print(f"{image['target']:9} 0x{image['address']:08X} {image['size']:8} bytes  "
              f"CRC32 {image['crc32']:08X}")
    print(args.output)


if __name__ == "__main__":
    main()
