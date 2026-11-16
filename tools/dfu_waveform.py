#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
#
# SPDX-License-Identifier: Apache-2.0

"""Extract a standalone INIT/DU header from an E0470A03 waveform BIN."""

import argparse
import itertools
from pathlib import Path
import struct


MAGIC = 0xE9D7AB1E
VERSION = 0x12
INDEX_END = 8 + 16 * 8
FRAME_SIZE = 64
INIT = 3
DU = 1
# BIN indices use (old gray << 4) | new gray, with black=0 and white=15.
MONO_TRANSITIONS = (0x00, 0x0F, 0xF0, 0xFF)


def drive_code(frame, transition):
    """Match the BIN reader's MSB-first groups of four two-bit values."""
    return (frame[transition // 4] >> (6 - 2 * (transition % 4))) & 3


def extract_mode(data, mode):
    count, offset = struct.unpack_from("<II", data, 8 + 8 * mode)
    if not count or not INDEX_END <= offset <= len(data) - count * 16:
        raise ValueError(f"mode {mode} has an invalid temperature table")
    previous_max = -128
    for zone in range(count):
        minimum, maximum, frames, start = struct.unpack_from(
            "<iiII", data, offset + zone * 16
        )
        if minimum != previous_max or not minimum < maximum <= 128:
            raise ValueError(f"mode {mode} has an invalid temperature range")
        if not frames or not INDEX_END <= start <= len(data) - frames * FRAME_SIZE:
            raise ValueError(f"mode {mode}, temperature {minimum}: invalid frame data")
        previous_max = maximum
        raw = [
            data[pos:pos + FRAME_SIZE]
            for pos in range(start, start + frames * FRAME_SIZE, FRAME_SIZE)
        ]
        if mode == INIT:
            # Clear an unknown prior screen only if every gray transition is identical.
            if any(frame != bytes([drive_code(frame, 0) * 0x55]) * FRAME_SIZE
                   for frame in raw):
                raise ValueError(f"INIT at {minimum} C depends on the old gray state")
        packed = [
            sum(drive_code(frame, index) << (2 * slot)
                for slot, index in enumerate(MONO_TRANSITIONS))
            for frame in raw
        ]
        runs = tuple(
            (len(list(group)), codes)
            for codes, group in itertools.groupby(packed)
        )
        if len(runs) > 255 or any(length > 255 for length, _ in runs):
            raise ValueError(f"mode {mode}, temperature {minimum}: run length exceeds format")

        expanded = [codes for length, codes in runs for _ in range(length)]
        if len(expanded) != frames or any(
            ((codes >> (2 * slot)) & 3) != drive_code(frame, index)
            for codes, frame in zip(expanded, raw)
            for slot, index in enumerate(MONO_TRANSITIONS)
        ):
            raise ValueError(f"mode {mode}, temperature {minimum}: round-trip mismatch")
        yield minimum, runs
    if previous_max != 128:
        raise ValueError(f"mode {mode} does not cover signed 8-bit temperatures")


def generate(data):
    if len(data) < INDEX_END or struct.unpack_from("<II", data) != (MAGIC, VERSION):
        raise ValueError("expected an E0470A03 waveform BIN version 0x12")

    all_runs = []
    run_offsets = {}
    modes = {}
    for mode, name in ((INIT, "init"), (DU, "du")):
        rows = []
        for minimum, runs in extract_mode(data, mode):
            if runs not in run_offsets:
                run_offsets[runs] = len(all_runs)
                all_runs.extend(runs)
            rows.append((minimum, len(runs), run_offsets[runs]))
        modes[name] = rows
    if len(all_runs) > 65536:
        raise ValueError("too many waveform runs")

    lines = [
        "/*",
        " * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd",
        " *",
        " * SPDX-License-Identifier: Apache-2.0",
        " */",
        "/* Generated E0470A03 INIT and DU frames. */",
        "#ifndef DFU_WAVEFORM_DATA_H",
        "#define DFU_WAVEFORM_DATA_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct { uint8_t frames, codes; } dfu_wave_run_t;",
        "typedef struct { int8_t minimum; uint8_t count; uint16_t offset; } dfu_wave_zone_t;",
        "",
        "static const dfu_wave_run_t dfu_wave_runs[] = {",
    ]
    lines.extend(f"    {{ {length}, 0x{codes:02x} }}," for length, codes in all_runs)
    lines.append("};")
    for name, rows in modes.items():
        lines.extend(("", f"static const dfu_wave_zone_t dfu_wave_{name}[] = {{"))
        lines.extend(f"    {{ {minimum}, {count}, {offset} }}," for minimum, count, offset in rows)
        lines.append("};")
    lines.extend(("", "#endif", ""))
    size = len(all_runs) * 2 + sum(map(len, modes.values())) * 4
    return "\n".join(lines), size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_bin", type=Path)
    parser.add_argument("output_header", type=Path)
    args = parser.parse_args()
    try:
        header, size = generate(args.input_bin.read_bytes())
        args.output_header.parent.mkdir(parents=True, exist_ok=True)
        args.output_header.write_bytes(header.encode("ascii"))
    except (OSError, ValueError) as exc:
        parser.exit(1, f"DFU waveform: {exc}\n")
    print(f"DFU waveform: verified INIT and DU, {size} bytes of table data")


if __name__ == "__main__":
    main()
