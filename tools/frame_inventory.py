"""Per-draw inventory of a captured frame (build/frame_capture.bin): primitive,
count, vertex/pixel shader (matched to build/shaders_split/*.bin by microcode),
bound textures (decoded fetch constants) and the vertex buffer (fetch 95).

Usage: python tools/frame_inventory.py [--summary]
"""
import collections, glob, os, struct, sys

sys.path.insert(0, os.path.dirname(__file__))
from analyze_capture import load
from xenos_fetch import decode_texture_fetch, decode_vertex_fetch
from pm4_decode import packets

PRIMITIVES = {1: "points", 2: "lines", 3: "line strip", 4: "triangles", 5: "fan", 6: "strip", 8: "rects", 13: "quads"}
FETCH_BASE = 0x480  # device copy of registers 0x4800..0x48BF


def shader_index():
    bins = {}
    for path in glob.glob("build/shaders_split/*.bin"):
        bins[os.path.basename(path)[:-4]] = open(path, "rb").read()
    return bins


def best_match(bins, candidates, code):
    """Candidate whose container holds the most dwords of `code` at the best
    alignment (patched vertex fetch words do not match; the rest does)."""
    words = [code[i:i + 4] for i in range(0, len(code) - 3, 4)]
    best, best_score = None, -1
    for name in candidates:
        data = bins[name]
        for start in range(0, len(data) - len(code) + 1, 4):
            score = sum(data[start + 4 * k:start + 4 * k + 4] == w for k, w in enumerate(words))
            if score > best_score:
                best, best_score = name, score
    return best, best_score, len(words)


def match(bins, head):
    hits = [name for name, data in bins.items() if head and head in data]
    return "/".join(hits[:2]) + ("+" if len(hits) > 2 else "") if hits else "?"


def main():
    bins = shader_index()
    draws = [r for r in load("build/frame_capture.bin", True) if r[0][1] != 3]
    stats = collections.Counter()
    code_sizes = {}
    for i, (header, device, objects) in enumerate(draws):
        kind, args = header[1], header[3:11]
        words = lambda off, n: list(struct.unpack_from(">%dI" % n, device, off))
        textures = []
        for slot in range(16):
            fetch = words(FETCH_BASE + slot * 24, 6)
            if fetch[0] & 3 == 2 and fetch[1] >> 12:
                t = decode_texture_fetch(fetch)
                textures.append("%d:%s %dx%d" % (slot, t["format_name"][2:], t["width"], t["height"]))
        vb = decode_vertex_fetch(words(FETCH_BASE + 190 * 4, 2))
        # Exact microcode sizes from the IM_LOAD packets this draw wrote (the
        # cursor points at the last written word, hence [4:]). The vertex
        # microcode starts 0x40 bytes after the object's code pointer.
        # A shader that stays bound is not reloaded: remember sizes per pointer.
        for name, payload in packets(objects.get("commands", b"")[4:]):
            if name.startswith("IM_LOAD") and len(payload) >= 2:
                stage = "vs" if payload[0] & 3 == 0 else "ps"
                code_sizes[(stage, objects[stage + "_code"][0])] = (payload[1] & 0xFFFF) * 4
        sizes = {stage: code_sizes[(stage, objects[stage + "_code"][0])]
                 for stage in ("vs", "ps") if (stage, objects[stage + "_code"][0]) in code_sizes}
        vs_code, ps_code = objects["vs_code"][1], objects["ps_code"][1]
        # Vertex microcode is patched in memory (vertex fetch bindings), so the
        # vertex shader is matched on the 0x40 bytes that precede it.
        vs = "-"
        if objects["vs_code"][0]:
            candidates = [n for n, d in bins.items() if vs_code[:0x40] in d] or list(bins)
            if len(candidates) > 1 and "vs" in sizes:
                name, score, total = best_match(bins, candidates, vs_code[0x40:0x40 + sizes["vs"]])
                vs = "%s(%d/%d)" % (name, score, total)
            else:
                vs = candidates[0] if candidates else "?"
        ps = match(bins, ps_code[:sizes["ps"]] if "ps" in sizes else ps_code[:32]) if objects["ps_code"][0] else "none (depth only)"
        prim = PRIMITIVES.get(args[1], str(args[1]))
        count = args[4] if kind == 0 else args[3]
        stats[(prim, vs, ps)] += 1
        if "--summary" not in sys.argv:
            print("%3d %-9s n=%-5d vs=%-9s ps=%-9s vb=%06X tex=[%s]" % (i, prim, count, vs, ps, vb["address"], ", ".join(textures)))
    print("\n%d draws, %d distinct (primitive, VS, PS) combinations; most common:" % (len(draws), len(stats)))
    for (prim, vs, ps), n in stats.most_common(12):
        print("  %4d  %-9s vs=%-9s ps=%s" % (n, prim, vs, ps))


if __name__ == "__main__":
    main()
