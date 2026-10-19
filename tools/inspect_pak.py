#!/usr/bin/env python3
"""Bounded reader for the observed Tongas v30000 package directory.

Directory layout was verified against the user's 1,612-entry data.pak.
Stored flag 1 denotes a complete LZMA-alone stream. Other flags are unverified.
Game contents are only written by an explicit shader extraction operation.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict, dataclass
import hashlib
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import sys

MAGIC = b"tongas_pack_v30000"
HEADER_SIZE = 26
MAX_DIRECTORY = 16 * 1024 * 1024
MAX_ENTRIES = 100_000
MAX_NAME = 1024
MAX_CONTENT = 64 * 1024 * 1024
SHADER_ROOT = "data/art/dxshaders/"


class PakError(ValueError):
    pass


@dataclass(frozen=True)
class PakEntry:
    name: str
    offset: int
    stored_size: int
    flags: int


def _bounds(offset, size, limit, label):
    if offset < 0 or size < 0 or offset > limit or size > limit - offset:
        raise PakError(f"{label} exceeds its containing range")


def safe_name(name: str) -> str:
    """Reject Windows/POSIX traversal, aliases, streams and device filenames."""
    if not name or len(name) > MAX_NAME or name.startswith("/") or "\\" in name:
        raise PakError("Unsafe archive path")
    if any(ord(char) < 32 or char in ':<>"|?*' for char in name):
        raise PakError("Unsafe archive path characters")
    parts = name.split("/")
    for part in parts:
        if not part or part in (".", "..") or part.endswith((".", " ")):
            raise PakError("Unsafe archive path component")
        stem = part.split(".")[0].upper()
        if stem in {"CON", "PRN", "AUX", "NUL"} or re.fullmatch(r"(?:COM|LPT)[1-9]", stem):
            raise PakError("Windows device archive path")
    return name


def parse_directory(data: bytes, file_size: int) -> list[PakEntry]:
    if len(data) < HEADER_SIZE or data[:18] != MAGIC:
        raise PakError("Not a complete Tongas v30000 header")
    count, directory_size = struct.unpack_from("<II", data, 18)
    if not 0 < count <= MAX_ENTRIES or not count * 17 <= directory_size <= MAX_DIRECTORY:
        raise PakError("Invalid directory count or size")
    end = HEADER_SIZE + directory_size
    _bounds(0, end, file_size, "Directory")
    _bounds(0, end, len(data), "Directory bytes")
    position = HEADER_SIZE
    entries = []
    names = set()
    for _ in range(count):
        _bounds(position, 16, end, "Entry fields")
        offset, size, flags, name_size = struct.unpack_from("<4I", data, position)
        position += 16
        if not 0 < name_size <= MAX_NAME:
            raise PakError("Invalid entry filename length")
        _bounds(position, name_size, end, "Entry filename")
        try:
            name = safe_name(data[position:position + name_size].decode("utf-8"))
        except UnicodeDecodeError as error:
            raise PakError("Entry filename is not UTF-8") from error
        position += name_size
        if name.casefold() in names:
            raise PakError("Duplicate archive filename")
        names.add(name.casefold())
        if flags != 1:
            raise PakError("Unknown Tongas storage flag")
        if offset < end or size == 0:
            raise PakError("Entry overlaps directory or has no stored content")
        _bounds(offset, size, file_size, "Entry payload")
        entries.append(PakEntry(name, offset, size, flags))
    if position != end:
        raise PakError("Directory size does not match parsed records")
    previous_end = end
    for entry in sorted(entries, key=lambda item: item.offset):
        if entry.offset < previous_end:
            raise PakError("Overlapping archive payloads")
        previous_end = entry.offset + entry.stored_size
    return entries


def read_directory(path: str | Path) -> list[PakEntry]:
    with Path(path).open("rb") as stream:
        file_size = os.fstat(stream.fileno()).st_size
        fixed = stream.read(HEADER_SIZE)
        if len(fixed) != HEADER_SIZE or fixed[:18] != MAGIC:
            raise PakError("Not a complete Tongas v30000 header")
        size = struct.unpack_from("<I", fixed, 22)[0]
        if size > MAX_DIRECTORY:
            raise PakError("Excessive directory size")
        _bounds(HEADER_SIZE, size, file_size, "Directory")
        return parse_directory(fixed + stream.read(size), file_size)


def decode_entry(stored: bytes, entry: PakEntry, *, max_output: int = MAX_CONTENT) -> bytes:
    if len(stored) != entry.stored_size or len(stored) > MAX_CONTENT:
        raise PakError("Stored entry size is truncated or excessive")
    if entry.flags != 1 or len(stored) < 13:
        raise PakError("Unsupported or truncated LZMA-alone entry")
    expected = struct.unpack_from("<Q", stored, 5)[0]
    unknown_size = expected == 0xFFFFFFFFFFFFFFFF
    if not unknown_size and expected > max_output:
        raise PakError("Declared LZMA output size exceeds limit")
    try:
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=MAX_CONTENT)
        content = decoder.decompress(stored, max_length=max_output + 1)
    except lzma.LZMAError as error:
        raise PakError(f"Invalid LZMA-alone payload: {error}") from error
    if (not unknown_size and len(content) != expected) or len(content) > max_output or not decoder.eof or decoder.unused_data:
        raise PakError("LZMA output length, end marker or trailing bytes are invalid")
    return content


def read_entry(path: str | Path, entry: PakEntry, *, max_output: int = MAX_CONTENT) -> bytes:
    if entry.stored_size > MAX_CONTENT:
        raise PakError("Stored entry exceeds read limit")
    with Path(path).open("rb") as stream:
        _bounds(entry.offset, entry.stored_size, os.fstat(stream.fileno()).st_size, "Entry payload")
        stream.seek(entry.offset)
        return decode_entry(stream.read(entry.stored_size), entry, max_output=max_output)


def _no_links(path: Path):
    for ancestor in reversed((path, *path.parents)):
        try:
            metadata = ancestor.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(metadata.st_mode) or getattr(metadata, "st_file_attributes", 0) & 0x400:
            raise PakError("Extraction/output path contains a reparse point or symlink")


def extract_shaders(path: str | Path, destination: str | Path) -> dict:
    """Extract shader sources/caches exclusively; existing outputs are never overwritten."""
    root = Path(os.path.abspath(destination))
    _no_links(root)
    entries = [entry for entry in read_directory(path) if entry.name.startswith(SHADER_ROOT)]
    targets = []
    for entry in entries:
        target = root.joinpath(*PurePosixPath(safe_name(entry.name)).parts)
        _no_links(target)
        if target.exists():
            raise PakError(f"Extraction target already exists: {entry.name}")
        targets.append((entry, target))
    for entry, target in targets:
        content = read_entry(path, entry)
        target.parent.mkdir(parents=True, exist_ok=True)
        _no_links(target.parent)
        with target.open("xb") as output:
            output.write(content)
    return {"extracted_count": len(targets), "destination": str(root)}


def inspect_pak(path: str | Path, *, include_entries: bool = False) -> dict:
    initial = Path(path).stat()
    entries = read_directory(path)
    with Path(path).open("rb") as stream:
        before = os.fstat(stream.fileno())
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
        after = os.fstat(stream.fileno())
    # Windows/Python versions can expose different ctime meanings from stat
    # versus fstat. Compare ctime only within the same API, preserving both
    # replacement detection and full before/after stability checks.
    identity = lambda state: (state.st_dev, state.st_ino, state.st_size, state.st_mtime_ns)
    stamp = lambda state: (*identity(state), state.st_ctime_ns)
    if identity(initial) != identity(before) or stamp(before) != stamp(after) or stamp(initial) != stamp(Path(path).stat()):
        raise PakError("Package changed during inspection")
    result = {"format": MAGIC.decode(), "file_size": before.st_size, "sha256": digest,
              "entry_count": len(entries), "storage_flags": dict(Counter(entry.flags for entry in entries)),
              "extensions": dict(sorted(Counter(PurePosixPath(entry.name).suffix.lower() for entry in entries).items())),
              "shader_entry_count": sum(entry.name.startswith(SHADER_ROOT) for entry in entries)}
    if include_entries:
        result["entries"] = [asdict(entry) for entry in entries]
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pak", nargs="?", default="game/data.pak", type=Path)
    parser.add_argument("--entries", action="store_true")
    parser.add_argument("--extract-shaders", type=Path, help="Extract only shader entries; never overwrite files")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = inspect_pak(args.pak, include_entries=args.entries)
        if args.extract_shaders:
            result["extraction"] = extract_shaders(args.pak, args.extract_shaders)
        result["ok"] = True
    except (OSError, PakError) as error:
        result = {"ok": False, "error": str(error)}
    print(json.dumps(result, indent=2) if args.json else result)
    return 0 if result["ok"] else 2


if __name__ == "__main__":
    sys.exit(main())
