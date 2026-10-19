"""Verify the owned texture import and original template preservation."""
from dataclasses import replace
from pathlib import Path
import hashlib
import struct
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_native_textures as textures
from inspect_pak import PakError, read_directory, read_entry


def fixture():
    props = (("TextureType", 1), ("DiffuseFile", 6))
    props += tuple((f"kind{kind}", kind) for kind in textures.FIXED_SIZES)
    props += tuple((f"absent{index}", 0) for index in range(24 - len(props)))
    values = tuple(None if name.startswith("absent") else
                   struct.pack("<H", 2) + textures.counted_string("stock/a.png") + textures.counted_string("")
                   if kind == 6 else struct.pack("<H", 2) + bytes([kind + 1, 255]) * textures.FIXED_SIZES[kind]
                   for name, kind in props)
    records = (textures.Record("stock/a.png", values),)
    records += tuple(textures.Record(f"stock/{index}", (None,) * len(props)) for index in range(1, 738))
    return textures.encode_template(textures.Template(37, "TEXTUREMASTER", 1, props, records, 0, 0, 0))


class NativeTextureTests(unittest.TestCase):
    def setUp(self):
        self.source = fixture()

    def append(self, source=None):
        source = self.source if source is None else source
        with patch.object(textures, "TEXTURE_SHA256", textures.digest(source)):
            return textures.append_textures(source)

    def assert_preserved(self, source, output, report):
        before, after = textures.parse_template(source), textures.parse_template(output)
        self.assertEqual(len(after.records), 742)
        # Check exact original byte ranges, including opaque numeric payloads.
        self.assertEqual(output[:before.count_offset], source[:before.count_offset])
        self.assertEqual(output[before.count_offset + 2:before.records_end], source[before.count_offset + 2:before.records_end])
        self.assertEqual(struct.unpack_from("<H", output, before.count_offset)[0], 742)
        self.assertEqual(output[after.records_end:], b"_bin\0")
        self.assertEqual(textures.encode_template(after), output)
        self.assertEqual(report["stock_record_sha256"], hashlib.sha256(source[before.records_begin:before.records_end]).hexdigest())
        for index, record in enumerate(after.records[-4:]):
            name = f"_avatar/cj_native_{index}.png"
            self.assertEqual(record.name, name)
            values = {prop: value for (prop, _), value in zip(after.properties, record.values, strict=True) if value is not None}
            self.assertEqual(values, {"TextureType": struct.pack("<HI", 1, 1),
                                      "DiffuseFile": struct.pack("<HI", 1, len(name)) + name.encode("ascii")})

    def test_all_property_kinds_and_stock_bytes_survive_append(self):
        output, report = self.append()
        self.assert_preserved(self.source, output, report)

    def test_supported_original_package_template_roundtrip_and_append(self):
        package = ROOT / "game/data.pak"
        if not package.is_file():
            self.skipTest("Private original package is unavailable")
        with package.open("rb") as stream:
            self.assertEqual(hashlib.file_digest(stream, "sha256").hexdigest(), textures.PAK_SHA256)
        entry = next(e for e in read_directory(package) if e.name == textures.TEXTURE_TEMPLATE)
        source = read_entry(package, entry)
        self.assertEqual(hashlib.sha256(source).hexdigest(), "7c29ea9e5f9bfc2e6c9d64e8b5317b061f12ecb39d3af3e93204fe96bb364e85")
        output, report = textures.append_textures(source)
        self.assert_preserved(source, output, report)

    def test_wrong_identity_duplicate_owned_name_and_wrong_counts_rejected(self):
        with self.assertRaisesRegex(PakError, "supported original"):
            textures.append_textures(self.source)
        template = textures.parse_template(self.source)
        for changed, message in ((replace(template, records=template.records[:-1]), "count/property"),
                                 (replace(template, records=(replace(template.records[0], name="_AVATAR/CJ_NATIVE_0.PNG"),) + template.records[1:]), "already exists")):
            with self.assertRaisesRegex(PakError, message):
                self.append(textures.encode_template(changed))

    def test_schema_kind_array_and_trailer_corruption_rejected(self):
        template = textures.parse_template(self.source)
        malformed = [self.source[:-1], self.source + b"x",
                     textures.encode_template(replace(template, class_version=2)),
                     textures.encode_template(replace(template, class_name="SHADERMASTER")),
                     textures.encode_template(replace(template, properties=(("unknown", 14),) + template.properties[1:]))]
        flag = template.records[0].begin + 2 + 4 + len(template.records[0].name)
        wrong_flag = bytearray(self.source); wrong_flag[flag] = ord("x"); malformed.append(wrong_flag)
        wrong_array = bytearray(self.source); struct.pack_into("<H", wrong_array, flag + 1, 1025); malformed.append(wrong_array)
        for source in malformed:
            with self.subTest(size=len(source)), self.assertRaises(PakError):
                textures.parse_template(source)

    @unittest.skipUnless((ROOT / "build_native/game_image.bin").is_file(), "Private original image is unavailable")
    def test_original_texture_property_and_image_dependency_oracles(self):
        raw = (ROOT / "build_native/game_image.bin").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),
                         "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7")
        words = {
            # Original property descriptors are name-based kind1/kind6. The
            # startup initializer installs their actual member callbacks.
            0x82A91A38: 0x82154F40, 0x82A91A40: 1,
            0x82A91B78: 0x82154F04, 0x82A91B80: 6,
            0x829D1F70: 0x912B0228, 0x829D1FD0: 0x90AB0218,
            0x829D21AC: 0x92EB0358, 0x829D224C: 0x928B0368,
            # TextureType dispatches VT176, stores actual+128 and allocates
            # its matching typed data. DiffuseFile dispatches VT196 and
            # copies the string into that typed data's first field.
            0x822EDE1C: 0x816C00B0, 0x82160BAC: 0x822AD620,
            0x822AD65C: 0x4BFFF0AD, 0x822AC738: 0x93DF0000,
            0x822EDD5C: 0x816C00C4, 0x82160BC0: 0x822AF480,
            0x822AF494: 0x4BFFD37D, 0x822AC840: 0x807F0084,
            0x822AC84C: 0x809E0000, 0x822AC860: 0x4BEFEBE9,
            # The genuine DX group factory installs the concrete master
            # and dependency interfaces. The first reference queues normal
            # dependency VT12; its worker VT8 performs the real load.
            0x8238B620: 0x4BF21ED1, 0x8238B62C: 0x392B0AFC,
            0x8238B630: 0x390A0C2C,
            0x822AF248: 0x816300A8, 0x822AF254: 0x916300A8,
            0x822AF2A4: 0x814B000C, 0x82160C38: 0x8223C9D8,
            0x82160C34: 0x822AF2D8, 0x822AF2F4: 0x814B00EC,
            # TextureType1's actual switch case is VT248 (not the Color
            # Type0 VT244). It reads DiffuseFile, then calls the normal
            # image object's VT32 and the non-DDS image-builder path.
            0x82160BE8: 0x822AD9C8, 0x822ADA04: 0x822ADA40,
            0x822ADA44: 0x814B00F8, 0x82160BF4: 0x822AE8D8,
            0x822AE91C: 0x815C0084, 0x822AE928: 0x80CA0000,
            0x822AEA74: 0x814B0020, 0x821613A0: 0x8238F1C8,
            0x8238F518: 0x4BFFE9D9,
        }
        for address, expected in words.items():
            self.assertEqual(struct.unpack_from(">I", raw, address - 0x82000000)[0],
                             expected, f"original word {address:08X}")
        for address, expected in ((0x82154F40, b"TextureType\0"),
                                  (0x82154F04, b"DiffuseFile\0")):
            offset = address - 0x82000000
            self.assertEqual(raw[offset:offset + len(expected)], expected)


if __name__ == "__main__":
    unittest.main()
