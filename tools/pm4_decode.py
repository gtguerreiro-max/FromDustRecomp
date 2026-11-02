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


def _valid_at(words, i):
    """Size of the packet at i if its header is plausible, else 0."""
    h = words[i]
    kind = h >> 30
    if kind == 0:
        n = ((h >> 16) & 0x3FFF) + 1
        base = h & 0x7FFF
        return 1 + n if i + 1 + n <= len(words) and base + (1 if (h >> 15) & 1 else n) <= 0x5003 else 0
    if kind == 1:
        return 3 if i + 3 <= len(words) else 0
    if kind == 2:
        return 1
    n = ((h >> 16) & 0x3FFF) + 1
    return 1 + n if ((h >> 8) & 0x7F) in OPCODES and i + 1 + n <= len(words) else 0


def _parses_to_end(words, i):
    while i < len(words):
        size = _valid_at(words, i)
        if not size:
            return False
        i += size
    return True


def packets_robust(data):
    """Like packets(), but an implausible header (unknown opcode, register out
    of range, payload past the end) is treated as inline data: parsing resumes
    at the first later word from which the stream parses cleanly to the end of
    the block (blocks end with the draw packet). D3D can leave data the GPU
    never executes as commands (e.g. constants) between packets."""
    nwords = len(data) // 4
    words = struct.unpack(">%dI" % nwords, data[:nwords * 4])
    good, i = bytearray(), 0
    spans = []
    while i < nwords:
        size = _valid_at(words, i)
        if not size:
            j = next((j for j in range(i + 1, nwords) if _parses_to_end(words, j)), nwords)
            i = j
            continue
        spans.append((i, size))
        i += size
    return [p for k, size in spans for p in packets(data[k * 4:(k + size) * 4])]
