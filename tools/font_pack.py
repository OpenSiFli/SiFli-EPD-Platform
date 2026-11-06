"""Lossless, independently compressed blocks for the built-in Tiny TTF font."""
import struct
import zlib
from concurrent.futures import ThreadPoolExecutor

BLOCK_SIZE = 16384
MAGIC = b"FTZ1"


def pack_font(data, block_size=BLOCK_SIZE):
    if not data or len(data) > 0xFFFFFFFF:
        raise ValueError("font size is outside the 32-bit resource range")
    if block_size not in (16384, 32768):
        raise ValueError("FTZ1 block size must be 16384 or 32768")
    blocks = [data[i:i + block_size] for i in range(0, len(data), block_size)]
    if block_size == 16384:
        # Dense independent streams keep smaller blocks within the font region.
        try:
            import zopfli.zlib
        except ImportError as error:
            raise RuntimeError(
                "Missing zopfli. Activate SiFli-SDK/export.ps1, then run "
                "python tools/prepare_build.py from the repository root.") from error
        with ThreadPoolExecutor(max_workers=4) as workers:
            chunks = list(workers.map(lambda block: zopfli.zlib.compress(block, numiterations=1), blocks))
    else:
        chunks = [zlib.compress(block, 9) for block in blocks]
    position = 16 + 4 * (len(chunks) + 1)
    offsets = [position]
    for chunk in chunks:
        position += len(chunk)
        offsets.append(position)
    if position > 0xFFFFFFFF:
        raise ValueError("compressed font exceeds the 32-bit resource range")
    return (struct.pack("<4sIII", MAGIC, len(data), block_size, len(chunks)) +
            struct.pack("<%dI" % len(offsets), *offsets) + b"".join(chunks))
