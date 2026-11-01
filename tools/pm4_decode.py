"""Split Xenos PM4 command bytes (big-endian) into packets.

packets(data) -> list of (name, payload words). Type 0 = register writes
("REG base xN"), type 3 = opcode (names from pm4_scan.OPCODES; "(p)" when
predicated), type 2 = NOP.
"""
import struct
from pm4_scan import OPCODES


def packets(data):
    words = struct.unpack(">%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
    out, i = [], 0
    while i < len(words):
        h = words[i]
        kind = h >> 30
        if kind == 0:
            n = ((h >> 16) & 0x3FFF) + 1
            out.append(("REG %04X x%d" % (h & 0x7FFF, n), words[i + 1:i + 1 + n]))
            i += 1 + n
        elif kind == 3:
            n = ((h >> 16) & 0x3FFF) + 1
            op = (h >> 8) & 0x7F
            out.append((OPCODES.get(op, "OP%02X" % op) + ("(p)" if h & 1 else ""), words[i + 1:i + 1 + n]))
            i += 1 + n
        elif kind == 2:
            out.append(("NOP", ()))
            i += 1
        else:
            out.append(("TYPE1", words[i + 1:i + 3]))
            i += 3
    return out
