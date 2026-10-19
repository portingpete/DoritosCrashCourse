"""Synthetic inputs only: no proprietary executable or decrypted fixtures."""

import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import types
import unittest
from unittest.mock import patch

from tools import inspect_xex as intake


HEADER_SIZE = 4096
SECURITY = 0x100
FORMAT = 0x300
EXECUTION = 0x324
TLS = 0x33C
RESOURCE = 0x34C
KEY_FIELD = 0x360
IMAGE_BASE = 0x82000000
IMAGE_SIZE = 65536


def put32(data, offset, value):
    struct.pack_into(">I", data, offset, value)


def synthetic_xex(encryption=1, compression=2):
    payload_size = IMAGE_SIZE if compression == 0 else 128
    data = bytearray(HEADER_SIZE + payload_size)
    records = [
        (intake.FILE_FORMAT, FORMAT), (intake.EXECUTION_INFO, EXECUTION),
        (intake.TLS_INFO, TLS), (intake.RESOURCE_INFO, RESOURCE),
        (intake.ENTRY_POINT, IMAGE_BASE + 0x100),
        (intake.IMAGE_BASE, IMAGE_BASE), (0x00040404, KEY_FIELD),
    ]
    struct.pack_into(">4s5I", data, 0, b"XEX2", 1, HEADER_SIZE, 0, SECURITY, len(records))
    for index, record in enumerate(records):
        struct.pack_into(">II", data, 24 + index * 8, *record)
    put32(data, SECURITY, 0x184 + 24)
    put32(data, SECURITY + 4, IMAGE_SIZE)
    put32(data, SECURITY + 0x110, IMAGE_BASE)
    put32(data, SECURITY + 0x180, 1)
    put32(data, SECURITY + 0x184, 0x11)  # One 64-KiB code page.
    format_size = {0: 8, 1: 16}.get(compression, 36)
    struct.pack_into(">IHH", data, FORMAT, format_size, encryption, compression)
    if compression == 1:
        struct.pack_into(">II", data, FORMAT + 8, 64, IMAGE_SIZE - 64)
    elif compression == 2:
        struct.pack_into(">II", data, FORMAT + 8, 32768, 64)
    struct.pack_into(">4I4BI", data, EXECUTION, 0x12345678, 2, 2, 0x58410A71, 0, 0, 0, 0, 0)
    struct.pack_into(">4I", data, TLS, 64, IMAGE_BASE + 0x200, 32, 16)
    put32(data, RESOURCE, 20)
    data[RESOURCE + 4:RESOURCE + 12] = b"TESTRES0"
    struct.pack_into(">II", data, RESOURCE + 12, IMAGE_BASE + 0x300, 16)
    data[KEY_FIELD:KEY_FIELD + 16] = b"never-print-keys"
    return data


def parse(data):
    return intake.parse_xex_header(bytes(data[:HEADER_SIZE]), len(data))


class XexHeaderTests(unittest.TestCase):
    def assert_rejected(self, data, message=None, code="malformed_xex"):
        with self.assertRaises(intake.XexError) as raised:
            parse(data)
        self.assertEqual(raised.exception.code, code)
        if message:
            self.assertIn(message, str(raised.exception))

    def test_encrypted_normal_compression_metadata(self):
        result = parse(synthetic_xex())
        self.assertTrue(result["encrypted"])
        self.assertEqual(result["compression"]["name"], "normal_lzx")
        self.assertEqual(result["execution"]["title_id"], "0x58410A71")
        self.assertEqual(result["security"]["pages_by_type"]["code"], 1)
        self.assertEqual(result["tls"]["raw_data_size"], 16)
        self.assertEqual(result["resources"], [{"address": "0x82000300", "size": 16}])
        output = json.dumps(result)
        self.assertNotIn("never-print-keys", output)
        self.assertNotIn("TESTRES0", output)

    def test_regular_unencrypted_input_stops(self):
        self.assert_rejected(synthetic_xex(encryption=0), "STOP", "unencrypted_xex")

    def test_unknown_encryption_fails_closed(self):
        self.assert_rejected(synthetic_xex(encryption=2), "encryption", "unsupported_encryption")

    def test_missing_file_format_does_not_assume_encryption(self):
        data = synthetic_xex()
        put32(data, 24, 0x000009FF)
        self.assert_rejected(data, "encryption cannot be established")

    def test_truncated_fixed_header_and_header_body(self):
        for length in (0, 4, 23, 24, 100, HEADER_SIZE - 1):
            with self.subTest(length=length), self.assertRaises(intake.XexError):
                intake.parse_xex_header(bytes(synthetic_xex()[:length]), HEADER_SIZE + 128)

    def test_payload_truncation(self):
        self.assert_rejected(synthetic_xex()[:-1], "AES-block aligned")

    def test_duplicate_optional_headers(self):
        data = synthetic_xex()
        put32(data, 32, intake.FILE_FORMAT)
        self.assert_rejected(data, "Duplicate optional")

    def test_duplicate_header_with_changed_size_encoding(self):
        data = synthetic_xex()
        put32(data, 32, 0x00000301)
        self.assert_rejected(data, "Duplicate optional")

    def test_excessive_header_count(self):
        data = synthetic_xex()
        put32(data, 20, 0xFFFFFFFF)
        self.assert_rejected(data, "excessive")

    def test_header_size_bounds(self):
        for size in (0, 23, len(synthetic_xex()), intake.MAX_HEADER_SIZE + 1):
            data = synthetic_xex()
            put32(data, 8, size)
            with self.subTest(size=size):
                self.assert_rejected(data, "header size")

    def test_optional_ranges_must_stay_in_header(self):
        for offset in (8, FORMAT + 1, HEADER_SIZE, 0xFFFFFFFC):
            data = synthetic_xex()
            put32(data, 28, offset)
            with self.subTest(offset=offset):
                self.assert_rejected(data)

    def test_zero_and_huge_variable_sizes(self):
        for size in (0, 3, 0xFFFFFFFF):
            data = synthetic_xex()
            put32(data, FORMAT, size)
            with self.subTest(size=size):
                self.assert_rejected(data)

    def test_overlapping_optional_ranges(self):
        data = synthetic_xex()
        put32(data, 44, EXECUTION)  # TLS starts inside execution metadata.
        self.assert_rejected(data, "Overlapping")

    def test_optional_range_overlapping_security(self):
        data = synthetic_xex()
        put32(data, 76, SECURITY + 0x150)  # LAN-key field aliases security.
        self.assert_rejected(data, "Overlapping")

    def test_invalid_security_range(self):
        data = synthetic_xex()
        put32(data, SECURITY, HEADER_SIZE)
        self.assert_rejected(data, "Security info")

    def test_descriptor_count_cannot_escape_security_info(self):
        data = synthetic_xex()
        put32(data, SECURITY + 0x180, 0xFFFFFFFF)
        self.assert_rejected(data, "Page-descriptor table")

    def test_bad_page_descriptors(self):
        for descriptor in (0, 0x10, 0x14, 0x21):
            data = synthetic_xex()
            put32(data, SECURITY + 0x184, descriptor)
            with self.subTest(descriptor=descriptor):
                self.assert_rejected(data)

    def test_guest_address_overflow(self):
        data = synthetic_xex()
        put32(data, SECURITY + 0x110, 0xFFFF8000)
        self.assert_rejected(data, "guest image address")

    def test_entrypoint_must_be_in_image_and_aligned(self):
        for address in (IMAGE_BASE - 4, IMAGE_BASE + IMAGE_SIZE, IMAGE_BASE + 1):
            data = synthetic_xex()
            put32(data, 60, address)
            with self.subTest(address=address):
                self.assert_rejected(data)

    def test_resource_must_be_inside_guest_image(self):
        data = synthetic_xex()
        put32(data, RESOURCE + 16, IMAGE_SIZE)
        self.assert_rejected(data, "Resource")

    def test_tls_template_bounds(self):
        data = synthetic_xex()
        put32(data, TLS + 12, 33)
        self.assert_rejected(data, "larger than TLS")
        data = synthetic_xex()
        put32(data, TLS + 4, IMAGE_BASE + IMAGE_SIZE)
        self.assert_rejected(data, "TLS raw template")

    def test_invalid_lzx_metadata(self):
        for offset, value in ((FORMAT + 8, 32000), (FORMAT + 12, 129), (FORMAT, 32)):
            data = synthetic_xex()
            put32(data, offset, value)
            with self.subTest(offset=offset):
                self.assert_rejected(data)

    def test_basic_and_uncompressed_inputs(self):
        for compression, name in ((0, "none"), (1, "basic")):
            with self.subTest(compression=compression):
                self.assertEqual(parse(synthetic_xex(compression=compression))["compression"]["name"], name)

    def test_basic_compression_sum_mismatch(self):
        data = synthetic_xex(compression=1)
        put32(data, FORMAT + 12, IMAGE_SIZE)
        self.assert_rejected(data, "do not cover")

    def test_patch_or_unknown_compression_rejected(self):
        self.assert_rejected(synthetic_xex(compression=3), "compression", "unsupported_compression")
        data = synthetic_xex()
        put32(data, 4, 0x11)
        self.assert_rejected(data, "Patch XEX")


class XexFileTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "default.xex"
        self.path.write_bytes(synthetic_xex())

    def test_file_inspection_is_read_only_and_reports_identity(self):
        before = self.path.read_bytes()
        before_stat = self.path.stat()
        result = intake.inspect_xex(self.path)
        self.assertEqual(result["sha256"], hashlib.sha256(before).hexdigest())
        self.assertFalse(result["supported"])
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(self.path.stat().st_mtime_ns, before_stat.st_mtime_ns)

    def test_require_supported_rejects_other_encrypted_inputs(self):
        with self.assertRaises(intake.XexError) as raised:
            intake.inspect_xex(self.path, require_supported=True)
        self.assertEqual(raised.exception.code, "unsupported_input")

    def test_supported_hash_and_size_are_both_required(self):
        sha = hashlib.sha256(self.path.read_bytes()).hexdigest()
        with patch.object(intake, "SUPPORTED_SHA256", sha), patch.object(intake, "SUPPORTED_SIZE", self.path.stat().st_size):
            self.assertTrue(intake.inspect_xex(self.path, require_supported=True)["supported"])
        with patch.object(intake, "SUPPORTED_SHA256", sha):
            self.assertFalse(intake.inspect_xex(self.path)["supported"])

    def test_concurrent_file_change_is_rejected(self):
        metadata = self.path.stat()
        altered = types.SimpleNamespace(st_mode=metadata.st_mode, st_size=metadata.st_size,
                                        st_mtime_ns=metadata.st_mtime_ns + 1, st_ctime_ns=metadata.st_ctime_ns)
        with patch.object(intake.os, "fstat", side_effect=[metadata, altered]):
            with self.assertRaises(intake.XexError) as raised:
                intake.inspect_xex(self.path)
        self.assertEqual(raised.exception.code, "input_changed")

    def test_cli_json_success_and_unencrypted_failure(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(intake.main([str(self.path), "--json"]), 0)
        self.assertTrue(json.loads(output.getvalue())["ok"])
        self.path.write_bytes(synthetic_xex(encryption=0))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(intake.main([str(self.path), "--json"]), 2)
        result = json.loads(output.getvalue())
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "unencrypted_xex")

    def test_cli_missing_file_is_machine_readable(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(intake.main([str(self.path.with_name("missing.xex")), "--json"]), 2)
        self.assertEqual(json.loads(output.getvalue())["error"]["code"], "input_io_error")


if __name__ == "__main__":
    unittest.main()
