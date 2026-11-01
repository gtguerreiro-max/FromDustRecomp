"""Xenos render state register decoder (subset)."""

REGISTERS = {
    0x2000: "RB_SURFACE_INFO",
    0x2001: "RB_COLOR_INFO",
    0x2002: "RB_DEPTH_INFO",
    0x2003: "RB_COLOR1_INFO",
    0x2004: "RB_COLOR2_INFO",
    0x2005: "RB_COLOR3_INFO",
    0x2006: "COHER_DEST_BASE_0",
    0x2007: "COHER_DEST_BASE_1",
    0x2008: "COHER_DEST_BASE_2",
    0x2009: "COHER_DEST_BASE_3",
    0x200A: "COHER_DEST_BASE_4",
    0x200B: "COHER_DEST_BASE_5",
    0x200C: "COHER_DEST_BASE_6",
    0x200D: "COHER_DEST_BASE_7",
    0x200E: "PA_SC_SCREEN_SCISSOR_TL",
    0x200F: "PA_SC_SCREEN_SCISSOR_BR",
    0x2080: "PA_SC_WINDOW_OFFSET",
    0x2081: "PA_SC_WINDOW_SCISSOR_TL",
    0x2082: "PA_SC_WINDOW_SCISSOR_BR",
    0x2083: "PA_SC_CLIPRECT_RULE",
    0x2084: "PA_SC_CLIPRECT_0_TL",
    0x2085: "PA_SC_CLIPRECT_0_BR",
    0x2086: "PA_SC_CLIPRECT_1_TL",
    0x2087: "PA_SC_CLIPRECT_1_BR",
    0x2088: "PA_SC_CLIPRECT_2_TL",
    0x2089: "PA_SC_CLIPRECT_2_BR",
    0x208A: "PA_SC_CLIPRECT_3_TL",
    0x208B: "PA_SC_CLIPRECT_3_BR",
    0x2100: "VGT_MAX_VTX_INDX",
    0x2101: "VGT_MIN_VTX_INDX",
    0x2102: "VGT_INDX_OFFSET",
    0x2103: "VGT_MULTI_PRIM_IB_RESET_INDX",
    0x2104: "RB_COLOR_MASK",
    0x2105: "RB_BLEND_RED",
    0x2106: "RB_BLEND_GREEN",
    0x2107: "RB_BLEND_BLUE",
    0x2108: "RB_BLEND_ALPHA",
    0x2109: "RB_FOG_COLOR_RED",
    0x210A: "RB_FOG_COLOR_GREEN",
    0x210B: "RB_FOG_COLOR_BLUE",
    0x210C: "RB_STENCILREFMASK_BF",
    0x210D: "RB_STENCILREFMASK",
    0x210E: "RB_ALPHA_REF",
    0x210F: "PA_CL_VPORT_XSCALE",
    0x2110: "PA_CL_VPORT_XOFFSET",
    0x2111: "PA_CL_VPORT_YSCALE",
    0x2112: "PA_CL_VPORT_YOFFSET",
    0x2113: "PA_CL_VPORT_ZSCALE",
    0x2114: "PA_CL_VPORT_ZOFFSET",
    0x2180: "SQ_PROGRAM_CNTL",
    0x2181: "SQ_CONTEXT_MISC",
    0x2182: "SQ_INTERPOLATOR_CNTL",
    0x2183: "SQ_WRAPPING_0",
    0x2184: "SQ_WRAPPING_1",
    0x21F4: "GFX_COPY_STATE",
    0x21F5: "SQ_CF_RD_BASE",
    0x21F6: "SQ_PS_PROGRAM",
    0x21F7: "SQ_VS_PROGRAM",
    0x21F9: "VGT_EVENT_INITIATOR",
    0x21FA: "VGT_DMA_BASE",
    0x21FB: "VGT_DMA_SIZE",
    0x21FC: "VGT_DRAW_INITIATOR",
    0x21FD: "VGT_IMMED_DATA",
    0x2200: "RB_DEPTHCONTROL",
    0x2201: "RB_BLENDCONTROL0",
    0x2202: "RB_COLORCONTROL",
    0x2203: "RB_HIZCONTROL",
    0x2204: "PA_CL_CLIP_CNTL",
    0x2205: "PA_SU_SC_MODE_CNTL",
    0x2206: "PA_CL_VTE_CNTL",
    0x2207: "VGT_CURRENT_BIN_ID_MIN",
    0x2208: "RB_MODECONTROL",
    0x2209: "RB_BLENDCONTROL1",
    0x220A: "RB_BLENDCONTROL2",
    0x220B: "RB_BLENDCONTROL3",
    0x2210: "GRAS_CONTROL",
    0x2280: "PA_SU_POINT_SIZE",
    0x2281: "PA_SU_POINT_MINMAX",
    0x2282: "PA_SU_LINE_CNTL",
    0x2283: "PA_SC_LINE_STIPPLE",
    0x2284: "VGT_OUTPUT_PATH_CNTL",
    0x2285: "VGT_HOS_CNTL",
    0x2286: "VGT_HOS_MAX_TESS_LEVEL",
    0x2287: "VGT_HOS_MIN_TESS_LEVEL",
    0x2288: "VGT_HOS_REUSE_DEPTH",
    0x2289: "VGT_GROUP_PRIM_TYPE",
    0x228A: "VGT_GROUP_FIRST_DECR",
    0x228B: "VGT_GROUP_DECR",
    0x228C: "VGT_GROUP_VECT_0_CNTL",
    0x228D: "VGT_GROUP_VECT_1_CNTL",
    0x228E: "VGT_GROUP_VECT_0_FMT_CNTL",
    0x228F: "VGT_GROUP_VECT_1_FMT_CNTL",
    0x2290: "UNKNOWN_2290",
    0x2291: "UNKNOWN_2291",
    0x2292: "PA_SC_MPASS_PS_CNTL",
    0x2293: "PA_SC_VIZ_QUERY",
    0x2294: "VGT_ENHANCE",
    0x2300: "PA_SC_LINE_CNTL",
    0x2301: "PA_SC_AA_CONFIG",
    0x2302: "PA_SU_VTX_CNTL",
    0x2303: "PA_CL_GB_VERT_CLIP_ADJ",
    0x2304: "PA_CL_GB_VERT_DISC_ADJ",
    0x2305: "PA_CL_GB_HORZ_CLIP_ADJ",
    0x2306: "PA_CL_GB_HORZ_DISC_ADJ",
    0x2307: "SQ_VS_CONST",
    0x2308: "SQ_PS_CONST",
    0x2309: "SQ_DEBUG_MISC_0",
    0x230A: "SQ_DEBUG_MISC_1",
    0x230B: "UNKNOWN_230B",
    0x230C: "UNKNOWN_230C",
    0x230D: "UNKNOWN_230D",
    0x230E: "UNKNOWN_230E",
    0x230F: "UNKNOWN_230F",
    0x2310: "UNKNOWN_2310",
    0x2311: "UNKNOWN_2311",
    0x2312: "PA_SC_AA_MASK",
    0x2313: "UNKNOWN_2313",
    0x2314: "UNKNOWN_2314",
    0x2315: "SQ_CF_PROGRAM_SIZE",
    0x2316: "VGT_VERTEX_REUSE_BLOCK_CNTL",
    0x2317: "VGT_OUT_DEALLOC_CNTL",
    0x2318: "RB_COPY_CONTROL",
    0x2319: "RB_COPY_DEST_BASE",
    0x231A: "RB_COPY_DEST_PITCH",
    0x231B: "RB_COPY_DEST_INFO",
    0x231C: "RB_HIZ_CLEAR",
    0x231D: "RB_DEPTH_CLEAR",
    0x231E: "RB_COLOR_CLEAR",
    0x231F: "RB_COLOR_CLEAR_LO",
    0x2320: "RB_COPY_FUNC",
    0x2321: "RB_COPY_REF",
    0x2322: "RB_COPY_MASK",
    0x2323: "RB_COPY_SURFACE_SLICE",
    0x2324: "RB_SAMPLE_COUNT_CTL",
    0x2325: "RB_SAMPLE_COUNT_ADDR",
    0x2326: "RB_COLOR_DEST_MASK",
    0x2340: "GRAS_UCP0X",
    0x2357: "GRAS_UCP5W",
    0x2360: "GRAS_UCP_ENABLED",
    0x2380: "PA_SU_POLY_OFFSET_FRONT_SCALE",
    0x2381: "PA_SU_POLY_OFFSET_FRONT_OFFSET",
    0x2382: "PA_SU_POLY_OFFSET_BACK_SCALE",
    0x2383: "PA_SU_POLY_OFFSET_BACK_OFFSET",
    0x2384: "PA_CL_POINT_X_RAD",
    0x2385: "PA_CL_POINT_Y_RAD",
    0x2386: "PA_CL_POINT_SIZE",
    0x2387: "PA_CL_POINT_CULL_RAD",
    0x2388: "PA_CL_UCP_0_X",
    0x2389: "PA_CL_UCP_0_Y",
    0x238A: "PA_CL_UCP_0_Z",
}

_NAME_TO_REGISTER = {name: addr for addr, name in REGISTERS.items()}
_NAME_TO_REGISTER["RB_BLENDCONTROL"] = 0x2201

_PA_SU_SC_MODE_CNTL_LAYOUT = (
    ("cull_front", 0, 1),
    ("cull_back", 1, 1),
    ("face", 2, 1),
    ("poly_mode", 3, 2),
    ("polymode_front_ptype", 5, 3),
    ("polymode_back_ptype", 8, 3),
    ("poly_offset_front_enable", 11, 1),
    ("poly_offset_back_enable", 12, 1),
    ("poly_offset_para_enable", 13, 1),
    ("_pad_14", 14, 1),
    ("msaa_enable", 15, 1),
    ("vtx_window_offset_enable", 16, 1),
    ("_pad_17", 17, 2),
    ("provoking_vtx_last", 19, 1),
    ("persp_corr_dis", 20, 1),
    ("multi_prim_ib_ena", 21, 1),
    ("_pad_22", 22, 1),
    ("quad_order_enable", 23, 1),
    ("sc_one_quad_per_clock", 24, 1),
    ("_pad_25", 25, 7),
)

_RB_COLORCONTROL_LAYOUT = (
    ("alpha_func", 0, 3),
    ("alpha_test_enable", 3, 1),
    ("alpha_to_mask_enable", 4, 1),
    ("_pad_5", 5, 19),
    ("alpha_to_mask_offset0", 24, 2),
    ("alpha_to_mask_offset1", 26, 2),
    ("alpha_to_mask_offset2", 28, 2),
    ("alpha_to_mask_offset3", 30, 2),
)

_RB_COLOR_MASK_LAYOUT = tuple(
    (f"write_{channel}{rt}", rt * 4 + index, 1)
    for rt in range(4)
    for index, channel in enumerate(("red", "green", "blue", "alpha"))
) + (("_pad_16", 16, 16),)

_RB_BLENDCONTROL_LAYOUT = (
    ("color_srcblend", 0, 5),
    ("color_comb_fcn", 5, 3),
    ("color_destblend", 8, 5),
    ("_pad_13", 13, 3),
    ("alpha_srcblend", 16, 5),
    ("alpha_comb_fcn", 21, 3),
    ("alpha_destblend", 24, 5),
    ("_pad_29", 29, 3),
)

_RB_DEPTHCONTROL_LAYOUT = (
    ("stencil_enable", 0, 1),
    ("z_enable", 1, 1),
    ("z_write_enable", 2, 1),
    ("_pad_3", 3, 1),
    ("zfunc", 4, 3),
    ("backface_enable", 7, 1),
    ("stencilfunc", 8, 3),
    ("stencilfail", 11, 3),
    ("stencilzpass", 14, 3),
    ("stencilzfail", 17, 3),
    ("stencilfunc_bf", 20, 3),
    ("stencilfail_bf", 23, 3),
    ("stencilzpass_bf", 26, 3),
    ("stencilzfail_bf", 29, 3),
)

_RB_STENCILREFMASK_LAYOUT = (
    ("stencilref", 0, 8),
    ("stencilmask", 8, 8),
    ("stencilwritemask", 16, 8),
    ("_pad_24", 24, 8),
)

_BITFIELD_LAYOUTS = {
    0x2205: _PA_SU_SC_MODE_CNTL_LAYOUT,
    0x2202: _RB_COLORCONTROL_LAYOUT,
    0x2104: _RB_COLOR_MASK_LAYOUT,
    0x2200: _RB_DEPTHCONTROL_LAYOUT,
    0x210D: _RB_STENCILREFMASK_LAYOUT,
}
for _reg in (0x2201, 0x2209, 0x220A, 0x220B):
    _BITFIELD_LAYOUTS[_reg] = _RB_BLENDCONTROL_LAYOUT

_COMPARE_FUNCTION_NAMES = {
    0: "never",
    1: "less",
    2: "equal",
    3: "less_equal",
    4: "greater",
    5: "not_equal",
    6: "greater_equal",
    7: "always",
}

_BLEND_FACTOR_NAMES = {
    0: "zero",
    1: "one",
    2: "src_color",
    3: "one_minus_src_color",
    4: "src_alpha",
    5: "one_minus_src_alpha",
    6: "dst_alpha",
    7: "one_minus_dst_alpha",
    8: "dst_color",
    9: "one_minus_dst_color",
    10: "src_alpha_sat",
}

_BLEND_OP_NAMES = {
    0: "add",
    1: "subtract",
    2: "reverse_subtract",
    3: "min",
    4: "max",
}


def _as_register(reg):
    if isinstance(reg, str):
        if reg in _NAME_TO_REGISTER:
            return _NAME_TO_REGISTER[reg]
        try:
            return int(reg, 0)
        except ValueError:
            return reg
    try:
        return int(reg)
    except (TypeError, ValueError):
        return reg


def _to_int(value, default=0):
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def decode(reg, value):
    reg = _as_register(reg)
    value = _to_int(value)
    masked_value = value & 0xFFFFFFFF

    layout = _BITFIELD_LAYOUTS.get(reg)
    if layout is None:
        return {"value": value}

    return {
        name: (masked_value >> shift) & ((1 << width) - 1)
        for name, shift, width in layout
    }


def _normalize_regs(regs):
    if regs is None:
        return {}

    normalized = {}
    for key, value in regs.items():
        key = _as_register(key)
        if isinstance(key, str):
            continue
        normalized[key] = _to_int(value) & 0xFFFFFFFF
    return normalized


def _get(regs, reg, default=0):
    return _to_int(regs.get(reg, default)) & 0xFFFFFFFF


def _color_mask_for(color_mask, rt):
    shift = rt * 4
    r = (color_mask >> shift) & 1
    g = (color_mask >> (shift + 1)) & 1
    b = (color_mask >> (shift + 2)) & 1
    a = (color_mask >> (shift + 3)) & 1
    return r | (g << 1) | (b << 2) | (a << 3)


def _blend_component(regs, register, shift):
    if register not in regs:
        return False, "one", "zero", "add"

    value = regs[register]
    src = (value >> shift) & 0x1F
    op = (value >> (shift + 5)) & 0x07
    dst = (value >> (shift + 8)) & 0x1F

    default_no_blend = (src == 1 and dst == 0 and op == 0)
    non_zero = (src != 0 or dst != 0 or op != 0)
    enabled = (not default_no_blend) and non_zero

    return (
        enabled,
        _BLEND_FACTOR_NAMES.get(src, f"unknown_{src}"),
        _BLEND_FACTOR_NAMES.get(dst, f"unknown_{dst}"),
        _BLEND_OP_NAMES.get(op, f"unknown_{op}"),
    )


def d3d12_summary(regs):
    regs = _normalize_regs(regs)

    pa_su_sc_mode_cntl = _get(regs, 0x2205, 0)
    cull_front = (pa_su_sc_mode_cntl >> 0) & 1
    cull_back = (pa_su_sc_mode_cntl >> 1) & 1
    face = (pa_su_sc_mode_cntl >> 2) & 1

    if cull_front and cull_back:
        cull_mode = "front_and_back"
    elif cull_front:
        cull_mode = "front"
    elif cull_back:
        cull_mode = "back"
    else:
        cull_mode = "none"

    front_face = "cw" if face else "ccw"

    rb_depthcontrol = _get(regs, 0x2200, 0)
    stencil_enable = bool((rb_depthcontrol >> 0) & 1)
    depth_test = bool((rb_depthcontrol >> 1) & 1)
    depth_write = bool((rb_depthcontrol >> 2) & 1)
    depth_function = _COMPARE_FUNCTION_NAMES.get(
        (rb_depthcontrol >> 4) & 0x7,
        "unknown",
    )

    color_enabled, color_src, color_dst, color_op = _blend_component(
        regs, 0x2201, 0
    )
    alpha_enabled, alpha_src, alpha_dst, alpha_op = _blend_component(
        regs, 0x2201, 16
    )

    color_mask = _get(regs, 0x2104, 0)
    color_write_mask = {
        "rt0": _color_mask_for(color_mask, 0),
        "rt1": _color_mask_for(color_mask, 1),
        "rt2": _color_mask_for(color_mask, 2),
        "rt3": _color_mask_for(color_mask, 3),
    }

    return {
        "cull_mode": cull_mode,
        "front_face": front_face,
        "depth_test": depth_test,
        "depth_write": depth_write,
        "depth_function": depth_function,
        "color_blend_enable": color_enabled,
        "color_src": color_src,
        "color_dst": color_dst,
        "color_op": color_op,
        "alpha_blend_enable": alpha_enabled,
        "alpha_src": alpha_src,
        "alpha_dst": alpha_dst,
        "alpha_op": alpha_op,
        "color_write_mask": color_write_mask,
        "stencil_enable": stencil_enable,
    }


if __name__ == "__main__":
    print(decode(0x2200, 0x00700770))
