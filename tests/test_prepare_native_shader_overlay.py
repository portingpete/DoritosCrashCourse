"""Append-only normal precache contracts; synthetic fixtures contain no game data."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_native_shader_overlay as overlay
from compile_title_variants import pack_cache, parse_cache, validate_source_capture
from configure_shader_inputs import IDENTITIES
from inspect_pak import read_directory, read_entry
from tests.test_compile_title_variants import shader_code, source_capture


def source_fixture():
    capture, code = source_capture()
    return capture, pack_cache([{"name": "multilayer/vs", "variants": [validate_source_capture(capture, code)["variant"]]}], True)


def stock_fixture(inventory=False):
    # The before/after order includes masters on both sides of the target.
    _, source = source_fixture()
    variant = parse_cache(source, True)[0]["variants"][0]
    def variants(count):
        return [{**copy.deepcopy(variant), "key": (0, 0, index)} for index in range(count)]
    shaders = [{"name": "before/vs", "variants": variants(1)},
               {"name": "multilayer/vs", "variants": variants(3)},
               {"name": "after/ps", "variants": variants(1)}]
    if inventory:
        shaders = [{"name": "multilayer/vs", "variants": variants(189)}]
        shaders += [{"name": f"fixture{index}/vs", "variants": variants(153 if index == 0 else 1)} for index in range(11)]
    return pack_cache(shaders, True)


class ShaderOverlayTests(unittest.TestCase):
    def test_append_preserves_all_stock_raw_bytes_and_exact_source_record(self):
        original = stock_fixture(); _, source = source_fixture()
        merged, info = overlay.merge_cache(original, source)
        self.assertEqual(merged[:12], original[:12])
        self.assertEqual(info["stock_variants"], 5)
        self.assertEqual(info["owned_variants_added"], 1)
        count, at, added = (info[key] for key in ("variant_count_offset", "append_offset", "append_bytes"))
        self.assertEqual(merged[:count], original[:count])
        self.assertEqual(merged[count + 4:at], original[count + 4:at])
        self.assertEqual(merged[at + added:], original[at:])
        self.assertEqual(struct.unpack_from(">I", merged, count)[0], 4)
        before, after = parse_cache(original, True), parse_cache(merged, True)
        self.assertEqual(after[0], before[0]); self.assertEqual(after[2], before[2])
        self.assertEqual(after[1]["variants"][:-1], before[1]["variants"])
        self.assertEqual(after[1]["variants"][-1], parse_cache(source, True)[0]["variants"][0])
        self.assertEqual(info["output_sha256"], hashlib.sha256(merged).hexdigest())

    def test_existing_key_unknown_master_unmeasured_key_and_platform_rejected(self):
        original = stock_fixture(); _, source = source_fixture()
        merged, _ = overlay.merge_cache(original, source)
        with self.assertRaisesRegex(ValueError, "replace"):
            overlay.merge_cache(merged, source)
        stock = parse_cache(original, True); stock[1]["name"] = "different/vs"
        with self.assertRaisesRegex(ValueError, "existing stock"):
            overlay.merge_cache(pack_cache(stock, True), source)
        extra = parse_cache(source, True); extra[0]["variants"][0]["key"] = (8388614, 0, 203)
        with self.assertRaisesRegex(ValueError, "measured"):
            overlay.merge_cache(original, pack_cache(extra, True))
        with self.assertRaises(ValueError): overlay.merge_cache(original, source[:-1])
        with self.assertRaises(ValueError): overlay.merge_cache(original, source + b"\0")
        with self.assertRaises(ValueError): overlay.merge_cache(original, pack_cache(extra))

    def test_prepare_reuses_real_pair_validator_and_writes_only_cache_and_sidecar(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            root = Path(directory); report_root = root / "report"; report_root.mkdir()
            capture, source = source_fixture(); original = stock_fixture(True)
            source_path, native_path = report_root / "source.bsh", report_root / "native.bsh"
            source_path.write_bytes(source)
            native = shader_code(register=0, count=192)
            native_path.write_bytes(pack_cache([{"name": "multilayer/vs", "variants": [
                {"key": (8388614, 0, 75), "parameters": [(81, 0, 192, 0)], "code": native}]}]))
            report = {**IDENTITIES, "extension_path": str(native_path), "extension_sha256": overlay.digest(native_path.read_bytes()),
                      "extension_variants": 1, "source_extension_path": str(source_path),
                      "source_extension_sha256": overlay.digest(source), "source_extension_variants": 1,
                      "source_capture_count": 1, "trusted_shader_stages": {name: True for name in
                      ("composite/vs", "composite/ps", "multilayer/vs", "multilayer/ps")},
                      "parameter_names": [f"g_unused{index}" for index in range(81)] + ["g_test"],
                      "results": [{"name": "multilayer/vs", "key": [8388614, 0, 75], "status": "verified",
                      "exit_code": 0, "known_native_key": False, "bytecode_sha256": overlay.digest(native),
                      "parameters": [[81, 0, 192, 0]], "source_capture": capture, "original_parameters": [[81, 8, 192, 0]]}]}
            manifest = report_root / "metadata.json"; manifest.write_text(json.dumps(report))
            package = root / "fixture.pak"; package.write_bytes(b"unchanged package fixture")
            output = root / "candidate"
            # The real pair validator is not mocked. Only package access is an
            # independent synthetic cache source with the observed inventory.
            identities = {**IDENTITIES, "xenon_cache_sha256": overlay.digest(original)}
            with patch.object(overlay, "IDENTITIES", identities), \
                 patch.object(overlay, "inspect_pak", return_value={"sha256": IDENTITIES["package_sha256"]}), \
                 patch.object(overlay, "read_directory", return_value=[SimpleNamespace(name=overlay.CACHE_NAME)]), \
                 patch.object(overlay, "read_entry", return_value=original):
                result = overlay.prepare(package, manifest, output)
                self.assertEqual(result["files"][0]["stock_variants"], 352)
                actual = {str(path.relative_to(output)).replace("\\", "/") for path in output.rglob("*") if path.is_file()}
                self.assertEqual(actual, {overlay.CACHE_NAME, "shader_overlay_manifest.json"})
                report["results"][0]["original_parameters"][0][1] = 9
                manifest.write_text(json.dumps(report))
                with self.assertRaisesRegex(ValueError, "reflected bindings"):
                    overlay.prepare(package, manifest, root / "rejected")
                self.assertFalse((root / "rejected").exists())
            self.assertEqual(package.read_bytes(), b"unchanged package fixture")

    def test_private_paths_and_missing_capture_reject_before_generation(self):
        with self.assertRaisesRegex(ValueError, "ignored build_native"):
            overlay.prepare(ROOT / "game/data.pak", ROOT / "game/unowned.json", ROOT / "build_native/unused")
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            root = Path(directory); report = root / "metadata.json"; report.write_text("{}")
            with self.assertRaisesRegex(ValueError, "one verified paired"):
                overlay.prepare(ROOT / "game/data.pak", report, root / "rejected")
            self.assertFalse((root / "rejected").exists())

    @unittest.skipUnless((ROOT / "game/data.pak").exists(), "Private original package unavailable")
    def test_private_original_cache_all352_stock_records_byte_roundtrip(self):
        package = ROOT / "game/data.pak"
        entries = {entry.name: entry for entry in read_directory(package)}
        original = read_entry(package, entries[overlay.CACHE_NAME])
        self.assertEqual(overlay.digest(original), IDENTITIES["xenon_cache_sha256"])
        self.assertEqual(pack_cache(parse_cache(original, True), True), original)
        _, source = source_fixture(); merged, info = overlay.merge_cache(original, source)
        self.assertEqual(info["stock_shader_masters"], 12); self.assertEqual(info["stock_variants"], 352)
        self.assertEqual(sum(len(shader["variants"]) for shader in parse_cache(merged, True)), 353)

    @unittest.skipUnless((ROOT / "build_native/game_image.bin").exists(), "Private original code unavailable")
    def test_original_cached_loader_readiness_and_shader_create_words(self):
        image = (ROOT / "build_native/game_image.bin").read_bytes()
        self.assertEqual(overlay.digest(image), IDENTITIES["image_sha256"])
        words = {0x82387BCC: 0x4800075D, 0x82387BD8: 0x91430008, 0x82387BE0: 0x9123000C,
                 0x82387BE8: 0x91030010, 0x82387CF0: 0x807E002C, 0x82387CF8: 0x907E0030,
                 0x82387D14: 0x80DE002C, 0x82387D30: 0x807E0030, 0x82387D48: 0x480388E1,
                 0x82387D94: 0x9ADE0004, 0x8238777C: 0x89630004, 0x82387784: 0x409A0010,
                 0x82387790: 0x4BFFF9B1}
        for address, expected in words.items():
            with self.subTest(address=hex(address)):
                self.assertEqual(struct.unpack_from(">I", image, address - 0x82000000)[0], expected)


if __name__ == "__main__":
    unittest.main()
