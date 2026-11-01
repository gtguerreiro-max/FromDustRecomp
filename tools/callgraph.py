"""Call graph summary for a range of Default.xex functions: per-frame call rate
(from build/func_counts.txt, if present), callers count, and direct callees
annotated with names from a small table of identified functions.

Usage: python tools/callgraph.py <low hex> <high hex> [--fps 30]
"""
import collections, glob, os, re, sys

KNOWN = {
    0x820D3F00: "SetRenderTarget", 0x820D4528: "SetRenderTarget'", 0x820D0E88: "ScissorHelper",
    0x820DA538: "SetVertexShaderConstantF", 0x820DA610: "SetPixelShaderConstantF",
    0x8250DDA0: "DrawIndexedVertices", 0x8250D988: "DrawVertices", 0x8250D480: "DrawVertices'",
    0x820E0AC8: "Swap", 0x820D7D88: "CmdBufferMakeRoom", 0x820EA830: "WriteContextRegs",
    0x820EABC8: "UploadConstants", 0x820EB9F0: "SetFetchConstant",
}


def main():
    low, high = int(sys.argv[1], 16), int(sys.argv[2], 16)
    fps = float(sys.argv[sys.argv.index("--fps") + 1]) if "--fps" in sys.argv else 30.0
    rate = {}
    if os.path.exists("build/func_counts.txt"):
        for line in open("build/func_counts.txt"):
            if not line.startswith("#"):
                a, r = line.split()
                rate[int(a, 16)] = float(r) / fps
    callees = collections.defaultdict(collections.Counter)
    callers = collections.Counter()
    for path in glob.glob("ppc/Default/ppc_recomp.*.cpp"):
        current = None
        for line in open(path, encoding="utf-8", errors="ignore"):
            m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)", line)
            if m:
                current = int(m.group(1), 16)
                continue
            for t in re.findall(r"\b(?:__imp__)?sub_([0-9A-F]{8})\(ctx, base\)", line):
                callers[int(t, 16)] += 1
                if current is not None and low <= current < high:
                    callees[current][int(t, 16)] += 1
            for t in re.findall(r"__imp__([A-Za-z]\w+)\(ctx, base\)", line):
                if current is not None and low <= current < high:
                    callees[current][t] += 1
    funcs = sorted(set(callees) | {a for a in rate if low <= a < high})
    for f in funcs:
        names = []
        for c, n in callees[f].most_common():
            label = c if isinstance(c, str) else KNOWN.get(c, "%08X" % c)
            names.append(label + ("" if n == 1 else "x%d" % n))
        print("%08X %7.1f/f  callers=%-3d -> %s" % (f, rate.get(f, 0.0), callers[f], ", ".join(names[:10])))


if __name__ == "__main__":
    main()
