# xenos_tiling.py
"""Self-contained Python 3 port of Xenia texture_address.h."""

k_storage_pitch_height_alignment_blocks_log2 = 5
k_storage_pitch_height_alignment_blocks = 1 << k_storage_pitch_height_alignment_blocks_log2
k_storage_depth_alignment_blocks_log2 = 2
k_storage_depth_alignment_blocks = 1 << k_storage_depth_alignment_blocks_log2

k_macro_tile_width_log2 = 5
k_macro_tile_width = 1 << k_macro_tile_width_log2
k_macro_tile_height_2d_log2 = 5
k_macro_tile_height_2d = 1 << k_macro_tile_height_2d_log2
k_macro_tile_height_3d_log2 = 4
k_macro_tile_height_3d = 1 << k_macro_tile_height_3d_log2
k_macro_tile_depth_log2 = 2
k_macro_tile_depth = 1 << k_macro_tile_depth_log2

_MASK32 = 0xFFFFFFFF
_MASK64 = 0xFFFFFFFFFFFFFFFF


def _mask(value, bits):
    return value & ((1 << bits) - 1)


def _signed(value, bits):
    value &= (1 << bits) - 1
    sign_bit = 1 << (bits - 1)
    if value & sign_bit:
        value -= 1 << bits
    return value


def _left_shift(value, shift, bits):
    # C++ shift behavior is not defined for shifts >= bit width.  The usual
    # two's-complement hardware result is zero in that case, and this avoids
    # constructing huge Python integers for wrapped uint32 shift counts.
    if shift >= bits:
        return 0
    return _mask(value << shift, bits)


def _tiled_combine_bits(outer_inner_bytes, bank, pipe, y_lsb, bits):
    bank &= _MASK32
    pipe &= _MASK32
    y_lsb &= _MASK32

    assert bank <= 0b1
    assert pipe <= 0b11
    assert y_lsb <= 0b1

    outer = _signed(outer_inner_bytes, bits)

    # int32_t((y_lsb << 4) | (pipe << 6) | (bank << 11))
    first32 = _signed((y_lsb << 4) | (pipe << 6) | (bank << 11), 32)
    first = _signed(first32, bits)

    result = (
        _mask(first, bits)
        | (outer & 0b1111)
        | (((outer >> 4) & 0b1) << 5)
        | (((outer >> 5) & 0b111) << 8)
        | _left_shift(outer >> 8, 12, bits)
    )
    return _signed(result, bits)


def tiled_combine(outer_inner_bytes, bank, pipe, y_lsb):
    """32-bit instantiation of C++ TiledCombine (Address = int32_t)."""
    return _tiled_combine_bits(outer_inner_bytes, bank, pipe, y_lsb, 32)


def tiled_2d(x, y, pitch_aligned, bytes_per_block_log2):
    x = _signed(x, 32)
    y = _signed(y, 32)
    pitch = pitch_aligned & _MASK32
    assert not (pitch & (k_storage_pitch_height_alignment_blocks - 1))

    bytes_per_block_log2 &= _MASK32

    outer = (
        (y >> k_macro_tile_height_2d_log2)
        * (pitch >> k_macro_tile_width_log2)
        + (x >> k_macro_tile_width_log2)
    )
    outer_blocks = _left_shift(outer, 6, 32)

    inner_blocks = ((((y >> 1) & 0b111) << 3) | (x & 0b111))

    outer_inner_bytes = _left_shift(outer_blocks | inner_blocks, bytes_per_block_log2, 32)

    bank = (y >> 4) & 0b1
    pipe = ((x >> 3) & 0b11) ^ (((y >> 3) & 0b1) << 1)

    return tiled_combine(_signed(outer_inner_bytes, 32), bank, pipe, y & 0b1)


def tiled_3d(x, y, z, pitch_aligned, height_aligned, bytes_per_block_log2):
    x = _signed(x, 32)
    y = _signed(y, 32)
    z = _signed(z, 32)

    pitch = pitch_aligned & _MASK32
    height = height_aligned & _MASK32

    assert not (pitch & (k_storage_pitch_height_alignment_blocks - 1))
    assert not (height & (k_storage_pitch_height_alignment_blocks - 1))

    bytes_per_block_log2 &= _MASK32

    # C++ computes the final outer_blocks value as int32_t.  The intermediate
    # unsigned/signed mixing only affects the low 32 bits in two's-complement
    # hardware, so modulo-32 accumulation gives the same bit pattern.
    val = z >> k_macro_tile_depth_log2
    val = val * (height >> k_macro_tile_height_3d_log2)
    val = _mask(val + (y >> k_macro_tile_height_3d_log2), 32)
    val = _mask(val * (pitch >> k_macro_tile_width_log2), 32)
    val = _mask(val + (x >> k_macro_tile_width_log2), 32)
    outer_blocks = _left_shift(val, 7, 32)

    inner_blocks = (
        ((z & 0b11) << 5)
        | (((y >> 1) & 0b11) << 3)
        | (x & 0b111)
    )

    # outer_blocks | inner_blocks is an int32_t in C++.  The explicit
    # int64_t() cast sign-extends it before the 64-bit left shift.
    outer_inner_bytes = _left_shift(_signed(outer_blocks | inner_blocks, 32), bytes_per_block_log2, 64)

    bank = ((y >> 3) ^ (z >> 2)) & 0b1
    pipe = ((x >> 3) & 0b11) ^ (bank << 1)

    return _tiled_combine_bits(_signed(outer_inner_bytes, 64), bank, pipe, y & 0b1, 64)


def _log2_power_of_two(value):
    if value <= 0 or (value & (value - 1)) != 0:
        raise ValueError("bytes_per_block must be a positive power of two")
    return value.bit_length() - 1


def untile_2d(data, width_blocks, height_blocks, pitch_blocks, bytes_per_block):
    """Read a 2D tiled image back into row-major linear bytes."""
    bytes_per_block_log2 = _log2_power_of_two(bytes_per_block)
    data = bytes(data)

    out = bytearray(width_blocks * height_blocks * bytes_per_block)
    pos = 0

    for y in range(height_blocks):
        for x in range(width_blocks):
            offset = tiled_2d(x, y, pitch_blocks, bytes_per_block_log2) & _MASK32
            out[pos:pos + bytes_per_block] = data[offset:offset + bytes_per_block]
            pos += bytes_per_block

    return bytes(out)


if __name__ == "__main__":
    width_blocks = 64
    height_blocks = 64
    pitch_blocks = 64
    bytes_per_block = 4
    bytes_per_block_log2 = 2

    total_blocks = width_blocks * height_blocks

    # Known linear pattern: each 4-byte block contains its linear block index.
    pattern = bytearray()
    for i in range(total_blocks):
        pattern.extend(i.to_bytes(bytes_per_block, "little"))

    # Safely size the tiled storage buffer.
    max_offset = 0
    for y in range(height_blocks):
        for x in range(width_blocks):
            offset = tiled_2d(x, y, pitch_blocks, bytes_per_block_log2) & _MASK32
            if offset > max_offset:
                max_offset = offset

    storage_size = max(
        pitch_blocks * height_blocks * bytes_per_block,
        max_offset + bytes_per_block,
    )

    tiled = bytearray(storage_size)

    # Tile: write each linear block index at its tiled offset.
    for y in range(height_blocks):
        for x in range(width_blocks):
            linear_index = y * width_blocks + x
            offset = tiled_2d(x, y, pitch_blocks, bytes_per_block_log2) & _MASK32
            tiled[offset:offset + bytes_per_block] = linear_index.to_bytes(bytes_per_block, "little")

    untiled = untile_2d(tiled, width_blocks, height_blocks, pitch_blocks, bytes_per_block)
    assert untiled == bytes(pattern)
