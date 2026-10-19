import lzma
import hashlib
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
from tools import inspect_pak as pak
from tools.renderer_inventory import image_inventory, shader_metadata


def compressed(content):
    return lzma.compress(content, format=lzma.FORMAT_ALONE)


def archive(records=None):
    if records is None:
        records = [("data/art/dxshaders/demo.sha", compressed(b"float4 mainPS() { return 1; }\n"), 1)]
    directory_size = sum(16 + len(name.encode()) for name, _, _ in records)
    offset = pak.HEADER_SIZE + directory_size
    directory = bytearray()
    payload = bytearray()
    for name, content, flags in records:
        encoded = name.encode()
        directory += struct.pack("<4I", offset, len(content), flags, len(encoded)) + encoded
        payload += content
        offset += len(content)
    return bytearray(pak.MAGIC + struct.pack("<II", len(records), directory_size) + directory + payload)


class PakTests(unittest.TestCase):
    def test_directory_and_bounded_lzma_round_trip(self):
        data = archive()
        entry = pak.parse_directory(data, len(data))[0]
        self.assertEqual(pak.decode_entry(data[entry.offset:], entry), b"float4 mainPS() { return 1; }\n")

    def test_stat_fstat_ctime_difference_does_not_imply_change(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "game.pak"
            source.write_bytes(archive())
            actual_fstat = pak.os.fstat

            def alternate_ctime(handle):
                state = actual_fstat(handle)
                return SimpleNamespace(st_dev=state.st_dev, st_ino=state.st_ino,
                    st_size=state.st_size, st_mtime_ns=state.st_mtime_ns,
                    st_ctime_ns=state.st_ctime_ns + 1_000_000)

            with mock.patch.object(pak.os, "fstat", side_effect=alternate_ctime):
                result = pak.inspect_pak(source)
            self.assertEqual(result["sha256"], hashlib.sha256(source.read_bytes()).hexdigest())

    def test_changed_file_metadata_during_hash_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "game.pak"
            source.write_bytes(archive())
            actual_digest = pak.hashlib.file_digest

            def changing_digest(stream, algorithm):
                result = actual_digest(stream, algorithm)
                current = source.stat()
                pak.os.utime(source, ns=(current.st_atime_ns, current.st_mtime_ns + 1_000_000_000))
                return result

            with mock.patch.object(pak.hashlib, "file_digest", side_effect=changing_digest), self.assertRaises(pak.PakError):
                pak.inspect_pak(source)

    def test_truncated_and_huge_directory(self):
        data = archive()
        for length in (0, 17, 25, 26, 40):
            with self.subTest(length=length), self.assertRaises(pak.PakError):
                pak.parse_directory(data[:length], len(data))
        struct.pack_into("<I", data, 22, 0xFFFFFFFF)
        with self.assertRaises(pak.PakError):
            pak.parse_directory(data, len(data))

    def test_path_traversal_and_windows_aliases(self):
        for name in ("../evil.sha", "/evil", "C:/evil", "a\\evil", "a/../evil", "a/./evil", "a//evil", "a/NUL.sha", "a/COM1", "a/evil.", "a/evil ", "a/f:stream", "a/x\0"):
            with self.subTest(name=name), self.assertRaises(pak.PakError):
                data = archive([(name, compressed(b"x"), 1)])
                pak.parse_directory(data, len(data))

    def test_duplicate_names_and_payload_overlaps(self):
        data = archive([("data/a", compressed(b"one"), 1), ("DATA/A", compressed(b"two"), 1)])
        with self.assertRaises(pak.PakError):
            pak.parse_directory(data, len(data))
        data = archive([("data/a", compressed(b"one"), 1), ("data/b", compressed(b"two"), 1)])
        struct.pack_into("<I", data, 26 + 16 + len("data/a"), struct.unpack_from("<I", data, 26)[0])
        with self.assertRaises(pak.PakError):
            pak.parse_directory(data, len(data))

    def test_payload_and_filename_ranges(self):
        for offset, value in ((26, 0), (30, 0xFFFFFFFF), (38, 0xFFFFFFFF), (18, 0xFFFFFFFF), (34, 2)):
            data = archive()
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(pak.PakError):
                pak.parse_directory(data, len(data))

    def test_lzma_size_bomb_truncation_and_trailing_bytes(self):
        data = compressed(b"hello")
        entry = pak.PakEntry("data/a", 0, len(data), 1)
        with self.assertRaises(pak.PakError):
            pak.decode_entry(data, entry, max_output=4)
        for body in (data[:-1], data + b"extra"):
            with self.subTest(size=len(body)), self.assertRaises(pak.PakError):
                pak.decode_entry(body, pak.PakEntry("data/a", 0, len(body), 1))
        body = bytearray(data)
        struct.pack_into("<Q", body, 5, pak.MAX_CONTENT + 1)
        with self.assertRaises(pak.PakError):
            pak.decode_entry(body, entry)

    def test_safe_extraction_is_shader_only_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "game.pak"
            source.write_bytes(archive([("data/art/dxshaders/demo.sha", compressed(b"hello"), 1), ("data/other.txt", compressed(b"untouched"), 1)]))
            original = source.read_bytes()
            result = pak.extract_shaders(source, root / "output")
            self.assertEqual(result["extracted_count"], 1)
            self.assertEqual((root / "output/data/art/dxshaders/demo.sha").read_bytes(), b"hello")
            self.assertFalse((root / "output/data/other.txt").exists())
            with self.assertRaises(pak.PakError):
                pak.extract_shaders(source, root / "output")
            self.assertEqual(source.read_bytes(), original)

    def test_existing_symlink_output_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "game.pak"
            source.write_bytes(archive())
            target = root / "actual"
            target.mkdir()
            link = root / "link"
            try:
                link.symlink_to(target, target_is_directory=True)
            except OSError:
                self.skipTest("Host does not permit test symlinks")
            with self.assertRaises(pak.PakError):
                pak.extract_shaders(source, link)

    def test_shader_inventory_keeps_metadata_without_source(self):
        result = shader_metadata("data/art/dxshaders/demo.sha", b'#include "common.fxh"\nfloat4 main_PS() { return 1; }\n')
        self.assertEqual(result["entry_candidates"], ["main_PS"])
        self.assertEqual(result["includes"], ["common.fxh"])
        self.assertNotIn("return 1", str(result))

    def test_single_terminal_nul_shader_source_is_text(self):
        content = b"float4 main_PS() { return 1; }\0"
        result = shader_metadata("data/art/dxshaders/composite.shader_demo", content)
        self.assertEqual(result["content_kind"], "text_shader_source")
        self.assertEqual(result["entry_candidates"], ["main_PS"])
        self.assertTrue(result["terminal_nul"])
        self.assertEqual(result["sha256"], hashlib.sha256(content).hexdigest())
        for malformed in (b"float4 main_PS() {\0 return 1; }", content + b"\0"):
            self.assertEqual(shader_metadata("x", malformed)["content_kind"], "binary_shader_cache")

    def test_verified_image_string_reference_candidates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = bytearray(512)
            image[32:44] = b"LoadShaders\0"
            # Real PPC lis r3,0x8200; addi r3,r3,32 address construction.
            struct.pack_into(">II", image, 128, 0x3C608200, 0x38630020)
            (root / "image.bin").write_bytes(image)
            (root / "map.cpp").write_text("{0x82000080, sub_82000080}, {0x82000100, sub_82000100}")
            metadata = {"image_sha256": hashlib.sha256(image).hexdigest(), "encryption": 1,
                        "image_base": 0x82000000, "imports": []}
            (root / "meta.json").write_text(json.dumps(metadata))
            result = image_inventory(root / "image.bin", root / "meta.json", root / "map.cpp")
            symbol = result["symbols"][0]
            self.assertEqual(symbol["name"], "LoadShaders")
            self.assertEqual(symbol["references"][0]["function_candidate"], "sub_82000080")
            metadata["image_sha256"] = "invalid"
            (root / "meta.json").write_text(json.dumps(metadata))
            with self.assertRaises(pak.PakError):
                image_inventory(root / "image.bin", root / "meta.json", root / "map.cpp")


if __name__ == "__main__":
    unittest.main()
