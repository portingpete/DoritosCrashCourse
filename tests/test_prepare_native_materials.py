"""Owned ShaderMaster declarations and original loader oracles; no game input."""
from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import json
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_native_materials as materials
from inspect_pak import PakError, read_directory, read_entry


def fixture() -> bytes:
    properties = (("Culling", 1), ("Type", 1), ("MultilayerDiffuse", 6))
    properties += tuple((f"kind{kind}", kind) for kind in materials.FIXED_SIZES)
    properties += tuple((f"absent{index}", 0) for index in range(118 - len(properties)))
    values = []
    for name, kind in properties:
        if name.startswith("absent"):
            values.append(None)
        elif kind == 6:
            values.append(struct.pack("<H", 2) + materials.counted_string("stock/first.png") +
                          materials.counted_string("stock/second.png"))
        else:
            # Includes arbitrary float/color spellings that must stay opaque.
            values.append(struct.pack("<H", 2) + bytes([kind + 1, 255]) * materials.FIXED_SIZES[kind])
    records = (materials.Record("stock/first", tuple(values)),)
    records += tuple(materials.Record(f"stock/{index}", (None,) * len(properties)) for index in range(1, 433))
    return materials.encode_template(materials.Template(36, "SHADERMASTER", 2, properties, records, 0, 0, 0))


class NativeMaterialTests(unittest.TestCase):
    def setUp(self):
        self.source = fixture()

    def append(self, source=None):
        source = self.source if source is None else source
        with patch.object(materials, "SHADER_SHA256", materials.digest(source)):
            return materials.append_materials(source)

    def test_every_observed_kind_round_trips_whole_template(self):
        template = materials.parse_template(self.source)
        self.assertEqual(len(template.properties), 118)
        self.assertEqual(len(template.records), 433)
        self.assertEqual(materials.encode_template(template), self.source)
        self.assertEqual({kind for _, kind in template.properties}, {*materials.FIXED_SIZES, 6})

    def test_append_keeps_all_stock_bytes_and_only_three_owned_properties(self):
        output, metadata = self.append()
        before, after = materials.parse_template(self.source), materials.parse_template(output)
        self.assertEqual(len(after.records), 437)
        self.assertEqual(output[:before.count_offset], self.source[:before.count_offset])
        self.assertEqual(output[before.count_offset + 2:before.records_end], self.source[before.count_offset + 2:before.records_end])
        self.assertEqual(output[after.records_end:], self.source[before.records_end:])
        self.assertEqual(materials.encode_template(after), output)
        for index, record in enumerate(after.records[-4:]):
            values = {name: value for (name, _), value in zip(after.properties, record.values, strict=True)
                      if value is not None}
            self.assertEqual(record.name, f"_native/cj_native_{index}")
            self.assertEqual(values, {"Culling": struct.pack("<HI", 1, 2), "Type": struct.pack("<HI", 1, 1),
                                      "MultilayerDiffuse": struct.pack("<H", 1) +
                                      materials.counted_string(f"_avatar/cj_native_{index}.png")})
        self.assertTrue(metadata["stock_records_unchanged"])
        self.assertEqual(metadata["stock_record_sha256"], materials.digest(self.source[before.records_begin:before.records_end]))
        self.assertEqual(metadata["whole_original_roundtrip_sha256"], materials.digest(self.source))

    def test_source_identity_and_duplicate_owned_names_rejected(self):
        with self.assertRaisesRegex(PakError, "supported original"):
            materials.append_materials(self.source)
        template = materials.parse_template(self.source)
        changed = replace(template, records=(replace(template.records[0], name="_NATIVE/CJ_NATIVE_0"),) + template.records[1:])
        with self.assertRaisesRegex(PakError, "already exists"):
            self.append(materials.encode_template(changed))

    def test_original_counts_and_required_property_kinds_checked(self):
        original = materials.parse_template(self.source)
        changed = replace(original, records=original.records[:-1])
        with self.assertRaisesRegex(PakError, "count/property contract"):
            self.append(materials.encode_template(changed))
        changed_properties = tuple(("OtherType" if name == "Type" else name, kind)
                                   for name, kind in original.properties)
        with self.assertRaisesRegex(PakError, "count/property contract"):
            self.append(materials.encode_template(replace(original, properties=changed_properties)))

    def test_unknown_duplicate_and_unbounded_schema_rejected(self):
        original = materials.parse_template(self.source)
        for properties in ((original.properties[0],) * 118,
                           (("unsupported", 14),) + original.properties[1:],
                           original.properties + (("excess", 0),) * 11):
            with self.subTest(properties=properties[:1]), self.assertRaises(PakError):
                materials.parse_template(materials.encode_template(replace(original, properties=properties)))

    def test_presence_array_record_class_and_trailer_checked(self):
        original = materials.parse_template(self.source)
        first_flag = original.records[0].begin + 2 + 4 + len(original.records[0].name)
        wrong_flag = bytearray(self.source); wrong_flag[first_flag] = ord("x")
        wrong_count = bytearray(self.source); struct.pack_into("<H", wrong_count, first_flag + 1, 1025)
        wrong_class = bytearray(self.source); struct.pack_into("<H", wrong_class, original.records[0].begin, 99)
        for content in (bytes(wrong_flag), bytes(wrong_count), bytes(wrong_class), self.source[:-1], self.source + b"x"):
            with self.subTest(size=len(content)), self.assertRaises(PakError):
                materials.parse_template(content)

    def test_class_version_and_counted_ascii_string_checked(self):
        original = materials.parse_template(self.source)
        for changed in (replace(original, class_name="OTHERMASTER"), replace(original, class_version=1),
                        replace(original, records=(replace(original.records[0], name="bad\0name"),) + original.records[1:])):
            with self.assertRaises(PakError):
                materials.parse_template(materials.encode_template(changed))

    def main_fixture(self, root: Path, *, invalid_assets=False):
        package = root / "fixture.pak"; package.write_bytes(b"original package fixture")
        output = root / "candidate"
        originals = {materials.SHADER_TEMPLATE: self.source, materials.CJ_CFG: b"original.cfg", materials.CJ_SKELETON: b"original.csf"}
        metadata = {"character_assets_manifest_sha256": "a" * 64, "owned_files": [{"name": "checked-input", "output_sha256": "b" * 64, "output_bytes": 3}]}
        validator = patch.object(materials, "character_assets", side_effect=PakError("invalid CJ assets") if invalid_assets else None,
                                 return_value=({}, metadata))
        with patch.object(materials, "PAK_SHA256", materials.digest(package.read_bytes())), \
             patch.object(materials, "SHADER_SHA256", materials.digest(self.source)), \
             patch.object(materials, "read_directory", return_value=[SimpleNamespace(name=name) for name in originals]), \
             patch.object(materials, "read_entry", side_effect=lambda path, entry: originals[entry.name]), \
             validator as validation, \
             patch.object(sys, "argv", ["prepare_native_materials", "--pak", str(package), "--output", str(output),
                                        "--character-assets", str(root / "checked.assets.json")]):
            if invalid_assets:
                with self.assertRaisesRegex(PakError, "invalid CJ assets"):
                    materials.main()
                self.assertFalse(output.exists())
            else:
                self.assertEqual(materials.main(), 0)
            validation.assert_called_once_with(root / "checked.assets.json", b"original.cfg", b"original.csf")
        self.assertEqual(package.read_bytes(), b"original package fixture")
        return output

    def test_main_reuses_checked_assets_and_writes_only_shader_and_separate_manifest(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            output = self.main_fixture(Path(directory))
            files = {str(path.relative_to(output)).replace("\\", "/") for path in output.rglob("*") if path.is_file()}
            self.assertEqual(files, {materials.SHADER_TEMPLATE, "material_manifest.json"})
            report = json.loads((output / "material_manifest.json").read_text())
            self.assertEqual(report["character_resource"], "BNMD:_avatar/cj_native.cfg")
            self.assertEqual(report["files"][0]["owned_declarations_added"], 4)
            self.assertEqual(report["character_assets_manifest_sha256"], "a" * 64)

    def test_invalid_character_assets_fail_before_any_output(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            self.main_fixture(Path(directory), invalid_assets=True)

    @unittest.skipUnless((ROOT / "game/data.pak").exists(), "private supported package unavailable")
    def test_private_original_whole_roundtrip_and_all_433_stock_records_preserved(self):
        package = ROOT / "game/data.pak"
        entries = {entry.name: entry for entry in read_directory(package)}
        source = read_entry(package, entries[materials.SHADER_TEMPLATE])
        self.assertEqual(materials.digest(source), materials.SHADER_SHA256)
        original = materials.parse_template(source)
        self.assertEqual(materials.encode_template(original), source)
        output, metadata = materials.append_materials(source)
        parsed = materials.parse_template(output)
        self.assertEqual(original.class_id, 36)
        self.assertEqual(original.count_offset, 2526)
        self.assertEqual(original.records_end, 273283)
        self.assertEqual(metadata["stock_declarations"], 433)
        for before, after in zip(original.records, parsed.records[:433], strict=True):
            self.assertEqual(source[before.begin:before.end], output[after.begin:after.end])

    @unittest.skipUnless((ROOT / "build_native/game_image.bin").exists(), "private original code image unavailable")
    def test_independent_original_shader_property_and_texture_load_oracles(self):
        raw = (ROOT / "build_native/game_image.bin").read_bytes()
        self.assertEqual(materials.digest(raw), "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7")
        words = {
            # Both genuine shader classes dispatch the same authored setters.
            0x82160E3C: 0x822CCB98, 0x82160E88: 0x822CD288,
            0x82157CCC: 0x822CCB98, 0x82157D18: 0x822CD288,
            0x8238B8C8: 0x38600210, 0x8238B8E8: 0x4BF3FE59,
            # Type is actual owner+292, then typed-data creation. Diffuse is
            # actual multilayer-data+80 copied using the original string ABI.
            0x822CCBAC: 0x81630124, 0x822CCBC8: 0x93DF0000, 0x822CCBD0: 0x4BFFD4E9,
            0x822CD2B8: 0x817F0128, 0x822CD2BC: 0x386B0050, 0x822CD2DC: 0x4BEDE16D,
            # Original Bone fallback authors Type1 and Culling2 through these
            # same setters; first reference actually invokes BuildTextures.
            0x823A0D4C: 0x38800001, 0x823A0D58: 0x814B0150,
            0x823A0D68: 0x38800002, 0x823A0D70: 0x810900A8,
            0x822CE7F4: 0x808BAB70, 0x822CE7F8: 0x4BF7FB69,
            0x822CE800: 0x4BF6D189, 0x822CE824: 0x4BFFE44D,
            # Type1 goes to the genuine texture manager and stores its actual
            # resolved master at ShaderMaster+340. VS and PS feature bits are
            # selected independently by original material binding.
            0x822CCCE0: 0x4BFFEDA9, 0x822CBB60: 0x809AAB78,
            0x822CBB64: 0x4BF827FD, 0x822CBB74: 0x90780000,
            0x822D06B0: 0x80BE0154, 0x822D0730: 0x60E60080,
            0x822D0734: 0x90C90008, 0x822D073C: 0x60A40002, 0x822D0740: 0x90880004,
            # Genuine constructor default alpha255 and Lighting flag bit0.
            0x822CB7E0: 0x907F0088, 0x822CB7E4: 0x913F0094,
        }
        for address, value in words.items():
            self.assertEqual(struct.unpack_from(">I", raw, address - 0x82000000)[0], value, f"original word {address:08X}")
        self.assertEqual(raw[0x82157114 - 0x82000000:0x82157114 - 0x82000000 + 5], b"SHDR\0")
        self.assertEqual(raw[0x821576D0 - 0x82000000:0x821576D0 - 0x82000000 + len(b"MultilayerDiffuse\0")], b"MultilayerDiffuse\0")
        self.assertEqual(struct.unpack_from(">I", raw, 0x82A94040 - 0x82000000)[0], 6)


if __name__ == "__main__":
    unittest.main()
