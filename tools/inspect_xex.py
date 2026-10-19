#!/usr/bin/env python3
"""Read-only encrypted XEX2 intake gate; never decrypts or dumps game data.

Layout reference: Xenia's src/xenia/kernel/util/xex2_info.h.
This checks container metadata, not RSA signatures or the encrypted payload.
The supported SHA-256 is the separate identity check for this project's input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys

SUPPORTED_SHA256 = "6c4fa5a12ef2ad51db4809d5d654b487300a1ace06c2cdf6cb14258fd273dfc1"
SUPPORTED_SIZE = 4_501_504
MAX_HEADER_SIZE = 16 * 1024 * 1024
MAX_OPTIONAL_HEADERS = 4096
FILE_FORMAT = 0x000003FF
EXECUTION_INFO = 0x00040006
TLS_INFO = 0x00020104
RESOURCE_INFO = 0x000002FF
ENTRY_POINT = 0x00010100
IMAGE_BASE = 0x00010201


class XexError(ValueError):
    """An input rejected by the fail-closed intake gate."""

    def __init__(self, message: str, code: str = "malformed_xex") -> None:
        super().__init__(message)
        self.code = code


def _range(offset: int, size: int, limit: int, label: str) -> None:
    if offset < 0 or size < 0 or offset > limit or size > limit - offset:
        raise XexError(f"{label} exceeds its containing range")


def _u32(data: bytes, offset: int) -> int:
    _range(offset, 4, len(data), "32-bit field")
    return struct.unpack_from(">I", data, offset)[0]


def _hex(value: int) -> str:
    return f"0x{value:08X}"


def parse_xex_header(header: bytes, file_size: int) -> dict:
    """Parse bounded plaintext metadata. Payload bytes are never required."""
    if len(header) < 24:
        raise XexError("XEX2 fixed header is truncated")
    magic, module_flags, header_size, reserved, security_offset, count = struct.unpack_from(
        ">4s5I", header
    )
    if magic != b"XEX2":
        raise XexError("Input is not an XEX2 container")
    if reserved != 0:
        raise XexError("XEX2 reserved header field is nonzero")
    if not 24 <= header_size <= MAX_HEADER_SIZE or header_size >= file_size:
        raise XexError("Invalid or unsupported XEX2 header size")
    _range(0, header_size, len(header), "XEX2 header")
    header = header[:header_size]
    if count == 0 or count > MAX_OPTIONAL_HEADERS:
        raise XexError("Invalid or excessive optional-header count")
    table_end = 24 + count * 8
    _range(24, count * 8, header_size, "Optional-header table")
    records = {}
    semantic_keys = set()
    spans = [(0, table_end, "fixed header and optional-header table")]
    record_summary = []
    for index in range(count):
        key, value = struct.unpack_from(">II", header, 24 + index * 8)
        semantic_key = key >> 8
        if semantic_key in semantic_keys:
            raise XexError(f"Duplicate optional header {_hex(key)}")
        semantic_keys.add(semantic_key)
        words = key & 0xFF
        extent = 0
        if words > 1:
            if value < table_end or value % 4:
                raise XexError(f"Invalid offset for optional header {_hex(key)}")
            extent = _u32(header, value) if words == 0xFF else words * 4
            if extent < 4:
                raise XexError(f"Invalid size for optional header {_hex(key)}")
            _range(value, extent, header_size, f"Optional header {_hex(key)}")
            spans.append((value, value + extent, f"optional header {_hex(key)}"))
        records[key] = (value, extent)
        # Do not disclose values of unknown headers or key-bearing headers.
        record_summary.append({"key": _hex(key), "storage": "inline" if words <= 1 else "offset", "size": extent})

    if FILE_FORMAT not in records:
        raise XexError("File-format header is missing; encryption cannot be established")
    format_offset, format_size = records[FILE_FORMAT]
    if format_size < 8:
        raise XexError("File-format header is truncated")
    encryption, compression = struct.unpack_from(">HH", header, format_offset + 4)
    if encryption == 0:
        raise XexError(
            "STOP: XEX is unencrypted. Supply the regular encrypted default.xex from your own game before proceeding.",
            "unencrypted_xex",
        )
    if encryption != 1:
        raise XexError("Unknown XEX encryption type; regular encrypted XEX is required", "unsupported_encryption")
    if module_flags & 0x70:
        raise XexError("Patch XEX containers are not regular full game executables")
    payload_size = file_size - header_size
    if payload_size % 16:
        raise XexError("Encrypted XEX payload is not AES-block aligned")
    compression_info = {"type": compression}
    if compression == 0:
        if format_size != 8:
            raise XexError("Uncompressed file-format metadata has an invalid size")
        compression_info["name"] = "none"
    elif compression == 1:
        if format_size < 16 or (format_size - 8) % 8:
            raise XexError("Basic-compression block table has an invalid size")
        data_total = zero_total = 0
        for offset in range(format_offset + 8, format_offset + format_size, 8):
            data_size, zero_size = struct.unpack_from(">II", header, offset)
            if data_size == 0 and zero_size == 0:
                raise XexError("Empty basic-compression block")
            data_total += data_size
            zero_total += zero_size
        if data_total > payload_size or data_total + zero_total > 0xFFFFFFFF:
            raise XexError("Basic-compression sizes exceed their containing range")
        compression_info.update(name="basic", block_count=(format_size - 8) // 8,
                                data_size=data_total, zero_size=zero_total)
    elif compression == 2:
        if format_size != 36:
            raise XexError("Normal LZX file-format metadata has an invalid size")
        window_size, first_block_size = struct.unpack_from(">II", header, format_offset + 8)
        if not 32768 <= window_size <= 2**21 or window_size & (window_size - 1):
            raise XexError("Invalid LZX window size")
        if not 24 <= first_block_size <= payload_size:
            raise XexError("LZX first block exceeds encrypted payload")
        compression_info.update(name="normal_lzx", window_size=window_size,
                                first_block_size=first_block_size)
    else:
        raise XexError("Unsupported compression type; a regular full XEX is required", "unsupported_compression")

    if security_offset < table_end or security_offset % 4:
        raise XexError("Invalid security-info offset")
    _range(security_offset, 0x184, header_size, "Security-info fixed fields")
    security_size = _u32(header, security_offset)
    descriptor_count = _u32(header, security_offset + 0x180)
    if security_size < 0x184 or descriptor_count == 0:
        raise XexError("Invalid security-info size or empty page-descriptor table")
    _range(security_offset, security_size, header_size, "Security info")
    if 0x184 + descriptor_count * 24 > security_size:
        raise XexError("Page-descriptor table exceeds security info")
    spans.append((security_offset, security_offset + security_size, "security info"))
    spans.sort()
    for previous, following in zip(spans, spans[1:]):
        if following[0] < previous[1]:
            raise XexError(f"Overlapping {previous[2]} and {following[2]}")

    image_size = _u32(header, security_offset + 4)
    image_flags = _u32(header, security_offset + 0x10C)
    image_base = _u32(header, security_offset + 0x110)
    if image_size == 0 or image_base == 0 or image_base + image_size > 2**32:
        raise XexError("Invalid guest image address range")
    if IMAGE_BASE in records and records[IMAGE_BASE][0] != image_base:
        raise XexError("Optional image base conflicts with security-info load address")

    def guest_range(address: int, size: int, label: str) -> None:
        if address < image_base:
            raise XexError(f"{label} lies below the guest image")
        _range(address - image_base, size, image_size, label)

    page_size = 4096 if image_flags & 0x10000000 else 65536
    page_counts = {"code": 0, "data": 0, "read_only": 0}
    names = {1: "code", 2: "data", 3: "read_only"}
    for index in range(descriptor_count):
        packed = _u32(header, security_offset + 0x184 + index * 24)
        kind, pages = packed & 15, packed >> 4
        if kind not in names or pages == 0:
            raise XexError("Invalid security page descriptor")
        page_counts[names[kind]] += pages
    if sum(page_counts.values()) != (image_size + page_size - 1) // page_size:
        raise XexError("Security page descriptors do not cover the guest image size")
    if compression == 0 and image_size > payload_size:
        raise XexError("Uncompressed guest image exceeds encrypted payload")
    if compression == 1 and image_size != compression_info["data_size"] + compression_info["zero_size"]:
        raise XexError("Basic-compression blocks do not cover the guest image size")

    entry = records.get(ENTRY_POINT, (None, 0))[0]
    if entry is not None:
        guest_range(entry, 4, "Entry point")
        if entry % 4:
            raise XexError("PowerPC entry point is not instruction aligned")
    identity = None
    if EXECUTION_INFO in records:
        offset, _ = records[EXECUTION_INFO]
        media, version, base_version, title, platform, table, disc, discs, save = struct.unpack_from(
            ">4I4BI", header, offset
        )
        if disc > discs:
            raise XexError("Execution-info disc number exceeds disc count")
        identity = {"title_id": _hex(title), "media_id": _hex(media),
                    "version": _hex(version), "base_version": _hex(base_version),
                    "platform": platform, "executable_table": table,
                    "disc_number": disc, "disc_count": discs, "savegame_id": _hex(save)}
    tls = None
    if TLS_INFO in records:
        offset, _ = records[TLS_INFO]
        slots, address, data_size, raw_size = struct.unpack_from(">4I", header, offset)
        if raw_size > data_size:
            raise XexError("TLS raw template is larger than TLS data size")
        if raw_size:
            guest_range(address, raw_size, "TLS raw template")
        tls = {"slot_count": slots, "raw_data_address": _hex(address),
               "data_size": data_size, "raw_data_size": raw_size}
    resources = []
    if RESOURCE_INFO in records:
        offset, extent = records[RESOURCE_INFO]
        if (extent - 4) % 16:
            raise XexError("Resource-info table has an invalid size")
        for resource_offset in range(offset + 4, offset + extent, 16):
            address, size = struct.unpack_from(">II", header, resource_offset + 8)
            guest_range(address, size, "Resource")
            # Names, raw payloads, signatures, hashes and key fields are omitted.
            resources.append({"address": _hex(address), "size": size})
    return {
        "format": "XEX2", "module_flags": _hex(module_flags),
        "header_size": header_size, "payload_size": payload_size,
        "encrypted": True, "encryption_type": encryption,
        "compression": compression_info, "execution": identity,
        "entry_point": _hex(entry) if entry is not None else None,
        "image_base": _hex(image_base), "image_size": image_size,
        "security": {"offset": security_offset, "size": security_size,
                     "image_flags": _hex(image_flags), "page_size": page_size,
                     "page_descriptor_count": descriptor_count, "pages_by_type": page_counts},
        "tls": tls, "resources": resources, "optional_headers": record_summary,
        "validation": "container_metadata_only; signatures_and_payload_not_verified",
    }


def inspect_xex(path: str | Path, *, require_supported: bool = False) -> dict:
    """Inspect one stable regular file through a single read-only handle."""
    with Path(path).open("rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise XexError("XEX input must be a regular file")
        fixed = stream.read(24)
        if len(fixed) != 24 or fixed[:4] != b"XEX2":
            raise XexError("Input is not a complete XEX2 fixed header")
        header_size = _u32(fixed, 8)
        if not 24 <= header_size <= MAX_HEADER_SIZE or header_size >= before.st_size:
            raise XexError("Invalid or unsupported XEX2 header size")
        header = fixed + stream.read(header_size - len(fixed))
        result = parse_xex_header(header, before.st_size)
        digest = hashlib.sha256(header)
        bytes_read = header_size
        while block := stream.read(1024 * 1024):
            digest.update(block)
            bytes_read += len(block)
        after = os.fstat(stream.fileno())
        if bytes_read != before.st_size or (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
            after.st_size, after.st_mtime_ns, after.st_ctime_ns
        ):
            raise XexError("XEX changed during inspection", "input_changed")
    sha256 = digest.hexdigest()
    supported = sha256 == SUPPORTED_SHA256 and before.st_size == SUPPORTED_SIZE
    if require_supported and not supported:
        raise XexError("Encrypted XEX does not match this project's pinned supported input SHA-256", "unsupported_input")
    result.update(ok=True, file_size=before.st_size, sha256=sha256, supported=supported)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xex", nargs="?", default="game/default.xex", type=Path)
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON, including failures")
    parser.add_argument("--require-supported", action="store_true", help="Also require the project's exact original encrypted XEX SHA-256")
    args = parser.parse_args(argv)
    try:
        result = inspect_xex(args.xex, require_supported=args.require_supported)
    except (OSError, XexError) as error:
        code = error.code if isinstance(error, XexError) else "input_io_error"
        if args.json:
            print(json.dumps({"ok": False, "error": {"code": code, "message": str(error)}}))
        else:
            print(f"XEX intake rejected: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        execution = result["execution"]
        print("Encrypted XEX2 accepted (metadata inspection).")
        if execution:
            print(f"Title: {execution['title_id']}; media: {execution['media_id']}")
        print(f"Compression: {result['compression']['name']}; SHA-256: {result['sha256']}")
        print(f"Matches supported input: {'yes' if result['supported'] else 'no'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
