"""Decode Xenos RB render-target register values.

Bitfields are extracted from the least significant bit in declaration order,
matching the MSVC/Clang bitfield layout for the provided C++ unions.
"""

__all__ = [
    "decode_surface_info",
    "decode_color_info",
    "decode_depth_info",
    "render_target_size",
]

_MSAA_NAMES = {
    0: "MsaaSamples.k1X",
    1: "MsaaSamples.k2X",
    2: "MsaaSamples.k4X",
}
_MSAA_COUNTS = {0: 1, 1: 2, 2: 4}

_COLOR_FORMAT_NAMES = {
    0: "ColorRenderTargetFormat.k_8_8_8_8",
    1: "ColorRenderTargetFormat.k_8_8_8_8_GAMMA",
    2: "ColorRenderTargetFormat.k_2_10_10_10",
    3: "ColorRenderTargetFormat.k_2_10_10_10_FLOAT",
    4: "ColorRenderTargetFormat.k_16_16",
    5: "ColorRenderTargetFormat.k_16_16_16_16",
    6: "ColorRenderTargetFormat.k_16_16_FLOAT",
    7: "ColorRenderTargetFormat.k_16_16_16_16_FLOAT",
    10: "ColorRenderTargetFormat.k_2_10_10_10_AS_10_10_10_10",
    12: "ColorRenderTargetFormat.k_2_10_10_10_FLOAT_AS_16_16_16_16",
    14: "ColorRenderTargetFormat.k_32_FLOAT",
    15: "ColorRenderTargetFormat.k_32_32_FLOAT",
}

_DEPTH_FORMAT_NAMES = {
    0: "DepthRenderTargetFormat.kD24S8",
    1: "DepthRenderTargetFormat.kD24FS8",
}


def _u32(value):
    return int(value) & 0xFFFFFFFF


def _field(value, shift, width):
    return (value >> shift) & ((1 << width) - 1)


def _signed_field(value, shift, width):
    field = _field(value, shift, width)
    sign_bit = 1 << (width - 1)
    if field & sign_bit:
        field -= 1 << width
    return field


def _enum_name(prefix, names, value):
    return names.get(value, f"{prefix}.{value}")


def decode_surface_info(value):
    v = _u32(value)
    msaa_samples = _field(v, 16, 2)
    return {
        "surface_pitch": _field(v, 0, 14),
        "_pad_14": _field(v, 14, 2),
        "msaa_samples": msaa_samples,
        "msaa_samples_name": _enum_name("MsaaSamples", _MSAA_NAMES, msaa_samples),
        "hiz_pitch": _field(v, 18, 14),
    }


def decode_color_info(value):
    v = _u32(value)
    color_format = _field(v, 16, 4)
    return {
        "color_base": _field(v, 0, 13),
        "_pad_13": _field(v, 13, 3),
        "color_format": color_format,
        "color_format_name": _enum_name(
            "ColorRenderTargetFormat", _COLOR_FORMAT_NAMES, color_format
        ),
        "color_exp_bias": _signed_field(v, 20, 6),
        "_pad_26": _field(v, 26, 6),
    }


def decode_depth_info(value):
    v = _u32(value)
    depth_format = _field(v, 16, 1)
    return {
        "depth_base": _field(v, 0, 13),
        "_pad_13": _field(v, 13, 3),
        "depth_format": depth_format,
        "depth_format_name": _enum_name(
            "DepthRenderTargetFormat", _DEPTH_FORMAT_NAMES, depth_format
        ),
        "_pad_17": _field(v, 17, 15),
    }


def render_target_size(surface_info):
    """
    Return (surface_pitch_pixels, msaa_sample_count).

    surface_pitch_pixels is the raw RB_SURFACE_INFO.surface_pitch field. Its
    C++ comment says only that it is "+0 in pixels". msaa_sample_count is the
    sample count represented by the MsaaSamples field: 1 for k1X, 2 for k2X,
    and 4 for k4X.

    For 1x MSAA this pitch is the rendered width in pixels. For 2x and 4x,
    Xenos EDRAM stores MSAA samples in wider/taller tiles, so this pitch is
    the pitch of the expanded sample surface rather than the logical rendered
    width. In that layout the 2x sample surface is twice as wide, and the 4x
    sample surface is twice as wide and twice as tall, so the logical rendered
    width is half of surface_pitch_pixels for both 2x and 4x. The register
    comment itself does not encode that scale factor; it only states that the
    field is in pixels.
    """
    if isinstance(surface_info, dict):
        pitch = int(surface_info.get("surface_pitch", 0))
        msaa_samples = int(surface_info.get("msaa_samples", 0))
    else:
        decoded = decode_surface_info(surface_info)
        pitch = decoded["surface_pitch"]
        msaa_samples = decoded["msaa_samples"]

    sample_count = _MSAA_COUNTS.get(msaa_samples, 1)
    return (pitch, sample_count)


if __name__ == "__main__":
    for value in (0x08000230, 0x10010410):
        print(f"surface {value:08x}: {decode_surface_info(value)}")

    for value in (0x005403CF, 0x000003CF):
        print(f"color {value:08x}: {decode_color_info(value)}")

    for value in (0x00010461, 0x000103CF):
        print(f"depth {value:08x}: {decode_depth_info(value)}")
