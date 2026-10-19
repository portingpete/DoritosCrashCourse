"""Declare CJ's four images through the original TextureMaster importer.

Keep the original package and every original template record byte-identical.
Only checked, owned inputs and derived outputs in build_native are supported.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

from inspect_pak import PakError, read_directory, read_entry
from prepare_native_character import (CJ_CFG, CJ_RESOURCE_NAME, CJ_SKELETON,
                                      PAK_SHA256, ROOT, Reader, character_assets,
                                      counted_string, digest, private_output)
from prepare_native_materials import (FIXED_SIZES, TEXTURE_NAMES, Record, Template,
                                      encode_record, encode_template)

TEXTURE_TEMPLATE = "data/templates/texture.wbr"
TEXTURE_SHA256 = "7c29ea9e5f9bfc2e6c9d64e8b5317b061f12ecb39d3af3e93204fe96bb364e85"
STOCK_PROPERTIES = 24
STOCK_RECORDS = 738


def parse_template(data: bytes) -> Template:
    if len(data) > 1024 * 1024:
        raise PakError("Texture template exceeds its bounded input budget")
    reader = Reader(data)
    if reader.u16() != 2 or reader.u16() != 1:
        raise PakError("Texture template must use the observed v2 single-class header")
    class_id, class_name, class_version = reader.u16(), reader.string(), reader.u16()
    if class_name != "TEXTUREMASTER" or class_version != 1:
        raise PakError("Texture template class-name/version remap differs")
    count = reader.u16()
    if not 0 < count <= 128:
        raise PakError("Unbounded texture template properties")
    properties = tuple((reader.string(), reader.take(1)[0]) for _ in range(count))
    if len({name for name, _ in properties}) != count:
        raise PakError("Duplicate texture template property")
    if any(kind not in {*FIXED_SIZES, 6} for _, kind in properties):
        raise PakError("Unobserved texture template property kind")
    count_offset = reader.position
    record_count = reader.u16()
    if record_count > 1024:
        raise PakError("Unbounded texture template records")
    records_begin, records = reader.position, []
    for _ in range(record_count):
        begin = reader.position
        if reader.u16() != class_id:
            raise PakError("Texture record class differs from its remapped table")
        name, values = reader.string(), []
        for _, kind in properties:
            flag = reader.take(1)
            if flag == b"0":
                values.append(None)
                continue
            if flag != b"1":
                raise PakError("Texture property presence differs from its original contract")
            payload_begin = reader.position
            size = reader.u16()
            if size > 1024:
                raise PakError("Unbounded texture property value array")
            if kind == 6:
                for _ in range(size):
                    reader.string()
            else:
                reader.take(FIXED_SIZES[kind] * size)
            values.append(data[payload_begin:reader.position])
        records.append(Record(name, tuple(values), begin, reader.position))
    records_end = reader.position
    if reader.take(5) != b"_bin\0" or reader.position != len(data):
        raise PakError("Texture template original trailer differs")
    return Template(class_id, class_name, class_version, properties, tuple(records),
                    count_offset, records_begin, records_end)


def append_textures(source: bytes) -> tuple[bytes, dict]:
    if digest(source) != TEXTURE_SHA256:
        raise PakError("Texture declaration source differs from its supported original")
    template = parse_template(source)
    if (len(template.properties) != STOCK_PROPERTIES or len(template.records) != STOCK_RECORDS or
            any(required not in template.properties
                for required in (("TextureType", 1), ("DiffuseFile", 6)))):
        raise PakError("Texture template original count/property contract differs")
    if encode_template(template) != source:
        raise PakError("Whole original texture template did not round-trip byte-identically")
    if any(record.name.casefold() in {name.casefold() for name in TEXTURE_NAMES}
           for record in template.records):
        raise PakError("Owned texture already exists in the source texture template")
    added, textures = [], []
    for filename in TEXTURE_NAMES:
        present = {"TextureType": struct.pack("<HI", 1, 1),
                   "DiffuseFile": struct.pack("<H", 1) + counted_string(filename)}
        record = Record(filename, tuple(present.get(name) for name, _ in template.properties))
        added.append(encode_record(template.class_id, template.properties, record))
        textures.append({"registered_name": filename, "diffuse_filename": filename,
                         "texture_type": 1,
                         "present_properties": [name for name, _ in template.properties if name in present]})
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
        raise PakError("Owned textures changed original texture template content")
    stock = source[template.records_begin:template.records_end]
    return output, {"name": TEXTURE_TEMPLATE, "input_sha256": digest(source), "output_sha256": digest(output),
                    "input_bytes": len(source), "output_bytes": len(output),
                    "file_version": 2, "class_name": template.class_name, "class_version": 1,
                    "class_id_policy": "original importer remaps the retained serialized class name",
                    "stock_declarations": STOCK_RECORDS, "owned_declarations_added": 4,
                    "stock_record_bytes": len(stock), "stock_record_sha256": digest(stock),
                    "stock_records_unchanged": True,
                    "original_header_unchanged_except_object_count": True,
                    "object_count_offset": template.count_offset, "append_offset": template.records_end,
                    "append_bytes": len(addition), "append_sha256": digest(addition),
                    "textures": textures,
                    "loader": "original TextureMaster declaration importer and image dependency loader",
                    "defaults": "all other properties absent; original constructor defaults retained"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pak", type=Path, default=ROOT / "game/data.pak")
    parser.add_argument("--character-assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build_native/native_texture_candidate")
    args = parser.parse_args()
    with args.pak.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != PAK_SHA256:
            raise PakError("Unsupported original package identity")
    entries = {entry.name: entry for entry in read_directory(args.pak)}
    _, assets = character_assets(args.character_assets, read_entry(args.pak, entries[CJ_CFG]),
                                 read_entry(args.pak, entries[CJ_SKELETON]))
    content, metadata = append_textures(read_entry(args.pak, entries[TEXTURE_TEMPLATE]))
    output = private_output(args.output)
    target = private_output(output.joinpath(*TEXTURE_TEMPLATE.split("/")))
    manifest = private_output(output / "texture_manifest.json")
    for destination in (target, manifest):
        if destination.exists() and not destination.is_file():
            raise PakError("Owned texture destination is not a file")
    report = {"policy": "four owned texture declarations; original package and image loader retained",
              "package_sha256": PAK_SHA256, "overlay_root": str(output), "files": [metadata],
              "character_resource": "BNMD:" + CJ_RESOURCE_NAME,
              "character_assets_manifest_sha256": assets["character_assets_manifest_sha256"],
              "validated_input_files": assets["owned_files"]}
    manifest_content = (json.dumps(report, indent=2) + "\n").encode("utf-8")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)
    if digest(target.read_bytes()) != metadata["output_sha256"]:
        raise PakError("Derived texture template failed its output hash check")
    manifest.write_bytes(manifest_content)
    if manifest.read_bytes() != manifest_content:
        raise PakError("Derived texture manifest failed its output check")
    print(json.dumps({"manifest": str(manifest), "stock_declarations_preserved": STOCK_RECORDS,
                      "owned_declarations_added": 4, "output_sha256": metadata["output_sha256"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
