import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from recover_offset_switches import recover


class OffsetSwitchContract(unittest.TestCase):
    def fixture(self, nop=True):
        image = bytearray(0x400)
        words = [0x2b0b0002, 0x41990060, 0x3d800000, 0x5560083c,
                 0x398c1300, 0x7c0c022e, 0x3d800000, 0x398c1080]
        if nop:
            words += [0x60000000]
        words += [0x7d8c0214, 0x7d8903a6, 0x4e800420]
        struct.pack_into(f">{len(words)}I", image, 0x20, *words)
        struct.pack_into(">3H", image, 0x300, 0, 16, 16)
        return image

    def read(self, image):
        return recover(image, 0x1000, [(0x1000, 0x300)])

    def test_original_offset_semantics_and_shared_edges(self):
        self.assertEqual(self.read(self.fixture()),
                         [{"base": 0x104c, "r": 11, "labels": [0x1080, 0x1090, 0x1090]}])

    def test_optional_scheduling_nop(self):
        self.assertEqual(self.read(self.fixture(False))[0]["base"], 0x1048)

    def test_unverified_bound_or_cr_never_guesses(self):
        image = self.fixture()
        struct.pack_into(">I", image, 0x20, 0x2b0a0002)
        self.assertEqual(self.read(image), [])
        image = self.fixture()
        struct.pack_into(">I", image, 0x24, 0x41810060)
        self.assertEqual(self.read(image), [])

    def test_data_target_is_rejected(self):
        image = self.fixture()
        struct.pack_into(">H", image, 0x302, 0x280)
        with self.assertRaisesRegex(ValueError, "executable"):
            self.read(image)

    def test_unaligned_target_is_rejected(self):
        image = self.fixture()
        struct.pack_into(">H", image, 0x302, 2)
        with self.assertRaisesRegex(ValueError, "executable"):
            self.read(image)

    def test_truncated_and_excessive_tables_are_rejected(self):
        image = self.fixture()
        struct.pack_into(">I", image, 0x20, 0x2b0b1000)
        with self.assertRaisesRegex(ValueError, "table"):
            self.read(image)

    def byte_fixture(self, nop=True):
        image = bytearray(0x400)
        words = [0x2b0b0002, 0x41990060, 0x3d800000, 0x398c1300,
                 0x7c0c58ae, 0x5400103a, 0x3d800000]
        if nop:
            words += [0x60000000]
        words += [0x398c1080, 0x7d8c0214, 0x7d8903a6, 0x4e800420]
        struct.pack_into(f">{len(words)}I", image, 0x20, *words)
        struct.pack_into(">3B", image, 0x300, 0, 4, 4)
        return image

    def test_byte_entries_are_unsigned_scaled_and_shared(self):
        self.assertEqual(self.read(self.byte_fixture()),
                         [{"base": 0x104c, "r": 11, "labels": [0x1080, 0x1090, 0x1090]}])
        self.assertEqual(self.read(self.byte_fixture(False))[0]["base"], 0x1048)

    def test_byte_indices_and_scale_require_exact_registers(self):
        image = self.byte_fixture()
        struct.pack_into(">I", image, 0x30, 0x7c0c50ae)
        self.assertEqual(self.read(image), [])
        image = self.byte_fixture()
        struct.pack_into(">I", image, 0x34, 0x5400083c)
        self.assertEqual(self.read(image), [])

    def test_unscaled_byte_offsets_and_scheduling_nops(self):
        image = bytearray(0x400)
        words = [0x2b0a0002, 0x41990060, 0x3d800000, 0x398c1300,
                 0x7c0c50ae, 0x3d800000, 0x60000000, 0x398c1080,
                 0x60000000, 0x7d8c0214, 0x7d8903a6, 0x4e800420]
        struct.pack_into(f">{len(words)}I", image, 0x20, *words)
        struct.pack_into(">3B", image, 0x300, 0, 16, 16)
        self.assertEqual(self.read(image),
                         [{"base": 0x104c, "r": 10, "labels": [0x1080, 0x1090, 0x1090]}])
        image[0x301] = 17
        with self.assertRaisesRegex(ValueError, "executable"):
            self.read(image)

    def test_conditional_return_is_a_real_unsigned_extent_guard(self):
        image = self.byte_fixture()
        struct.pack_into(">I", image, 0x24, 0x4d990020)
        self.assertEqual(self.read(image),
                         [{"base": 0x104c, "r": 11, "labels": [0x1080, 0x1090, 0x1090]}])
        for unproven in (0x4d990021, 0x4d990420, 0x4d910020, 0x4d990820):
            struct.pack_into(">I", image, 0x24, unproven)
            self.assertEqual(self.read(image), [])

    def test_valid_case_branch_with_separate_default_preserves_bound(self):
        image = self.byte_fixture()
        tail = bytes(image[0x28:0x50])
        struct.pack_into(">II", image, 0x24, 0x40990008, 0x480000d8)
        image[0x2c:0x54] = tail
        self.assertEqual(self.read(image),
                         [{"base": 0x1050, "r": 11, "labels": [0x1080, 0x1090, 0x1090]}])
        for unproven in (0x48000004, 0x480000d9):
            struct.pack_into(">I", image, 0x28, unproven)
            self.assertEqual(self.read(image), [])
        struct.pack_into(">II", image, 0x24, 0x4099000c, 0x480000d8)
        self.assertEqual(self.read(image), [])

    def test_byte_target_and_extent_are_bounded(self):
        image = self.byte_fixture()
        image[0x301] = 255
        with self.assertRaisesRegex(ValueError, "executable"):
            self.read(image)
        image = self.byte_fixture()
        struct.pack_into(">I", image, 0x20, 0x2b0b1000)
        with self.assertRaisesRegex(ValueError, "table"):
            self.read(image)
        image = self.fixture()
        struct.pack_into(">I", image, 0x30, 0x398c13ff)
        with self.assertRaisesRegex(ValueError, "table"):
            self.read(image)

    def scheduled_fixture(self, changed=None):
        image = self.byte_fixture()
        # Original parser ordering: lhax, rlwinm, subf, lwz and stw
        # preserve bounded r11 and comparison CR6 before the bgt.
        schedule = [0x7fbe4aae, 0x57bc103a, 0x7d5c5050, 0x814a0004, 0x915f0018]
        if changed is not None:
            schedule[2] = changed
        tail = bytes(image[0x24:0x50])
        struct.pack_into(">5I", image, 0x24, *schedule)
        image[0x38:0x64] = tail
        return image

    def test_parser_scheduled_guard_preserves_exact_table_edges(self):
        self.assertEqual(self.read(self.scheduled_fixture()),
                         [{"base": 0x1060, "r": 11, "labels": [0x1080, 0x1090, 0x1090]}])

    def test_unchanged_selector_scheduling_after_guard(self):
        image = self.byte_fixture()
        tail = bytes(image[0x28:0x50])
        # lis r10, li r22, li r19 and non-updating lfd preserve r11/CR6.
        struct.pack_into(">4I", image, 0x28, 0x3d408200, 0x3ac0ffff, 0x3a600101, 0xcbea9dc0)
        image[0x38:0x60] = tail
        self.assertEqual(self.read(image),
                         [{"base": 0x105c, "r": 11, "labels": [0x1080, 0x1090, 0x1090]}])
        struct.pack_into(">I", image, 0x2c, 0x39600000)  # li bounded r11,0
        self.assertEqual(self.read(image), [])

    def test_scheduled_register_condition_or_control_clobber_is_rejected(self):
        # Record-form arithmetic, a load into bounded r11, an update load
        # and a call cannot provide the proven unchanged guard relation.
        for instruction in (0x7d5c5051, 0x816a0004, 0x856b0004, 0x48000001):
            with self.subTest(instruction=hex(instruction)):
                self.assertEqual(self.read(self.scheduled_fixture(instruction)), [])


if __name__ == "__main__":
    unittest.main()
