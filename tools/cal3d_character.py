#!/usr/bin/env python3
"""Observed Tongas Cal3D 1010 character reader/writer and Blender handoff.

Rest poses, geometry and animation assets are read from the user's files, never
embedded here. Derived originals and conversion scenes stay in ignored build_native.
Only the observed uncompressed CSF/CMF layout is supported; CAFs remain untouched.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess

from inspect_pak import _no_links

ROOT = Path(__file__).resolve().parents[1]
VERSION = 1010
MAX_FILE = 64 * 1024 * 1024
HARDWARE_BONES = 48
HARDWARE_VERTICES = 64000
HARDWARE_TRIANGLES = 64000
HARDWARE_GROUP_VERTICES = 50000


class CharacterError(ValueError):
    pass


class Reader:
    def __init__(self, data: bytes):
        if len(data) > MAX_FILE:
            raise CharacterError("Character file exceeds its bound")
        self.data, self.position = data, 0

    def take(self, size: int) -> bytes:
        if size < 0 or size > len(self.data) - self.position:
            raise CharacterError("Truncated character record")
        result = self.data[self.position:self.position + size]
        self.position += size
        return result

    def integer(self) -> int:
        return struct.unpack("<i", self.take(4))[0]

    def count(self, maximum: int, minimum: int = 0) -> int:
        result = self.integer()
        if not minimum <= result <= maximum:
            raise CharacterError("Character count exceeds its bound")
        return result

    def floats(self, count: int) -> tuple[float, ...]:
        result = struct.unpack(f"<{count}f", self.take(count * 4))
        if not all(math.isfinite(value) for value in result):
            raise CharacterError("Nonfinite character attribute")
        return result

    def header(self, magic: bytes):
        if self.take(4) != magic or self.integer() != VERSION:
            raise CharacterError("Only the observed Cal3D 1010 layout is supported")

    def end(self):
        if self.position != len(self.data):
            raise CharacterError("Unparsed character bytes")


@dataclass
class Bone:
    name: str
    translation: tuple[float, ...]
    rotation: tuple[float, ...]
    inverse_bind_translation: tuple[float, ...]
    inverse_bind_rotation: tuple[float, ...]
    parent: int
    children: tuple[int, ...]


@dataclass
class Skeleton:
    bones: list[Bone]


@dataclass
class Vertex:
    position: tuple[float, ...]
    normal: tuple[float, ...]
    collapse: int
    collapse_faces: int
    uv: tuple[tuple[float, ...], ...]
    influences: tuple[tuple[int, float], ...]


@dataclass
class Submesh:
    material: int
    uv_sets: int
    vertices: list[Vertex]
    faces: list[tuple[int, int, int]]


@dataclass
class Mesh:
    submeshes: list[Submesh]


def validate_skeleton(skeleton: Skeleton):
    bones = skeleton.bones
    if not 0 < len(bones) <= 256 or len({bone.name for bone in bones}) != len(bones):
        raise CharacterError("Invalid or duplicate skeleton identities")
    if any(bone.parent < -1 or bone.parent >= len(bones) or bone.parent == index
           for index, bone in enumerate(bones)):
        raise CharacterError("Invalid bone parent")
    for index, bone in enumerate(bones):
        if not bone.name or "\0" in bone.name or len(bone.name.encode("ascii")) > 255:
            raise CharacterError("Invalid skeleton name")
        if bone.parent < -1 or bone.parent >= len(bones) or bone.parent == index:
            raise CharacterError("Invalid bone parent")
        actual_children = {child for child, entry in enumerate(bones) if entry.parent == index}
        if len(set(bone.children)) != len(bone.children) or set(bone.children) != actual_children:
            raise CharacterError("Bone parent and child lists disagree")
        ancestry, parent = {index}, bone.parent
        while parent != -1:
            if parent in ancestry:
                raise CharacterError("Cyclic skeleton hierarchy")
            ancestry.add(parent)
            parent = bones[parent].parent
        for values, length in [(bone.translation, 3), (bone.rotation, 4),
                               (bone.inverse_bind_translation, 3), (bone.inverse_bind_rotation, 4)]:
            if len(values) != length or not all(math.isfinite(value) for value in values):
                raise CharacterError("Invalid bone transform")
        for quaternion in (bone.rotation, bone.inverse_bind_rotation):
            if abs(sum(value * value for value in quaternion) - 1) > 0.002:
                raise CharacterError("Bone quaternion is not unit length")


def read_csf(data: bytes) -> Skeleton:
    reader = Reader(data)
    reader.header(b"CSF\0")
    count = reader.count(256, 1)
    bones = []
    for _ in range(count):
        name_bytes = reader.take(reader.count(256, 2))
        if name_bytes[-1] != 0 or b"\0" in name_bytes[:-1]:
            raise CharacterError("Bone name lacks its single terminal NUL")
        try:
            name = name_bytes[:-1].decode("ascii")
        except UnicodeDecodeError as error:
            raise CharacterError("Non-ASCII bone identity") from error
        translation, rotation = reader.floats(3), reader.floats(4)
        inverse_translation, inverse_rotation = reader.floats(3), reader.floats(4)
        parent = reader.integer()
        children = tuple(reader.integer() for _ in range(reader.count(256)))
        bones.append(Bone(name, translation, rotation, inverse_translation, inverse_rotation, parent, children))
    reader.end()
    result = Skeleton(bones)
    validate_skeleton(result)
    return result


def write_csf(skeleton: Skeleton) -> bytes:
    validate_skeleton(skeleton)
    result = bytearray(b"CSF\0" + struct.pack("<ii", VERSION, len(skeleton.bones)))
    for bone in skeleton.bones:
        name = bone.name.encode("ascii") + b"\0"
        result += struct.pack("<i", len(name)) + name
        result += struct.pack("<14f", *bone.translation, *bone.rotation,
                              *bone.inverse_bind_translation, *bone.inverse_bind_rotation)
        result += struct.pack("<ii", bone.parent, len(bone.children))
        result += struct.pack(f"<{len(bone.children)}i", *bone.children)
    return bytes(result)


def validate_mesh(mesh: Mesh, skeleton: Skeleton | None = None):
    if not 0 < len(mesh.submeshes) <= 128:
        raise CharacterError("Invalid submesh count")
    bone_count = len(skeleton.bones) if skeleton else 256
    for submesh in mesh.submeshes:
        if submesh.material < -1 or not 1 <= submesh.uv_sets <= 2:
            raise CharacterError("Unsupported material or UV layout")
        if not 0 < len(submesh.vertices) <= 65535 or not 0 < len(submesh.faces) <= 100000:
            raise CharacterError("Submesh exceeds the observed vertex/index bounds")
        for vertex in submesh.vertices:
            if vertex.collapse != -1 or vertex.collapse_faces != 0:
                raise CharacterError("LOD collapse records are not supported")
            if len(vertex.position) != 3 or len(vertex.normal) != 3 or len(vertex.uv) != submesh.uv_sets:
                raise CharacterError("Invalid vertex attribute layout")
            attributes = (*vertex.position, *vertex.normal, *(v for uv in vertex.uv for v in uv))
            if any(len(uv) != 2 for uv in vertex.uv) or not all(math.isfinite(value) for value in attributes):
                raise CharacterError("Invalid vertex attribute")
            if not 1 <= len(vertex.influences) <= 4:
                raise CharacterError("The real skin shader requires one to four influences")
            ids = [bone for bone, _ in vertex.influences]
            if len(set(ids)) != len(ids) or any(not 0 <= bone < bone_count for bone in ids):
                raise CharacterError("Invalid or duplicate bone influence identity")
            weights = [weight for _, weight in vertex.influences]
            if any(not math.isfinite(weight) or not 0 <= weight <= 1 for weight in weights) or abs(sum(weights) - 1) > 0.0001:
                raise CharacterError("Skin influences must have normalized finite weights")
        if any(len(face) != 3 or any(not 0 <= index < len(submesh.vertices) for index in face) for face in submesh.faces):
            raise CharacterError("Face references a missing vertex")


def read_cmf(data: bytes, skeleton: Skeleton | None = None) -> Mesh:
    reader = Reader(data)
    reader.header(b"CMF\0")
    if reader.integer() != 0:
        raise CharacterError("Compressed/extended CMF flags are not supported")
    submeshes = []
    for _ in range(reader.count(128, 1)):
        material = reader.integer()
        vertices, faces = reader.count(65535, 1), reader.count(100000, 1)
        if reader.integer() != 0 or reader.integer() != 0:
            raise CharacterError("LOD and spring records are not supported")
        uv_sets = reader.count(2, 1)
        vertex_records = []
        for _ in range(vertices):
            position, normal = reader.floats(3), reader.floats(3)
            collapse, collapse_faces = reader.integer(), reader.integer()
            uv = tuple(reader.floats(2) for _ in range(uv_sets))
            influences = tuple((reader.integer(), reader.floats(1)[0]) for _ in range(reader.count(4, 1)))
            vertex_records.append(Vertex(position, normal, collapse, collapse_faces, uv, influences))
        face_records = [tuple(reader.integer() for _ in range(3)) for _ in range(faces)]
        submeshes.append(Submesh(material, uv_sets, vertex_records, face_records))
    reader.end()
    result = Mesh(submeshes)
    validate_mesh(result, skeleton)
    return result


def preflight_hardware(mesh: Mesh, skeleton: Skeleton | None = None) -> dict:
    """Predict the original title's hardware groups without changing CMF data.

    Independently implemented from observed original control flow: 8239EDF8
    supplies fixed 64000*80 vertex and 64000*6 index buffers; 8239EF4C calls
    825025A0 with maxBones=48. That loader sizes its per-group ushort vertex
    lookup to 50000, checks each face with 82501530 and remaps through 82501D80.
    The face check counts every influence absent from the current group, even
    repeated new bones within a triangle. Deduplicating that check would
    predict fewer groups and potentially underestimate duplicated vertices.
    """
    validate_mesh(mesh, skeleton)
    triangles = sum(len(submesh.faces) for submesh in mesh.submeshes)
    if triangles > HARDWARE_TRIANGLES:
        raise CharacterError("Original hardware index staging exceeds 64000 triangles")
    groups = []
    total_vertices = total_triangles = referenced_vertices = 0
    for submesh_id, submesh in enumerate(mesh.submeshes):
        bone_ids, bone_set, vertex_ids = [], set(), set()
        group_triangles = 0
        referenced_vertices += len({index for face in submesh.faces for index in face})

        def finish_group():
            nonlocal total_vertices, total_triangles
            groups.append({"submesh": submesh_id, "bone_ids": list(bone_ids),
                           "vertices": len(vertex_ids), "triangles": group_triangles,
                           "base_vertex": total_vertices, "start_index": total_triangles * 3})
            total_vertices += len(vertex_ids)
            total_triangles += group_triangles

        for face in submesh.faces:
            missing = sum(bone not in bone_set for index in face
                          for bone, _ in submesh.vertices[index].influences)
            if len(bone_ids) + missing > HARDWARE_BONES:
                finish_group()
                bone_ids, bone_set, vertex_ids = [], set(), set()
                group_triangles = 0
            for index in face:
                # The original map is reset at every hardware partition. A
                # shared source vertex therefore occupies another output slot.
                if index in vertex_ids:
                    continue
                vertex_ids.add(index)
                if len(vertex_ids) > HARDWARE_GROUP_VERTICES:
                    raise CharacterError("Original hardware group vertex lookup exceeds 50000 vertices")
                if total_vertices + len(vertex_ids) > HARDWARE_VERTICES:
                    raise CharacterError("Original hardware vertex staging exceeds 64000 vertices after partitioning")
                for bone, _ in submesh.vertices[index].influences:
                    if bone not in bone_set:
                        bone_set.add(bone)
                        bone_ids.append(bone)
            group_triangles += 1
        finish_group()
    return {"source_vertices": sum(len(submesh.vertices) for submesh in mesh.submeshes),
            "referenced_vertices": referenced_vertices, "hardware_vertices": total_vertices,
            "hardware_triangles": total_triangles, "duplicated_vertices": total_vertices - referenced_vertices,
            "groups": groups}


def write_cmf(mesh: Mesh, skeleton: Skeleton | None = None) -> bytes:
    preflight_hardware(mesh, skeleton)
    result = bytearray(b"CMF\0" + struct.pack("<iii", VERSION, 0, len(mesh.submeshes)))
    for submesh in mesh.submeshes:
        result += struct.pack("<6i", submesh.material, len(submesh.vertices), len(submesh.faces), 0, 0, submesh.uv_sets)
        for vertex in submesh.vertices:
            result += struct.pack("<6fii", *vertex.position, *vertex.normal, vertex.collapse, vertex.collapse_faces)
            for uv in vertex.uv:
                result += struct.pack("<2f", *uv)
            result += struct.pack("<i", len(vertex.influences))
            for bone, weight in vertex.influences:
                result += struct.pack("<if", bone, weight)
        for face in submesh.faces:
            result += struct.pack("<3i", *face)
    return bytes(result)


def private_output(path: Path) -> Path:
    path = Path(os.path.abspath(path))
    _no_links(path)
    if not path.is_relative_to(ROOT / "build_native"):
        raise CharacterError("Derived character outputs must remain beneath ignored build_native")
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def save(path: Path, data: bytes):
    path = private_output(path)
    if path.exists() and path.read_bytes() != data:
        raise CharacterError(f"Refusing to overwrite a different output: {path}")
    path.write_bytes(data)


def read(path: Path) -> bytes:
    _no_links(path.absolute())
    if path.stat().st_size > MAX_FILE:
        raise CharacterError("Character input exceeds its bound")
    return path.read_bytes()


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def metadata(skeleton: Skeleton, mesh: Mesh) -> dict:
    positions = [vertex.position for submesh in mesh.submeshes for vertex in submesh.vertices]
    return {
        "version": VERSION, "bone_count": len(skeleton.bones),
        "bones": [dict(id=index, **asdict(bone)) for index, bone in enumerate(skeleton.bones)],
        "bounds": {"min": [min(position[axis] for position in positions) for axis in range(3)],
                   "max": [max(position[axis] for position in positions) for axis in range(3)]},
        "submeshes": [{"material_thread_id": submesh.material, "uv_sets": submesh.uv_sets,
                       "vertices": len(submesh.vertices), "faces": len(submesh.faces),
                       "bone_ids": sorted({bone for vertex in submesh.vertices for bone, _ in vertex.influences})}
                      for submesh in mesh.submeshes],
        "max_influences": max(len(vertex.influences) for submesh in mesh.submeshes for vertex in submesh.vertices),
        "hardware": preflight_hardware(mesh, skeleton),
    }


def make_overlay(template: Path, skeleton_path: Path, mesh_path: Path, textures: list[Path],
                 output: Path, name: str) -> dict:
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,47}", name):
        raise CharacterError("Owned character basename must be simple lowercase ASCII")
    csf = read(skeleton_path)
    skeleton = read_csf(csf)
    cmf = read(mesh_path)
    mesh = read_cmf(cmf, skeleton)
    hardware = preflight_hardware(mesh, skeleton)
    original_cfg = read(template)
    try:
        text = original_cfg.decode("ascii")
    except UnicodeDecodeError as error:
        raise CharacterError("Model CFG is not the observed ASCII layout") from error
    authored_skeleton = re.findall(r"^skeleton=([^\r\n]+)", text, re.MULTILINE)
    if len(authored_skeleton) != 1 or authored_skeleton[0].lower() != skeleton_path.name.lower():
        raise CharacterError("CFG must reference the supplied original skeleton in its own directory")
    replaced, count = re.subn(r"^mesh=[^\r\n]+", f"mesh={name}.cmf", text, flags=re.MULTILINE)
    if count != 1 or not re.search(r"^animation=", text, re.MULTILINE):
        raise CharacterError("CFG must have one authored mesh and existing original animations")
    if len(textures) == 1:
        textures = textures * len(mesh.submeshes)
    if len(textures) != len(mesh.submeshes):
        raise CharacterError("Supply one diffuse texture or one per physical submesh")
    # Validate image structure using the existing title inventory readers. No
    # color transformation or source-material guessing occurs in this step.
    from title_texture_recon import png_metadata, jpeg_metadata, dds_metadata
    asset_root = output / "data/art/_avatar"
    pending_files = {asset_root / f"{name}.cfg": replaced.encode("ascii"), asset_root / f"{name}.cmf": cmf}
    material_records = []
    emitted_textures = {}
    for index, texture in enumerate(textures):
        data = read(texture)
        suffix = texture.suffix.lower()
        if suffix == ".png":
            info = png_metadata(data)
            if info["sample_bits"] != 8:
                raise CharacterError("Owned diffuse PNG must use the observed eight-bit channels")
        elif suffix in (".jpg", ".jpeg"):
            info = jpeg_metadata(data)
        elif suffix == ".dds":
            info = dds_metadata(data)
            if info["faces"] != 1:
                raise CharacterError("Owned diffuse material needs a flat texture, not a cubemap")
        else:
            raise CharacterError("Diffuse images must use an observed PNG/JPEG/DDS format")
        key = (digest(data), suffix)
        if key not in emitted_textures:
            emitted_textures[key] = f"{name}_{len(emitted_textures)}{suffix}"
            pending_files[asset_root / emitted_textures[key]] = data
            if sum(len(content) for content in pending_files.values()) > 256 * 1024 * 1024:
                raise CharacterError("Character overlay exceeds the bounded asset budget")
        filename = emitted_textures[key]
        material_records.append({"submesh": index, "registered_name": f"_native/{name}_{index}",
                                 "diffuse_filename": f"_avatar/{filename}", "texture_sha256": digest(data),
                                 "texture_metadata": info})
    mat_text = "\r\n".join(item["registered_name"] for item in material_records) + "\r\nendmats\r\n"
    pending_files[asset_root / f"{name}.mat.txt"] = mat_text.encode("ascii")
    report = {"resource": f"BNMD:_avatar/{name}.cfg", "mesh_sha256": digest(cmf),
              "original_cfg_sha256": digest(original_cfg), "cfg_sha256": digest(replaced.encode("ascii")),
              "skeleton_sha256": digest(csf), "materials": material_records,
              "hardware": hardware,
              "animations": "Original relative CSF/CAF/mask paths and callbacks preserved byte-for-byte",
              "requires_native_material_binding": True}
    pending_files[output / f"{name}.assets.json"] = (json.dumps(report, indent=2) + "\n").encode()
    # Validate all source images and destination conflicts before writing any
    # asset. The binding manifest is last, so interrupted I/O is incomplete.
    for path, data in pending_files.items():
        target = private_output(path)
        if target.exists() and target.read_bytes() != data:
            raise CharacterError(f"Refusing to overwrite a different output: {target}")
    for path, data in pending_files.items():
        save(path, data)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    roundtrip = sub.add_parser("roundtrip")
    roundtrip.add_argument("--skeleton", type=Path, required=True)
    roundtrip.add_argument("--mesh", type=Path, required=True)
    roundtrip.add_argument("--output", type=Path, required=True)
    prepare = sub.add_parser("prepare-blend")
    prepare.add_argument("--skeleton", type=Path, required=True)
    prepare.add_argument("--mesh", type=Path, required=True)
    prepare.add_argument("--source", type=Path, required=True)
    prepare.add_argument("--output", type=Path, required=True)
    prepare.add_argument("--blender", type=Path, required=True)
    export = sub.add_parser("export-blend")
    export.add_argument("--scene", type=Path, required=True)
    export.add_argument("--output", type=Path, required=True)
    export.add_argument("--blender", type=Path, required=True)
    export.add_argument("--weights", choices=("existing", "surface"), default="existing")
    overlay = sub.add_parser("make-overlay")
    overlay.add_argument("--template", type=Path, required=True)
    overlay.add_argument("--skeleton", type=Path, required=True)
    overlay.add_argument("--mesh", type=Path, required=True)
    overlay.add_argument("--diffuse", type=Path, action="append", required=True)
    overlay.add_argument("--output", type=Path, required=True)
    overlay.add_argument("--name", default="cj_native")
    args = parser.parse_args()
    if args.command == "roundtrip":
        csf, cmf = read(args.skeleton), read(args.mesh)
        skeleton = read_csf(csf)
        mesh = read_cmf(cmf, skeleton)
        emitted_csf, emitted_cmf = write_csf(skeleton), write_cmf(mesh, skeleton)
        if csf != emitted_csf or cmf != emitted_cmf:
            raise CharacterError("Character reader/writer did not reproduce the exact original bytes")
        save(args.output / args.skeleton.name, emitted_csf)
        save(args.output / args.mesh.name, emitted_cmf)
        report = metadata(skeleton, mesh)
        report.update(skeleton_sha256=digest(csf), mesh_sha256=digest(cmf), exact_roundtrip=True)
        save(args.output / "roundtrip.json", (json.dumps(report, indent=2) + "\n").encode())
        print(json.dumps({key: report[key] for key in ("bone_count", "max_influences", "exact_roundtrip", "skeleton_sha256", "mesh_sha256")}))
    elif args.command == "make-overlay":
        report = make_overlay(args.template, args.skeleton, args.mesh, args.diffuse, args.output, args.name)
        print(json.dumps({"resource": report["resource"], "materials": len(report["materials"]),
                          "requires_native_material_binding": True}))
    else:
        private_output(args.output)
        command = [str(args.blender), "--background", "--python-exit-code", "1"]
        if args.command == "export-blend":
            command += [str(args.scene)]
        command += ["--python", str(ROOT / "tools/blender_character.py"), "--", args.command, "--output", str(args.output)]
        if args.command == "prepare-blend":
            command += ["--skeleton", str(args.skeleton), "--mesh", str(args.mesh), "--source", str(args.source)]
        else:
            command += ["--weights", args.weights]
        subprocess.run(command, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
