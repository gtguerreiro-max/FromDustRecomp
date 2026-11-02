import math
import struct

def f7e3_to_float(v: int) -> float:
    v &= 0x3FF
    if v == 0:
        return 0.0

    mantissa = v & 0x7F
    exponent = v >> 7

    if exponent == 0:
        lzcnt = 8 - mantissa.bit_length()
        exponent = 1 - lzcnt
        mantissa = (mantissa << lzcnt) & 0x7F

    ieee = ((exponent + 124) << 23) | (mantissa << 16)
    return struct.unpack('f', struct.pack('I', ieee & 0xFFFFFFFF))[0]

def float_to_f7e3(x: float) -> int:
    if math.isnan(x):
        return 0
    if x <= 0.0:
        return 0
    if x >= 32.0:
        return 0x3FF

    packed = struct.pack('f', x)
    u = struct.unpack('I', packed)[0]

    if u >= 0x41FF0000:
        return 0x3FF
    if u == 0:
        return 0

    e32 = u >> 23
    if e32 == 0:
        return 0

    if e32 < 125:
        f32 = struct.unpack('f', packed)[0]
        n = round(f32 * 512.0)
        if n >= 128:
            return 0x80
        if n < 0:
            return 0
        return n

    e = e32 - 124
    frac = u & 0x7FFFFF
    m = frac >> 16
    drop = frac & 0xFFFF
    sticky = drop & 0x7FFF

    if drop > 0x8000 or (drop == 0x8000 and (sticky != 0 or (m & 1) != 0)):
        m += 1

    if m == 128:
        e += 1
        m = 0
        if e > 7:
            return 0x3FF

    return (e << 7) | m

for v in range(0x400):
    assert float_to_f7e3(f7e3_to_float(v)) == v

print(f7e3_to_float(0x3FF))
