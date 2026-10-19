"""Exercise the AOT integrity gate without executing game-derived code."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
import recompile


class GenerationGateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="doritos-gate-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.xenon = self.root / "third_party/XenonRecomp"
        self.xenon.mkdir(parents=True)
        self.generator = self.xenon / "generator.exe"
        self.generator.write_bytes(b"synthetic fixture, never executed")
        self.source = self.root / "config.txt"
        self.source.write_bytes(b"configuration fixture")
        self.output = self.root / "generated"
        self.output.mkdir()
        self.generated = self.output / "fixture.cpp"
        self.generated.write_bytes(b"generated fixture")
        self.manifest = {"format": 1, "profile": "complete-translation", "runtime_verified": False,
                         "semantic_diagnostics": 0, "generator_path": self.generator.relative_to(self.root).as_posix(),
                         "generator_sha256": recompile.sha256(self.generator),
                         "inputs": {"config.txt": recompile.sha256(self.source)},
                         "outputs": {"fixture.cpp": recompile.sha256(self.generated)}}
        self.save()

    def save(self):
        (self.output / "manifest.json").write_text(json.dumps(self.manifest))

    def verify(self, diagnostic=False):
        with patch.object(recompile, "ROOT", self.root), patch.object(recompile, "XENON", self.xenon), \
             patch.object(recompile, "sources", return_value=[self.source]), \
             patch.object(recompile, "validate_source"), contextlib.redirect_stdout(io.StringIO()):
            return recompile.verify(self.output, diagnostic)

    def test_complete_translation_is_accepted(self):
        self.assertEqual(self.verify(), 0)

    def test_changed_source_is_rejected(self):
        self.source.write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "Changed AOT inputs"):
            self.verify()

    def test_changed_output_is_rejected(self):
        self.generated.write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "Changed AOT outputs"):
            self.verify()

    def test_changed_generator_is_rejected(self):
        self.generator.write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "Generator binary changed"):
            self.verify()

    def test_diagnostic_output_cannot_pass_ordinary_gate(self):
        self.manifest.update(profile="diagnostic", semantic_diagnostics=1)
        self.save()
        with self.assertRaisesRegex(RuntimeError, "ordinary build refused"):
            self.verify()
        self.assertEqual(self.verify(True), 0)

    def test_false_complete_profile_is_rejected(self):
        self.manifest["semantic_diagnostics"] = 1
        self.save()
        with self.assertRaisesRegex(RuntimeError, "profile disagrees"):
            self.verify(True)

    def test_path_escape_is_rejected(self):
        self.manifest["outputs"] = {"../config.txt": recompile.sha256(self.source)}
        self.save()
        with self.assertRaisesRegex(RuntimeError, "Changed AOT outputs"):
            self.verify()

    def test_unencrypted_source_stops_before_hash_check(self):
        game = self.root / "game"
        game.mkdir()
        source = bytearray(48)
        source[:4] = b"XEX2"
        struct.pack_into(">I", source, 20, 1)
        struct.pack_into(">II", source, 24, 0x3FF, 32)
        struct.pack_into(">IHH", source, 32, 8, 0, 0)
        (game / "default.xex").write_bytes(source)
        with patch.object(recompile, "ROOT", self.root), self.assertRaisesRegex(RuntimeError, "STOP"):
            recompile.validate_source()

    def test_undefined_function_local_label_is_rejected(self):
        self.generated.write_text("PPC_FUNC_IMPL(first) {\n goto loc_2000;\n}\n"
                                  "PPC_FUNC_IMPL(second) {\nloc_2000:\n return;\n}\n")
        with self.assertRaisesRegex(RuntimeError, "undefined labels"):
            recompile.verify_control_flow(self.generated)

    def test_resolved_function_local_label_is_accepted(self):
        self.generated.write_text("PPC_FUNC_IMPL(first) {\n goto loc_2000;\nloc_2000:\n return;\n}\n")
        recompile.verify_control_flow(self.generated)


if __name__ == "__main__":
    unittest.main()
