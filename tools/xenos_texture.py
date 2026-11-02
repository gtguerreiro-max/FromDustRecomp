"""Decode Xenos/Xbox 360 texture memory to RGBA8.

Only the Python standard library is used.  Tiled textures require the
companion module ``xenos_tiling`` with:

    untile_2d(data, width_blocks, height_blocks, pitch_blocks,
              bytes_per_block) -> bytes
"""

import struct
import zlib

_BLOCK_BYTES = {
    "k_DXT1": 8,
    "k_DXT2_3": 16,
    "k_DXT4_5": 16,
    "k_DXN": 16,
}

_BYTES_PER_TEXEL = {
    "k_8_8_8_8": 4,
    "k_16_16": 4,
    "k_8": 1,
    "k_5_6_5": 2,
    "k_8_8": 2,
    "k_16_16_16_16": 8,
    "k_16_16_FLOAT": 4,
    "k_16_16_16_16_FLOAT": 8,
    "k_32_FLOAT": 4,
    "k_2_10_10_10": 4,
    "k_10_11_11": 4,
    "k_11_11_10": 4,
    "k_24_8": 4,
    "k_24_8_FLOAT": 4,
    "k_8_8_8_8_A": 4,
}


def _fix_endian(data, endian):
    """Undo Xenos endianness swaps.

    endian:
        0: none
        1: 8-bit elements in 16-bit words  (swap bytes in each 16-bit word)
        2: 8-bit elements in 32-bit words  (reverse bytes in each 32-bit word)
        3: 16-bit elements in 32-bit words (swap 16-bit halves)
    """
    b = bytearray(data)
    n = len(b)

    if endian == 1:
        for i in range(0, n - 1, 2):
            b[i], b[i + 1] = b[i + 1], b[i]
    elif endian == 2:
        for i in range(0, n - 3, 4):
            b[i], b[i + 1], b[i + 2], b[i + 3] = (
                b[i + 3],
                b[i + 2],
                b[i + 1],
                b[i],
            )
    elif endian == 3:
        for i in range(0, n - 3, 4):
            b[i], b[i + 2] = b[i + 2], b[i]
            b[i + 1], b[i + 3] = b[i + 3], b[i + 1]

    return bytes(b)


def _expand_565(value):
    """Expand a 16-bit 565 value to 8-bit RGB."""
    r5 = (value >> 11) & 0x1F
    g6 = (value >> 5) & 0x3F
    b5 = value & 0x1F
    return (
        (r5 << 3) | (r5 >> 2),
        (g6 << 2) | (g6 >> 4),
        (b5 << 3) | (b5 >> 2),
    )


def _fields_to_rgb(r5, g6, b5):
    return (
        (r5 << 3) | (r5 >> 2),
        (g6 << 2) | (g6 >> 4),
        (b5 << 3) | (b5 >> 2),
    )


def _unorm16_to_8(value):
    """Convert an unsigned 16-bit normalised value to 8 bits."""
    return (value * 255 + 0x8000) >> 16


def _unorm_n_to_8(value, bits):
    """Convert an unsigned n-bit normalised value to 8 bits."""
    max_value = (1 << bits) - 1
    if max_value == 0:
        return 0
    return (value * 255 + max_value // 2) // max_value


def _half_to_float(value):
    """Decode an IEEE 754 half float to a Python float."""
    sign = value >> 15
    exp = (value >> 10) & 0x1F
    mant = value & 0x3FF

    if exp == 0x1F:
        if mant == 0:
            return float("inf") if sign == 0 else float("-inf")
        return float("nan")

    if exp == 0:
        v = float(mant) * 2.0 ** -24
    else:
        v = float(1.0 + mant / 1024.0) * 2.0 ** (exp - 15)

    return -v if sign else v


def _clamp01(value):
    """Clamp a float to 0.0-1.0, treating NaN as 0.0."""
    if value != value:
        return 0.0
    if value <= 0.0:
        return 0.0
    if value >= 1.0:
        return 1.0
    return value


def _clamp01_to_8(value):
    """Clamp a float to 0.0-1.0 and scale it to 0-255."""
    return int(_clamp01(value) * 255.0 + 0.5)


def _float16_to_8(value):
    """Convert a half float to 8 bits with 0-1 clamping."""
    return _clamp01_to_8(_half_to_float(value))


def _float32_to_8(value_bytes):
    """Convert a 32-bit float to 8 bits with 0-1 clamping."""
    value = struct.unpack("<f", value_bytes)[0]
    return _clamp01_to_8(value)


def _float20e4_to_float(value24):
    """Decode a Xenos 20e4 float stored in 24 bits to a Python float."""
    e = (value24 >> 20) & 0xF
    m = value24 & 0xFFFFF

    if e == 0:
        return float(m) * 2.0 ** -34
    return float(1.0 + m / 1048576.0) * 2.0 ** (e - 15)


def _put_pixel(rgba, width, height, x, y, r, g, b, a):
    if 0 <= x < width and 0 <= y < height:
        i = (y * width + x) * 4
        rgba[i] = r & 0xFF
        rgba[i + 1] = g & 0xFF
        rgba[i + 2] = b & 0xFF
        rgba[i + 3] = a & 0xFF


def _dxt1_rgb_block(block):
    """Decode the 8-byte DXT1/BC1 color part to 16 RGB triples."""
    if len(block) < 8:
        block = block + b"\x00" * (8 - len(block))

    min16 = int.from_bytes(block[0:2], "little")
    max16 = int.from_bytes(block[2:4], "little")
    index_word = int.from_bytes(block[4:8], "little")

    min_r5 = (min16 >> 11) & 0x1F
    min_g6 = (min16 >> 5) & 0x3F
    min_b5 = min16 & 0x1F

    max_r5 = (max16 >> 11) & 0x1F
    max_g6 = (max16 >> 5) & 0x3F
    max_b5 = max16 & 0x1F

    c_min = _fields_to_rgb(min_r5, min_g6, min_b5)
    c_max = _fields_to_rgb(max_r5, max_g6, max_b5)

    if min16 == max16:
        colors = [
            c_min,
            (0, 0, 0),
            c_max,
            (0, 0, 0),
        ]
    else:
        c1 = _fields_to_rgb(
            (2 * min_r5 + max_r5) // 3,
            (2 * min_g6 + max_g6) // 3,
            (2 * min_b5 + max_b5) // 3,
        )
        c2 = _fields_to_rgb(
            (min_r5 + 2 * max_r5) // 3,
            (min_g6 + 2 * max_g6) // 3,
            (min_b5 + 2 * max_b5) // 3,
        )
        colors = [c_min, c1, c2, c_max]

    out = []
    for i in range(16):
        out.append(colors[(index_word >> (2 * i)) & 0x3])
    return out


def _decode_block(fmt, block, x0, y0, width, height, rgba):
    need = _BLOCK_BYTES[fmt]
    if len(block) < need:
        block = block + b"\x00" * (need - len(block))

    if fmt == "k_DXT1":
        rgb = _dxt1_rgb_block(block)
        min16 = int.from_bytes(block[0:2], "little")
        max16 = int.from_bytes(block[2:4], "little")
        index_word = int.from_bytes(block[4:8], "little")
        equal = min16 == max16

        for p, (r, g, b) in enumerate(rgb):
            idx = (index_word >> (2 * p)) & 0x3
            a = 0 if (equal and idx in (1, 3)) else 255
            _put_pixel(
                rgba,
                width,
                height,
                x0 + (p % 4),
                y0 + (p // 4),
                r,
                g,
                b,
                a,
            )

    elif fmt == "k_DXT2_3":
        # BC2: 8 bytes of 4-bit alpha first, then a DXT1 colour block.
        rgb = _dxt1_rgb_block(block[8:16])
        alpha = int.from_bytes(block[0:8], "little")
        for p, (r, g, b) in enumerate(rgb):
            _put_pixel(rgba, width, height, x0 + (p % 4), y0 + (p // 4), r, g, b, ((alpha >> (4 * p)) & 0xF) * 17)

    elif fmt == "k_DXT4_5":
        # BC3: alpha endpoints a0, a1 and 16 3-bit indices first, then a DXT1 colour block.
        rgb = _dxt1_rgb_block(block[8:16])
        a0, a1 = block[0], block[1]
        if a0 > a1:
            palette = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
        else:
            palette = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
        bits = int.from_bytes(block[2:8], "little")
        for p, (r, g, b) in enumerate(rgb):
            _put_pixel(rgba, width, height, x0 + (p % 4), y0 + (p // 4), r, g, b, palette[(bits >> (3 * p)) & 7])

    elif fmt == "k_DXN":
        # Two 4-bit channels, stored as two 8-byte planes: R then G.
        r_plane = block[0:8]
        g_plane = block[8:16]

        for p in range(16):
            r_nibble = (r_plane[p >> 1] >> ((p & 1) * 4)) & 0xF
            g_nibble = (g_plane[p >> 1] >> ((p & 1) * 4)) & 0xF
            _put_pixel(
                rgba,
                width,
                height,
                x0 + (p % 4),
                y0 + (p // 4),
                r_nibble * 17,
                g_nibble * 17,
                0,
                255,
            )


def _decode_pixel(fmt, px, x, y, width, height, rgba):
    bpt = _BYTES_PER_TEXEL[fmt]
    if len(px) < bpt:
        px = px + b"\x00" * (bpt - len(px))

    if fmt == "k_8_8_8_8":
        # After the endian swap the texel is a little-endian dword with X in
        # bits 0-7 ... W in bits 24-31 (Xenia's k_8_8_8_8): bytes X Y Z W.
        r, g, b, a = px[0], px[1], px[2], px[3]
        _put_pixel(rgba, width, height, x, y, r, g, b, a)

    elif fmt == "k_16_16":
        v0 = int.from_bytes(px[0:2], "little")
        v1 = int.from_bytes(px[2:4], "little")
        _put_pixel(
            rgba,
            width,
            height,
            x,
            y,
            _unorm16_to_8(v0),
            _unorm16_to_8(v1),
            0,
            255,
        )

    elif fmt == "k_8":
        v = px[0]
        _put_pixel(rgba, width, height, x, y, v, v, v, 255)

    elif fmt == "k_5_6_5":
        v = int.from_bytes(px[0:2], "little")
        r, g, b = _expand_565(v)
        _put_pixel(rgba, width, height, x, y, r, g, b, 255)

    elif fmt == "k_8_8":
        r, g = px[0], px[1]
        _put_pixel(rgba, width, height, x, y, r, g, 0, 255)

    elif fmt == "k_16_16_16_16":
        v0 = int.from_bytes(px[0:2], "little")
        v1 = int.from_bytes(px[2:4], "little")
        v2 = int.from_bytes(px[4:6], "little")
        v3 = int.from_bytes(px[6:8], "little")
        _put_pixel(
            rgba,
            width,
            height,
            x,
            y,
            _unorm16_to_8(v0),
            _unorm16_to_8(v1),
            _unorm16_to_8(v2),
            _unorm16_to_8(v3),
        )

    elif fmt == "k_16_16_FLOAT":
        h0 = int.from_bytes(px[0:2], "little")
        h1 = int.from_bytes(px[2:4], "little")
        _put_pixel(rgba, width, height, x, y, _float16_to_8(h0), _float16_to_8(h1), 0, 255)

    elif fmt == "k_16_16_16_16_FLOAT":
        h0 = int.from_bytes(px[0:2], "little")
        h1 = int.from_bytes(px[2:4], "little")
        h2 = int.from_bytes(px[4:6], "little")
        h3 = int.from_bytes(px[6:8], "little")
        _put_pixel(
            rgba,
            width,
            height,
            x,
            y,
            _float16_to_8(h0),
            _float16_to_8(h1),
            _float16_to_8(h2),
            _float16_to_8(h3),
        )

    elif fmt == "k_32_FLOAT":
        v = _float32_to_8(px[0:4])
        _put_pixel(rgba, width, height, x, y, v, v, v, 255)

    elif fmt == "k_2_10_10_10":
        v = int.from_bytes(px[0:4], "little")
        r = _unorm_n_to_8(v & 0x3FF, 10)
        g = _unorm_n_to_8((v >> 10) & 0x3FF, 10)
        b = _unorm_n_to_8((v >> 20) & 0x3FF, 10)
        a = _unorm_n_to_8((v >> 30) & 0x3, 2)
        _put_pixel(rgba, width, height, x, y, r, g, b, a)

    elif fmt == "k_10_11_11":
        v = int.from_bytes(px[0:4], "little")
        r = _unorm_n_to_8(v & 0x3FF, 10)
        g = _unorm_n_to_8((v >> 10) & 0x7FF, 11)
        b = _unorm_n_to_8((v >> 21) & 0x7FF, 11)
        _put_pixel(rgba, width, height, x, y, r, g, b, 255)

    elif fmt == "k_11_11_10":
        v = int.from_bytes(px[0:4], "little")
        r = _unorm_n_to_8(v & 0x7FF, 11)
        g = _unorm_n_to_8((v >> 11) & 0x7FF, 11)
        b = _unorm_n_to_8((v >> 22) & 0x3FF, 10)
        _put_pixel(rgba, width, height, x, y, r, g, b, 255)

    elif fmt == "k_24_8":
        v = int.from_bytes(px[0:4], "little")
        depth = (v >> 8) & 0xFFFFFF
        grey = _unorm_n_to_8(depth, 24)
        _put_pixel(rgba, width, height, x, y, grey, grey, grey, 255)

    elif fmt == "k_24_8_FLOAT":
        v = int.from_bytes(px[0:4], "little")
        depth = (v >> 8) & 0xFFFFFF
        grey = _clamp01_to_8(_float20e4_to_float(depth))
        _put_pixel(rgba, width, height, x, y, grey, grey, grey, 255)

    elif fmt == "k_8_8_8_8_A":
        # After endianness correction: R8 G8 B8 A8.
        r, g, b, a = px[0], px[1], px[2], px[3]
        _put_pixel(rgba, width, height, x, y, r, g, b, a)


def _choose_stride_blocks(length, height_blocks, width_blocks, pitch_blocks, bytes_per_block):
    pitch_len = height_blocks * pitch_blocks * bytes_per_block
    tight_len = height_blocks * width_blocks * bytes_per_block

    if length == tight_len and tight_len != pitch_len:
        return width_blocks
    if length >= pitch_len:
        return pitch_blocks
    if length >= tight_len:
        return width_blocks
    return width_blocks


def _choose_stride_texels(length, height, width, pitch, bytes_per_texel):
    pitch_len = height * pitch * bytes_per_texel
    tight_len = height * width * bytes_per_texel

    if length == tight_len and tight_len != pitch_len:
        return width
    if length >= pitch_len:
        return pitch
    if length >= tight_len:
        return width
    return width


def decode_texture(data, fetch):
    """Decode mip level 0 to RGBA8.

    Returns:
        (width, height, rgba_bytes)
    """
    width = int(fetch.get("width", 0))
    height = int(fetch.get("height", 0))
    pitch = int(fetch.get("pitch", width) or width)
    tiled = bool(fetch.get("tiled", 0))
    endian = int(fetch.get("endianness", 0) or 0)
    fmt = str(fetch.get("format_name") or fetch.get("format") or "").strip()

    if width <= 0 or height <= 0:
        return width, height, b""

    if fmt not in _BLOCK_BYTES and fmt not in _BYTES_PER_TEXEL:
        raise ValueError("Unsupported Xenos format: %r" % fmt)

    raw = _fix_endian(bytes(data), endian)
    is_block = fmt in _BLOCK_BYTES

    if tiled:
        from xenos_tiling import untile_2d

        if is_block:
            width_blocks = (width + 3) // 4
            height_blocks = (height + 3) // 4
            pitch_blocks = max(pitch // 4, width_blocks)
            bytes_per_block = _BLOCK_BYTES[fmt]
        else:
            width_blocks = width
            height_blocks = height
            pitch_blocks = max(pitch, width)
            bytes_per_block = _BYTES_PER_TEXEL[fmt]

        expected = height_blocks * pitch_blocks * bytes_per_block
        if len(raw) < expected:
            raw += b"\x00" * (expected - len(raw))

        raw = bytes(untile_2d(raw[:expected], width_blocks, height_blocks, pitch_blocks, bytes_per_block))

    rgba = bytearray(width * height * 4)

    if is_block:
        bytes_per_block = _BLOCK_BYTES[fmt]
        width_blocks = (width + 3) // 4
        height_blocks = (height + 3) // 4
        pitch_blocks = max(pitch // 4, width_blocks)
        stride_blocks = _choose_stride_blocks(
            len(raw),
            height_blocks,
            width_blocks,
            pitch_blocks,
            bytes_per_block,
        )

        for by in range(height_blocks):
            for bx in range(width_blocks):
                offset = (by * stride_blocks + bx) * bytes_per_block
                _decode_block(
                    fmt,
                    raw[offset:offset + bytes_per_block],
                    bx * 4,
                    by * 4,
                    width,
                    height,
                    rgba,
                )
    else:
        bytes_per_texel = _BYTES_PER_TEXEL[fmt]
        stride_texels = _choose_stride_texels(
            len(raw),
            height,
            width,
            max(pitch, width),
            bytes_per_texel,
        )

        for y in range(height):
            base = y * stride_texels * bytes_per_texel
            for x in range(width):
                offset = base + x * bytes_per_texel
                _decode_pixel(
                    fmt,
                    raw[offset:offset + bytes_per_texel],
                    x,
                    y,
                    width,
                    height,
                    rgba,
                )

    return width, height, bytes(rgba)


def write_png(path, width, height, rgba):
    """Write RGBA8 pixel data as a minimal PNG file."""
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")

    expected = width * height * 4
    if len(rgba) < expected:
        raise ValueError("rgba buffer too small")

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # PNG filter type 0 (None)
        raw.extend(rgba[y * stride:(y + 1) * stride])

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw))))
        f.write(chunk(b"IEND", b""))


def _pack_565(r, g, b):
    r5 = (int(r) * 31 + 127) // 255
    g6 = (int(g) * 63 + 31) // 255
    b5 = (int(b) * 31 + 127) // 255
    return (r5 << 11) | (g6 << 5) | b5


def _self_test():
    width = 8
    height = 8

    red = (255, 0, 0)
    green = (0, 255, 0)

    red565 = _pack_565(*red)
    green565 = _pack_565(*green)

    # Build an untiled 8x8 DXT1 texture:
    # left half red, right half green.
    blocks = []
    for _by in range(2):
        for bx in range(2):
            value = red565 if bx == 0 else green565
            block = (
                value.to_bytes(2, "little")
                + value.to_bytes(2, "little")
                + (0).to_bytes(4, "little")
            )
            blocks.append(block)

    data = b"".join(blocks)

    fetch = {
        "format_name": "k_DXT1",
        "width": width,
        "height": height,
        "pitch": width,
        "tiled": 0,
        "endianness": 0,
    }

    w, h, rgba = decode_texture(data, fetch)

    def pixel(x, y):
        i = (y * w + x) * 4
        return tuple(rgba[i:i + 4])

    assert w == 8
    assert h == 8
    assert pixel(0, 0) == (255, 0, 0, 255)
    assert pixel(3, 0) == (255, 0, 0, 255)
    assert pixel(4, 0) == (0, 255, 0, 255)
    assert pixel(7, 7) == (0, 255, 0, 255)

    for y in range(height):
        for x in range(width):
            expected = red + (255,) if x < 4 else green + (255,)
            assert pixel(x, y) == expected, (x, y, pixel(x, y), expected)

    # Single-pixel checks for the added non-block formats.
    cases = [
        (
            "k_8_8",
            bytes([255, 0]),
            (255, 0, 0, 255),
        ),
        (
            "k_16_16_16_16",
            (65535).to_bytes(2, "little")
            + (0).to_bytes(2, "little")
            + (0).to_bytes(2, "little")
            + (65535).to_bytes(2, "little"),
            (255, 0, 0, 255),
        ),
        (
            "k_16_16_FLOAT",
            (0x3C00).to_bytes(2, "little") + (0x3800).to_bytes(2, "little"),
            (255, 128, 0, 255),
        ),
        (
            "k_16_16_16_16_FLOAT",
            (0x3C00).to_bytes(2, "little")
            + (0x3800).to_bytes(2, "little")
            + (0).to_bytes(2, "little")
            + (0x3C00).to_bytes(2, "little"),
            (255, 128, 0, 255),
        ),
        (
            "k_32_FLOAT",
            struct.pack("<f", 0.5),
            (128, 128, 128, 255),
        ),
        (
            "k_2_10_10_10",
            (0xC00003FF).to_bytes(4, "little"),
            (255, 0, 0, 255),
        ),
        (
            "k_10_11_11",
            (0x001FFFFF).to_bytes(4, "little"),
            (255, 255, 0, 255),
        ),
        (
            "k_11_11_10",
            (0xFFC007FF).to_bytes(4, "little"),
            (255, 0, 255, 255),
        ),
        (
            "k_24_8",
            (0xFFFFFF00).to_bytes(4, "little"),
            (255, 255, 255, 255),
        ),
        (
            "k_24_8_FLOAT",
            (0xE0000000).to_bytes(4, "little"),
            (128, 128, 128, 255),
        ),
        (
            "k_8_8_8_8_A",
            bytes([255, 0, 0, 255]),
            (255, 0, 0, 255),
        ),
    ]

    for fmt, raw, expected in cases:
        w, h, rgba = decode_texture(
            raw,
            {
                "format_name": fmt,
                "width": 1,
                "height": 1,
                "pitch": 1,
                "tiled": 0,
                "endianness": 0,
            },
        )
        assert w == 1
        assert h == 1
        assert tuple(rgba[0:4]) == expected, (fmt, tuple(rgba[0:4]), expected)

    print("xenos_texture self-test ok")


if __name__ == "__main__":
    _self_test()
