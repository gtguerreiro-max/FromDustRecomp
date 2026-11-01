"""Decoder for Xenos vertex fetch (vfetch) instructions.

Bitfields are allocated from the least significant bit of each uint32, in
declaration order (MSVC/Clang). The provided Xenia struct has three 32-bit
Data member structs; no field spans a dword_0/1/2 boundary.
"""

_OPCODE_NAMES = {
    0: "kVertexFetch",
    1: "kTextureFetch",
    16: "kGetTextureBorderColorFrac",
    17: "kGetTextureComputedLod",
    18: "kGetTextureGradients",
    19: "kGetTextureWeights",
    24: "kSetTextureLod",
    25: "kSetTextureGradientsHorz",
    26: "kSetTextureGradientsVert",
}

_VERTEX_FORMAT_NAMES = {
    0: "kUndefined",
    6: "k_8_8_8_8",
    7: "k_2_10_10_10",
    16: "k_10_11_11",
    17: "k_11_11_10",
    25: "k_16_16",
    26: "k_16_16_16_16",
    31: "k_16_16_FLOAT",
    32: "k_16_16_16_16_FLOAT",
    33: "k_32",
    34: "k_32_32",
    35: "k_32_32_32_32",
    36: "k_32_FLOAT",
    37: "k_32_32_FLOAT",
    38: "k_32_32_32_32_FLOAT",
    57: "k_32_32_32_FLOAT",
}

_DEST_SWIZZLE_NAMES = ("x", "y", "z", "w", "0", "1", "unused", "keep")
_SRC_SWIZZLE_NAMES = ("x", "y", "z", "w")


def _opcode_name(value):
    return _OPCODE_NAMES.get(value, "unknown_%d" % value)


def _vertex_format_name(value):
    return _VERTEX_FORMAT_NAMES.get(value, "unknown_%d" % value)


def _sign_extend(value, bits):
    if bits <= 0:
        return 0
    sign_bit = 1 << (bits - 1)
    if value & sign_bit:
        return value - (sign_bit << 1)
    return value


def _dest_swizzle_string(swizzle):
    # 12 bits total: four 3-bit absolute component selectors.
    parts = []
    for component in range(4):
        value = (swizzle >> (3 * component)) & 0b111
        parts.append(_DEST_SWIZZLE_NAMES[value])
    return " ".join(parts)


def decode_vfetch(words):
    """Decode one Xenos fetch instruction encoded as three uint32 dwords.

    Args:
      words: sequence of three host integer dwords, in dword 0..2 order.

    Returns:
      dict with decoded fields.
    """
    words = tuple(words)
    if len(words) != 3:
        raise ValueError("decode_vfetch expects exactly 3 uint32 dwords")

    word0 = int(words[0]) & 0xFFFFFFFF
    word1 = int(words[1]) & 0xFFFFFFFF
    word2 = int(words[2]) & 0xFFFFFFFF

    # dword_0:
    #   bits  0- 4: opcode_value
    #   bits  5-10: src_reg
    #   bit  11: src_reg_am
    #   bits 12-17: dst_reg
    #   bit  18: dst_reg_am
    #   bit  19: must_be_one
    #   bits 20-24: const_index
    #   bits 25-26: const_index_sel
    #   bits 27-29: prefetch_count
    #   bits 30-31: src_swiz
    opcode_value = (word0 >> 0) & 0b11111
    src_reg = (word0 >> 5) & 0x3F
    dst_reg = (word0 >> 12) & 0x3F
    const_index = (word0 >> 20) & 0x1F
    const_index_sel = (word0 >> 25) & 0x3
    src_swizzle_raw = (word0 >> 30) & 0x3

    # dword_1:
    #   bits  0-11: dst_swiz
    #   bit  12: fomat_comp_all
    #   bit  13: num_format_all
    #   bit  14: signed_rf_mode_all
    #   bit  15: is_index_rounded
    #   bits 16-21: format
    #   bits 22-23: reserved2
    #   bits 24-29: exp_adjust
    #   bit  30: is_mini_fetch
    #   bit  31: is_predicated
    dst_swizzle_raw = word1 & 0x0FFF
    is_signed = ((word1 >> 12) & 0x1) == 1
    num_format = (word1 >> 13) & 0x1
    is_normalized = num_format == 0
    format_value = (word1 >> 16) & 0x3F
    exp_adjust = _sign_extend((word1 >> 24) & 0x3F, 6)
    is_mini_fetch = ((word1 >> 30) & 0x1) == 1
    predicated = ((word1 >> 31) & 0x1) == 1

    # dword_2:
    #   bits  0- 7: stride
    #   bits  8-30: offset
    #   bit  31: pred_condition
    stride = word2 & 0xFF
    offset = _sign_extend((word2 >> 8) & 0x7FFFFF, 23)

    is_vertex_fetch = opcode_value == 0
    if is_vertex_fetch:
        vertex_fetch_kind = "mini" if is_mini_fetch else "full"
    else:
        vertex_fetch_kind = "non_vertex_fetch"

    return {
        "opcode": opcode_value,
        "opcode_name": _opcode_name(opcode_value),
        "is_vertex_fetch": is_vertex_fetch,
        "vertex_fetch_kind": vertex_fetch_kind,
        "is_full_fetch": is_vertex_fetch and not is_mini_fetch,
        "is_mini_fetch": is_mini_fetch,
        "dest_reg": dst_reg,
        "dest_swizzle": _dest_swizzle_string(dst_swizzle_raw),
        "src_reg": src_reg,
        "src_swizzle": _SRC_SWIZZLE_NAMES[src_swizzle_raw],
        "const_index": const_index,
        "const_index_sel": const_index_sel,
        "fetch_constant_index": const_index * 3 + const_index_sel,
        "format": format_value,
        "format_name": _vertex_format_name(format_value),
        "is_signed": is_signed,
        "signed": is_signed,
        "is_normalized": is_normalized,
        "num_format": num_format,
        "exp_adjust": exp_adjust,
        "offset": offset,
        "stride": stride,
        "predicated": predicated,
    }


def vertex_layout(instructions):
    """Group decoded vfetch instructions by vertex-fetch stream.

    Args:
      instructions: list of dicts returned by decode_vfetch.

    Returns:
      dict mapping (const_index, const_index_sel) to:
        {
          "stride": stride in dwords from the first full fetch for that stream,
          "elements": sorted list of (offset, format_name, dest_reg) tuples
        }
    """
    elements_by_stream = {}
    stride_by_stream = {}
    streams_with_full = set()

    for instr in instructions:
        if not instr.get("is_vertex_fetch", True):
            continue

        key = (
            int(instr.get("const_index", 0)),
            int(instr.get("const_index_sel", 0)),
        )

        offset = int(instr.get("offset", 0))

        format_name = instr.get("format_name")
        if format_name is None:
            format_name = _vertex_format_name(int(instr.get("format", 0)))
        else:
            format_name = str(format_name)

        dest_reg = int(instr.get("dest_reg", instr.get("dest", 0)))
        elements_by_stream.setdefault(key, []).append(
            (offset, format_name, dest_reg)
        )

        if not instr.get("is_mini_fetch", False):
            if key not in streams_with_full:
                stride_by_stream[key] = int(instr.get("stride", 0))
                streams_with_full.add(key)

    result = {}
    for key, elements in elements_by_stream.items():
        result[key] = {
            "stride": stride_by_stream.get(key, 0),
            "elements": sorted(elements, key=lambda item: (item[0], item[1], item[2])),
        }
    return result


if __name__ == "__main__":
    sample = (0x19003780, 0x0D501001, 0x0000000C)
    print(decode_vfetch(sample))
