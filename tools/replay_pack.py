"""Pack a captured frame (build/frame_capture.bin, buffers.bin, shader_inits.bin)
into build/replay/replay.bin for tools/fd_replay, and compile the vertex
shaders it needs to DXIL (build/replay/<container>.vs.dxil).

Each draw: indexed triangles drawn with its original vertex shader (XenosRecomp
HLSL) and a debug pixel shader, so geometry and constants can be checked.

replay.bin (little-endian):
  u32 'FDRP', u32 draw count, then per draw:
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
  4096 bytes pixel shader constants (c0..c223, swapped, zero padded),
  then u32 x 32: texture base address per fetch slot (0 if none),
  then u32 x 32: fetch swizzle per slot (4 x 3 bits: 0-3 XYZW, 4 zero, 5 one),
  then u32 x 40: registers 0x4900-0x4927 (boolean and loop constants).
A record whose vs name is "#resolve" is an EDRAM -> texture copy instead:
  u32 RB_COPY_DEST_BASE, RB_COPY_CONTROL, RB_SURFACE_INFO, RB_COLOR_INFO,
  RB_DEPTH_INFO, RB_COPY_DEST_INFO.

textures.rgba: u32 count, per texture u32 width, u32 height, RGBA8 pixels
(decoded by xenos_texture.py; unsupported formats become 1x1 white).
"""
import os, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(__file__))
from analyze_capture import load
from pm4_decode import packets
from xenos_vfetch import decode_vfetch
from xenos_fetch import decode_texture_fetch
import xenos_texture

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
        _, address, length = struct.unpack_from("<3I", data, i)
        out[address] = data[i + 12:i + 12 + length]
        i += 12 + length
    return out


def shader_objects(path, bins):
    """Vertex shader object -> container name, from shader_inits.bin."""
    data, rec, out = open(path, "rb").read(), 16 + 1024, {}
    for i in range(0, len(data), rec):
        _, kind, obj, _ = struct.unpack_from("<4I", data, i)
        code = data[i + 16:i + 16 + 1024]
        for length in (1024, 512, 256, 128, 64, 32, 16):
            hits = [n for n, b in bins.items() if code[:length] in b]
            if hits:
                out[obj] = (kind, hits[0])
                break
    return out


def live_vfetches(code):
    """Vertex fetch instructions in live microcode (D3D patches format, offset,
    stride and fetch constant into them at bind time), in address order."""
    words = struct.unpack(">%dI" % (len(code) // 4), code[: len(code) // 4 * 4])
    out = []
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
                   0x2003, 0x2004, 0x2005, 0x2209, 0x220A, 0x220B]  # RB_COLOR1-3_INFO, RB_BLENDCONTROL1-3


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
            return ("DRAW_INDX", p) if op == 0x22 else ("DRAW_INDX_2", (0,) + tuple(p))
    return None


def track_registers(regs, commands):
    """Apply a draw's type-0 register writes (commands start one word in)."""
    for name, payload in packets(commands[4:]):
        if name.startswith("REG "):
            base = int(name.split()[1], 16)
            for k, value in enumerate(payload):
                regs[base + k] = value


def load_textures(path):
    """Captured textures (frame_capture.cpp): base address -> (fetch words, bytes)."""
    data, i, out = open(path, "rb").read(), 0, {}
    while i < len(data):
        fetch = list(struct.unpack_from("<6I", data, i + 4))
        length = struct.unpack_from("<I", data, i + 28)[0]
        out[fetch[1] & 0xFFFFF000] = (fetch, data[i + 32:i + 32 + length])
        i += 32 + length
    return out


def build_texture_table(textures):
    """Decode every captured texture to RGBA8; returns (base -> index, table bytes)."""
    index, blobs = {}, []
    for base, (fetch, raw) in textures.items():
        try:
            width, height, rgba = xenos_texture.decode_texture(raw, decode_texture_fetch(fetch))
        except Exception:
            width, height, rgba = 1, 1, bytes([255] * 4)
        index[base] = len(blobs)
        blobs.append(struct.pack("<2I", width, height) + bytes(rgba))
    return index, struct.pack("<I", len(blobs)) + b"".join(blobs)


def main():
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, "debug.ps.hlsl"), "w").write(DEBUG_PS)
    subprocess.run([DXC, "-T", "ps_6_0", "-E", "main", os.path.join(OUT, "debug.ps.hlsl"), "-Fo",
                    os.path.join(OUT, "debug.ps.dxil")], check=True, capture_output=True)
    bins = {os.path.basename(p)[:-4]: open(os.path.join(ROOT, "build", "shaders_split", p), "rb").read()
            for p in os.listdir(os.path.join(ROOT, "build", "shaders_split")) if p.endswith(".bin")}
    objects = shader_objects(os.path.join(ROOT, "build", "shader_inits.bin"), bins)
    buffers = load_buffers(os.path.join(ROOT, "build", "buffers.bin"))
    texture_index, table = build_texture_table(load_textures(os.path.join(ROOT, "build", "textures.bin")))
    open(os.path.join(OUT, "textures.rgba"), "wb").write(table)
    draws = [r for r in load(os.path.join(ROOT, "build", "frame_capture.bin"), True) if r[0][1] != 3]
    compiled, records, skipped = {}, [], {}
    streams, blob = {}, bytearray()  # pulled vertex streams: address -> (blob offset, size)
    regs = {}
    for header, device, objs in draws:
        track_registers(regs, objs.get("commands", b""))
        if header[1] == 4:  # resolve / clear (D3D internal draw)
            # D3DDevice_Resolve(device, flags, rect, dest texture, ...): the
            # destination fetch constant (object + 0x1C) gives the address.
            if "resolve" in objs:
                flags, texture, obj = objs["resolve"]
                dest = struct.unpack_from(">I", obj, 0x1C + 4)[0] & 0x1FFFF000 if texture else 0
                if dest:
                    name = "#resolve"
                    rec = struct.pack("<I", len(name)) + name.encode()
                    rec += struct.pack("<6I", dest, flags, regs.get(0x2000, 0), regs.get(0x2001, 0),
                                       regs.get(0x2002, 0), 0)
                    records.append(rec)
                    skipped["resolves"] = skipped.get("resolves", 0) + 1
            continue
        vs_obj = objs[0x3290][0]
        name = objects.get(vs_obj, (0, None))[1]
        # DRAW_INDX: [viz query, initiator, index base, size]; DRAW_INDX_2 (auto-indexed
        # rects of post passes): [initiator, ...], normalised to the same layout.
        draw = next(((n, p if n.startswith("DRAW_INDX(") or n == "DRAW_INDX" else (0,) + tuple(p))
                     for n, p in packets(objs.get("commands", b"")[4:])
                     if n.split("(")[0] in ("DRAW_INDX", "DRAW_INDX_2")), None) or scan_draw(objs.get("commands", b"")[4:])
        source = (draw[1][1] >> 6) & 3 if draw and len(draw[1]) >= 2 else 3
        if not name or not draw or len(draw[1]) < 2 or source not in (0, 2) or (source == 0 and len(draw[1]) < 3):
            skipped["no vs/indexed draw"] = skipped.get("no vs/indexed draw", 0) + 1
            continue
        prim, count, index32 = draw[1][1] & 0x3F, draw[1][1] >> 16, (draw[1][1] >> 11) & 1
        indices = buffers.get(draw[1][2] & 0x1FFFFFFF) if source == 0 else b""
        vfetch = struct.unpack_from(">2I", device, 0x480 + 190 * 4)
        vertices = buffers.get(vfetch[0] & 0x1FFFFFFC)
        stride, layout, pulls = vertex_layout(objs["vs_code"][1][0x40:], name)
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
            skipped["vs compile"] = skipped.get("vs compile", 0) + 1
            continue
        if source == 0:
            n = len(indices) // (4 if index32 else 2)
            index_le = struct.pack("<%d%s" % (n, "I" if index32 else "H"), *struct.unpack(">%d%s" % (n, "I" if index32 else "H"), indices))
        else:  # auto-indexed: vertices 0..count-1
            index32 = 1
            index_le = struct.pack("<%dI" % count, *range(count))
        constants = swap32(device[0x780:0x780 + 4096])
        vdata = b"".join(convert_element(vertices, None if offset is None else base + offset, v)
                         for base in range(0, len(vertices) - stride + 1, stride)
                         for offset, v in elements)
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
            ps_name = ""
        rec += struct.pack("<I", len(ps_name)) + ps_name.encode()
        rec += swap32(device[0x1780:0x1780 + 3584]) + bytes(512)
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
        records.append(rec)
    open(os.path.join(OUT, "streams.bin"), "wb").write(blob)
    with open(os.path.join(OUT, "replay.bin"), "wb") as f:
        f.write(struct.pack("<2I", 0x50524446, len(records)))
        for rec in records:
            f.write(rec)
    print("packed %d of %d draws; skipped %s; shaders compiled %d, failed %s" % (len(records), len(draws), skipped,
          sum(compiled.values()), [k for k, v in compiled.items() if not v]))


if __name__ == "__main__":
    main()
