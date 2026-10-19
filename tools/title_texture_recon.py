#!/usr/bin/env python3
"""Inspect original Tongas texture metadata without embedding asset bytes or source.

The supplied package stores stock PNG/JPEG/standard DDS files. GPU tiling is
performed after decoding by original upload routines, not by a .texture file.
An optional explicit --dds-out extracts only the two observed DDS entries to
ignored local build storage for native parser validation.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import sys
import zlib

try:
    from .inspect_pak import PakError, _no_links, inspect_pak, read_directory, read_entry
except ImportError:
    from inspect_pak import PakError, _no_links, inspect_pak, read_directory, read_entry

ROOT = Path(__file__).resolve().parents[1]
MAX_DIMENSION = 8192


def dimensions(width: int, height: int):
    if not 0 < width <= MAX_DIMENSION or not 0 < height <= MAX_DIMENSION:
        raise PakError("Texture dimensions exceed the verified native range")


def png_metadata(content: bytes) -> dict:
    if not content.startswith(b"\x89PNG\r\n\x1a\n"):
        raise PakError("PNG entry has invalid signature")
    position, count, result = 8, 0, None
    while position < len(content):
        if len(content) - position < 12:
            raise PakError("Truncated PNG chunk")
        length = struct.unpack_from(">I", content, position)[0]
        kind = content[position + 4:position + 8]
        end = position + 12 + length
        if end > len(content):
            raise PakError("PNG chunk exceeds file")
        crc = struct.unpack_from(">I", content, end - 4)[0]
        if zlib.crc32(content[position + 4:end - 4]) != crc:
            raise PakError("PNG chunk CRC mismatch")
        if count == 0:
            if kind != b"IHDR" or length != 13:
                raise PakError("PNG first chunk is not IHDR")
            width, height, depth, color, compression, filtering, interlace = struct.unpack_from(">IIBBBBB", content, position + 8)
            dimensions(width, height)
            if compression or filtering or interlace > 1:
                raise PakError("Unsupported PNG header")
            result = {"format": "PNG", "width": width, "height": height,
                      "sample_bits": depth, "color_type": color, "interlace": interlace}
        count += 1
        position = end
        if kind == b"IEND":
            if length or position != len(content):
                raise PakError("Invalid PNG end or trailing data")
            return result
    raise PakError("PNG has no complete IEND")


def jpeg_metadata(content: bytes) -> dict:
    if not content.startswith(b"\xff\xd8"):
        raise PakError("JPEG entry has invalid signature")
    position = 2
    while position < len(content):
        if content[position] != 255:
            raise PakError("Invalid JPEG marker boundary")
        while position < len(content) and content[position] == 255:
            position += 1
        if position >= len(content):
            raise PakError("Truncated JPEG marker")
        marker = content[position]
        position += 1
        if marker in (0, 0xd8, 0xd9, 0xda):
            raise PakError("JPEG has no supported frame header before image data")
        if marker == 1 or 0xd0 <= marker <= 0xd7:
            continue
        if len(content) - position < 2:
            raise PakError("Truncated JPEG segment")
        length = struct.unpack_from(">H", content, position)[0]
        if length < 2 or length > len(content) - position:
            raise PakError("JPEG segment exceeds file")
        if marker in (0xc0, 0xc1, 0xc2):
            if length < 8:
                raise PakError("Truncated JPEG frame")
            bits, height, width, components = struct.unpack_from(">BHHB", content, position + 2)
            dimensions(width, height)
            if bits != 8 or not 1 <= components <= 4 or length != 8 + 3 * components:
                raise PakError("Unsupported JPEG frame layout")
            return {"format": "JPEG", "width": width, "height": height,
                    "sample_bits": bits, "components": components, "progressive": marker == 0xc2}
        position += length
    raise PakError("JPEG has no complete frame header")


def dds_metadata(content: bytes) -> dict:
    if len(content) < 128 or content[:4] != b"DDS ":
        raise PakError("DDS entry has invalid signature")
    header = struct.unpack_from("<31I", content, 4)
    if header[0] != 124 or header[18] != 32 or header[19] != 4:
        raise PakError("Unverified DDS header or pixel layout")
    height, width = header[2:4]
    dimensions(width, height)
    fourcc = struct.pack("<I", header[20]).decode("ascii")
    if fourcc not in ("DXT1", "DXT5"):
        raise PakError("Unverified DDS compressed format")
    caps2 = header[27]
    cube = bool(caps2 & 0x200)
    if header[5] > 1 or caps2 & ~0xfe00 or (cube and (caps2 != 0xfe00 or width != height)) or (not cube and caps2):
        raise PakError("Unverified DDS volume or partial cube")
    mips, faces = max(1, header[6]), 6 if cube else 1
    if mips > max(width, height).bit_length():
        raise PakError("Excessive DDS mip count")
    position, images = 128, []
    for face in range(faces):
        w, h = width, height
        for level in range(mips):
            pitch = ((w + 3) // 4) * (8 if fourcc == "DXT1" else 16)
            size = pitch * ((h + 3) // 4)
            if size > len(content) - position:
                raise PakError("Truncated DDS mip data")
            images.append({"face": face, "level": level, "width": w, "height": h,
                           "row_pitch": pitch, "offset": position, "bytes": size})
            position += size
            w, h = max(1, w // 2), max(1, h // 2)
    if position != len(content):
        raise PakError("DDS payload has trailing data")
    return {"format": "DDS", "fourcc": fourcc, "width": width, "height": height,
            "faces": faces, "mips": mips, "byte_order": "little_endian_PC_blocks",
            "tiled": False, "images": images}


def inventory(pak: Path, dds_out: Path | None = None) -> dict:
    before = inspect_pak(pak)
    entries = read_directory(pak)
    images = []
    parsers = {".png": png_metadata, ".jpg": jpeg_metadata, ".dds": dds_metadata}
    if dds_out:
        dds_out = dds_out.absolute()
        if not dds_out.is_relative_to(ROOT / "build_native"):
            raise PakError("DDS extraction must remain in ignored build_native storage")
        _no_links(dds_out)
    for entry in entries:
        suffix = PurePosixPath(entry.name).suffix.lower()
        if suffix not in parsers:
            continue
        content = read_entry(pak, entry)
        item = {"name": entry.name, "decoded_bytes": len(content),
                "sha256": hashlib.sha256(content).hexdigest(), **parsers[suffix](content)}
        images.append(item)
        if dds_out and suffix == ".dds":
            target = dds_out / PurePosixPath(entry.name).name
            _no_links(target)
            if target.exists():
                raise PakError("Refusing to overwrite existing DDS extraction")
            target.parent.mkdir(parents=True, exist_ok=True)
            _no_links(target.parent)
            with target.open("xb") as stream:
                stream.write(content)
            item["local_parser_input"] = str(target)
    after = inspect_pak(pak)
    if before != after:
        raise PakError("Original package changed during texture inspection")
    return {"package": before, "image_counts": dict(Counter(image["format"] for image in images)),
            "texture_file_count": sum(entry.name.lower().endswith(".texture") for entry in entries),
            "images": images,
            "verified_original_boundaries": {
                "texture_master_schema": "data/templates/texture.wbr",
                "texture_load": "0x8238F1C8",
                "decoded_descriptor": {"width": 0, "height": 4, "alpha_byte": 12, "ARGB_linear_pixels": 16},
                "create_texture": {"address": "0x823D6BB8", "r3": "width", "r4": "height", "r5": "depth_or_cube_faces", "r6": "mip_levels", "r7": "usage", "r8": "Xbox_format", "r10": "type_3_2D_18_cube"},
                "create_surface": {"address": "0x823D6CD8", "r3": "width", "r4": "height", "r5": "Xbox_format", "r6": "multisample_0_none_1_2x_2_4x", "r7": "optional_EDRAM_placement"},
                "texture_surface": {"address": "0x823D6968", "r3": "texture", "r4": "mip_level"},
                "cube_surface": {"address": "0x823D6A48", "r3": "texture", "r4": "face", "r5": "mip_level"},
                "upload_surface": {"address": "0x82648F60", "r3": "surface", "r4": "destination_palette", "r5": "destination_RECT", "r6": "linear_source", "r7": "Xbox_source_format", "r8": "source_pitch", "r9": "source_palette", "r10": "source_RECT", "stack_84": "resample_enable", "stack_92": "resample_width", "stack_100": "resample_height", "stack_108": "filter", "stack_116": "color_key", "stock_options": [0, 0, 0, 1, 0]},
                "bind_texture": {"address": "0x823D6F38", "r3": "device", "r4": "slot", "r5": "texture", "r6": "GPU_dirty_mask"},
                "bind_render_target": {"address": "0x823C6FC0", "r3": "device", "r4": "index", "r5": "surface"},
                "bind_depth_surface": {"address": "0x823C6CE8", "r3": "device", "r4": "surface"},
                "gamma_ramp_get_set": ["0x823C63D0", "0x823C6300"]},
            "limitations": ["Metadata inspection does not establish a rendered game frame.",
                            "Xbox tiled source decoding is not implemented; observed stock upload sources are linear.",
                            "Original resource wrappers must be intercepted before opaque native tokens reach GPU field dereferences."]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pak", type=Path, default=ROOT / "game/data.pak")
    parser.add_argument("--output", type=Path, default=ROOT / "build_native/title_texture_recon.json")
    parser.add_argument("--dds-out", type=Path)
    args = parser.parse_args()
    try:
        result = inventory(args.pak, args.dds_out)
        _no_links(args.output.absolute())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"image_counts": result["image_counts"], "texture_file_count": result["texture_file_count"], "output": str(args.output)}))
    except (OSError, ValueError) as error:
        print(f"Texture inspection failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
