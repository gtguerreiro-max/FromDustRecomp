"""Decode captured mip banks using layouts supplied by Xenia's texture utilities."""
import struct
from pathlib import Path

from xenos_fetch import decode_texture_fetch
from xenos_texture import _BLOCK_BYTES, _BYTES_PER_TEXEL, _fix_endian, decode_texture
from xenos_tiling import tiled_2d


def load_mips(path, register_hash):
    data = Path(path).read_bytes()
    if len(data) < 16:
        raise ValueError("Truncated mip capture")
    magic, count, digest = struct.unpack_from("<IIQ", data)
    if magic != 0x31504D46 or digest != register_hash:
        raise ValueError("Mip capture belongs to a different GPU register stream")
    at, result = 16, {}
    for _ in range(count):
        if at + 36 > len(data):
            raise ValueError("Truncated mip record")
        base, *record = struct.unpack_from("<9I", data, at)
        fetch, levels, size = record[:6], record[6], record[7]
        at += 36
        if not 1 <= levels <= 15 or at + levels * 16 + size > len(data):
            raise ValueError("Invalid mip extent")
        layouts = [struct.unpack_from("<4I", data, at + mip * 16) for mip in range(levels)]
        at += levels * 16
        info = decode_texture_fetch(fetch)
        bank = _fix_endian(data[at:at + size], info["endianness"])
        at += size
        fmt = info["format_name"]
        if fmt in ("k_16_16", "k_16_16_16_16"):
            continue  # These table textures use float32 texels rather than RGBA8.
        compressed = fmt in _BLOCK_BYTES
        block = 4 if compressed else 1
        bpp = (_BLOCK_BYTES if compressed else _BYTES_PER_TEXEL)[fmt]
        output = []
        for mip, (pitch, offset, x, y) in enumerate(layouts, 1):
            width, height = max(1, info["width"] >> mip), max(1, info["height"] >> mip)
            cols, rows = (width + block - 1) // block, (height + block - 1) // block
            linear = bytearray(cols * rows * bpp)
            for by in range(rows):
                for bx in range(cols):
                    source = offset + (tiled_2d(x + bx, y + by, pitch // bpp, bpp.bit_length() - 1)
                                       if info["tiled"] else (y + by) * pitch + (x + bx) * bpp)
                    if source < 0 or source + bpp > len(bank):
                        raise ValueError("Mip texel outside captured bank")
                    target = (by * cols + bx) * bpp
                    linear[target:target + bpp] = bank[source:source + bpp]
            view = dict(info, width=width, height=height, pitch=width, tiled=False, endianness=0, packed_mips=False)
            output.append(decode_texture(linear, view))
        result[base] = output
    if at != len(data):
        raise ValueError("Unexpected trailing mip data")
    return result


def build_mip_table(mips, texture_index):
    records = []
    for base, levels in mips.items():
        if base not in texture_index:
            continue
        records.append(struct.pack("<2I", texture_index[base], len(levels)) +
                       b"".join(struct.pack("<2I", width, height) + rgba for width, height, rgba in levels))
    return struct.pack("<I", len(records)) + b"".join(records)
