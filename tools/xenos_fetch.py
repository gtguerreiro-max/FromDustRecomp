# -*- coding: utf-8 -*-
"""Decoder for Xbox 360 / Xenos texture and vertex fetch constants.

Bitfields are decoded from the least significant bit of each uint32, in
declaration order, matching MSVC/Clang behavior (Xenia's xenos.h unions).
Written by the local Qwen model; dwords 3-5 corrected against xenos.h.
"""

_TEXTURE_FORMAT_NAMES = {
    0: "k_1_REVERSE",
    1: "k_1",
    2: "k_8",
    3: "k_1_5_5_5",
    4: "k_5_6_5",
    5: "k_6_5_5",
    6: "k_8_8_8_8",
    7: "k_2_10_10_10",
    8: "k_8_A",
    9: "k_8_B",
    10: "k_8_8",
    11: "k_Cr_Y1_Cb_Y0_REP",
    12: "k_Y1_Cr_Y0_Cb_REP",
    13: "k_16_16_EDRAM",
    14: "k_8_8_8_8_A",
    15: "k_4_4_4_4",
    16: "k_10_11_11",
    17: "k_11_11_10",
    18: "k_DXT1",
    19: "k_DXT2_3",
    20: "k_DXT4_5",
    21: "k_16_16_16_16_EDRAM",
    22: "k_24_8",
    23: "k_24_8_FLOAT",
    24: "k_16",
    25: "k_16_16",
    26: "k_16_16_16_16",
    27: "k_16_EXPAND",
    28: "k_16_16_EXPAND",
    29: "k_16_16_16_16_EXPAND",
    30: "k_16_FLOAT",
    31: "k_16_16_FLOAT",
    32: "k_16_16_16_16_FLOAT",
    33: "k_32",
    34: "k_32_32",
    35: "k_32_32_32_32",
    36: "k_32_FLOAT",
    37: "k_32_32_FLOAT",
    38: "k_32_32_32_32_FLOAT",
    39: "k_32_AS_8",
    40: "k_32_AS_8_8",
    41: "k_16_MPEG",
    42: "k_16_16_MPEG",
    43: "k_8_INTERLACED",
    44: "k_32_AS_8_INTERLACED",
    45: "k_32_AS_8_8_INTERLACED",
    46: "k_16_INTERLACED",
    47: "k_16_MPEG_INTERLACED",
    48: "k_16_16_MPEG_INTERLACED",
    49: "k_DXN",
}

_DATA_DIMENSION_NAMES = {
    0: "k1D",
    1: "k2DOrStacked",
    2: "k3D",
    3: "kCube",
}


def _bits(word, start, count):
    """Extract `count` bits starting at bit `start` from a uint32 value."""
    if count <= 0:
        return 0
    return (word >> start) & ((1 << count) - 1)


def _validate_words(words, expected_count, label):
    if len(words) != expected_count:
        raise ValueError(
            f"{label} fetch constant must have {expected_count} dwords, got {len(words)}"
        )

    out = []
    for i, word in enumerate(words):
        if isinstance(word, bool) or not isinstance(word, int):
            raise ValueError(f"{label} dword {i} must be an int")
        if word < 0 or word > 0xFFFFFFFF:
            raise ValueError(f"{label} dword {i} is not a uint32")
        out.append(word)
    return tuple(out)


def decode_texture_fetch(words):
    """Decode a 6-dword Xenos texture fetch constant."""
    d0, d1, d2, d3, d4, d5 = _validate_words(words, 6, "texture")

    type_value = _bits(d0, 0, 2)

    sign_x = _bits(d0, 2, 2)
    sign_y = _bits(d0, 4, 2)
    sign_z = _bits(d0, 6, 2)
    sign_w = _bits(d0, 8, 2)

    clamp_x = _bits(d0, 10, 3)
    clamp_y = _bits(d0, 13, 3)
    clamp_z = _bits(d0, 16, 3)

    pitch_field = _bits(d0, 22, 9)
    tiled = _bits(d0, 31, 1)

    format_value = _bits(d1, 0, 6)
    endianness = _bits(d1, 6, 2)
    request_size = _bits(d1, 8, 2)
    stacked = _bits(d1, 10, 1)
    nearest_clamp_policy = _bits(d1, 11, 1)
    base_address_page = _bits(d1, 12, 20)

    num_format = _bits(d3, 0, 1)
    swizzle_x = _bits(d3, 1, 3)
    swizzle_y = _bits(d3, 4, 3)
    swizzle_z = _bits(d3, 7, 3)
    swizzle_w = _bits(d3, 10, 3)
    mag_filter = _bits(d3, 19, 2)
    min_filter = _bits(d3, 21, 2)
    mip_filter = _bits(d3, 23, 2)
    aniso_filter = _bits(d3, 25, 3)

    mip_min_level = _bits(d4, 2, 4)
    mip_max_level = _bits(d4, 6, 4)

    dimension_value = _bits(d5, 9, 2)
    packed_mips = _bits(d5, 11, 1)
    mip_address_page = _bits(d5, 12, 20)

    base_address = base_address_page << 12
    mip_address = mip_address_page << 12

    # The struct stores the pitch in texels shifted right by 5.
    pitch = pitch_field << 5

    width = 1
    height = 1
    depth = 1

    if dimension_value == 0:
        # 1D
        width = _bits(d2, 0, 24) + 1
        height = 1
        depth = 1
    elif dimension_value == 1:
        # 2D or stacked 2D
        width = _bits(d2, 0, 13) + 1
        height = _bits(d2, 13, 13) + 1
        stack_depth = _bits(d2, 26, 6)
        depth = stack_depth + 1 if stacked else 1
    elif dimension_value == 2:
        # 3D
        width = _bits(d2, 0, 11) + 1
        height = _bits(d2, 11, 11) + 1
        depth = _bits(d2, 22, 10) + 1
    else:
        # Cube: uses the 2D/stacked size union.  A non-stacked cube is
        # expected to have stack_depth == 5, i.e. 6 faces.
        width = _bits(d2, 0, 13) + 1
        height = _bits(d2, 13, 13) + 1
        stack_depth = _bits(d2, 26, 6)
        depth = stack_depth + 1 if stacked else 6

    return {
        "type": type_value,
        "format": format_value,
        "format_name": _TEXTURE_FORMAT_NAMES.get(
            format_value, f"unknown_format_{format_value}"
        ),
        "endianness": endianness,
        "base_address": base_address,
        "mip_address": mip_address,
        "width": width,
        "height": height,
        "depth": depth,
        "dimension": dimension_value,
        "dimension_name": _DATA_DIMENSION_NAMES.get(
            dimension_value, f"unknown_dimension_{dimension_value}"
        ),
        "pitch": pitch,
        "pitch_field": pitch_field,
        "pitch_units": "texels",
        "tiled": tiled,
        "signs": [sign_x, sign_y, sign_z, sign_w],
        "swizzle": [swizzle_x, swizzle_y, swizzle_z, swizzle_w],
        "mip_min_level": mip_min_level,
        "mip_max_level": mip_max_level,
        "packed_mips": packed_mips,
        "clamp_x": clamp_x,
        "clamp_y": clamp_y,
        "clamp_z": clamp_z,
        "stacked": stacked,
        "num_format": num_format,
        "filters": {"mag": mag_filter, "min": min_filter, "mip": mip_filter, "aniso": aniso_filter},
        "request_size": request_size,
        "nearest_clamp_policy": nearest_clamp_policy,
    }


def decode_vertex_fetch(words):
    """Decode a 2-dword Xenos vertex fetch constant."""
    d0, d1 = _validate_words(words, 2, "vertex")

    type_value = _bits(d0, 0, 2)
    address_field = _bits(d0, 2, 30)
    endian = _bits(d1, 0, 2)
    size = _bits(d1, 2, 24)

    return {
        "type": type_value,
        "address": address_field << 2,
        "size": size,
        "endian": endian,
    }


if __name__ == "__main__":
    print(
        decode_texture_fetch(
            [
                0x04000156,
                0xAA9D4099,
                0x003FE1FF,
                0x00001690,
                0x00000000,
                0x00000200,
            ]
        )
    )
    print(decode_vertex_fetch([0x0000154B, 0x00000003]))
