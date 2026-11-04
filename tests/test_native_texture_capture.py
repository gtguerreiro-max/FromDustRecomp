import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from replay_pack import register_stream_hash
from validate_native_texture_capture import validate, mip_records


class NativeTextureCaptureTests(unittest.TestCase):
    def setUp(self):
        # Keep owned fixtures inside the workspace, including restricted runs.
        build = Path(__file__).resolve().parents[1] / 'build'
        build.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='test-native-texture-', dir=build)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        registers = struct.pack('<4I', 0x32584446, 1, 2, 3)
        digest = register_stream_hash(registers)
        (self.directory / 'xenia_regs.bin').write_bytes(registers)
        fetch = [2 | 1 << 22, 0x1006, 0, 0, 0, 1 << 9]
        raw = bytes([11, 23, 45, 255]) + bytes(124)
        (self.directory / 'gpu_textures.bin').write_bytes(
            struct.pack('<8I', 0x46445458, *fetch, len(raw)) + raw)
        (self.directory / 'gpu_mips.bin').write_bytes(struct.pack('<IIQ', 0x31504D46, 0, digest))
        (self.directory / 'native-textures.rgba').write_bytes(struct.pack('<3I', 1, 1, 1) + raw[:4])
        for table in ('cubes', 'mips'):
            (self.directory / f'native-{table}.rgba').write_bytes(struct.pack('<I', 0))
        self.report = dict(mode='native-memory-texture-producer',
                           scope='gpu-consumer-first-use-per-address', complete=True,
                           passed=True, native_rendering=False, failures=0,
                           register_hash=f'{digest:016X}', textures=1, mip_levels=0)
        self.save_report()

    def save_report(self):
        (self.directory / 'native-texture-report.json').write_text(json.dumps(self.report))

    def test_exact_independent_pixels_pass(self):
        result = validate(self.directory)
        self.assertTrue(result['passed'])
        self.assertEqual(result['textures'], 1)

    def test_other_register_stream_is_rejected(self):
        self.report['register_hash'] = '0000000000000000'
        self.save_report()
        with self.assertRaisesRegex(ValueError, 'different GPU register stream'):
            validate(self.directory)

    def test_wrong_native_pixels_are_rejected(self):
        path = self.directory / 'native-textures.rgba'
        data = bytearray(path.read_bytes())
        data[-1] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'textures differ'):
            validate(self.directory)

    def test_failed_or_incomplete_producer_is_rejected(self):
        for field in ('complete', 'passed'):
            with self.subTest(field=field):
                self.report[field] = False
                self.save_report()
                with self.assertRaisesRegex(ValueError, 'did not pass'):
                    validate(self.directory)
                self.report[field] = True

    def test_mip_order_is_irrelevant_and_corruption_is_rejected(self):
        a = struct.pack('<4I', 2, 1, 1, 1) + bytes([1, 2, 3, 4])
        b = struct.pack('<4I', 5, 1, 1, 1) + bytes([5, 6, 7, 8])
        self.assertEqual(mip_records(struct.pack('<I', 2) + a + b),
                         mip_records(struct.pack('<I', 2) + b + a))
        for invalid in (struct.pack('<I', 2) + a + a,
                        struct.pack('<I', 1) + a[:-1], struct.pack('<I', 0) + b'x'):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                mip_records(invalid)


if __name__ == '__main__':
    unittest.main()
