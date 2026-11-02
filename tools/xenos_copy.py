_COPY_SAMPLE_SELECT_NAMES = {
    0: "k0",
    1: "k1",
    2: "k2",
    3: "k3",
    4: "k01",
    5: "k23",
    6: "k0123",
}

_COPY_COMMAND_NAMES = {
    0: "kRaw",
    1: "kConvert",
    2: "kConvertTo1111",
    3: "kNull",
}

_COLOR_FORMAT_NAMES = {
    2: "k_8",
    3: "k_1_5_5_5",
    4: "k_5_6_5",
    5: "k_6_5_5",
    6: "k_8_8_8_8",
    7: "k_2_10_10_10",
    8: "k_8_A",
    9: "k_8_B",
    10: "k_8_8",
    14: "k_8_8_8_8_A",
    15: "k_4_4_4_4",
    16: "k_10_11_11",
    17: "k_11_11_10",
    24: "k_16",
    25: "k_16_16",
    26: "k_16_16_16_16",
    30: "k_16_FLOAT",
    31: "k_16_16_FLOAT",
    32: "k_16_16_16_16_FLOAT",
    36: "k_32_FLOAT",
    37: "k_32_32_FLOAT",
    38: "k_32_32_32_32_FLOAT",
    50: "k_8_8_8_8_AS_16_16_16_16",
    54: "k_2_10_10_10_AS_16_16_16_16",
    55: "k_10_11_11_AS_16_16_16_16",
    56: "k_11_11_10_AS_16_16_16_16",
}

# Bytes per pixel used to turn a byte pitch into a width. Formats whose exact
# pitch contribution is not unambiguous are intentionally omitted.
_COLOR_FORMAT_BYTES = {
    "k_8": 1,
    "k_1_5_5_5": 2,
    "k_5_6_5": 2,
    "k_6_5_5": 2,
    "k_8_8_8_8": 4,
    "k_2_10_10_10": 4,
    "k_8_8": 2,
    "k_8_8_8_8_A": 4,
    "k_4_4_4_4": 2,
    "k_10_11_11": 4,
    "k_11_11_10": 4,
    "k_16": 2,
    "k_16_16": 4,
    "k_16_16_16_16": 8,
    "k_16_FLOAT": 2,
    "k_16_16_FLOAT": 4,
    "k_16_16_16_16_FLOAT": 8,
    "k_32_FLOAT": 4,
    "k_32_32_FLOAT": 8,
    "k_32_32_32_32_FLOAT": 16,
    "k_8_8_8_8_AS_16_16_16_16": 12,
    "k_2_10_10_10_AS_16_16_16_16": 12,
    "k_10_11_11_AS_16_16_16_16": 12,
    "k_11_11_10_AS_16_16_16_16": 12,
}


def _enum_value(names, raw):
    return names.get(raw, raw)


def _signed_value(value, shift, width):
    raw = (value >> shift) & ((1 << width) - 1)
    if raw & (1 << (width - 1)):
        raw -= (1 << width)
    return raw


def decode_copy_control(v):
    v = int(v) & 0xFFFFFFFF
    sample_raw = (v >> 4) & 0x7
    command_raw = (v >> 20) & 0x3

    return {
        "copy_src_select": (v >> 0) & 0x7,
        "_pad_3": (v >> 3) & 0x1,
        "copy_sample_select": _enum_value(_COPY_SAMPLE_SELECT_NAMES, sample_raw),
        "_pad_7": (v >> 7) & 0x1,
        "color_clear_enable": (v >> 8) & 0x1,
        "depth_clear_enable": (v >> 9) & 0x1,
        "_pad_10": (v >> 10) & 0x3FF,
        "copy_command": _enum_value(_COPY_COMMAND_NAMES, command_raw),
        "_pad_22": (v >> 22) & 0x3FF,
    }


def decode_copy_dest_info(v):
    v = int(v) & 0xFFFFFFFF
    format_raw = (v >> 7) & 0x3F

    return {
        "copy_dest_endian": (v >> 0) & 0x7,
        "copy_dest_array": (v >> 3) & 0x1,
        "copy_dest_slice": (v >> 4) & 0x7,
        "copy_dest_format": _enum_value(_COLOR_FORMAT_NAMES, format_raw),
        "copy_dest_number": (v >> 13) & 0x7,
        "copy_dest_exp_bias": _signed_value(v, 16, 6),
        "_pad_22": (v >> 22) & 0x3,
        "copy_dest_swap": (v >> 24) & 0x1,
        "_pad_25": (v >> 25) & 0x7F,
    }


def _source_label(copy_src_select):
    return {
        0: "color RT0",
        1: "color RT1",
        2: "color RT2",
        3: "color RT3",
        4: "depth",
    }.get(copy_src_select, f"src {copy_src_select}")


def _width_from_pitch(pitch, dest_format):
    bytes_per_pixel = _COLOR_FORMAT_BYTES.get(dest_format)
    if bytes_per_pixel and pitch % bytes_per_pixel == 0:
        return pitch // bytes_per_pixel
    return pitch


def describe_copy(control, dest_info, dest_base, dest_pitch):
    control = decode_copy_control(control)
    dest_info = decode_copy_dest_info(dest_info)

    dest_base = int(dest_base) & 0xFFFFFFFF
    dest_pitch = int(dest_pitch) & 0xFFFFFFFF

    pitch = dest_pitch & 0x3FFF
    height = (dest_pitch >> 16) & 0x3FFF
    width = _width_from_pitch(pitch, dest_info["copy_dest_format"])

    source = _source_label(control["copy_src_select"])
    sample = control["copy_sample_select"]
    sample_part = "" if sample == "k0" else f" {sample}"

    command = control["copy_command"]
    command_part = "" if command == "kRaw" else f" {command}"

    text = (
        f"copy {source}{sample_part} -> "
        f"0x{dest_base:08X} {dest_info['copy_dest_format']} "
        f"{width}x{height}{command_part}"
    )

    clears = []
    if control["color_clear_enable"]:
        clears.append("color")
    if control["depth_clear_enable"]:
        clears.append("depth")

    if clears:
        text += ", then clear " + " and ".join(clears)

    return text


if __name__ == "__main__":
    for v in (0x00000000, 0x00000014, 0x00000300):
        print(f"RB_COPY_CONTROL 0x{v:08X}: {decode_copy_control(v)}")

    for v in (0x00000000, 0x0003C000):
        print(f"RB_COPY_DEST_INFO 0x{v:08X}: {decode_copy_dest_info(v)}")
