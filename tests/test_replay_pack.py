"""Regression checks for matching captured shader initialisation data."""
import io
import struct
import sys
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from replay_pack import (shader_objects, cube_face_stride, build_cube_table, build_texture_table,
                         clear_registers, align_xenia, copy_destination_offset, scan_draw,
                         load_xenia_registers, load_gpu_draws, register_stream_hash, load_buffers)
from analyze_capture import load, OBJECT_COUNT, OBJECT_SIZE, CODE_SIZE
from xenos_tiling import tiled_2d


def container(payload, vertex=False, metadata=b""):
    virtual_size = 36 + len(metadata)
    return (struct.pack(">3I", 0x102A1100 | vertex, virtual_size, len(payload))
            + bytes(24) + metadata + payload)


class ShaderObjectTests(unittest.TestCase):
    def match(self, code, bins, kind=1):
        capture = (struct.pack("<4I", 0x46445349, kind, 0x408C3C38, 0)
                   + (code + bytes(1024))[:1024])
        with patch("builtins.open", return_value=io.BytesIO(capture)):
            return shader_objects("shader_inits.bin", bins)

    def test_short_shaders_sharing_prefix(self):
        prefix = bytes(range(32))
        correct = prefix + b"B" * 28
        result = self.match(correct + b"unrelated memory", {
            "wrong": container(prefix + b"A" * 28),
            "ui": container(correct),
        })
        self.assertEqual(result[0x408C3C38], (1, "ui"))

    def test_virtual_metadata_is_not_shader_code(self):
        code = bytes(range(60))
        result = self.match(code, {
            "metadata_only": container(b"different shader", metadata=code),
            "correct": container(code),
        })
        self.assertEqual(result[0x408C3C38], (1, "correct"))

    def test_stage_must_match(self):
        code = bytes(range(60))
        result = self.match(code, {
            "vertex": container(code, vertex=True),
            "pixel": container(code),
        })
        self.assertEqual(result[0x408C3C38], (1, "pixel"))

    def test_large_shader_matches_capture_limit(self):
        code = bytes(range(256)) * 8
        result = self.match(code, {"large": container(code, vertex=True)}, kind=0)
        self.assertEqual(result[0x408C3C38], (0, "large"))

    def test_unknown_shader_does_not_match_partial_prefix(self):
        prefix = bytes(range(32))
        result = self.match(prefix + b"B" * 28,
                            {"wrong": container(prefix + b"A" * 28)})
        self.assertEqual(result, {})


class ClearCaptureTests(unittest.TestCase):
    def capture(self, rectangles):
        header = lambda kind: struct.pack("<12I", 0x46445257, kind, *([0] * 10))
        objects = bytes(OBJECT_COUNT * (8 + OBJECT_SIZE) + 2 * (8 + CODE_SIZE))
        clear = (b"FDCL" + struct.pack("<fI", 0.25, len(rectangles))
                 + b"".join(struct.pack(">4i", *r) for r in rectangles))
        return header(5) + objects + b"BCDF" + bytes(4) + clear + header(3)

    def test_clear_parameters_and_next_record(self):
        rectangles = [(2, 3, 32, 48), (-1, 0, 16, 16)]
        with patch("builtins.open", return_value=io.BytesIO(self.capture(rectangles))):
            records = load("capture.bin", with_objects=True)
        self.assertEqual([r[0][1] for r in records], [5, 3])
        self.assertEqual(records[0][2]["clear"], (0.25, rectangles))

    def test_full_clear_without_objects(self):
        with patch("builtins.open", return_value=io.BytesIO(self.capture([]))):
            records = load("capture.bin")
        self.assertEqual([r[0][1] for r in records], [5, 3])

    def test_real_surface_state_replaces_clear_scratch_target(self):
        surface = bytearray(128)
        struct.pack_into(">2I", surface, 0x18, 0x10010410, 0x3CF)
        struct.pack_into(">I", surface, 0x24, (1023 << 18) | (599 << 3))
        objects = {off: (0, bytes(128)) for off in (0x3190, 0x3194, 0x3198, 0x319C, 0x31A0)}
        objects[0x3190] = (0x40000000, surface)
        regs = clear_registers({0x2000: 0x8020208, 0x2002: 0x10000}, objects)
        self.assertEqual((regs[0x2000], regs[0x2001], regs[0x2002]), (0x10010410, 0x3CF, 0x10000))
        self.assertEqual(regs[0x2082], (600 << 16) | 1024)


class CubeCaptureTests(unittest.TestCase):
    info = {"format_name": "k_DXT1", "dimension": 3, "width": 64, "height": 64,
            "pitch": 128, "tiled": 1, "endianness": 0}

    def test_small_bc_cube_faces_retain_tiled_bank_bytes(self):
        self.assertEqual(cube_face_stride(self.info), 8192)
        colors = (0xF800, 0x07E0, 0x001F, 0xFFFF, 0xFFE0, 0)
        raw = bytearray()
        for color in colors:
            face = bytearray(8192)
            block = struct.pack("<HHI", color, color, 0)
            for y in range(16):
                for x in range(16):
                    offset = tiled_2d(x, y, 32, 3)
                    face[offset:offset + 8] = block
            raw.extend(face)
        with patch("replay_pack.decode_texture_fetch", return_value=self.info):
            packed = build_cube_table({123: ([0] * 6, raw)}, {123: 7})
        self.assertEqual(struct.unpack_from("<4I", packed), (1, 7, 64, 64))
        expected = ((255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255),
                    (255, 255, 255, 255), (255, 255, 0, 255), (0, 0, 0, 255))
        for face, color in enumerate(expected):
            pixels = packed[16 + face * 16384:16 + (face + 1) * 16384]
            self.assertEqual(pixels, bytes(color) * 4096)

    def test_incomplete_cube_requires_recapture(self):
        with patch("replay_pack.decode_texture_fetch", return_value=self.info):
            with self.assertRaisesRegex(ValueError, "recapture"):
                build_cube_table({123: ([0] * 6, bytes(4096))}, {123: 7})


class GpuRegisterTests(unittest.TestCase):
    def read(self, data):
        with patch("os.path.exists", return_value=True), patch("builtins.open", return_value=io.BytesIO(data)):
            return load_xenia_registers("registers.bin")

    def test_old_and_extended_records(self):
        old = struct.pack("<1065I", 0x52584446, *range(1064))
        new = struct.pack("<1065I", 0x32584446, *range(1064)) + struct.pack("<2048I", *range(2048))
        records = self.read(old + new)
        self.assertEqual(len(records), 2)
        self.assertNotIn(0x4000, records[0])
        self.assertEqual(records[1][0x4400], 1024)
        self.assertEqual(records[1][0x47FF], 2047)

    def test_truncated_record_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Truncated"):
            self.read(struct.pack("<I", 0x32584446) + bytes(4256))


class ResolveOffsetTests(unittest.TestCase):
    def test_ring_wrap_recovers_exact_gpu_copy_registers(self):
        state = {0x2318: 0x100000, 0x2319: 0x100000, 0x231A: 0x2000200, 0x231B: 0xA6C81}
        info = {"mip_max_level": 0, "mip_address": 0, "width": 512}
        with patch("replay_pack.decode_texture_fetch", return_value=info):
            self.assertEqual(copy_destination_offset(b"", [0, 0x100019, 0, 0, 0, 0], [state]),
                             (0xA6C81, 0, 0, 0, 1))

    def test_missing_copy_without_gpu_evidence_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Missing or ambiguous"):
            copy_destination_offset(b"", [0, 0x100019, 0, 0, 0, 0])

    def offset(self, x, y, tiled=True):
        base, pitch, bpp = 0x0D5E0000, 512, 4
        delta = tiled_2d(x, y, pitch, 2) if tiled else (y * pitch + x) * bpp
        commands = struct.pack(">5I", (3 << 16) | 0x2318,
                               0x100000, base + delta, (2048 - y) << 16 | pitch, 0xA6C81)
        fetch = [0, base | 25, 0, 0, 0, 0]
        info = {"format_name": "k_16_16", "pitch": pitch, "width": 512,
                "height": 2048, "tiled": tiled, "mip_max_level": 0, "mip_address": 0}
        with patch("replay_pack.decode_texture_fetch", return_value=info):
            return copy_destination_offset(commands, fetch)[:3]

    def test_all_shadow_atlas_cascades(self):
        for y in (0, 512, 1024, 1536):
            self.assertEqual(self.offset(0, y), (0xA6C81, 0, y))

    def test_tiled_horizontal_offset(self):
        self.assertEqual(self.offset(128, 96), (0xA6C81, 128, 96))

    def test_linear_offset(self):
        self.assertEqual(self.offset(7, 41, False), (0xA6C81, 7, 41))

    def test_mip_copy_keeps_level_zero(self):
        commands = struct.pack(">5I", (3 << 16) | 0x2318, 0x100000,
                               0x02378000, 64 << 16 | 64, 0x1000302)
        info = {"format_name": "k_8_8_8_8", "pitch": 256, "width": 256,
                "height": 256, "tiled": True, "mip_max_level": 3,
                "mip_address": 0xA2368000}
        with patch("replay_pack.decode_texture_fetch", return_value=info):
            result = copy_destination_offset(commands, [0, 0xA2328006, 0, 0, 0, 0])
        self.assertEqual(result, (0x1000302, 0, 0, 2, 4))


class BufferExtentTests(unittest.TestCase):
    def load(self, snapshots):
        data = b"".join(struct.pack("<3I", 0x46444246, 0x1000, len(b)) + b for b in snapshots)
        with patch("builtins.open", return_value=io.BytesIO(data)):
            return load_buffers("buffers.bin")[0x1000]

    def test_shorter_static_draw_keeps_the_complete_index_buffer(self):
        full = struct.pack(">6H", 0, 1, 2, 3, 4, 5)
        self.assertEqual(self.load([full, full[:6]]), full)
        self.assertEqual(self.load([full[:6], full]), full)

    def test_changed_contents_are_not_treated_as_a_shorter_static_view(self):
        self.assertEqual(self.load([b"abcdef", b"XYZ"]), b"XYZ")


class TextureFailureTests(unittest.TestCase):
    info = dict(width=1, height=1, pitch=1, tiled=0, endianness=0,
                dimension=1, format_name="k_8_8_8_8")

    def pack(self, info, format_code=6):
        with patch("replay_pack.decode_texture_fetch", return_value=info):
            return build_texture_table({0x123000: ([0, format_code, 0, 0, 0, 0], bytes((1, 2, 3, 4)))})

    def test_valid_texture_keeps_its_pixels(self):
        index, data = self.pack(self.info)
        self.assertEqual(index, {0x123000: 0})
        self.assertEqual(data, struct.pack("<3I", 1, 1, 1) + bytes((1, 2, 3, 4)))

    def test_unknown_format_cannot_be_replaced_by_white(self):
        with self.assertRaisesRegex(ValueError, "00123000.*Unsupported"):
            self.pack(dict(self.info, format_name="unknown"))

    def test_volume_cannot_be_accepted_as_one_2d_slice(self):
        with self.assertRaisesRegex(ValueError, "00123000.*3D texture"):
            self.pack(dict(self.info, dimension=2))

    def test_stacked_texture_cannot_be_accepted_as_one_2d_slice(self):
        with self.assertRaisesRegex(ValueError, "00123000.*Stacked 2D texture"):
            self.pack(dict(self.info, stacked=True, depth=2))

    def test_fixed16_failure_cannot_fall_back_to_rgba8(self):
        with patch("replay_pack.decode_fixed16", side_effect=ValueError("bad fixed16")):
            with self.assertRaisesRegex(ValueError, "00123000.*bad fixed16"):
                self.pack(dict(self.info, format_name="k_16_16"), 25)


class GpuSubmissionTests(unittest.TestCase):
    def load(self, flags, registers, count):
        with patch("replay_pack.os.path.exists", return_value=True), \
             patch("builtins.open", side_effect=[io.BytesIO(flags), io.BytesIO(registers)]):
            return load_gpu_draws("gpu_draws.bin", "xenia_regs.bin", count)

    def test_same_draw_count_but_wrong_register_capture_is_rejected(self):
        registers = struct.pack("<2I", 0x32584446, 7)
        other = struct.pack("<2I", 0x32584446, 8)
        flags = struct.pack("<IIQ", 0x45474446, 2, register_stream_hash(registers)) + bytes((1, 0))
        self.assertEqual(self.load(flags, registers, 2), bytes((1, 0)))
        with self.assertRaisesRegex(ValueError, "different register capture"):
            self.load(flags, other, 2)

    def test_truncated_or_invalid_flags_are_rejected(self):
        registers = bytes(4)
        header = struct.pack("<IIQ", 0x45474446, 2, register_stream_hash(registers))
        for flags in (header + bytes((1,)), header + bytes((1, 2))):
            with self.assertRaisesRegex(ValueError, "Invalid"):
                self.load(flags, registers, 2)


class DrawAlignmentTests(unittest.TestCase):
    def test_colour_pass_does_not_match_identical_alpha_geometry(self):
        draws = [([0, 0], b"", {"commands": b"", "key": (0x60000004, 0xD39B000)})]
        states = [{0x21FC: 0x60000004, 0x21FA: 0xD39B000, 0x2000: 0x10010410,
                   0x2001: 0x3CF, 0x2180: shader} for shader in (0x10010101, 0x10310904)]
        def track(regs, commands):
            regs.update({0x2000: 0x10010410, 0x2001: 0x3CF, 0x2180: 0x10310904})
        with patch("replay_pack.draw_key", side_effect=lambda obj: obj["key"]), \
             patch("replay_pack.track_registers", side_effect=track):
            self.assertEqual(align_xenia(draws, states), {0: 1})

    def test_final_draw_survives_inline_fake_packet(self):
        words = (0xC0022200, 0, 2609, 16777216,
                 0xC0032200, 0, 0x1EC0004, 0x2562000, 0x400001EC)
        self.assertEqual(scan_draw(struct.pack(">%dI" % len(words), *words)),
                         ("DRAW_INDX", (0, 0x1EC0004, 0x2562000, 0x400001EC)))

    def align(self, keys, parsed, actual):
        draws = [([0, 0], b"", {"key": key, "commands": target})
                 for key, target in zip(keys, parsed)]
        xenia = [{0x21FC: key[0], 0x21FA: key[1], 0x2000: target[0], 0x2001: target[1]}
                 for key, target in zip(keys, actual)]
        def track(regs, target):
            regs.update({0x2000: target[0], 0x2001: target[1]})
        with patch("replay_pack.draw_key", side_effect=lambda obj: obj["key"]), \
             patch("replay_pack.track_registers", side_effect=track):
            return align_xenia(draws, xenia)

    def test_recovers_indexed_mesh_between_target_anchors(self):
        keys = [(0x60004, 0x1000), (0x60004, 0x2000), (0x60004, 0x3000)]
        actual = [(0x10010410, 0x3CF)] * 3
        parsed = [actual[0], (0xC0043C00, 3), actual[2]]
        self.assertEqual(self.align(keys, parsed, actual), {0: 0, 2: 2, 1: 1})

    def test_repeated_auto_indexed_pass_requires_target_match(self):
        keys = [(0x30088, 0)]
        self.assertEqual(self.align(keys, [(0xC0043C00, 3)], [(0x10010410, 0x3CF)]), {})


if __name__ == "__main__":
    unittest.main()
