"""Lossless, independently compressed blocks for the built-in Tiny TTF font."""
import struct
import zlib

BLOCK_SIZE = 32768
MAGIC = b"FTZ1"


def pack_font(data):
    if not data or len(data) > 0xFFFFFFFF:
        raise ValueError("font size is outside the 32-bit resource range")
    chunks = [zlib.compress(data[i:i + BLOCK_SIZE], 9)
              for i in range(0, len(data), BLOCK_SIZE)]
    position = 16 + 4 * (len(chunks) + 1)
    offsets = [position]
    for chunk in chunks:
        position += len(chunk)
        offsets.append(position)
    if position > 0xFFFFFFFF:
        raise ValueError("compressed font exceeds the 32-bit resource range")
    return (struct.pack("<4sIII", MAGIC, len(data), BLOCK_SIZE, len(chunks)) +
            struct.pack("<%dI" % len(offsets), *offsets) + b"".join(chunks))
