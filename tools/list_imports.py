"""List the kernel/XAM imports of every recompiled module.

Usage: python list_imports.py <game_dir> <dump_dir> [--json out.json]

Each import record in an XEX holds (type << 24) | ordinal: type 1 is a
function thunk (recompiled as __imp__Name), type 0 an import slot the loader
must fill (with a variable's guest address for kVariable exports).
"""
import json
import os
import re
import struct
import sys

from xex_dump import be32

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLES = os.path.join(ROOT, "XenonRecomp", "XenonUtils", "xbox")
MODULES = {
    "Default": "Default.xex",
    "GalileoKernel": "GalileoKernel.xex",
    "GalileoPathfind_9_9": "GalileoPathfind_9_9.xex",
    "GalileoSimulatorWater": "GalileoSimulatorWater.xex",
    "GalileoSimulatorWater_9_9": "GalileoSimulatorWater_9_9.xex",
    "GalileoTexture": "GalileoTexture.xex",
    "dustai.x360.f": "AInative/bin/dll/dustai.x360.f.xex",
}


def load_table(name):
    out = {}
    pat = re.compile(r"XE_EXPORT\(\w+,\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\)")
    for line in open(os.path.join(TABLES, name)):
        m = pat.search(line)
        if m:
            out[int(m.group(1), 16)] = (m.group(2), m.group(3))
    return out


def imports_of(xex_path, image, load_addr):
    b = open(xex_path, "rb").read()
    count = be32(b, 0x14)
    off = None
    for i in range(count):
        key, val = struct.unpack(">II", b[0x18 + i * 8:0x20 + i * 8])
        if key == 0x000103FF:
            off = val
    if off is None:
        return []
    _, str_size, nlibs = struct.unpack(">III", b[off:off + 12])
    names = [s.decode() for s in b[off + 12:off + 12 + str_size].split(b"\0") if s]
    o = off + 12 + str_size
    out = []
    for _ in range(nlibs):
        size = be32(b, o)
        lib = names[struct.unpack(">H", b[o + 0x24:o + 0x26])[0] & 0xFF]
        n = struct.unpack(">H", b[o + 0x26:o + 0x28])[0]
        for r in range(n):
            rec = be32(b, o + 0x28 + r * 4)
            value = be32(image, rec - load_addr)
            out.append((lib, rec, value >> 24, value & 0xFFFF))
        o += size
    return out


def main():
    game, dump = sys.argv[1], sys.argv[2]
    tables = {"xboxkrnl.exe": load_table("xboxkrnl_table.inc"), "xam.xex": load_table("xam_table.inc")}
    functions, variables = {}, {}
    for mod, rel in MODULES.items():
        image = open(os.path.join(dump, mod + ".bin"), "rb").read()
        pe = struct.unpack("<I", image[0x3C:0x40])[0]
        load_addr = struct.unpack("<I", image[pe + 0x34:pe + 0x38])[0]  # PE ImageBase
        for lib, rec, typ, ordinal in imports_of(os.path.join(game, rel), image, load_addr):
            name, kind = tables[lib].get(ordinal, ("%s_%X" % (lib.split(".")[0], ordinal), "kUnknown"))
            if typ == 1:
                functions.setdefault(name, {"lib": lib, "ordinal": ordinal, "modules": []})["modules"].append(mod)
            elif kind == "kVariable":
                variables.setdefault(name, {"lib": lib, "ordinal": ordinal, "slots": []})["slots"].append("0x%08X" % rec)

    print("function imports: %d unique" % len(functions))
    print("variable imports: %d unique" % len(variables))
    for name, v in sorted(variables.items()):
        print("   var %-32s %s" % (name, ", ".join(v["slots"])))
    if "--json" in sys.argv:
        with open(sys.argv[sys.argv.index("--json") + 1], "w") as f:
            json.dump({"functions": functions, "variables": variables}, f, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
