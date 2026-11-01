"""Split Xbox 360 shader containers out of game files and run each through
XenosRecomp (HLSL) and DXC (DXIL), reporting what converts.

Usage: python tools/shader_survey.py <out dir> <input file>...
Outputs land in <out dir> (derived from game data: keep it out of git).
"""
import os, struct, subprocess, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XENOS = os.path.join(ROOT, "XenosRecomp", "build", "XenosRecomp", "XenosRecomp.exe")
COMMON = os.path.join(ROOT, "XenosRecomp", "XenosRecomp", "shader_common.h")
DXC = os.path.join(ROOT, "XenosRecomp", "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")


def containers(data):
    # Same scan as XenosRecomp's directory mode (main.cpp).
    i = 0
    while i + 0x24 < len(data):
        flags, vsize, psize = struct.unpack_from(">III", data, i)
        f1c, f20 = struct.unpack_from(">II", data, i + 0x1C)
        size = vsize + psize
        if (flags & 0xFFFFFF00) == 0x102A1100 and size <= len(data) - i and f1c == 0 and f20 == 0 and size:
            yield data[i:i + size]
            i += size
        else:
            i += 2


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    seen, results = set(), collections.Counter()
    failures = collections.defaultdict(list)
    for path in sys.argv[2:]:
        for blob in containers(open(path, "rb").read()):
            if blob in seen:
                continue
            seen.add(blob)
            n = len(seen) - 1
            base = os.path.join(out, "%03d" % n)
            open(base + ".bin", "wb").write(blob)
            r = subprocess.run([XENOS, base + ".bin", base + ".hlsl", COMMON], capture_output=True, text=True, timeout=60)
            if r.returncode != 0 or not os.path.exists(base + ".hlsl"):
                results["recompiler crash"] += 1
                failures["recompiler crash"].append(n)
                continue
            hlsl = open(base + ".hlsl", encoding="utf-8", errors="replace").read()
            stage = "ps_6_0" if "SV_Target" in hlsl else "vs_6_0"
            # Shaders with specialization constants are linked at runtime (see XenosRecomp's dxc_compiler.cpp).
            target = "lib_6_3" if "g_SpecConstants" in hlsl else stage
            r = subprocess.run([DXC, "-T", target, "-HV", "2021", "-all-resources-bound",
                                "-Wno-ignored-attributes", "-DUNLEASHED_RECOMP", base + ".hlsl", "-Fo", base + ".dxil"],
                               capture_output=True, text=True, timeout=120)
            if r.returncode == 0:
                results["ok " + stage[:2]] += 1
            else:
                err = next((l for l in r.stderr.splitlines() if "error" in l), r.stderr.strip()[:120])
                key = err.split("error:", 1)[-1].strip()[:70]
                results["dxc error"] += 1
                failures[key].append(n)
                open(base + ".err.txt", "w").write(r.stderr)
    print("shaders:", len(seen))
    for k, v in results.most_common():
        print("  %-18s %d" % (k, v))
    print("failure kinds:")
    for k, v in sorted(failures.items(), key=lambda kv: -len(kv[1]))[:25]:
        print("  %4d  %s  e.g. %s" % (len(v), k, v[:5]))


if __name__ == "__main__":
    main()
