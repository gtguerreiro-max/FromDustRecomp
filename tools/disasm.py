"""Disassemble guest code from a dumped image.

Usage: python disasm.py <address_hex> [count=64] [module=Default]
"""
import os
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASES = {
    "Default": 0x82000000, "GalileoKernel": 0x88100000, "GalileoPathfind_9_9": 0x88480000,
    "GalileoSimulatorWater": 0x88500000, "GalileoSimulatorWater_9_9": 0x89180000,
    "GalileoTexture": 0x88300000, "dustai.x360.f": 0x8A000000,
}


def main():
    addr = int(sys.argv[1], 16)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    module = sys.argv[3] if len(sys.argv) > 3 else "Default"
    img = open(os.path.join(ROOT, "dump", module + ".bin"), "rb").read()
    base = BASES[module]
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    for i in range(count):
        a = addr + i * 4
        o = a - base
        word = struct.unpack(">I", img[o:o + 4])[0]
        ins = next(md.disasm(img[o:o + 4], a), None)
        text = "%-8s %s" % (ins.mnemonic, ins.op_str) if ins else ".long 0x%08X" % word
        print("%08X  %08X  %s" % (a, word, text))


if __name__ == "__main__":
    main()
