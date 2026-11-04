"""Pack a captured frame (build/frame_capture.bin, buffers.bin, shader_inits.bin)
into build/replay/replay.bin for tools/fd_replay, and compile the vertex
shaders it needs to DXIL (build/replay/<container>.vs.dxil).

Each draw uses its original vertex/pixel shaders (XenosRecomp HLSL), constants,
resources and render state. Resolve and clear operations retain frame order.

replay.bin (little-endian):
  u32 'FDR9', u32 draw count, then per draw:
  u32 vs name length, name bytes (container number, e.g. "143"),
  u32 primitive, u32 index count, u32 index bytes, index data (u16 or u32),
  u32 vertex stride bytes, u32 vertex bytes, vertex data (every element
  converted to float32 x4, so stride = 16 * elements),
  u32 element count, per element: u32 semantic length, semantic (e.g. POSITION0),
  4096 bytes vertex shader constants (float4 c0..c255, swapped),
  u32 index32, then u32 x STATE_REGISTERS (current values of those GPU
  registers at the draw, tracked from every draw's register writes),
  then i32 x 32: texture table index per fetch slot (0-15 pixel, 16-31
  vertex samplers), -1 if none,
  then u32 ps name length, name (empty: no pixel shader, depth only),
  4096 bytes pixel shader constants (c0..c255, swapped),
  then u32 x 32: texture base address per fetch slot (0 if none),
  then u32 x 32: fetch swizzle per slot (4 x 3 bits: 0-3 XYZW, 4 zero, 5 one),
  then u32 x 40: registers 0x4900-0x4927 (boolean and loop constants),
  512 bytes vertex fetch descriptors, i32 x 32 texture exponent adjustments,
  then u32 x 32: original Xenos texture format per fetch slot (0xFFFFFFFF if unused),
  then u32 x 32: clamp XYZ (3 bits each), mag/min/mip filters (2 bits each),
  anisotropic filter (3 bits), packed low to high (0xFFFFFFFF if unused),
  then 32 x 6 u32: complete host-endian texture fetch descriptors (LOD ranges,
  bias, border colour and dimensionality retained for the native resource layer),
  then 1024 u32: complete context registers 0x2000..0x23FF (including clip/depth bias).
A record whose vs name is "#resolve" is an EDRAM -> texture copy instead:
  u32 RB_COPY_DEST_BASE, RB_COPY_CONTROL, RB_SURFACE_INFO, RB_COLOR_INFO,
  RB_DEPTH_INFO, RB_COPY_DEST_INFO, destination width and height, destination X/Y,
  destination mip level and mip count.
A "#clear" record contains STATE_REGISTERS, u32 flags, ARGB colour, float depth,
  u32 stencil, rectangle count, then that many i32 (left, top, right, bottom).

textures.rgba: u32 count, per texture u32 width, u32 height, RGBA8 pixels
(decoded by xenos_texture.py; unsupported formats fail explicitly).
Width bit 31 selects RGBA float32 texels for fixed-point table textures.
cubes.rgba: u32 count, then table index, width, height and six RGBA8 faces.
"""
import argparse, hashlib, json, os, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(__file__))
from analyze_capture import load
from pm4_decode import packets_robust as packets
from xenos_vfetch import decode_vfetch
from xenos_fetch import decode_texture_fetch
import xenos_texture
from xenos_tiling import tiled_2d

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DXC = os.path.join(ROOT, "XenosRecomp", "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")
OUT = os.path.join(ROOT, "build", "replay")
import math, re

# xenos VertexFormat -> (struct code per component, component count); halves
# are converted from IEEE half.
COMPONENTS = {6: ("B", 4), 25: ("H", 2), 26: ("H", 4), 31: ("e", 2), 32: ("e", 4),
              36: ("f", 1), 37: ("f", 2), 38: ("f", 4), 57: ("f", 3), 33: ("I", 1), 34: ("I", 2), 35: ("I", 4)}


def swap32(data):
    n = len(data) // 4
    return struct.pack("<%dI" % n, *struct.unpack(">%dI" % n, data[:n * 4]))


def load_buffers(path):
    data, i, out = open(path, "rb").read(), 0, {}
    while i < len(data):
        if i + 12 > len(data):
            raise ValueError("Truncated captured buffer header")
        _, address, length = struct.unpack_from("<3I", data, i)
        if i + 12 + length > len(data):
            raise ValueError("Truncated captured buffer payload")
        payload = data[i + 12:i + 12 + length]
        previous = out.get(address, b"")
        # Reusing a static index buffer through a shorter draw must not discard
        # its complete earlier snapshot. Changed contents still select the latest.
        if len(previous) <= length or previous[:length] != payload:
            out[address] = payload
        i += 12 + length
    return out


def shader_objects(path, bins):
    """Shader object -> (stage, container name), from shader_inits.bin."""
    data, rec, out = open(path, "rb").read(), 16 + 1024, {}
    physical = []
    for name, blob in bins.items():
        flags, virtual_size, physical_size = struct.unpack_from(">3I", blob)
        physical.append((name, 0 if flags & 1 else 1,
                         blob[virtual_size:virtual_size + physical_size]))
    for i in range(0, len(data), rec):
        _, kind, obj, _ = struct.unpack_from("<4I", data, i)
        code = data[i + 16:i + 16 + 1024]
        # The init pointer is the start of the container's physical section.
        # Short shaders (e.g. the 60-byte UI pixel shader) are followed by
        # unrelated memory in the 1024-byte snapshot. Compare their actual
        # length: a shared 32-byte prefix can select a different shader.
        hits = [(min(len(payload), len(code)), name) for name, stage, payload in physical
                if stage == kind and payload and
                payload[:len(code)] == code[:len(payload)]]
        if hits:
            _, name = max(hits, key=lambda hit: hit[0])
            out[obj] = (kind, name)
    return out


def live_vfetches(code):
    """Vertex fetch instructions in live microcode (D3D patches format, offset,
    stride and fetch constant into them at bind time), in program order: the
    control flow's exec instructions give each instruction slot and whether it
    is a fetch (like Xenia's shader translator); falls back to a word scan."""
    words = struct.unpack(">%dI" % (len(code) // 4), code[: len(code) // 4 * 4])
    out, bound, slots = [], len(words) // 3, []
    i = 0
    while i < bound:
        w0, w1, w2 = words[i * 3:i * 3 + 3]
        for d0, d1 in ((w0, w1 & 0xFFFF), ((w1 >> 16) | ((w2 << 16) & 0xFFFFFFFF), w2 >> 16)):
            if (d1 >> 12) & 0xF in (1, 2, 3, 4, 5, 6, 13, 14):  # exec variants
                address, count, sequence = d0 & 0xFFF, (d0 >> 12) & 7, (d0 >> 16) & 0xFFF
                bound = min(bound, address)
                for k in range(count):
                    if (sequence >> (2 * k)) & 1:
                        slots.append(address + k)
        i += 1
    for slot in slots:
        w = list(words[slot * 3:slot * 3 + 3])
        if len(w) == 3 and w[0] & 0x1F == 0:  # FetchOpcode::kVertexFetch
            v = decode_vfetch(w)
            if v["format"] in COMPONENTS:
                out.append(v)
    if out:
        return out
    for i in range(len(words) - 2):
        w = list(words[i:i + 3])
        if w[0] & 0x1F or not (w[0] >> 19) & 1:
            continue
        v = decode_vfetch(w)
        if v["format"] in COMPONENTS and 0 <= v["stride"] <= 64:
            out.append(v)
    return out


def hlsl_vfetches(name):
    """Semantics of the shader's vertex fetches in program order (FDVFETCH
    comments written by the patched XenosRecomp)."""
    hlsl = open(os.path.join(ROOT, "build", "shaders_split", name + ".hlsl"), encoding="utf-8", errors="replace").read()
    return [(int(r), sem) for r, sem in re.findall(r"r(\d+)\.\w+ = /\* FDVFETCH ([A-Z]+[0-9]+) ", hlsl)]


def vertex_layout(code, name):
    """Tries the live microcode at the object's +0x40 (the usual place) and at
    +0 (some shaders), keeping the one whose fetches pair with the HLSL's."""
    for start in (0x40, 0):
        result = vertex_layout_at(code[start:], name)
        if result[0] or result[1]:
            return result
    return 0, [], []


def vertex_layout_at(code, name):
    """(stride bytes, [(offset bytes, vfetch, semantic)], [(ordinal, vfetch)]):
    the input layout of stream fetch 95 and the fetches of other streams, which
    the shader pulls from g_VertexStreams (their input elements get offset None,
    zero data). (0, [], []) when the shader's fetches cannot be matched."""
    live, fetches = live_vfetches(code), hlsl_vfetches(name)
    # Pair each HLSL fetch with the live fetch writing the same register, in
    # order (the microcode scan can also match non-fetch instructions).
    pairs, used = [], set()
    for reg, semantic in fetches:
        match = next((k for k, v in enumerate(live) if k not in used and v["dest_reg"] == reg), None)
        if match is None:
            return 0, [], []
        used.add(match)
        pairs.append((live[match], semantic))
    if not pairs:
        return 0, [], []
    stride, elements, pulls = 0, {}, []
    for ordinal, (v, semantic) in enumerate(pairs):
        if os.environ.get("FD_PACK_DEBUG") and name in os.environ["FD_PACK_DEBUG"].split(","):
            print("VS", name, semantic, "reg", v["dest_reg"], "src", v.get("src_reg"), "const", v["fetch_constant_index"], "off", v["offset"], "stride", v["stride"], "fmt", v["format"], "mini", v["is_mini_fetch"])
        if v["fetch_constant_index"] != 95:
            pulls.append((ordinal, v))
            elements.setdefault(semantic, (None, v))
            continue
        stride = stride or (v["stride"] if not v["is_mini_fetch"] else 0)
        if elements.get(semantic, (0,))[0] is None:
            del elements[semantic]
        elements.setdefault(semantic, (v["offset"] * 4, v))
    return stride * 4, [(off, v, sem) for sem, (off, v) in elements.items()], pulls


def vertex_fetch_descriptors(pulls, device, buffers, streams, blob):
    """32 uint4 g_VertexFetch descriptors (see XenosRecomp's shader_common.h):
    pulled fetches read their stream from the streams blob (guest buffers with
    dwords byte-swapped), appended once per guest address."""
    desc = [(0, 0, 0, 0)] * 32
    for ordinal, v in pulls:
        if ordinal >= 32:
            continue
        fetch = struct.unpack_from(">2I", device, 0x480 + v["fetch_constant_index"] * 8)
        address = fetch[0] & 0x1FFFFFFC
        data = buffers.get(address)
        if data is None:
            continue
        if address not in streams:
            streams[address] = (len(blob), len(data))
            blob += swap32(data) + bytes(-len(data) % 4)
        base, size = streams[address]
        flags = v["format"] | v["is_signed"] << 8 | (v["num_format"] & 1) << 9 | (v["exp_adjust"] & 63) << 10 | 1 << 31
        desc[ordinal] = (base, v["stride"] * 4 | v["offset"] * 4 << 16, size, flags)
    return b"".join(struct.pack("<4I", *d) for d in desc)


def convert_element(data, offset, v):
    """One vertex element (big-endian guest bytes) -> 4 floats, like the vertex fetch unit."""
    if offset is None:  # pulled by the shader: placeholder input
        return bytes(16)
    code, n = COMPONENTS[v["format"]]
    size = struct.calcsize(code)
    raw = data[offset:offset + size * n]
    if v["format"] == 6:  # 8_8_8_8: components in memory order (skinning: weights, then bone indices)
        values = list(raw) if len(raw) == 4 else [0] * 4
    else:
        values = list(struct.unpack(">%d%s" % (n, code), raw)) if len(raw) == size * n else [0] * n
    out = []
    bits = {"B": 8, "H": 16}.get(code)
    for x in values:
        if bits:  # integer formats: sign, normalisation (num_format 0) and exponent
            if v["is_signed"] and x >= 1 << (bits - 1):
                x -= 1 << bits
            if v["num_format"] == 0:
                x = max(x / ((1 << (bits - 1)) - 1), -1.0) if v["is_signed"] else x / ((1 << bits) - 1)
            x = x * math.ldexp(1.0, v["exp_adjust"])
        out.append(float(x))
    while len(out) < 4:
        out.append(1.0 if len(out) == 3 else 0.0)
    return struct.pack("<4f", *out[:4])


def uses_textures(name):
    hlsl = open(os.path.join(ROOT, "build", "shaders_split", name + ".hlsl"), encoding="utf-8", errors="replace").read()
    return "tfetch" in hlsl[hlsl.index("void main("):]


def hlsl_inputs(name):
    hlsl = open(os.path.join(ROOT, "build", "shaders_split", name + ".hlsl"), encoding="utf-8", errors="replace").read()
    main = hlsl[hlsl.index("void main("):]
    return re.findall(r"in (?:float4|uint4) i\w+ : ([A-Z]+[0-9]+)", main[:main.index("{")])


def compile_shader(name, stage):
    """XenosRecomp HLSL -> <name>.<stage>.dxil (spec constants fixed at 0)."""
    src = os.path.join(ROOT, "build", "shaders_split", name + ".hlsl")
    hlsl = open(src, encoding="utf-8", errors="replace").read() + "\nuint g_SpecConstants() { return 0; }\n"
    tmp = os.path.join(OUT, "%s.%s.hlsl" % (name, stage))
    dst = os.path.join(OUT, "%s.%s.dxil" % (name, stage))
    open(tmp, "w", encoding="utf-8").write(hlsl)
    r = subprocess.run([DXC, "-T", stage + "_6_0", "-E", "main", "-HV", "2021", "-all-resources-bound",
                        "-Wno-ignored-attributes", tmp, "-Fo", dst], capture_output=True, text=True)
    if r.returncode:
        open(os.path.join(OUT, "%s.%s.compile.log" % (name, stage)), "w", encoding="utf-8").write(r.stderr)
    return r.returncode == 0


DEBUG_PS = """// fd_replay debug pixel shader: shades geometry by depth (near = bright).
float4 main(float4 position : SV_Position) : SV_Target
{
    float shade = saturate(position.z); // reversed depth: near = 1
    return float4(shade, shade * 0.8, 0.4 + 0.6 * shade, 1.0);
}
"""


# GPU registers the replay needs per draw (render target, viewport, state).
STATE_REGISTERS = [0x2000, 0x2001, 0x2002, 0x2081, 0x2082, 0x2104, 0x2200, 0x2201, 0x2205,
                   0x2102, 0x210F, 0x2110, 0x2111, 0x2112, 0x2113, 0x2114, 0x2206,
                   0x2003, 0x2004, 0x2005, 0x2209, 0x220A, 0x220B,
                   0x2302, 0x210D, 0x210C,  # pixel centres, stencil ref/read/write masks (front/back)
                   0x2105, 0x2106, 0x2107, 0x2108]  # RGBA blend constants


def expand_rects(vdata, element_count):
    """Xenos rectangle lists -> triangles: per 3 vertices, mirror the vertex
    opposite the longest edge (Xenia's rule, measured on element 0 x/y), giving
    4 vertices and 6 indices per rectangle. vdata: float4 x element_count per vertex."""
    size = 16 * element_count
    verts = [vdata[i:i + size] for i in range(0, len(vdata) - size + 1, size)]
    out, indices = [], []
    for k in range(0, len(verts) - 2, 3):
        v = [struct.unpack("<%df" % (4 * element_count), verts[k + j]) for j in range(3)]
        dist = lambda a, b: (v[a][0] - v[b][0]) ** 2 + (v[a][1] - v[b][1]) ** 2
        e12, e20, e01 = dist(1, 2), dist(2, 0), dist(0, 1)
        order = (0, 1, 2) if e12 >= e20 and e12 >= e01 else (1, 2, 0) if e20 >= e01 else (2, 0, 1)
        a, b, c = (v[i] for i in order)
        d = [b[i] + c[i] - a[i] for i in range(len(a))]
        base = len(out)
        out += [a, b, c, d]
        indices += [base, base + 1, base + 2, base + 2, base + 1, base + 3]
    data = b"".join(struct.pack("<%df" % len(x), *x) for x in out)
    return data, struct.pack("<%dH" % len(indices), *indices), len(indices)


def scan_draw(commands):
    """Fallback when packet parsing derails: the last type-3 DRAW_INDX /
    DRAW_INDX_2 header whose packet fits in the block, by raw word scan."""
    words = struct.unpack(">%dI" % (len(commands) // 4), commands[: len(commands) // 4 * 4])
    for i in range(len(words) - 1, -1, -1):
        h = words[i]
        op, n = (h >> 8) & 0x7F, ((h >> 16) & 0x3FFF) + 1
        if h >> 30 == 3 and op in (0x22, 0x36) and i + 1 + n <= len(words) and n <= 8:
            p = words[i + 1:i + 1 + n]
            initiator = p[1] if op == 0x22 and len(p) > 1 else p[0] if op == 0x36 else 0
            if initiator & 0x3F not in (4, 8) or not initiator >> 16:
                continue
            return ("DRAW_INDX", p) if op == 0x22 else ("DRAW_INDX_2", (0,) + tuple(p))
    return None


def copy_dest_registers(commands):
    """Resolve registers: D3D writes RB_COPY_CONTROL..DEST_INFO
    (0x2318-0x231B) in one type-0 packet, then zeroes them after the copy."""
    words = struct.unpack(">%dI" % (len(commands) // 4), commands[: len(commands) // 4 * 4])
    for i, h in enumerate(words):
        if h >> 30 == 0 and h & 0x7FFF == 0x2318 and ((h >> 16) & 0x3FFF) >= 3 and i + 4 < len(words):
            return words[i + 1:i + 5]
    return (0, 0, 0, 0)


def copy_destination_offset(commands, fetch, gpu_states=()):
    """Locate a partial resolve within its destination texture.

    D3D advances the copy base to the destination macro-tile, while the
    texture object's fetch base still addresses the whole atlas.
    """
    control, base, pitch, info = copy_dest_registers(commands)
    if not control:
        # A ring wrap leaves no contiguous CPU command block. The GPU dump
        # still contains the copy's exact registers; accept only one unique
        # matching configuration, never guess an atlas region.
        dest = fetch[1] & 0x1FFFF000
        candidates = {tuple(state[r] for r in range(0x2318, 0x231C)) for state in gpu_states
                      if state.get(0x2318, 0) and state.get(0x2319, 0) & 0x1FFFFFFF == dest}
        if len(candidates) != 1:
            raise ValueError("Missing or ambiguous GPU resolve metadata for %08X" % dest)
        control, base, pitch, info = candidates.pop()
    texture = decode_texture_fetch(fetch)
    base &= 0x1FFFFFFF
    mip_count = texture["mip_max_level"] + 1
    copy_pitch = pitch & 0x3FFF
    # The observed render-to-mip copies use separate 32x32-aligned levels.
    # Mip storage has its own base and must never overwrite level zero.
    mip_base = texture["mip_address"] & 0x1FFFFFFF
    if mip_base and base >= mip_base and copy_pitch and copy_pitch < texture["width"]:
        for mip in range(1, mip_count):
            if max(1, texture["width"] >> mip) == copy_pitch and copy_pitch >= 32:
                return info, 0, 0, mip, mip_count
        raise ValueError("Unsupported packed mip resolve")
    delta = base - (fetch[1] & 0x1FFFF000)
    if not delta:
        return info, 0, 0, 0, mip_count
    bpp = xenos_texture._BYTES_PER_TEXEL.get(texture["format_name"])
    if delta < 0 or not bpp:
        raise ValueError("Unsupported partial resolve address/format")
    pitch = pitch & 0x3FFF or texture["pitch"]
    if not texture["tiled"]:
        y, xbytes = divmod(delta, pitch * bpp)
        if xbytes % bpp == 0 and xbytes // bpp < texture["width"] and y < texture["height"]:
            return info, xbytes // bpp, y, 0, mip_count
    else:
        for y in range(0, texture["height"], 32):
            for x in range(0, texture["width"], 32):
                if tiled_2d(x, y, (pitch + 31) & ~31, bpp.bit_length() - 1) == delta:
                    return info, x, y, 0, mip_count
    raise ValueError("Partial resolve base does not identify a destination tile")


def draw_key(objs):
    """(VGT_DRAW_INITIATOR, VGT_DMA_BASE or 0) of a record's draw packet."""
    commands = objs.get("commands", b"")[4:]
    draw = scan_draw(commands) or next(((n, p if n.startswith("DRAW_INDX(") or n == "DRAW_INDX" else (0,) + tuple(p))
                 for n, p in packets(commands) if n.split("(")[0] in ("DRAW_INDX", "DRAW_INDX_2")), None)
    if not draw or len(draw[1]) < 2:
        return None
    source = (draw[1][1] >> 6) & 3
    return draw[1][1], draw[1][2] if source == 0 and len(draw[1]) > 2 else 0


def align_xenia(draws, xenia):
    """Draw ordinal (records with a draw packet, in order) -> Xenia event index,
    by sequence alignment of the draw keys (Xenia also sees D3D-internal draws)."""
    import difflib
    # Key: draw initiator, index base, and the colour target (RB_SURFACE_INFO,
    # RB_COLOR_INFO), plus SQ_PROGRAM_CNTL. Depth/alpha prepasses can use
    # exactly the same indexed geometry and target as the later colour pass.
    ours, tracked = [], {}
    for header, _, objs in draws:
        track_registers(tracked, objs.get("commands", b""))
        if header[1] not in (4, 5):
            key = draw_key(objs)
            ours.append(key + (tracked.get(0x2000, 0), tracked.get(0x2001, 0), tracked.get(0x2180, 0)) if key else None)
    theirs = [(x[0x21FC], x[0x21FA] if (x[0x21FC] >> 6) & 3 == 0 else 0,
               x[0x2000], x[0x2001], x.get(0x2180, 0)) for x in xenia]
    out = {}
    for block in difflib.SequenceMatcher(None, ours, theirs, autojunk=False).get_matching_blocks():
        for k in range(block.size):
            out[block.a + k] = block.b + k
    # A damaged/resynchronised PM4 region can make the parsed target bogus.
    # Indexed meshes still have a strong key (initiator + physical index
    # address). Recover those matches between the already aligned anchors;
    # auto-indexed full-screen passes remain strict because their keys repeat.
    anchors = sorted(out.items())
    used = set(out.values())
    rough = [key[:2] + key[4:] if key else None for key in ours]
    for block in difflib.SequenceMatcher(None, rough, [key[:2] + key[4:] for key in theirs], autojunk=False).get_matching_blocks():
        for n in range(block.size):
            a, b = block.a + n, block.b + n
            if a in out or b in used or not rough[a] or not rough[a][1]:
                continue
            lo = max((j for i, j in anchors if i < a), default=-1)
            hi = min((j for i, j in anchors if i > a), default=len(theirs))
            if lo < b < hi:
                out[a] = b
                used.add(b)
    return out


def load_xenia_registers(path):
    """GPU state per draw: FDXR has render/bool registers; FDX2 also has
    float constants 0x4000-0x47FF, after D3D has flushed and patched them."""
    if not os.path.exists(path):
        return []
    data, out = open(path, "rb").read(), []
    at = 0
    while at < len(data):
        magic = struct.unpack_from("<I", data, at)[0]
        if magic not in (0x52584446, 0x32584446):
            raise ValueError("Unknown GPU register record at %d" % at)
        size = 4260 + (8192 if magic == 0x32584446 else 0)
        if at + size > len(data):
            raise ValueError("Truncated GPU register record at %d" % at)
        values = struct.unpack_from("<1064I", data, at + 4)
        regs = dict(zip(range(0x2000, 0x2400), values[:0x400]))
        regs.update(zip(range(0x4900, 0x4928), values[0x400:]))
        if magic == 0x32584446:
            regs.update(zip(range(0x4000, 0x4800), struct.unpack_from("<2048I", data, at + 4260)))
        out.append(regs)
        at += size
    return out


def register_stream_hash(data):
    """Capture's FNV-style hash of little-endian words, including record magics."""
    value = 14695981039346656037
    for word, in struct.iter_unpack("<I", data):
        value = ((value ^ word) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def load_gpu_draws(path, register_path, count):
    """Optional exact host-submission flags; reject stale or incomplete metadata."""
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    if len(data) < 16:
        raise ValueError("Truncated GPU submission metadata")
    magic, recorded_count, digest = struct.unpack_from("<IIQ", data)
    if magic != 0x45474446 or recorded_count != count or len(data) != 16 + count or any(x > 1 for x in data[16:]):
        raise ValueError("Invalid GPU submission metadata")
    if register_stream_hash(open(register_path, "rb").read()) != digest:
        raise ValueError("GPU submission metadata belongs to a different register capture")
    return data[16:]


def track_registers(regs, commands):
    """Apply a draw's type-0 register writes (commands start one word in)."""
    for name, payload in packets(commands[4:]):
        if name.startswith("REG "):
            base = int(name.split()[1], 16)
            for k, value in enumerate(payload):
                regs[base + k] = value


def clear_registers(regs, objects):
    """Clear/resolve use temporary GPU targets; bound D3D surface objects
    retain the real surface/colour/depth registers at +0x18/+0x1C."""
    regs = dict(regs)
    size = None
    for offset, register in ((0x3190, 0x2001), (0x3194, 0x2003),
                             (0x3198, 0x2004), (0x319C, 0x2005), (0x31A0, 0x2002)):
        pointer, surface = objects[offset]
        if pointer:
            info, format_info = struct.unpack_from(">2I", surface, 0x18)
            regs[0x2000] = info
            regs[register] = format_info
            if size is None:
                size = struct.unpack_from(">I", surface, 0x24)[0]
    if size is not None:
        width, height = (size >> 18) + 1, ((size >> 3) & 0x1FFF) + 1
        regs[0x2081], regs[0x2082] = 0, width | height << 16
        for register, value in zip(range(0x210F, 0x2115),
                                   (width / 2, width / 2, -height / 2, height / 2, -1, 1)):
            regs[register] = struct.unpack("<I", struct.pack("<f", value))[0]
    return regs


def load_textures(path):
    """Captured textures (frame_capture.cpp): base address -> (fetch words, bytes)."""
    with open(path, "rb") as file:
        data = file.read()
    i, out = 0, {}
    while i < len(data):
        fetch = list(struct.unpack_from("<6I", data, i + 4))
        length = struct.unpack_from("<I", data, i + 28)[0]
        out[fetch[1] & 0xFFFFF000] = (fetch, data[i + 32:i + 32 + length])
        i += 32 + length
    return out


FIXED16_ORDER = int(os.environ.get("FD_FIXED16_ORDER", "0"))


def decode_fixed16(raw, fetch):
    """k_16_16 / k_16_16_16_16 (fixed point, not float) as RGBA float32, with the
    value a shader fetch returns: sign per component (fetch dword 0 bits 2-9),
    fraction or integer (dword 3 bit 0) and 2^exp_adjust (dword 3 bits 13-18)."""
    info = decode_texture_fetch(fetch)
    width, height = int(info["width"]), int(info["height"])
    pitch, n = max(int(info.get("pitch", width) or width), width), 2 if info["format_name"] == "k_16_16" else 4
    data = xenos_texture._fix_endian(bytes(raw), int(info.get("endianness", 0) or 0))
    packed = xenos_texture._extract_packed_base(data, info)
    if packed is not None:
        data, pitch = packed, width
    elif info.get("tiled"):
        from xenos_tiling import untile_2d
        expected = height * pitch * 2 * n
        data = bytes(untile_2d((data + bytes(max(0, expected - len(data))))[:expected], width, height, pitch, 2 * n))
    stride = xenos_texture._choose_stride_texels(len(data), height, width, pitch, 2 * n)
    signs = [(fetch[0] >> (2 + 2 * c)) & 3 for c in range(4)]
    integer, exp = fetch[3] & 1, (((fetch[3] >> 13) & 0x3F) ^ 32) - 32
    scale = math.ldexp(1.0, exp)
    out = bytearray(width * height * 16)
    for y in range(height):
        row = data[y * stride * 2 * n:(y * stride + width) * 2 * n]
        row += bytes(width * 2 * n - len(row))
        values = struct.unpack((">" if FIXED16_ORDER & 2 else "<") + "%dH" % (width * n), row)
        if FIXED16_ORDER & 1:  # components in memory order: swap each pair
            values = [values[i ^ 1] for i in range(len(values))]
        texels = []
        for x in range(width):
            px = []
            for c in range(n):
                v = values[x * n + c]
                if signs[c] == 1:
                    v = v - 65536 if v >= 32768 else v
                    v = v if integer else max(v / 32767.0, -1.0)
                else:
                    v = v if integer else v / 65535.0
                px.append(v * scale)
            texels.extend(px + [0.0, 1.0][len(px) - 2:] if n == 2 else px)
        struct.pack_into("<%df" % (width * 4), out, y * width * 16, *texels)
    return width, height, bytes(out)


def build_texture_table(textures):
    """Decode every captured texture to RGBA8, or RGBA float32 for 16-bit fixed
    point formats (width bit 31 set); returns (base -> index, table bytes)."""
    index, blobs = {}, []
    for base, (fetch, raw) in textures.items():
        info = decode_texture_fetch(fetch)
        try:
            if info.get("dimension") == 2:
                raise ValueError("3D texture upload is not implemented")
            if info.get("dimension") == 1 and info.get("stacked"):
                raise ValueError("Stacked 2D texture upload is not implemented")
            if fetch[1] & 0x3F in (25, 26):  # k_16_16, k_16_16_16_16
                width, height, data = decode_fixed16(raw, fetch)
                index[base] = len(blobs)
                blobs.append(struct.pack("<2I", width | 0x80000000, height) + data)
                continue
            width, height, rgba = xenos_texture.decode_texture(raw, info)
        except (ValueError, KeyError, struct.error) as error:
            raise ValueError("Texture %08X (%s): %s" % (base, info["format_name"], error)) from error
        index[base] = len(blobs)
        blobs.append(struct.pack("<2I", width, height) + bytes(rgba))
    return index, struct.pack("<I", len(blobs)) + b"".join(blobs)


def cube_face_stride(info):
    """Xenos cube face stride: 32x32 blocks, then 4 KB alignment."""
    fmt = info["format_name"]
    compressed = fmt in xenos_texture._BLOCK_BYTES
    block = 4 if compressed else 1
    size = (xenos_texture._BLOCK_BYTES if compressed else xenos_texture._BYTES_PER_TEXEL)[fmt]
    pitch = max(info["width"], info.get("pitch", info["width"]))
    cols = ((pitch + block - 1) // block + 31) & ~31
    rows = ((info["height"] + block - 1) // block + 31) & ~31
    return (cols * rows * size + 4095) & ~4095


def build_cube_table(textures, index):
    """u32 count; each cube: table index, width, height, six RGBA8 faces."""
    records = []
    for base, (fetch, raw) in textures.items():
        info = decode_texture_fetch(fetch)
        if info["dimension"] != 3:
            continue
        stride = cube_face_stride(info)
        if len(raw) < 6 * stride:
            raise ValueError("cube %08X has only %d bytes; recapture with six faces (%d bytes)" %
                             (base, len(raw), 6 * stride))
        faces = [xenos_texture.decode_texture(raw[face * stride:(face + 1) * stride], info)[2]
                 for face in range(6)]
        records.append(struct.pack("<3I", index[base], info["width"], info["height"]) + b"".join(faces))
    return struct.pack("<I", len(records)) + b"".join(records)


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-dir", default=os.path.join(ROOT, "build"))
    parser.add_argument("--out", default=OUT)
    parser.add_argument("--gpu-textures", action="store_true", help="use textures captured on the GPU consumer thread")
    args = parser.parse_args()
    capture = args.capture_dir
    OUT = args.out
    os.makedirs(OUT, exist_ok=True)
    # Depth -> 8_8_8_8 resolve emulation pass (tools/fd_replay/depth_pack.hlsl).
    pack = os.path.join(ROOT, "tools", "fd_replay", "depth_pack.hlsl")
    for stage, entry in (("vs", "vs_main"), ("ps", "ps_main")):
        subprocess.run([DXC, "-T", stage + "_6_0", "-E", entry, pack, "-Fo",
                        os.path.join(OUT, "depth_pack.%s.dxil" % stage)], check=True, capture_output=True)
    subprocess.run([DXC, "-T", "ps_6_0", "-E", "float_main", pack, "-Fo",
                    os.path.join(OUT, "depth_float.ps.dxil")], check=True, capture_output=True)
    open(os.path.join(OUT, "debug.ps.hlsl"), "w").write(DEBUG_PS)
    subprocess.run([DXC, "-T", "ps_6_0", "-E", "main", os.path.join(OUT, "debug.ps.hlsl"), "-Fo",
                    os.path.join(OUT, "debug.ps.dxil")], check=True, capture_output=True)
    bins = {os.path.basename(p)[:-4]: open(os.path.join(ROOT, "build", "shaders_split", p), "rb").read()
            for p in os.listdir(os.path.join(ROOT, "build", "shaders_split")) if p.endswith(".bin")}
    objects = shader_objects(os.path.join(capture, "shader_inits.bin"), bins)
    buffers = load_buffers(os.path.join(capture, "buffers.bin"))
    textures = load_textures(os.path.join(capture, "gpu_textures.bin" if args.gpu_textures else "textures.bin"))
    texture_index, table = build_texture_table(textures)
    open(os.path.join(OUT, "textures.rgba"), "wb").write(table)
    open(os.path.join(OUT, "cubes.rgba"), "wb").write(build_cube_table(textures, texture_index))
    from xenos_mips import load_mips, build_mip_table
    mip_capture = os.path.join(capture, "gpu_mips.bin")
    mips = load_mips(mip_capture, register_stream_hash(open(os.path.join(capture, "xenia_regs.bin"), "rb").read())) if os.path.exists(mip_capture) else {}
    open(os.path.join(OUT, "mips.rgba"), "wb").write(build_mip_table(mips, texture_index))
    draws = [r for r in load(os.path.join(capture, "frame_capture.bin"), True) if r[0][1] != 3]
    for header, _, objs in draws:
        # BeginVertices through D3D's user-pointer draw wrapper (returns to
        # 0x8250D964, the 2D layer): the completed region has the draw. Other
        # kind-2 calls are nested in an engine draw whose packet would be
        # duplicated.
        if header[1] == 2 and "up" in objs and header[2] == 0x8250D964 and not os.environ.get("FD_NO_UP"):
            objs["commands"] = objs["up"][1]
    compiled, records, skipped, vertex_cache = {}, [], {}, {}
    record_index = []
    streams, blob = {}, bytearray()  # pulled vertex streams: address -> (blob offset, size)
    # GPU registers persist across frames: start from the state the previous
    # frame ended in, approximated by this frame's final state (two passes).
    regs = {}
    for _, _, objs in load(os.path.join(capture, "frame_capture.bin"), True):  # Swap included: the frame ends there
        track_registers(regs, objs.get("commands", b""))
    # The Swap's commands are not captured; D3D leaves blending off (ONE, ZERO)
    # between frames, which the first passes (the simulation) rely on.
    for r in (0x2201, 0x2209, 0x220A, 0x220B):
        regs[r] = 0x00010001
    print("initial RB_BLENDCONTROL0 %08X RB_COLOR_MASK %08X RB_DEPTHCONTROL %08X" % (regs.get(0x2201, 0), regs.get(0x2104, 0), regs.get(0x2200, 0)))
    xenia, matched, draw_ordinal = load_xenia_registers(os.path.join(capture, "xenia_regs.bin")), 0, 0
    submitted = load_gpu_draws(os.path.join(capture, "gpu_draws.bin"), os.path.join(capture, "xenia_regs.bin"), len(xenia))
    alignment = align_xenia(draws, xenia)
    tracked = regs
    for cpu_record, (header, device, objs) in enumerate(draws):
        before_clear = dict(tracked) if header[1] == 5 else None
        track_registers(tracked, objs.get("commands", b""))
        regs = tracked
        ordinal = draw_ordinal  # index among records with a draw (align_xenia)
        if header[1] not in (4, 5):
            draw_ordinal += 1
        if header[1] == 5:
            if "clear" in objs:
                depth, rectangles = objs["clear"]
                clear_regs = clear_registers(before_clear, objs)
                flags = header[6]
                for slot in range(4):
                    if not objs[0x3190 + slot * 4][0]:
                        flags &= ~(1 << slot)
                if not objs[0x31A0][0]:
                    flags &= ~0x30
                name = "#clear"
                rec = struct.pack("<I", len(name)) + name.encode()
                rec += struct.pack("<%dI" % len(STATE_REGISTERS), *(clear_regs.get(r, 0) for r in STATE_REGISTERS))
                rec += struct.pack("<IIfII", flags, header[7], depth, header[9], len(rectangles))
                rec += b"".join(struct.pack("<4i", *r) for r in rectangles)
                records.append(rec)
                record_index.append(dict(record=len(records) - 1, cpu_record=cpu_record, kind="clear",
                                         surface=clear_regs.get(0x2000, 0), color=clear_regs.get(0x2001, 0), flags=flags))
                skipped["clears"] = skipped.get("clears", 0) + 1
            continue
        if header[1] == 4:  # resolve (D3D internal draw)
            # D3DDevice_Resolve(device, flags, rect, dest texture, ...): the
            # destination fetch constant (object + 0x1C) gives the address.
            if "resolve" in objs:
                flags, texture, obj = objs["resolve"]
                dest = struct.unpack_from(">I", obj, 0x1C + 4)[0] & 0x1FFFF000 if texture else 0
                if dest:
                    # XDK's copy/clear command setup temporarily changes GPU
                    # targets; a resynchronised CPU command block may also
                    # contain inline data. The bound surface objects identify
                    # the actual source, just as they do for explicit clears.
                    regs = clear_registers(regs, objs)
                    name = "#resolve"
                    rec = struct.pack("<I", len(name)) + name.encode()
                    # Destination size: texture fetch dword 2 (width - 1 bits 0-12, height - 1 bits 13-25).
                    size = struct.unpack_from(">I", obj, 0x1C + 8)[0]
                    fetch = struct.unpack_from(">6I", obj, 0x1C)
                    copy_info, copy_x, copy_y, copy_mip, mip_count = copy_destination_offset(objs.get("commands", b"")[4:], fetch, xenia)
                    rec += struct.pack("<12I", dest, flags, regs.get(0x2000, 0), regs.get(0x2001, 0),
                                       regs.get(0x2002, 0), copy_info,
                                       (size & 0x1FFF) + 1, ((size >> 13) & 0x1FFF) + 1, copy_x, copy_y, copy_mip, mip_count)
                    records.append(rec)
                    record_index.append(dict(record=len(records) - 1, cpu_record=cpu_record, kind="resolve",
                                             destination="%08X" % dest, width=(size & 0x1FFF) + 1,
                                             height=((size >> 13) & 0x1FFF) + 1, x=copy_x, y=copy_y,
                                             mip=copy_mip, copy_info=copy_info, source=flags & 7,
                                             surface=regs.get(0x2000, 0), color=regs.get(0x2001, 0)))
                    skipped["resolves"] = skipped.get("resolves", 0) + 1
            continue
        vs_obj = objs[0x3290][0]
        name = objects.get(vs_obj, (0, None))[1]
        # DRAW_INDX: [viz query, initiator, index base, size]; DRAW_INDX_2 (auto-indexed
        # rects of post passes): [initiator, ...], normalised to the same layout.
        draw = scan_draw(objs.get("commands", b"")[4:]) or next(((n, p if n.startswith("DRAW_INDX(") or n == "DRAW_INDX" else (0,) + tuple(p))
                     for n, p in packets(objs.get("commands", b"")[4:])
                     if n.split("(")[0] in ("DRAW_INDX", "DRAW_INDX_2")), None) or scan_draw(objs.get("commands", b"")[4:])
        source = (draw[1][1] >> 6) & 3 if draw and len(draw[1]) >= 2 else 3
        if not name or not draw or len(draw[1]) < 2 or source not in (0, 2) or (source == 0 and len(draw[1]) < 3):
            skipped["no vs/indexed draw"] = skipped.get("no vs/indexed draw", 0) + 1
            continue
        # Exact GPU state: Xenia's register file at the matching draw (same
        # VGT_DRAW_INITIATOR and, when indexed, VGT_DMA_BASE), in order.
        k = alignment.get(ordinal)
        if submitted is not None and k is not None and not submitted[k]:
            skipped["not submitted by GPU backend"] = skipped.get("not submitted by GPU backend", 0) + 1
            continue
        if k is not None:
            regs = dict(tracked)
            regs.update(xenia[k])
            matched += 1
        prim, count, index32 = draw[1][1] & 0x3F, draw[1][1] >> 16, (draw[1][1] >> 11) & 1
        original_count = count
        indices = buffers.get(draw[1][2] & 0x1FFFFFFF) if source == 0 else b""
        vfetch = struct.unpack_from(">2I", device, 0x480 + 190 * 4)
        if "up" in objs and 0x48BE in tracked:  # BeginVertices: fetch 95 set in the stream
            vfetch = (tracked[0x48BE], tracked[0x48BF])
        vertices = buffers.get(vfetch[0] & 0x1FFFFFFC)
        stride, layout, pulls = vertex_layout(objs["vs_code"][1], name)
        elements = [(off, v) for off, v, _ in layout]
        if indices is None or vertices is None or not stride or not elements or prim not in (4, 8):
            skipped["missing data/layout"] = skipped.get("missing data/layout", 0) + 1
            continue
        semantics = [sem for _, _, sem in layout]
        if not elements or set(semantics) != set(hlsl_inputs(name)):
            skipped["vertex format/semantics"] = skipped.get("vertex format/semantics", 0) + 1
            continue
        if name not in compiled:
            compiled[name] = compile_shader(name, "vs")
        if not compiled[name]:
            raise RuntimeError("Vertex shader %s failed; see %s.vs.compile.log" % (name, name))
        if source == 0:
            n = len(indices) // (4 if index32 else 2)
            if n < count:
                raise ValueError("CPU record %d needs %d indices but captured buffer has only %d" % (cpu_record, count, n))
            index_le = struct.pack("<%d%s" % (n, "I" if index32 else "H"), *struct.unpack(">%d%s" % (n, "I" if index32 else "H"), indices))
        else:  # auto-indexed: vertices 0..count-1
            index32 = 1
            index_le = struct.pack("<%dI" % count, *range(count))
        gpu_state = xenia[k] if k is not None else {}
        constants = (struct.pack("<1024I", *(gpu_state[r] for r in range(0x4000, 0x4400)))
                     if 0x4000 in gpu_state else swap32(device[0x780:0x780 + 4096]))
        vertex_key = (vfetch[0] & 0x1FFFFFFC, stride,
                      tuple((offset, v["format"], v["is_signed"], v["num_format"], v["exp_adjust"])
                            for offset, v in elements))
        vdata = vertex_cache.get(vertex_key)
        if vdata is None:
            vdata = b"".join(convert_element(vertices, None if offset is None else base + offset, v)
                             for base in range(0, len(vertices) - stride + 1, stride)
                             for offset, v in elements)
            vertex_cache[vertex_key] = vdata
        if prim == 8:  # rectangle list: expand the referenced vertices to triangles
            order = struct.unpack("<%d%s" % (len(index_le) // (4 if index32 else 2), "I" if index32 else "H"), index_le)
            size = 16 * len(elements)
            picked = b"".join(vdata[i * size:(i + 1) * size] for i in order[:count] if (i + 1) * size <= len(vdata))
            vdata, index_le, count = expand_rects(picked, len(elements))
            index32, prim = 0, 4
        rec = struct.pack("<I", len(name)) + name.encode()
        rec += struct.pack("<3I", prim, count, len(index_le)) + index_le
        rec += struct.pack("<2I", 16 * len(elements), len(vdata)) + vdata
        rec += struct.pack("<I", len(semantics)) + b"".join(struct.pack("<I", len(x)) + x.encode() for x in semantics)
        rec += constants + struct.pack("<I", index32)
        rec += struct.pack("<%dI" % len(STATE_REGISTERS), *(regs.get(r, 0) for r in STATE_REGISTERS))
        slots = []
        for slot in range(32):
            fetch = struct.unpack_from(">6I", device, 0x480 + slot * 24)
            slots.append(texture_index.get(fetch[1] & 0xFFFFF000, -1) if fetch[0] & 3 == 2 else -1)
        rec += struct.pack("<32i", *slots)
        ps_name = objects.get(objs[0x328C][0], (0, ""))[1] if objs[0x328C][0] else ""
        if ps_name and ("ps", ps_name) not in compiled:
            compiled[("ps", ps_name)] = compile_shader(ps_name, "ps")
        if ps_name and not compiled[("ps", ps_name)]:
            raise RuntimeError("Pixel shader %s failed; see %s.ps.compile.log" % (ps_name, ps_name))
        rec += struct.pack("<I", len(ps_name)) + ps_name.encode()
        # Engine globals such as ps_ZBufferParam use c230: keep the full
        # 256-register bank, just like the vertex shader constants above.
        rec += (struct.pack("<1024I", *(gpu_state[r] for r in range(0x4400, 0x4800)))
                if 0x4400 in gpu_state else swap32(device[0x1780:0x1780 + 4096]))
        bases = []
        for slot in range(32):
            fetch = struct.unpack_from(">6I", device, 0x480 + slot * 24)
            bases.append(fetch[1] & 0x1FFFF000 if fetch[0] & 3 == 2 else 0)
        rec += struct.pack("<32I", *bases)
        # Fetch swizzle per slot (texture fetch dword 3 bits 1-12).
        rec += struct.pack("<32I", *((struct.unpack_from(">6I", device, 0x480 + slot * 24)[3] >> 1) & 0xFFF
                                     for slot in range(32)))
        # Boolean (0x4900-0x4907: vertex 0x4900+, pixel 0x4904+) and loop
        # constants (0x4908-0x4927, vertex shader loops).
        rec += struct.pack("<40I", *(regs.get(r, 0) for r in range(0x4900, 0x4928)))
        rec += vertex_fetch_descriptors(pulls, device, buffers, streams, blob)
        # Texture fetch constant exp_adjust per slot (dword 3 bits 13-18, signed).
        rec += struct.pack("<32i", *((((struct.unpack_from(">6I", device, 0x480 + slot * 24)[3] >> 13) & 0x3F) ^ 32) - 32
                                     for slot in range(32)))
        rec += struct.pack("<32I", *(fetch[1] & 0x3F if fetch[0] & 3 == 2 else 0xFFFFFFFF
                                     for fetch in (struct.unpack_from(">6I", device, 0x480 + slot * 24)
                                                   for slot in range(32))))
        rec += struct.pack("<32I", *((fetch[0] >> 10 & 0x1FF) | ((fetch[3] >> 19 & 0x1FF) << 9)
                                     if fetch[0] & 3 == 2 else 0xFFFFFFFF
                                     for fetch in (struct.unpack_from(">6I", device, 0x480 + slot * 24)
                                                   for slot in range(32))))
        rec += swap32(device[0x480:0x480 + 32 * 24])
        rec += struct.pack("<1024I", *(regs.get(r, 0) for r in range(0x2000, 0x2400)))
        records.append(rec)
        record_index.append(dict(record=len(records) - 1, cpu_record=cpu_record, kind="draw",
                                 vs=name, ps=ps_name, indices=original_count, gpu_draw=k,
                                 surface=regs.get(0x2000, 0), color=regs.get(0x2001, 0)))
    open(os.path.join(OUT, "streams.bin"), "wb").write(blob)
    with open(os.path.join(OUT, "replay.bin"), "wb") as f:
        f.write(struct.pack("<2I", 0x39524446, len(records)))
        for rec in records:
            f.write(rec)
    with open(os.path.join(OUT, "replay-index.json"), "w", encoding="utf-8") as f:
        json.dump(dict(capture_sha256=hashlib.sha256(open(os.path.join(capture, "frame_capture.bin"), "rb").read()).hexdigest(),
                       textures=[dict(index=i, base="%08X" % (base & 0x1FFFFFFF)) for base, i in texture_index.items()],
                       records=record_index), f, indent=2)
    print("matched %d draws to Xenia's register file (%d GPU draws)" % (matched, len(xenia)))
    print("packed %d of %d draws; skipped %s; shaders compiled %d, failed %s" % (len(records), len(draws), skipped,
          sum(compiled.values()), [k for k, v in compiled.items() if not v]))


if __name__ == "__main__":
    main()
