"""Compare generated native switch edges with the original inline table bytes."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tomllib

from inspect_xex import inspect_xex
from recover_offset_switches import recover

ROOT = Path(__file__).resolve().parents[1]
SWITCH = re.compile(r"// lis r12,(-?\d+)\n[\s\S]{0,600}?// addi r12,r12,(-?\d+)\n"
                    r"[\s\S]{0,500}?// bctr[^\n]*\n\s*switch \(ctx\.r(\d+)\.u32\) \{"
                    r"([\s\S]*?)\n\s*\}")


def executable_ranges(image: bytes, image_base: int) -> list[tuple[int, int]]:
    """Read the original PE's executable virtual extents without trusting labels."""
    if len(image) < 64:
        raise ValueError("Original image has no complete PE header")
    pe = struct.unpack_from("<I", image, 0x3c)[0]
    if pe > len(image) - 24:
        raise ValueError("Original PE header exceeds image bounds")
    count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    section_table = pe + 24 + optional_size
    if section_table > len(image) or count * 40 > len(image) - section_table:
        raise ValueError("Original PE section table exceeds image bounds")
    ranges = []
    for index in range(count):
        offset = section_table + index * 40
        size, address = struct.unpack_from("<II", image, offset + 8)
        flags = struct.unpack_from("<I", image, offset + 36)[0]
        if flags & 0x20000000:
            if address > len(image) or size > len(image) - address:
                raise ValueError("Original executable section exceeds image bounds")
            ranges.append((image_base + address, size))
    return ranges


def explicit_offset_descriptor(image: bytes, image_base: int, entry: dict) -> dict:
    """Check a reviewed descriptor against original instructions and table bytes.

    Explicit selector-domain proofs belong to the title configuration. This
    oracle does not infer a domain or accept labels as their own evidence.
    Currently reviewed explicit descriptors use the exact BE16 dispatch form.
    """
    required = {"base", "r", "labels", "table", "entry_bytes", "scale", "code_base"}
    if set(entry) != required or any(type(entry[key]) is not int for key in required - {"labels"}):
        raise ValueError("Explicit offset switch has invalid oracle metadata")
    branch, register = entry["base"], entry["r"]
    labels = entry["labels"]
    if (entry["entry_bytes"] != 2 or entry["scale"] != 1 or not 1 <= register <= 31 or
            register == 12 or type(labels) is not list or not 1 <= len(labels) <= 4096 or
            any(type(label) is not int for label in labels)):
        raise ValueError("Unsupported explicit offset switch descriptor")
    position = branch - image_base
    table_offset = entry["table"] - image_base
    if (branch & 3 or position < 36 or position + 4 > len(image) or
            entry["table"] & 1 or table_offset < 0 or len(labels) * 2 > len(image) - table_offset):
        raise ValueError("Explicit offset switch exceeds original image bounds")
    words = struct.unpack_from(">10I", image, position - 36)
    if (words[0] >> 16 != 0x3d80 or words[1] & 0xfc1fffff != 0x5400083c or
            (words[1] >> 21) & 31 != register or words[2] >> 16 != 0x398c or
            words[3] != 0x7c0c022e or words[4] >> 16 != 0x3d80 or
            words[5] >> 16 != 0x398c or words[6:] != (0x60000000, 0x7d8c0214, 0x7d8903a6, 0x4e800420)):
        raise ValueError(f"Explicit offset switch instructions/register differ at 0x{branch:08X}")
    signed_low = lambda word: (word & 65535) - (65536 if word & 32768 else 0)
    table = ((words[0] & 65535) << 16) + signed_low(words[2])
    code = ((words[4] & 65535) << 16) + signed_low(words[5])
    if table != entry["table"] or code != entry["code_base"]:
        raise ValueError("Explicit offset switch table/code address differs from original instructions")
    values = struct.unpack_from(f">{len(labels)}H", image, table_offset)
    expected = [code + value for value in values]
    code_ranges = executable_ranges(image, image_base)
    if labels != expected or any(label & 3 or not any(start <= label < start + size
            for start, size in code_ranges) for label in labels):
        raise ValueError("Explicit offset switch labels differ from original BE16 table")
    return entry


def verify() -> dict:
    inspect_xex(ROOT / "game/default.xex", require_supported=True)
    image = (ROOT / "build_native/game_image.bin").read_bytes()
    metadata = json.loads((ROOT / "build_native/game_image.json").read_text())
    if metadata["encryption"] != 1 or hashlib.sha256(image).hexdigest() != metadata["image_sha256"]:
        raise ValueError("Switch oracle image does not match original encrypted input metadata")
    base = metadata["image_base"]
    recovered = re.findall(r"Recovered inline switch at ([0-9A-F]+): (\d+) cases",
                           (ROOT / "build_native/generated/generation.log").read_text())
    if len(recovered) < 400:
        raise ValueError("Expected recovered original inline switches are absent")
    generated = {}
    for file in (ROOT / "build_native/generated").glob("ppc_recomp.*.cpp"):
        for match in SWITCH.finditer(file.read_text()):
            table = (int(match[1]) * 65536 + int(match[2])) & 0xffffffff
            edges = [(int(index), int(target, 16)) for index, target in
                     re.findall(r"case (\d+):\s*goto loc_([0-9A-F]+);", match[4])]
            generated.setdefault(table, []).append((int(match[3]), edges))
    edge_count = 0
    for raw_branch, raw_count in recovered:
        branch, count = int(raw_branch, 16), int(raw_count)
        table = branch + 4
        offset = table - base
        if offset < 0 or count * 4 > len(image) - offset:
            raise ValueError("Original switch oracle table is outside decoded image")
        words = struct.unpack_from(f">{count}I", image, offset)
        expected = list(enumerate(words))
        first, second = struct.unpack_from(">II", image, branch - 16 - base)
        rotate = first if first & 0xFC1FFFFF == 0x5400103A else second
        register = (rotate >> 21) & 31
        candidates = generated.get(table, [])
        if not candidates or any(candidate != (register, expected) for candidate in candidates):
            raise ValueError(f"Native switch edges/register differ from original table at 0x{branch:08X}")
        edge_count += count
    offsets = recover(image, base, detailed=True)
    explicit = [explicit_offset_descriptor(image, base, entry) for entry in
                tomllib.loads((ROOT / "config/switch_tables.toml").read_text()).get("switch", [])]
    by_branch = {entry["base"]: entry for entry in offsets}
    for entry in explicit:
        previous = by_branch.get(entry["base"])
        if previous is not None and any(previous[field] != entry[field] for field in
                ("r", "labels", "table", "entry_bytes", "scale", "code_base")):
            raise ValueError("Explicit switch descriptor conflicts with automatic original recovery")
        by_branch[entry["base"]] = entry
    offsets = list(by_branch.values())
    for entry in offsets:
        table = entry["table"]
        count = len(entry["labels"])
        values = struct.unpack_from(f">{count}{'H' if entry['entry_bytes'] == 2 else 'B'}", image, table-base)
        expected = (entry["r"], list(enumerate(entry["code_base"] + entry["scale"]*value for value in values)))
        candidates = generated.get(table, [])
        if not candidates or any(candidate != expected for candidate in candidates):
            raise ValueError(f"Native offset switch differs from original offset table at 0x{entry['base']:08X}")
        edge_count += len(entry["labels"])
    return {"inline_switches": len(recovered), "explicit_offset_switches": len(explicit),
            "halfword_offset_switches": sum(entry["entry_bytes"] == 2 for entry in offsets),
            "byte_offset_switches": sum(entry["entry_bytes"] == 1 for entry in offsets), "original_edges": edge_count,
            "oracle": "original big-endian absolute and offset jump-table bytes", "status": "passed"}


if __name__ == "__main__":
    try:
        print(json.dumps(verify(), indent=2))
    except (OSError, ValueError, KeyError) as error:
        print(f"Native switch contract failed: {error}", file=sys.stderr)
        raise SystemExit(1)
