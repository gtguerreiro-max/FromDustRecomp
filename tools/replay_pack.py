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
  u32 index32.
"""
import os, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(__file__))
from analyze_capture import load
from pm4_decode import packets
from xenos_vfetch import decode_vfetch

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


def vertex_layout(code):
    """Vertex fetches of stream fetch constant 95 in live microcode: stride and elements."""
    words = struct.unpack(">%dI" % (len(code) // 4), code[: len(code) // 4 * 4])
    stride, elements = 0, {}
    for i in range(len(words) - 2):
        w = list(words[i:i + 3])
        if w[0] & 0x1F or not (w[0] >> 19) & 1:
            continue
        v = decode_vfetch(w)
        if v["fetch_constant_index"] != 95 or not 0 < v["stride"] <= 64:
            continue
        stride = stride or v["stride"]
        elements.setdefault(v["offset"] * 4, v)  # microcode order is kept (dict order)
    return stride * 4, list(elements.items())


def convert_element(data, offset, v):
    """One vertex element (big-endian guest bytes) -> 4 floats, like the vertex fetch unit."""
    code, n = COMPONENTS[v["format"]]
    size = struct.calcsize(code)
    raw = data[offset:offset + size * n]
    if v["format"] == 6:  # 8_8_8_8 is one dword: components from its little end (Xenos order W Z Y X)
        values = list(raw[::-1]) if len(raw) == 4 else [0] * 4
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


def compile_vs(name):
    src = os.path.join(ROOT, "build", "shaders_split", name + ".hlsl")
    hlsl = open(src, encoding="utf-8", errors="replace").read() + "\nuint g_SpecConstants() { return 0; }\n"
    tmp = os.path.join(OUT, name + ".vs.hlsl")
    dst = os.path.join(OUT, name + ".vs.dxil")
    open(tmp, "w", encoding="utf-8").write(hlsl)
    r = subprocess.run([DXC, "-T", "vs_6_0", "-E", "main", "-HV", "2021", "-all-resources-bound",
                        "-Wno-ignored-attributes", tmp, "-Fo", dst], capture_output=True, text=True)
    return r.returncode == 0


DEBUG_PS = """// fd_replay debug pixel shader: shades geometry by depth (near = bright).
float4 main(float4 position : SV_Position) : SV_Target
{
    float shade = saturate(1.0 - position.z);
    return float4(shade, shade * 0.8, 0.4 + 0.6 * shade, 1.0);
}
"""


def main():
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, "debug.ps.hlsl"), "w").write(DEBUG_PS)
    subprocess.run([DXC, "-T", "ps_6_0", "-E", "main", os.path.join(OUT, "debug.ps.hlsl"), "-Fo",
                    os.path.join(OUT, "debug.ps.dxil")], check=True, capture_output=True)
    bins = {os.path.basename(p)[:-4]: open(os.path.join(ROOT, "build", "shaders_split", p), "rb").read()
            for p in os.listdir(os.path.join(ROOT, "build", "shaders_split")) if p.endswith(".bin")}
    objects = shader_objects(os.path.join(ROOT, "build", "shader_inits.bin"), bins)
    buffers = load_buffers(os.path.join(ROOT, "build", "buffers.bin"))
    draws = [r for r in load(os.path.join(ROOT, "build", "frame_capture.bin"), True) if r[0][1] != 3]
    compiled, records, skipped = {}, [], {}
    for header, device, objs in draws:
        vs_obj = objs[0x3290][0]
        name = objects.get(vs_obj, (0, None))[1]
        draw = next(((n, p) for n, p in packets(objs.get("commands", b"")[4:]) if n.startswith("DRAW_INDX") and not n.startswith("DRAW_INDX_")), None)
        # The saved commands end one word early (frame_capture.cpp), so the
        # last DRAW_INDX may lack its index size word; only words 1-2 are needed.
        if not name or not draw or len(draw[1]) < 3 or (draw[1][1] >> 6) & 3:
            skipped["no vs/indexed draw"] = skipped.get("no vs/indexed draw", 0) + 1
            continue
        prim, count, index32 = draw[1][1] & 0x3F, draw[1][1] >> 16, (draw[1][1] >> 11) & 1
        indices = buffers.get(draw[1][2] & 0x1FFFFFFF)
        vfetch = struct.unpack_from(">2I", device, 0x480 + 190 * 4)
        vertices = buffers.get(vfetch[0] & 0x1FFFFFFC)
        stride, elements = vertex_layout(objs["vs_code"][1][0x40:])
        if indices is None or vertices is None or not stride or not elements or prim != 4:
            skipped["missing data/layout"] = skipped.get("missing data/layout", 0) + 1
            continue
        # No texture bindings yet: skip vertex shaders that fetch textures (terrain heightfield).
        if uses_textures(name):
            skipped["vs fetches textures"] = skipped.get("vs fetches textures", 0) + 1
            continue
        semantics = hlsl_inputs(name)
        if any(v["format"] not in COMPONENTS for _, v in elements) or len(semantics) != len(elements):
            skipped["vertex format/semantics"] = skipped.get("vertex format/semantics", 0) + 1
            continue
        if name not in compiled:
            compiled[name] = compile_vs(name)
        if not compiled[name]:
            skipped["vs compile"] = skipped.get("vs compile", 0) + 1
            continue
        n = len(indices) // (4 if index32 else 2)
        index_le = struct.pack("<%d%s" % (n, "I" if index32 else "H"), *struct.unpack(">%d%s" % (n, "I" if index32 else "H"), indices))
        constants = swap32(device[0x780:0x780 + 4096])
        rec = struct.pack("<I", len(name)) + name.encode()
        rec += struct.pack("<3I", prim, count, len(index_le)) + index_le
        vdata = b"".join(convert_element(vertices, base + offset, v)
                         for base in range(0, len(vertices) - stride + 1, stride) for offset, v in elements)
        rec += struct.pack("<2I", 16 * len(elements), len(vdata)) + vdata
        rec += struct.pack("<I", len(semantics)) + b"".join(struct.pack("<I", len(x)) + x.encode() for x in semantics)
        rec += constants + struct.pack("<I", index32)
        records.append(rec)
    with open(os.path.join(OUT, "replay.bin"), "wb") as f:
        f.write(struct.pack("<2I", 0x50524446, len(records)))
        for rec in records:
            f.write(rec)
    print("packed %d of %d draws; skipped %s; shaders %s" % (len(records), len(draws), skipped,
          {k: v for k, v in compiled.items()}))


if __name__ == "__main__":
    main()
