"""Append one owned character declaration through the original template importer.

The supported package remains immutable. All original BoneModel declarations
are retained byte for byte; only its object count and one new record change.
Derived game data is restricted to ignored build_native output.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import struct

from inspect_pak import PakError, _no_links, read_directory, read_entry

ROOT = Path(__file__).resolve().parents[1]
PAK_SHA256 = "0819b4e9f90695b70e083690e98ac6f76f0df0cf2bbb817f196316c930a54fc3"
BONE_TEMPLATE = "data/templates/bonedmodel.wbr"
AVATAR_TEMPLATE = "data/templates/avatarmodel.wbr"
INPUTS = {
    BONE_TEMPLATE: "a01f668f073c53d9907769afaa5193ecdbf00fcce9647152f2d6431b373981af",
    AVATAR_TEMPLATE: "6f304c1754fbb5c051ba558ba5ff3ddfd728e3d1b32f29d6577df63d25f1509e",
}
RESOURCE_NAME = "_avatar/DummyModel.cfg"
RESOURCE = "BNMD:" + RESOURCE_NAME
CJ_RESOURCE_NAME = "_avatar/cj_native.cfg"
CJ_CFG = "data/art/_avatar/dummymodel.cfg"
CJ_SKELETON = "data/art/_avatar/p_test.csf"
CJ_ASSET_NAMES = tuple("data/art/_avatar/" + name for name in
                       ("cj_native.cfg", "cj_native.cmf", "cj_native.mat.txt",
                        "cj_native_0.png", "cj_native_1.png", "cj_native_2.png", "cj_native_3.png"))


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Reader:
    def __init__(self, data: bytes):
        self.data, self.position = data, 0

    def take(self, size: int) -> bytes:
        if size < 0 or size > len(self.data) - self.position:
            raise PakError("Truncated character template")
        value = self.data[self.position:self.position + size]
        self.position += size
        return value

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def string(self) -> str:
        size = struct.unpack("<I", self.take(4))[0]
        if size > 32768:
            raise PakError("Unbounded character template string")
        try:
            value = self.take(size).decode("ascii")
        except UnicodeDecodeError as error:
            raise PakError("Character template string is not observed ASCII") from error
        if "\0" in value:
            raise PakError("Character template counted string contains NUL")
        return value


@dataclass(frozen=True)
class Record:
    name: str
    begin: int
    end: int
    strings: dict[str, tuple[str, ...]]
    present: tuple[str, ...]


@dataclass(frozen=True)
class Template:
    class_id: int
    class_name: str
    properties: tuple[tuple[str, int], ...]
    count_offset: int
    records_begin: int
    records: tuple[Record, ...]
    records_end: int


def parse_template(data: bytes) -> Template:
    """Read only the observed v2, one-class template layout and value types."""
    if len(data) > 512 * 1024:
        raise PakError("Character template exceeds its bounded input budget")
    reader = Reader(data)
    if reader.u16() != 2 or reader.u16() != 1:
        raise PakError("Character template must use the observed v2 single-class header")
    class_id, class_name = reader.u16(), reader.string()
    if reader.u16() != 1:
        raise PakError("Character template class version differs")
    count = reader.u16()
    if not 0 < count <= 64:
        raise PakError("Unbounded character template properties")
    properties = tuple((reader.string(), reader.take(1)[0]) for _ in range(count))
    if len({name for name, _ in properties}) != count:
        raise PakError("Duplicate character template property")
    # Original8225DE18 dispatches these observed kinds. Numeric bytes remain
    # opaque: preserving their original spelling is the only required action.
    fixed_sizes = {0: 4, 1: 4, 2: 1, 3: 4, 9: 12, 12: 3}
    if any(kind not in {*fixed_sizes, 6} for _, kind in properties):
        raise PakError("Unobserved character template property kind")
    count_offset = reader.position
    record_count = reader.u16()
    if record_count > 1024:
        raise PakError("Unbounded character template records")
    records_begin, records = reader.position, []
    for _ in range(record_count):
        begin = reader.position
        if reader.u16() != class_id:
            raise PakError("Character record class differs from its remapped table")
        name, strings, present = reader.string(), {}, []
        for property_name, kind in properties:
            flag = reader.take(1)
            if flag == b"0":
                continue
            if flag != b"1":
                raise PakError("Character property presence differs from its original contract")
            present.append(property_name)
            values = reader.u16()
            if values > 1024:
                raise PakError("Unbounded character property value array")
            if kind == 6:
                strings[property_name] = tuple(reader.string() for _ in range(values))
            else:
                reader.take(fixed_sizes[kind] * values)
        records.append(Record(name, begin, reader.position, strings, tuple(present)))
    records_end = reader.position
    if reader.take(5) != b"_bin\0" or reader.position != len(data):
        raise PakError("Character template original trailer differs")
    return Template(class_id, class_name, properties, count_offset, records_begin,
                    tuple(records), records_end)


def counted_string(value: str) -> bytes:
    data = value.encode("ascii")
    return struct.pack("<I", len(data)) + data


def append_declaration(bone_data: bytes, avatar_data: bytes,
                       resource_name: str = RESOURCE_NAME) -> tuple[bytes, dict]:
    if resource_name not in (RESOURCE_NAME, CJ_RESOURCE_NAME):
        raise PakError("Character declaration name is outside the exact owned resource scope")
    if digest(bone_data) != INPUTS[BONE_TEMPLATE] or digest(avatar_data) != INPUTS[AVATAR_TEMPLATE]:
        raise PakError("Character declaration source differs from its supported original")
    bone, avatar = parse_template(bone_data), parse_template(avatar_data)
    if bone.class_name != "BONEDMODELMASTER" or avatar.class_name != "AVATARMODELMASTER":
        raise PakError("Character declaration class-name remap differs")
    if ("FileName", 6) not in bone.properties or ("FileName", 6) not in avatar.properties:
        raise PakError("Character template FileName is not the original string property")
    selected = [record for record in avatar.records
                if record.name in {"random", "gamepad0", "gamepad1", "gamepad2", "gamepad3"}]
    if {record.name for record in selected} != {"random", "gamepad0", "gamepad1", "gamepad2", "gamepad3"}:
        raise PakError("Character template lacks the exact supported Avatar masters")
    filenames = [record.strings.get("FileName") for record in selected]
    if any(value is None or len(value) != 1 or value[0].casefold() != RESOURCE_NAME.casefold()
           for value in filenames):
        raise PakError("Character template source FileName differs from the selected original Avatar")
    source_filename = next(record for record in selected if record.name == "random").strings["FileName"][0]
    if source_filename != RESOURCE_NAME:
        raise PakError("Original failed-SDK random FileName differs from its exact runtime resource name")
    if any(record.name.casefold() == resource_name.casefold() for record in bone.records):
        raise PakError("Owned character master already exists in the source template")
    if len(bone.records) >= 1024:
        raise PakError("Character template has no bounded declaration capacity")
    filename = source_filename if resource_name == RESOURCE_NAME else resource_name
    new_record = struct.pack("<H", bone.class_id) + counted_string(resource_name)
    for property_name, _ in bone.properties:
        new_record += (b"1" + struct.pack("<H", 1) + counted_string(filename)
                       if property_name == "FileName" else b"0")
    prefix = bytearray(bone_data[:bone.records_end])
    struct.pack_into("<H", prefix, bone.count_offset, len(bone.records) + 1)
    output = bytes(prefix) + new_record + bone_data[bone.records_end:]
    parsed = parse_template(output)
    old_records = bone_data[bone.records_begin:bone.records_end]
    if (output[:bone.count_offset] != bone_data[:bone.count_offset] or
            output[bone.count_offset + 2:bone.records_end] != bone_data[bone.count_offset + 2:bone.records_end] or
            output[bone.records_end + len(new_record):] != bone_data[bone.records_end:] or
            len(parsed.records) != len(bone.records) + 1 or
            parsed.records[-1].present != ("FileName",) or
            parsed.records[-1].strings != {"FileName": (filename,)}):
        raise PakError("Character declaration changed original template content")
    return output, {
        "name": BONE_TEMPLATE, "input_sha256": digest(bone_data), "output_sha256": digest(output),
        "input_bytes": len(bone_data), "output_bytes": len(output), "resource": "BNMD:" + resource_name,
        "class_name": bone.class_name, "class_version": 1, "file_version": 2,
        "class_id_policy": "original importer remaps the serialized ID by the retained class name",
        "stock_declarations": len(bone.records), "owned_declarations_added": 1,
        "stock_record_bytes": len(old_records), "stock_record_sha256": digest(old_records),
        "stock_records_unchanged": True, "original_header_unchanged_except_object_count": True,
        "object_count_offset": bone.count_offset, "append_offset": bone.records_end,
        "append_bytes": len(new_record), "append_sha256": digest(new_record),
        "present_properties": ["FileName"], "filename_kind": 6,
        "filename": filename,
        "avatar_input_sha256": digest(avatar_data),
        "source_avatar_masters": [record.name for record in selected],
        "loader": "original template importer, property writer, LoadSelf, master registry and child pool",
        "resolver_policy": "existing named lookup; original type flags are retained",
    }


def private_output(path: Path) -> Path:
    _no_links(path.absolute())
    output = Path(os.path.abspath(path))
    _no_links(output)
    if not output.is_relative_to(ROOT / "build_native"):
        raise PakError("Derived character template must stay in ignored build_native")
    return output


def checked_asset(path: Path, maximum: int) -> bytes:
    path = private_output(path)
    if not path.is_file() or not 0 < path.stat().st_size <= maximum:
        raise PakError("Owned character asset is missing or exceeds its input budget: " + str(path))
    return path.read_bytes()


def character_assets(manifest: Path, original_cfg: bytes, original_csf: bytes) -> tuple[dict[str, bytes], dict]:
    """Read seven fixed owned files; never trust manifest strings as paths."""
    from cal3d_character import CharacterError, preflight_hardware, read_cmf, read_csf
    from title_texture_recon import png_metadata

    manifest = private_output(manifest)
    manifest_bytes = checked_asset(manifest, 64 * 1024)
    data = json.loads(manifest_bytes)
    if not isinstance(data, dict) or data.get("resource") != "BNMD:" + CJ_RESOURCE_NAME:
        raise PakError("Character asset manifest must describe the exact owned CJ resource")
    if data.get("original_cfg_sha256") != digest(original_cfg) or data.get("skeleton_sha256") != digest(original_csf):
        raise PakError("Character asset manifest differs from the actual original CFG/skeleton")
    files = {name: checked_asset(manifest.parent.joinpath(*name.split("/")),
                                 64 * 1024 if name.endswith((".cfg", ".txt")) else 64 * 1024 * 1024)
             for name in CJ_ASSET_NAMES}
    cfg, cmf, mats = (files[name] for name in CJ_ASSET_NAMES[:3])
    try:
        expected_cfg, replacements = re.subn(rb"^mesh=[^\r\n]+", b"mesh=cj_native.cmf",
                                              original_cfg, flags=re.MULTILINE)
        if replacements != 1 or cfg != expected_cfg or data.get("cfg_sha256") != digest(cfg):
            raise PakError("CJ CFG must preserve every original byte except its single mesh filename")
        if data.get("mesh_sha256") != digest(cmf):
            raise PakError("CJ mesh differs from its asset manifest hash")
        skeleton = read_csf(original_csf)
        mesh = read_cmf(cmf, skeleton)
        hardware = preflight_hardware(mesh, skeleton)
        if len(skeleton.bones) != 72 or not 1 <= len(mesh.submeshes) <= 4 or hardware != data.get("hardware"):
            raise PakError("Character mesh must retain the actual 72-bone skeleton and one to four bounded submeshes")
    except CharacterError as error:
        raise PakError("CJ mesh validation failed: " + str(error)) from error
    material_names = [f"_native/cj_native_{index}" for index in range(4)]
    expected_mats = ("\r\n".join(material_names[:len(mesh.submeshes)]) + "\r\nendmats\r\n").encode("ascii")
    if mats != expected_mats:
        raise PakError("Character material companion must match its exact ordered physical submeshes")
    materials = data.get("materials")
    if not isinstance(materials, list) or len(materials) != 4:
        raise PakError("CJ asset manifest must contain four ordered material records")
    for index, material in enumerate(materials):
        if (not isinstance(material, dict) or type(material.get("submesh")) is not int or
                material["submesh"] != index or material.get("registered_name") != material_names[index] or
                material.get("diffuse_filename") != f"_avatar/cj_native_{index}.png"):
            raise PakError("CJ material name/order/texture filename differs from its exact owned scope")
        texture = files[CJ_ASSET_NAMES[3 + index]]
        info = png_metadata(texture)
        if (digest(texture) != material.get("texture_sha256") or info != material.get("texture_metadata") or
                info["sample_bits"] != 8 or info["color_type"] not in (2, 6) or info["interlace"] != 0):
            raise PakError("CJ diffuse texture differs from its hash or verified eight-bit PNG layout")
    if sum(map(len, files.values())) > 256 * 1024 * 1024:
        raise PakError("Owned CJ files exceed their aggregate byte budget")
    return files, {"character_resource": "BNMD:" + CJ_RESOURCE_NAME,
                   "character_assets_manifest_sha256": digest(manifest_bytes),
                   "owned_files": [{"name": name, "output_sha256": digest(content), "output_bytes": len(content)}
                                   for name, content in files.items()]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pak", type=Path, default=ROOT / "game/data.pak")
    parser.add_argument("--output", type=Path, default=ROOT / "build_native/native_character_candidate")
    parser.add_argument("--character-assets", type=Path,
                        help="Checked CJ asset manifest in ignored build_native; copies only its seven fixed owned files")
    args = parser.parse_args()
    with args.pak.open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != PAK_SHA256:
            raise PakError("Unsupported original package identity")
    entries = {entry.name: entry for entry in read_directory(args.pak)}
    owned_files, owned_metadata = {}, {}
    resource_name = RESOURCE_NAME
    if args.character_assets is not None:
        owned_files, owned_metadata = character_assets(args.character_assets,
            read_entry(args.pak, entries[CJ_CFG]), read_entry(args.pak, entries[CJ_SKELETON]))
        resource_name = CJ_RESOURCE_NAME
    content, metadata = append_declaration(read_entry(args.pak, entries[BONE_TEMPLATE]),
                                           read_entry(args.pak, entries[AVATAR_TEMPLATE]), resource_name)
    output = private_output(args.output)
    target = private_output(output.joinpath(*BONE_TEMPLATE.split("/")))
    manifest = private_output(output / "character_manifest.json")
    targets = {private_output(output.joinpath(*name.split("/"))): value for name, value in owned_files.items()}
    # Inspect every destination before any writes, including existing links.
    for destination in targets:
        if destination.exists() and not destination.is_file():
            raise PakError("Owned character destination is not a file")
    for destination, value in targets.items():
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(value)
        if digest(destination.read_bytes()) != digest(value):
            raise PakError("Owned character asset failed its output hash check")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)
    if digest(target.read_bytes()) != metadata["output_sha256"]:
        raise PakError("Derived character declaration failed its output hash check")
    report = {"policy": "owned character declaration; original package and original resource creation policy retained",
              "package_sha256": PAK_SHA256, "overlay_root": str(output), "files": [metadata], **owned_metadata}
    manifest.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"manifest": str(manifest), "resource": metadata["resource"],
                      "stock_declarations_preserved": metadata["stock_declarations"],
                      "output_sha256": metadata["output_sha256"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
