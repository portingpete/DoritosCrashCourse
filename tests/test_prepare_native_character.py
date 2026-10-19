"""Owned BoneModel declaration contracts; no game launch or native build."""
from __future__ import annotations

from pathlib import Path
import copy
import json
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_native_character as character
from inspect_pak import PakError, read_directory, read_entry
from cal3d_character import (Bone, Mesh, Skeleton, Submesh, Vertex, preflight_hardware,
                             read_cmf, write_cmf, write_csf)
from title_texture_recon import png_metadata


def fixture(class_name, properties, records, class_id=55):
    data = struct.pack("<HHH", 2, 1, class_id) + character.counted_string(class_name)
    data += struct.pack("<HH", 1, len(properties))
    data += b"".join(character.counted_string(name) + bytes([kind]) for name, kind in properties)
    data += struct.pack("<H", len(records))
    for name, values in records:
        data += struct.pack("<H", class_id) + character.counted_string(name)
        for property_name, _ in properties:
            value = values.get(property_name)
            data += b"0" if value is None else b"1" + struct.pack("<H", 1) + value
    return data + b"_bin\0"


def cj_assets_fixture(root):
    skeleton = Skeleton([Bone(f"bone{index}", (0, 0, 0), (0, 0, 0, 1),
                              (0, 0, 0), (0, 0, 0, 1), -1, ()) for index in range(72)])
    csf = write_csf(skeleton)
    vertex = lambda position: Vertex(position, (0, 0, 1), -1, 0, ((0, 0),), ((71, 1.0),))
    mesh = Mesh([Submesh(-1, 1, [vertex((0, 0, 0)), vertex((1, 0, 0)), vertex((0, 1, 0))], [(0, 1, 2)])
                 for _ in range(4)])
    cmf = write_cmf(mesh, skeleton)
    original_cfg = b"skeleton=p_test.CSF\r\nanimation=IDLE:idle.CAF\r\nmesh=stock.cmf\r\nmask=UPPER:mask.txt\r\nrun=IDLE callback()\r\n"
    cfg = original_cfg.replace(b"mesh=stock.cmf", b"mesh=cj_native.cmf")
    names = [f"_native/cj_native_{index}" for index in range(4)]
    mat = ("\r\n".join(names) + "\r\nendmats\r\n").encode()
    chunk = lambda kind, value: struct.pack(">I", len(value)) + kind + value + struct.pack(">I", zlib.crc32(kind + value))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(b"\0\x80\x40\x20")) + chunk(b"IEND", b""))
    values = [cfg, cmf, mat] + [png] * 4
    files = dict(zip(character.CJ_ASSET_NAMES, values, strict=True))
    for name, value in files.items():
        path = root.joinpath(*name.split("/")); path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(value)
    data = {"resource": "BNMD:" + character.CJ_RESOURCE_NAME, "original_cfg_sha256": character.digest(original_cfg),
            "skeleton_sha256": character.digest(csf), "cfg_sha256": character.digest(cfg),
            "mesh_sha256": character.digest(cmf), "hardware": preflight_hardware(mesh, skeleton),
            "materials": [{"submesh": index, "registered_name": names[index],
                           "diffuse_filename": f"_avatar/cj_native_{index}.png",
                           "texture_sha256": character.digest(png), "texture_metadata": png_metadata(png)} for index in range(4)]}
    manifest = root / "cj_native.assets.json"; manifest.write_text(json.dumps(data))
    return manifest, data, files, original_cfg, csf


class CharacterDeclarationTests(unittest.TestCase):
    def setUp(self):
        self.bone = fixture("BONEDMODELMASTER", (("FileName", 6), ("BoundingBoxMin", 9), ("Name", 6)),
                            [("owned-test-prop", {"FileName": character.counted_string("props/test.cfg"),
                                                  "BoundingBoxMin": struct.pack("<3f", -1, -2, -3)})])
        self.avatar = fixture("AVATARMODELMASTER", (("DiffuseColor", 12), ("FileName", 6)),
                              [(name, {"DiffuseColor": bytes([1, 2, 3]),
                                       "FileName": character.counted_string(character.RESOURCE_NAME
                                                                            if name == "random" else character.RESOURCE_NAME.lower())})
                               for name in ("gamepad0", "gamepad1", "gamepad2", "gamepad3", "random")])

    def append_fixture(self, bone=None, avatar=None, resource_name=character.RESOURCE_NAME):
        bone, avatar = bone or self.bone, avatar or self.avatar
        with patch.object(character, "INPUTS", {character.BONE_TEMPLATE: character.digest(bone),
                                                character.AVATAR_TEMPLATE: character.digest(avatar)}):
            return character.append_declaration(bone, avatar, resource_name)

    def test_cj_declaration_uses_exact_owned_key_and_preserves_stock_records(self):
        result, metadata = self.append_fixture(resource_name=character.CJ_RESOURCE_NAME)
        before, after = character.parse_template(self.bone), character.parse_template(result)
        self.assertEqual(result[before.records_begin:before.records_end], self.bone[before.records_begin:before.records_end])
        self.assertEqual(after.records[-1].name, character.CJ_RESOURCE_NAME)
        self.assertEqual(after.records[-1].strings, {"FileName": (character.CJ_RESOURCE_NAME,)})
        self.assertEqual(metadata["resource"], "BNMD:_avatar/cj_native.cfg")
        with self.assertRaisesRegex(PakError, "scope"):
            self.append_fixture(resource_name="_avatar/../escape.cfg")

    def test_cj_assets_validate_exact_seven_files_and_actual_global_bone_indices(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            root = Path(directory); manifest, data, expected, cfg, csf = cj_assets_fixture(root)
            (root / "data/art/_avatar/decoy.CSF").write_bytes(b"must never be copied")
            files, metadata = character.character_assets(manifest, cfg, csf)
            self.assertEqual(files, expected)
            self.assertEqual([item["name"] for item in metadata["owned_files"]], list(character.CJ_ASSET_NAMES))
            self.assertEqual(metadata["character_assets_manifest_sha256"], character.digest(manifest.read_bytes()))
            mesh = read_cmf(files[character.CJ_ASSET_NAMES[1]])
            self.assertEqual(mesh.submeshes[0].vertices[0].influences, ((71, 1.0),))

    def test_cj_assets_reject_manifest_hash_scope_order_and_hardware_mismatches(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            root = Path(directory); manifest, data, _, cfg, csf = cj_assets_fixture(root)
            bad = []
            for field, value in (("resource", "BNMD:../escape.cfg"), ("skeleton_sha256", "0" * 64),
                                 ("mesh_sha256", "0" * 64), ("cfg_sha256", "0" * 64), ("hardware", {})):
                changed = copy.deepcopy(data); changed[field] = value; bad.append(changed)
            for field, value in (("submesh", True), ("registered_name", "_native/cj_native_3"),
                                 ("diffuse_filename", "_avatar/../escape.png"), ("texture_sha256", "0" * 64),
                                 ("texture_metadata", {})):
                changed = copy.deepcopy(data); changed["materials"][0][field] = value; bad.append(changed)
            bad.append({**data, "materials": data["materials"][:-1]})
            for changed in bad:
                manifest.write_text(json.dumps(changed))
                with self.subTest(changed=changed), self.assertRaises(PakError):
                    character.character_assets(manifest, cfg, csf)

    def test_cj_assets_preserve_cfg_callbacks_even_if_modified_hash_is_reported(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            root = Path(directory); manifest, data, files, cfg, csf = cj_assets_fixture(root)
            changed_cfg = files[character.CJ_ASSET_NAMES[0]].replace(b"callback()", b"different()")
            root.joinpath(*character.CJ_ASSET_NAMES[0].split("/")).write_bytes(changed_cfg)
            data["cfg_sha256"] = character.digest(changed_cfg); manifest.write_text(json.dumps(data))
            with self.assertRaisesRegex(PakError, "every original byte"):
                character.character_assets(manifest, cfg, csf)

    def test_cj_assets_reject_missing_companion_wrong_order_and_outside_private_root(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            root = Path(directory); manifest, _, _, cfg, csf = cj_assets_fixture(root)
            root.joinpath(*character.CJ_ASSET_NAMES[2].split("/")).write_bytes(b"_native/cj_native_3\r\nendmats\r\n")
            with self.assertRaisesRegex(PakError, "ordered physical submeshes"):
                character.character_assets(manifest, cfg, csf)
        with self.assertRaisesRegex(PakError, "ignored build_native"):
            character.character_assets(ROOT / "game/unowned.assets.json", b"", b"")

    def test_cj_main_copies_only_checked_owned_files_and_declares_real_resource(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            root = Path(directory); manifest, _, expected, cfg, csf = cj_assets_fixture(root / "input")
            package = root / "fixture.pak"; package.write_bytes(b"owned package fixture")
            originals = {character.BONE_TEMPLATE: self.bone, character.AVATAR_TEMPLATE: self.avatar,
                         character.CJ_CFG: cfg, character.CJ_SKELETON: csf}
            output = root / "candidate"
            with patch.object(character, "PAK_SHA256", character.digest(package.read_bytes())), \
                 patch.object(character, "INPUTS", {character.BONE_TEMPLATE: character.digest(self.bone), character.AVATAR_TEMPLATE: character.digest(self.avatar)}), \
                 patch.object(character, "read_directory", return_value=[SimpleNamespace(name=name) for name in originals]), \
                 patch.object(character, "read_entry", side_effect=lambda path, entry: originals[entry.name]), \
                 patch.object(sys, "argv", ["prepare_native_character", "--pak", str(package), "--output", str(output), "--character-assets", str(manifest)]):
                self.assertEqual(character.main(), 0)
            report = json.loads((output / "character_manifest.json").read_text())
            actual = {str(path.relative_to(output)).replace("\\", "/") for path in output.rglob("*") if path.is_file()}
            self.assertEqual(actual, set(character.CJ_ASSET_NAMES) | {character.BONE_TEMPLATE, "character_manifest.json"})
            for name, value in expected.items(): self.assertEqual(output.joinpath(*name.split("/")).read_bytes(), value)
            self.assertEqual(report["character_resource"], "BNMD:_avatar/cj_native.cfg")
            self.assertEqual(report["files"][0]["resource"], report["character_resource"])
            self.assertEqual(package.read_bytes(), b"owned package fixture")

    def test_append_preserves_all_stock_bytes_and_only_file_name_is_present(self):
        result, metadata = self.append_fixture()
        source, output = character.parse_template(self.bone), character.parse_template(result)
        self.assertEqual(len(output.records), len(source.records) + 1)
        self.assertEqual(result[:source.count_offset], self.bone[:source.count_offset])
        self.assertEqual(result[source.count_offset + 2:source.records_end], self.bone[source.count_offset + 2:source.records_end])
        self.assertEqual(result[output.records_end:], self.bone[source.records_end:])
        owned = output.records[-1]
        self.assertEqual(owned.name, "_avatar/DummyModel.cfg")
        self.assertEqual(owned.present, ("FileName",))
        self.assertEqual(owned.strings, {"FileName": ("_avatar/DummyModel.cfg",)})
        self.assertTrue(metadata["stock_records_unchanged"])
        self.assertEqual(metadata["stock_record_sha256"], character.digest(self.bone[source.records_begin:source.records_end]))

    def test_real_api_rejects_unsupported_source_identity(self):
        with self.assertRaisesRegex(PakError, "supported original"):
            character.append_declaration(self.bone, self.avatar)

    def test_class_name_remap_and_file_name_kind_are_required(self):
        wrong_class = self.bone.replace(b"BONEDMODELMASTER", b"OTHER_MODEL_KIND")
        with self.assertRaisesRegex(PakError, "class-name remap"):
            self.append_fixture(bone=wrong_class)
        wrong_kind = fixture("BONEDMODELMASTER", (("FileName", 1),), [])
        with self.assertRaisesRegex(PakError, "string property"):
            self.append_fixture(bone=wrong_kind)

    def test_duplicate_owned_name_and_wrong_avatar_filename_are_rejected(self):
        duplicate = fixture("BONEDMODELMASTER", (("FileName", 6),), [(character.RESOURCE_NAME, {})])
        with self.assertRaisesRegex(PakError, "already exists"):
            self.append_fixture(bone=duplicate)
        wrong_path = self.avatar.replace(b"DummyModel.cfg", b"OtherModel.cfg")
        with self.assertRaisesRegex(PakError, "FileName differs"):
            self.append_fixture(avatar=wrong_path)

    def test_missing_avatar_master_and_class_version_are_rejected(self):
        missing = self.avatar.replace(b"gamepad3", b"gamepad4")
        with self.assertRaisesRegex(PakError, "exact supported"):
            self.append_fixture(avatar=missing)
        wrong_version = bytearray(self.bone)
        struct.pack_into("<H", wrong_version, 6 + 4 + len("BONEDMODELMASTER"), 2)
        with self.assertRaisesRegex(PakError, "class version"):
            character.parse_template(bytes(wrong_version))

    def test_truncated_unknown_kind_and_extra_trailer_bytes_rejected(self):
        for data in (self.bone[:-1], self.bone + b"x"):
            with self.assertRaises(PakError):
                character.parse_template(data)
        with self.assertRaisesRegex(PakError, "Unobserved"):
            character.parse_template(fixture("BONEDMODELMASTER", (("FileName", 14),), []))

    def test_output_cannot_escape_ignored_build_directory(self):
        for output in (ROOT / "game/character-test", ROOT / "build_native/../game/character-test"):
            with self.assertRaisesRegex(PakError, "ignored build_native"):
                character.private_output(output)

    @unittest.skipUnless((ROOT / "game/data.pak").exists(), "private original package unavailable")
    def test_private_supported_original_preserves_all_25_declarations(self):
        package = ROOT / "game/data.pak"
        entries = {entry.name: entry for entry in read_directory(package)}
        bone = read_entry(package, entries[character.BONE_TEMPLATE])
        avatar = read_entry(package, entries[character.AVATAR_TEMPLATE])
        output, metadata = character.append_declaration(bone, avatar)
        self.assertEqual(metadata["stock_declarations"], 25)
        self.assertEqual(len(character.parse_template(output).records), 26)
        self.assertEqual(metadata["source_avatar_masters"], ["gamepad0", "gamepad1", "gamepad2", "gamepad3", "random"])
        before, after = character.parse_template(bone), character.parse_template(output)
        for old, new in zip(before.records, after.records, strict=False):
            self.assertEqual(bone[old.begin:old.end], output[new.begin:new.end])

    @unittest.skipUnless((ROOT / "build_native/game_image.bin").exists(), "private original code image unavailable")
    def test_independent_original_import_register_and_existing_named_lookup_oracles(self):
        image = (ROOT / "build_native/game_image.bin").read_bytes()
        self.assertEqual(character.digest(image), "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7")
        words = {
            # The serialized class string is remapped, stored, then used by the
            # binary importer when it calls the ordinary declared factory.
            0x8225E0C0: 0xB3E100B4, 0x8225E1C0: 0xB13F0014,
            0x8225E3AC: 0xA3CB0024, 0x8225E3B8: 0x48001769,
            0x8225E528: 0x480007C1, 0x8225E908: 0x814B005C, 0x8225E910: 0x4E800421,
            # Declared creation dispatches group VT28 and registers its real
            # master; it retains the original fully typed prefix/name key.
            0x8225FD20: 0x4BFEE7A1, 0x8224E4DC: 0x814B001C,
            0x8224E4E4: 0x4E800421, 0x8224E4F4: 0x48000025,
            0x8224E5A0: 0x4BF5CCB1, 0x8224E5C8: 0x4BFEF201,
            # BB90 first queries the registry and copies the original handle;
            # group lookup uses the same typed key and exact stored spelling.
            0x8223BBC8: 0x48001F91, 0x8223BBD4: 0xE9630010,
            0x8224E344: 0x4BFEF815, 0x8223DC04: 0x808B0018, 0x8223DC1C: 0x48396F35,
        }
        for address, expected in words.items():
            self.assertEqual(struct.unpack_from(">I", image, address - 0x82000000)[0], expected,
                             f"independent original instruction at {address:08X}")
        self.assertEqual(image[0x8213D0B8 - 0x82000000:0x8213D0B8 - 0x82000000 + 6], b"%s:%s\0")


if __name__ == "__main__":
    unittest.main()
