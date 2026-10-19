"""Exercise the real native loader against disposable intake copies.

Set NATIVE_XEX_INSPECTOR to a built NativeXexInspect executable. Proprietary
inputs and decoded outputs stay in temporary storage and are never fixtures.
"""

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = ROOT / "game" / "default.xex"
XEX_SHA256 = "6c4fa5a12ef2ad51db4809d5d654b487300a1ace06c2cdf6cb14258fd273dfc1"
IMAGE_SHA256 = "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7"


class NativeLoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        executable = os.environ.get("NATIVE_XEX_INSPECTOR")
        if not executable or not Path(executable).is_file():
            raise unittest.SkipTest("NATIVE_XEX_INSPECTOR is unavailable")
        if not ORIGINAL.is_file():
            raise unittest.SkipTest("Original user-supplied XEX is unavailable")
        cls.executable = str(Path(executable).resolve())
        cls.stock = ORIGINAL.read_bytes()
        if hashlib.sha256(cls.stock).hexdigest() != XEX_SHA256:
            raise unittest.SkipTest("Original dump is a different revision")
        count = struct.unpack_from(">I", cls.stock, 20)[0]
        fields = dict(struct.iter_unpack(">II", cls.stock[24:24 + count * 8]))
        cls.format = fields[0x3FF]

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="doritos-native-loader-")
        self.directory = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def tearDown(self):
        self.assertEqual(hashlib.sha256(ORIGINAL.read_bytes()).hexdigest(), XEX_SHA256)

    def invoke(self, source, *arguments):
        return subprocess.run([self.executable, str(source), *map(str, arguments)],
                              text=True, capture_output=True, timeout=30)

    def reject(self, data, expected):
        source = self.directory / "input.xex"
        output = self.directory / "image.bin"
        source.write_bytes(data)
        result = self.invoke(source, "--output", output)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(expected, result.stderr)
        self.assertFalse(output.exists(), "Rejected intake must not produce an image")

    def test_supported_encrypted_image_has_paired_hash_and_original_imports(self):
        output, metadata = self.directory / "image.bin", self.directory / "image.json"
        result = self.invoke(ORIGINAL, "--output", output, "--metadata", metadata)
        self.assertEqual(result.returncode, 0, result.stderr)
        image = output.read_bytes()
        facts = json.loads(metadata.read_text())
        self.assertEqual(hashlib.sha256(image).hexdigest(), IMAGE_SHA256)
        self.assertEqual(facts["xex_sha256"], XEX_SHA256)
        self.assertEqual(facts["image_sha256"], IMAGE_SHA256)
        self.assertEqual(facts["encryption"], 1)
        self.assertEqual(facts["entry_point"], 0x823E1790)
        self.assertEqual(len(facts["imports"]), 503)
        for record in facts["imports"]:
            word = struct.unpack_from(">I", image, record["address"] - facts["image_base"])[0]
            self.assertEqual(word >> 24, record["kind"])
            self.assertEqual(word & 0xFFFF, record["ordinal"])

    def test_unencrypted_version_stops_before_output(self):
        data = bytearray(self.stock)
        struct.pack_into(">H", data, self.format + 4, 0)
        self.reject(data, "STOP: default.xex is unencrypted")

    def test_encrypted_payload_tampering_is_rejected(self):
        data = bytearray(self.stock)
        data[-1] ^= 1
        self.reject(data, "differs from the supported AOT revision")

    def test_truncated_input_is_rejected(self):
        self.reject(self.stock[:24], "header layout")

    def test_oversized_optional_directory_is_rejected(self):
        data = bytearray(self.stock)
        struct.pack_into(">I", data, 20, 0xFFFFFFFF)
        self.reject(data, "header layout")

    def test_out_of_bounds_format_header_is_rejected(self):
        data = bytearray(self.stock)
        count = struct.unpack_from(">I", data, 20)[0]
        for index in range(count):
            if struct.unpack_from(">I", data, 24 + index * 8)[0] == 0x3FF:
                struct.pack_into(">I", data, 28 + index * 8, 0xFFFFFFFF)
                break
        self.reject(data, "truncated")

    def test_inspector_cannot_overwrite_original_input(self):
        result = self.invoke(ORIGINAL, "--output", ORIGINAL)
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot overwrite the original XEX", result.stderr)

    def test_case_insensitive_output_collision_is_rejected(self):
        result = self.invoke(ORIGINAL, "--output", self.directory / "image.bin",
                             "--metadata", self.directory / "IMAGE.BIN")
        self.assertEqual(result.returncode, 1)
        self.assertIn("different paths", result.stderr)

    def test_hardlinked_output_collision_is_rejected_without_writing(self):
        image, metadata = self.directory / "image.bin", self.directory / "image.json"
        sentinel = b"existing output must stay intact"
        image.write_bytes(sentinel)
        os.link(image, metadata)
        result = self.invoke(ORIGINAL, "--output", image, "--metadata", metadata)
        self.assertEqual(result.returncode, 1)
        self.assertIn("different paths", result.stderr)
        self.assertEqual(image.read_bytes(), sentinel)
        self.assertEqual(metadata.read_bytes(), sentinel)


if __name__ == "__main__":
    unittest.main()
