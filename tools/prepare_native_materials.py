"""Append four owned CJ materials through the original ShaderMaster importer.

The supported package stays immutable. The derived template retains every stock
record and constructor default, adding only Type, Culling and diffuse filenames.
Outputs and checked character inputs are restricted to ignored build_native.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct

from inspect_pak import PakError, read_directory, read_entry
from prepare_native_character import (CJ_CFG, CJ_RESOURCE_NAME, CJ_SKELETON,
                                      PAK_SHA256, ROOT, Reader, character_assets,
                                      counted_string, digest, private_output)

SHADER_TEMPLATE = "data/templates/shader.wbr"
SHADER_SHA256 = "a386b4025bbcf1ef921ce428721a5ce019c1ab56b299e7cfd5c6ac2587a09130"
STOCK_PROPERTIES = 118
STOCK_RECORDS = 433
MATERIAL_NAMES = tuple(f"_native/cj_native_{index}" for index in range(4))
TEXTURE_NAMES = tuple(f"_avatar/cj_native_{index}.png" for index in range(4))
FIXED_SIZES = {0: 4, 1: 4, 2: 1, 3: 4, 8: 8, 9: 12, 10: 16, 12: 3}


@dataclass(frozen=True)
class Record:
    name: str
    # Present payloads include their original u16 array count. Keeping numeric
    # bytes opaque also preserves NaNs, float spelling and original colours.
    values: tuple[bytes | None, ...]
    begin: int = 0
    end: int = 0


@dataclass(frozen=True)
class Template:
    class_id: int
    class_name: str
    class_version: int
    properties: tuple[tuple[str, int], ...]
    records: tuple[Record, ...]
    count_offset: int
    records_begin: int
    records_end: int


def parse_template(data: bytes) -> Template:
    """Bounded reader for the observed v2 single-class ShaderMaster schema."""
    if len(data) > 512 * 1024:
        raise PakError("Shader template exceeds its bounded input budget")
    reader = Reader(data)
    if reader.u16() != 2 or reader.u16() != 1:
        raise PakError("Shader template must use the observed v2 single-class header")
    class_id, class_name, class_version = reader.u16(), reader.string(), reader.u16()
    if class_name != "SHADERMASTER" or class_version != 2:
        raise PakError("Shader template class-name/version remap differs")
    count = reader.u16()
    if not 0 < count <= 128:
        raise PakError("Unbounded shader template properties")
    properties = tuple((reader.string(), reader.take(1)[0]) for _ in range(count))
    if len({name for name, _ in properties}) != count:
        raise PakError("Duplicate shader template property")
    if any(kind not in {*FIXED_SIZES, 6} for _, kind in properties):
        raise PakError("Unobserved shader template property kind")
    count_offset = reader.position
    record_count = reader.u16()
    if record_count > 1024:
        raise PakError("Unbounded shader template records")
    records_begin, records = reader.position, []
    for _ in range(record_count):
        begin = reader.position
        if reader.u16() != class_id:
            raise PakError("Shader record class differs from its remapped table")
        name, values = reader.string(), []
        for _, kind in properties:
            flag = reader.take(1)
            if flag == b"0":
                values.append(None)
                continue
            if flag != b"1":
                raise PakError("Shader property presence differs from its original contract")
            payload_begin = reader.position
            size = reader.u16()
            if size > 1024:
                raise PakError("Unbounded shader property value array")
            if kind == 6:
                for _ in range(size):
                    reader.string()
            else:
                reader.take(FIXED_SIZES[kind] * size)
            values.append(data[payload_begin:reader.position])
        records.append(Record(name, tuple(values), begin, reader.position))
    records_end = reader.position
    if reader.take(5) != b"_bin\0" or reader.position != len(data):
        raise PakError("Shader template original trailer differs")
    return Template(class_id, class_name, class_version, properties, tuple(records),
                    count_offset, records_begin, records_end)


def encode_record(class_id: int, properties: tuple[tuple[str, int], ...], record: Record) -> bytes:
    if len(record.values) != len(properties):
        raise PakError("Shader record property count differs")
    return (struct.pack("<H", class_id) + counted_string(record.name) +
            b"".join(b"0" if value is None else b"1" + value for value in record.values))


def encode_template(template: Template) -> bytes:
    """Reconstruct the whole parsed original, including each opaque value."""
    return (struct.pack("<HHH", 2, 1, template.class_id) + counted_string(template.class_name) +
            struct.pack("<HH", template.class_version, len(template.properties)) +
            b"".join(counted_string(name) + bytes([kind]) for name, kind in template.properties) +
            struct.pack("<H", len(template.records)) +
            b"".join(encode_record(template.class_id, template.properties, record)
                     for record in template.records) + b"_bin\0")


def append_materials(source: bytes) -> tuple[bytes, dict]:
    if digest(source) != SHADER_SHA256:
        raise PakError("Shader declaration source differs from its supported original")
    template = parse_template(source)
    if (len(template.properties) != STOCK_PROPERTIES or len(template.records) != STOCK_RECORDS or
            any(required not in template.properties
                for required in (("Culling", 1), ("Type", 1), ("MultilayerDiffuse", 6)))):
        raise PakError("Shader template original count/property contract differs")
    if encode_template(template) != source:
        raise PakError("Whole original shader template did not round-trip byte-identically")
    if any(record.name.casefold() in {name.casefold() for name in MATERIAL_NAMES}
           for record in template.records):
        raise PakError("Owned material already exists in the source shader template")
    added, materials = [], []
    for name, filename in zip(MATERIAL_NAMES, TEXTURE_NAMES, strict=True):
        # VT336 Type1 creates actual multilayer data; VT412 copies its diffuse
        # filename. The original Bone material fallback also uses Culling2.
        # All other flags stay absent, preserving the genuine lit/white/opaque
        # ShaderMaster constructor defaults rather than reproducing them here.
        present = {"Culling": struct.pack("<HI", 1, 2),
                   "Type": struct.pack("<HI", 1, 1),
                   "MultilayerDiffuse": struct.pack("<H", 1) + counted_string(filename)}
        record = Record(name, tuple(present.get(property_name) for property_name, _ in template.properties))
        added.append(encode_record(template.class_id, template.properties, record))
        materials.append({"registered_name": name, "resource": "SHDR:" + name,
                          "diffuse_filename": filename, "type": 1, "culling": 2,
                          "present_properties": [property_name for property_name, _ in template.properties
                                                 if property_name in present]})
    addition = b"".join(added)
    prefix = bytearray(source[:template.records_end])
    struct.pack_into("<H", prefix, template.count_offset, STOCK_RECORDS + len(added))
    output = bytes(prefix) + addition + source[template.records_end:]
    parsed = parse_template(output)
    if (encode_template(parsed) != output or len(parsed.records) != STOCK_RECORDS + 4 or
            output[:template.count_offset] != source[:template.count_offset] or
            output[template.count_offset + 2:template.records_end] != source[template.count_offset + 2:template.records_end] or
            output[template.records_end + len(addition):] != source[template.records_end:] or
            any(old.values != new.values or old.name != new.name
                for old, new in zip(template.records, parsed.records[:STOCK_RECORDS], strict=True))):
        raise PakError("Owned materials changed original shader template content")
    stock = source[template.records_begin:template.records_end]
    return output, {"name": SHADER_TEMPLATE, "input_sha256": digest(source), "output_sha256": digest(output),
                    "input_bytes": len(source), "output_bytes": len(output),
                    "file_version": 2, "class_name": template.class_name, "class_version": 2,
                    "class_id_policy": "original importer remaps the retained serialized class name",
                    "stock_declarations": STOCK_RECORDS, "owned_declarations_added": 4,
                    "stock_record_bytes": len(stock), "stock_record_sha256": digest(stock),
                    "stock_records_unchanged": True, "original_header_unchanged_except_object_count": True,
                    "object_count_offset": template.count_offset, "append_offset": template.records_end,
                    "append_bytes": len(addition), "append_sha256": digest(addition),
                    "whole_original_roundtrip_sha256": digest(encode_template(template)),
                    "materials": materials,
                    "loader": "original ShaderMaster declaration importer and first-reference BuildTextures",
                    "defaults": "all other properties absent; actual lit, white, opaque constructor defaults retained"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pak", type=Path, default=ROOT / "game/data.pak")
    parser.add_argument("--character-assets", type=Path, required=True,
                        help="Checked CJ manifest in ignored build_native; verifies all seven fixed owned inputs")
    parser.add_argument("--output", type=Path, default=ROOT / "build_native/native_material_candidate")
    args = parser.parse_args()
    with args.pak.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != PAK_SHA256:
            raise PakError("Unsupported original package identity")
    entries = {entry.name: entry for entry in read_directory(args.pak)}
    _, assets = character_assets(args.character_assets, read_entry(args.pak, entries[CJ_CFG]),
                                 read_entry(args.pak, entries[CJ_SKELETON]))
    content, metadata = append_materials(read_entry(args.pak, entries[SHADER_TEMPLATE]))
    output = private_output(args.output)
    target = private_output(output.joinpath(*SHADER_TEMPLATE.split("/")))
    manifest = private_output(output / "material_manifest.json")
    for destination in (target, manifest):
        if destination.exists() and not destination.is_file():
            raise PakError("Owned material destination is not a file")
    report = {"policy": "four owned shader declarations; original package and shader/texture loader retained",
              "package_sha256": PAK_SHA256, "overlay_root": str(output), "files": [metadata],
              "character_resource": "BNMD:" + CJ_RESOURCE_NAME,
              "character_assets_manifest_sha256": assets["character_assets_manifest_sha256"],
              "validated_input_files": assets["owned_files"]}
    manifest_content = (json.dumps(report, indent=2) + "\n").encode("utf-8")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)
    if digest(target.read_bytes()) != metadata["output_sha256"]:
        raise PakError("Derived material template failed its output hash check")
    manifest.write_bytes(manifest_content)
    if manifest.read_bytes() != manifest_content:
        raise PakError("Derived material manifest failed its output check")
    print(json.dumps({"manifest": str(manifest), "stock_declarations_preserved": STOCK_RECORDS,
                      "owned_declarations_added": 4, "output_sha256": metadata["output_sha256"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
