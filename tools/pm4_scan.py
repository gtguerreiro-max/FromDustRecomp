"""Find guest functions that build Xenos PM4 type-3 packets, to identify the
statically linked D3D library without symbols.

A packet header is 0xC0000000 | (count - 1) << 16 | opcode << 8. In the
recompiled C it appears as `lis` (ctx.rN.s64 = <hi << 16>) followed by `ori`
or `addi` on the same register. Prints, per function, the opcodes it emits.

Usage: python tools/pm4_scan.py [ppc/Default]
"""
import collections, glob, os, re, sys

OPCODES = {
    0x10: "NOP", 0x21: "DRAW_INDX_BIN", 0x22: "DRAW_INDX", 0x23: "VIZ_QUERY", 0x25: "SET_STATE",
    0x26: "WAIT_FOR_IDLE", 0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE", 0x2D: "SET_CONSTANT",
    0x2E: "LOAD_CONSTANT_CONTEXT", 0x2F: "LOAD_ALU_CONSTANT", 0x34: "DRAW_INDX_IMM",
    0x36: "DRAW_INDX_2", 0x37: "SET_BIN_MASK", 0x3B: "INVALIDATE_STATE", 0x3C: "WAIT_REG_MEM",
    0x3D: "MEM_WRITE", 0x3E: "REG_RMW", 0x3F: "COND_EXEC", 0x44: "REG_TO_MEM", 0x45: "EVENT_WRITE_SHD",
    0x46: "EVENT_WRITE", 0x47: "SET_SHADER_BASES", 0x48: "ME_INIT", 0x4A: "INDIRECT_BUFFER",
    0x4B: "SET_BIN_MASK_LO", 0x4C: "SET_BIN_SELECT_LO", 0x50: "CONTEXT_UPDATE", 0x54: "INTERRUPT",
    0x55: "SET_CONSTANT2", 0x56: "SET_SHADER_CONSTANTS", 0x58: "EVENT_WRITE_EXT", 0x59: "EVENT_WRITE_ZPD",
    0x5B: "WAIT_REG_EQ", 0x5C: "WAIT_REG_GTE", 0x5D: "COND_WRITE", 0x5E: "EVENT_WRITE_CFL",
}
FUNC = re.compile(r"^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
LIS = re.compile(r"ctx\.(r\d+)\.s64 = (-?\d+);")
LOW = re.compile(r"ctx\.r\d+\.(?:u64 = ctx\.(r\d+)\.u64 \| |s64 = ctx\.(r\d+)\.s64 \+ )(-?\d+);")


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "ppc/Default"
    emitted = collections.defaultdict(collections.Counter)
    for path in glob.glob(os.path.join(folder, "ppc_recomp.*.cpp")):
        func, pending = None, {}
        for line in open(path, encoding="utf-8", errors="ignore"):
            m = FUNC.match(line)
            if m:
                func, pending = int(m.group(1), 16), {}
                continue
            m = LIS.search(line)
            if m:
                value = int(m.group(2)) & 0xFFFFFFFF
                if value & 0xFFFF == 0 and value >> 30 == 3 and (value >> 16) & 0x3FFF < 0x800:
                    pending[m.group(1)] = value
                else:
                    pending.pop(m.group(1), None)
                continue
            m = LOW.search(line)
            source = m and (m.group(1) or m.group(2))
            if m and source in pending:
                value = (pending[source] + (int(m.group(3)) & 0xFFFF)) & 0xFFFFFFFF
                opcode = (value >> 8) & 0x7F
                if value & 0xFE == 0 and opcode in OPCODES:  # bit 0: predicated
                    emitted[func][OPCODES[opcode]] += 1
    for func in sorted(emitted):
        print("%08X  %s" % (func, ", ".join("%s%s" % (k, "" if v == 1 else " x%d" % v) for k, v in sorted(emitted[func].items()))))
    print("%d functions emit PM4 packets" % len(emitted), file=sys.stderr)


if __name__ == "__main__":
    main()
