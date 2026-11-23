"""Package and verify the eight-application V1.1 factory firmware."""

import argparse
import hashlib
import json
import os
import re
import shutil
from pathlib import Path, PurePosixPath
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, BadZipFile, ZipFile

from littlefs import LittleFS, LittleFSError, UserContext

ROOT = Path(__file__).resolve().parents[1]
BOARD = "sf32lb57-epd-gyd7n6_hcpu"
APPS = {"books", "weather", "ai_demo", "board", "calendr", "gallery", "music", "notes"}
IMAGES = {
    "ftab": "ftab/ftab.bin",
    "bootloader": "bootloader/bootloader.bin",
    "main:1": "main.bin/ER_IROM1.bin",
    "main:2": "main.bin/ER_IROM2.bin",
    "acpu": "acpu/acpu.bin",
    "dfu": "dfu/dfu.bin",
    "fs_root": "fs_root.bin",
    "wave_table": "waveform/epd_waveform.bin",
}
FLASH = r"""@echo off
setlocal
pushd "%~dp0" || exit /b 1
set "FLASH_PORT=%~1"
if not defined FLASH_PORT set /p "FLASH_PORT=Serial port: "
if not defined FLASH_PORT (
    popd
    exit /b 2
)
".\sftool.exe" -p "%FLASH_PORT%" -b 1000000 config sftool_param.json
set "FLASH_RESULT=%ERRORLEVEL%"
popd
exit /b %FLASH_RESULT%
"""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def check(condition, message):
    if not condition:
        raise ValueError(message)


def json_bytes(data):
    return (json.dumps(data, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def image_ranges(partition_table):
    flash, = [memory for memory in partition_table if memory.get("mem") == "flash2"]
    base = int(flash["base"], 0)
    # The SDK reserves the leading sectors for ftab and the NOR bootloader.
    ranges = {"ftab": (base, 0x10000), "bootloader": (base + 0x10000, 0x10000)}
    for region in flash["regions"]:
        name = region.get("name")
        if name in IMAGES:
            ranges[name] = (base + int(region["offset"], 0), int(region["max_size"], 0))
    check(set(ranges) == set(IMAGES), "Partition table lacks a factory image region")
    return ranges


def inspect_images(config, ranges, read_image, packaged):
    check(config.get("chip") == "SF32LB57" and str(config.get("memory")).upper() == "NOR",
          "Expected SF32LB57 NOR programming")
    write = config["write_flash"]
    check(write.get("verify") is True and not write.get("erase_all"), "Expected verified image writes")
    by_address = {address: name for name, (address, _) in ranges.items()}
    result = []
    seen = set()
    for entry in write["files"]:
        address = int(entry["address"], 0)
        region = by_address.get(address)
        check(region is not None and region not in seen, f"Unexpected or repeated address {address:#x}")
        name = IMAGES[region]
        size_limit = ranges[region][1]
        path = entry["path"].replace("\\", "/")
        check(path == name or (not packaged and region == "wave_table" and
                               PurePosixPath(path).name == "epd_waveform.bin"),
              f"Unexpected image path for {region}: {path}")
        data = read_image(path, region)
        check(0 < len(data) <= size_limit, f"{name} exceeds its partition or is empty")
        if region == "fs_root":
            check(len(data) == size_limit, "LittleFS image size differs from its partition")
        seen.add(region)
        result.append({"path": name, "address": f"0x{address:08X}",
                       "bytes": len(data), "sha256": sha256(data)})
    check(seen == set(IMAGES), f"Missing factory images: {sorted(set(IMAGES) - seen)}")
    return result


def inspect_apps(data):
    fs = LittleFS(context=UserContext(buffer=bytearray(data)), mount=False,
                  block_size=4096, block_count=len(data) // 4096, read_size=32, prog_size=256)
    fs.mount()
    check(set(fs.listdir("/apps")) == APPS, "LittleFS does not contain exactly the eight factory applications")

    def read(path):
        with fs.open(path, "rb") as stream:
            return stream.read()

    apps = {}
    for name in sorted(APPS):
        prefix = f"/apps/{name}/"
        manifest = json.loads(read(prefix + "app.json"))
        check(manifest["id"] == name and manifest["abi"] == 2 and manifest["package_format"] == 1,
              f"{name}: incompatible application manifest")
        index = read(prefix + "files.sha256")
        check(sha256(index) == manifest["files_sha256"], f"{name}: application index checksum mismatch")
        listed = set()
        for record in index.decode("utf-8").splitlines():
            digest, separator, path = record.partition("  ")
            relative = PurePosixPath(path)
            check(separator and path and not relative.is_absolute() and ".." not in relative.parts
                  and "\\" not in path and path not in listed, f"{name}: invalid application index")
            check(sha256(read(prefix + path)) == digest, f"{name}: application file checksum mismatch: {path}")
            listed.add(path)
        check(f"{name}.so" in listed, f"{name}: missing application binary")
        apps[name] = {key: manifest[key] for key in ("version", "data_version", "build_id")}
    ids = {app["build_id"] for app in apps.values()}
    check(len(ids) == 1 and re.fullmatch("[a-f0-9]{64}", next(iter(ids))),
          "Preinstalled applications do not share a firmware build ID")
    return apps


def verify(path, legacy=False):
    with ZipFile(path) as archive:
        entries = archive.namelist()
        roots = {name.split("/", 1)[0] for name in entries}
        check(len(entries) == len(set(entries)) and len(roots) == 1 and all(
            "/" in name and ".." not in PurePosixPath(name).parts and "\\" not in name for name in entries
        ), "Expected a single portable package directory")
        root = next(iter(roots))

        def read(name):
            return archive.read(f"{root}/{name}")

        config = json.loads(read("sftool_param.json"))
        ranges = image_ranges(json.loads(read("ptab.json")))
        images = inspect_images(config, ranges, lambda name, _: read(name), packaged=True)
        apps = inspect_apps(read("fs_root.bin"))
        check(read("sftool.exe")[:2] == b"MZ", "Missing Windows sftool executable")
        check(b"config sftool_param.json" in read("flash_factory.bat"), "Invalid factory flash script")
        if legacy:
            return {"images": images, "preinstalled_apps": apps}
        manifest = json.loads(read("firmware.json"))
        profile = manifest["profile"]
        check(profile["board"] == BOARD and profile["firmware_variant"] in ("dev", "release"),
              "Invalid package board or variant")
        check(manifest["images"] == images and manifest["preinstalled_apps"] == apps,
              "Package contents differ from the release record")
        check({app["build_id"] for app in apps.values()} == {profile["build_id"]},
              "LittleFS applications were built for a different firmware")
        check(root == bundle_name(profile, manifest["ui_version"]), "Package name differs from its profile")
        return manifest


def bundle_name(profile, version):
    for key in ("platform_commit", "sdk_commit", "build_id"):
        length = 64 if key == "build_id" else 40
        check(re.fullmatch(f"[a-f0-9]{{{length}}}", profile[key]), f"Invalid profile field: {key}")
    expected = r"\d+\.\d+\.\d+-" + profile["build_id"][:6]
    if profile["firmware_variant"] == "dev":
        expected += "-dev"
    check(re.fullmatch(expected, version), "UI version does not match the build variant and ID")
    return f"SiFli-EPD-Platform_{version}_8apps_PCB-V1.1"


def create(build, output, sftool):
    build = build.resolve()
    profile = json.loads((build / "app-profile.json").read_bytes())
    check(profile["board"] == BOARD and profile["firmware_variant"] in ("dev", "release"),
          "Expected a V1.1 build profile with a firmware variant")
    version = profile["ui_version"]
    name = bundle_name(profile, version)
    ptab = ROOT / "project" / BOARD / "ptab.json"
    ranges = image_ranges(json.loads(ptab.read_bytes()))
    config = json.loads((build / "sftool_param.json").read_bytes())
    inputs = {}

    def read_image(path, region):
        if region == "wave_table":
            source = ROOT / "src/boards/epd_e0470a03_57x/waveform/epd_waveform.bin"
            check((build / path).resolve() == source.resolve(), "Waveform differs from the board source")
        else:
            source = build / path
        data = source.read_bytes()
        inputs[IMAGES[region]] = data
        return data

    images = inspect_images(config, ranges, read_image, packaged=False)
    apps = inspect_apps(inputs["fs_root.bin"])
    check({app["build_id"] for app in apps.values()} == {profile["build_id"]},
          "LittleFS applications were built for a different firmware")
    bundle_config = {"chip": "SF32LB57", "memory": "NOR", "write_flash": {
        "verify": True, "files": [{key: item[key] for key in ("path", "address")} for item in images],
    }}
    record = {"ui_version": version, "profile": profile,
              "images": images, "preinstalled_apps": apps}
    output.mkdir(parents=True, exist_ok=True)
    final = output / f"{name}.zip"
    with TemporaryDirectory(dir=output) as temporary:
        archive_path = Path(temporary) / final.name
        with ZipFile(archive_path, "w", compression=ZIP_DEFLATED) as archive:
            for relative, data in inputs.items():
                archive.writestr(f"{name}/{relative}", data)
            archive.write(sftool, f"{name}/sftool.exe")
            archive.write(ptab, f"{name}/ptab.json")
            archive.writestr(f"{name}/sftool_param.json", json_bytes(bundle_config))
            archive.writestr(f"{name}/firmware.json", json_bytes(record))
            archive.writestr(f"{name}/flash_factory.bat", FLASH.replace("\n", "\r\n"))
            archive.writestr(f"{name}/uart_download.bat", '@call "%~dp0flash_factory.bat" %*\r\n')
        verify(archive_path)
        archive_path.replace(final)
    print(final)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack = commands.add_parser("create")
    pack.add_argument("--build", type=Path, required=True)
    pack.add_argument("--output", type=Path, default=ROOT / "outputs" / "firmware")
    pack.add_argument("--sftool", default=os.environ.get("SFTOOL_BIN") or shutil.which("sftool"))
    validate = commands.add_parser("verify")
    validate.add_argument("package", type=Path)
    validate.add_argument("--legacy", action="store_true", help="Verify an older package without firmware.json")
    args = parser.parse_args()
    try:
        if args.command == "create":
            check(args.sftool, "Specify the Windows flashing tool with --sftool or SFTOOL_BIN")
            create(args.build, args.output, args.sftool)
        else:
            print(json_bytes(verify(args.package, args.legacy)).decode("utf-8"), end="")
    except (OSError, ValueError, KeyError, LittleFSError, BadZipFile) as error:
        parser.exit(1, f"Firmware bundle error: {error}\n")


if __name__ == "__main__":
    main()
