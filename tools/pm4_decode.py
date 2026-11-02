import struct
from pm4_scan import OPCODES


def packets(data):
    nwords = len(data) // 4
    words = struct.unpack(f">{nwords}I", data[:nwords * 4])
    total = len(words)
    out = []
    i = 0

    while i < total:
        h = words[i]
        kind = (h >> 30) & 3
        avail = total - i - 1

        if kind == 0:
            n = ((h >> 16) & 0x3FFF) + 1
            if n > avail:
                n = avail
            payload = words[i + 1:i + 1 + n]
            out.append(("REG %04X x%d" % ((h & 0x7FFF), n), payload))
            i += 1 + n

        elif kind == 1:
            n = 2
            if n > avail:
                n = avail
            payload = words[i + 1:i + 1 + n]
            out.append(("TYPE1", payload))
            i += 1 + n

        elif kind == 2:
            out.append(("NOP", ()))
            i += 1

        elif kind == 3:
            n = ((h >> 16) & 0x3FFF) + 1
            if n > avail:
                n = avail
            payload = words[i + 1:i + 1 + n]
            op = (h >> 8) & 0x7F
            name = OPCODES.get(op, "OP%02X" % op)
            if h & 1:
                name += "(p)"
            out.append((name, payload))
            i += 1 + n

        else:
            # Defensive: 32-bit PM4 headers should only have kinds 0..3.
            out.append(("BAD", ()))
            i += 1

    return out
