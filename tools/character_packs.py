"""Portable character packs; original rigs, CFGs and animations stay local."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import uuid
import zlib

from cal3d_character import CharacterError, Mesh, preflight_hardware, private_output, read_cmf, read_csf
from inspect_pak import PakError, _no_links, read_directory, read_entry, safe_name
from prepare_native_character import CJ_CFG, CJ_SKELETON, PAK_SHA256, character_assets
from title_texture_recon import png_metadata

ROOT = Path(__file__).resolve().parents[1]
SKELETON_SHA256 = "599839637025264e824f1d018d4d79c8e3ee104d7cfb70373a7c920e801a5f54"
MAX_FILE = 64 * 1024 * 1024
MAX_PACK = 256 * 1024 * 1024


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def bounded_read(path: Path, maximum: int = MAX_FILE) -> bytes:
    path = Path(os.path.abspath(path))
    _no_links(path)
    with path.open("rb") as source:
        data = source.read(maximum + 1)
    if not data or len(data) > maximum:
        raise CharacterError(f"Empty or oversized character file: {path.name}")
    return data


def identifier(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[a-z][a-z0-9-]{0,47}", value):
        raise CharacterError("Character ID must be 1–48 lowercase letters, digits or hyphens, starting with a letter")
    safe_name(value)
    return value


def checked_png(data: bytes) -> dict:
    info = png_metadata(data)
    if (info["sample_bits"] != 8 or info["color_type"] not in (2, 6) or info["interlace"] or
            max(info["width"], info["height"]) > 4096):
        raise CharacterError("Diffuse textures must be noninterlaced RGB/RGBA 8-bit PNG, at most 4096×4096")
    position, compressed = 8, bytearray()
    while position < len(data):
        size = struct.unpack_from(">I", data, position)[0]
        if data[position + 4:position + 8] == b"IDAT":
            compressed += data[position + 8:position + 8 + size]
        position += size + 12
    stride = info["width"] * (3 if info["color_type"] == 2 else 4) + 1
    expected = stride * info["height"]
    decoder = zlib.decompressobj()
    try:
        pixels = decoder.decompress(compressed, expected + 1)
    except zlib.error as error:
        raise CharacterError("PNG has invalid compressed pixel data") from error
    if (len(pixels) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail or
            any(pixels[row * stride] > 4 for row in range(info["height"]))):
        raise CharacterError("PNG pixel data is truncated, oversized or has invalid row filters")
    return info


def solid_png(color) -> bytes:
    if len(color) != 4 or any(not isinstance(x, (float, int)) or not math.isfinite(x) or not 0 <= x <= 1 for x in color):
        raise CharacterError("Material base color must contain four finite values between zero and one")
    def srgb(value):
        return value * 12.92 if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055
    pixel = bytes(round(srgb(float(x)) * 255) for x in color[:3]) + bytes([round(color[3] * 255)])
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"\0" + pixel)) + chunk(b"IEND", b""))


@dataclass
class Pack:
    path: Path
    metadata: dict
    mesh: Mesh
    mesh_bytes: bytes
    textures: list[bytes]
    fingerprint: str


def validate_pack(path: Path) -> Pack:
    path = Path(os.path.abspath(path))
    if path.is_dir():
        path /= "character.json"
    metadata_bytes = bounded_read(path, 64 * 1024)
    data = json.loads(metadata_bytes)
    if not isinstance(data, dict) or type(data.get("schema_version")) is not int or data["schema_version"] != 1:
        raise CharacterError("Unsupported character pack schema")
    identifier(data.get("id"))
    if not isinstance(data.get("name"), str) or not 1 <= len(data["name"]) <= 100 or any(ord(c) < 32 for c in data["name"]):
        raise CharacterError("Character pack needs a readable display name")
    if data.get("skeleton_sha256") != SKELETON_SHA256:
        raise CharacterError("Character pack targets a different original skeleton")
    mesh_record = data.get("mesh")
    if not isinstance(mesh_record, dict) or mesh_record.get("file") != "model.cmf":
        raise CharacterError("Character pack mesh must be model.cmf")
    binary = bounded_read(path.parent / "model.cmf")
    if digest(binary) != mesh_record.get("sha256"):
        raise CharacterError("Character mesh hash differs from its manifest")
    mesh = read_cmf(binary)
    if not 1 <= len(mesh.submeshes) <= 4:
        raise CharacterError("This native character slot supports one to four material/submesh groups; merge or bake materials in Blender")
    if any(bone >= 72 for sub in mesh.submeshes for vertex in sub.vertices for bone, _ in vertex.influences):
        raise CharacterError("Character mesh references a bone outside the actual 72-bone skeleton")
    preflight_hardware(mesh)
    records = data.get("textures")
    if not isinstance(records, list) or len(records) not in (1, len(mesh.submeshes)):
        raise CharacterError("Supply one diffuse texture or one for each submesh")
    textures = []
    for index, row in enumerate(records):
        if not isinstance(row, dict) or row.get("file") != f"diffuse-{index}.png":
            raise CharacterError("Texture filenames must be ordered diffuse-0.png, diffuse-1.png, …")
        content = bounded_read(path.parent / row["file"])
        if digest(content) != row.get("sha256"):
            raise CharacterError("Diffuse texture hash differs from its manifest")
        checked_png(content)
        textures.append(content)
    if len(binary) + sum(map(len, textures)) > MAX_PACK:
        raise CharacterError("Character pack exceeds 256 MiB")
    # Fingerprint includes identity/credits and the already verified asset hashes.
    return Pack(path, data, mesh, binary, textures, digest(metadata_bytes))


def write_pack(output: Path, name: str, pack_id: str, mesh: bytes, textures: list[bytes], **credits) -> Pack:
    output = private_output(output)
    if output.exists():
        raise CharacterError("A character pack with this output path already exists; use a new name")
    identifier(pack_id)
    pending = {"model.cmf": mesh, **{f"diffuse-{i}.png": value for i, value in enumerate(textures)}}
    metadata = {"schema_version": 1, "id": pack_id, "name": name, "skeleton_sha256": SKELETON_SHA256,
                "mesh": {"file": "model.cmf", "sha256": digest(mesh)},
                "textures": [{"file": f"diffuse-{i}.png", "sha256": digest(value)} for i, value in enumerate(textures)],
                **credits}
    output.mkdir()
    for filename, value in pending.items():
        (output / filename).write_bytes(value)
    (output / "character.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return validate_pack(output)


def original_inputs(pak: Path) -> dict[str, bytes]:
    _no_links(pak.absolute())
    with pak.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != PAK_SHA256:
            raise CharacterError("The supported original game/data.pak is required")
    entries = {row.name: row for row in read_directory(pak)}
    names = (CJ_CFG, CJ_SKELETON, "data/art/_avatar/dummymodel.cmf")
    result = {name: read_entry(pak, entries[name]) for name in names}
    if digest(result[CJ_SKELETON]) != SKELETON_SHA256:
        raise CharacterError("Original skeleton identity differs from the supported character rig")
    return result


def prepare_pack(pack: Pack, pak: Path, output: Path | None = None) -> Path:
    originals = original_inputs(pak)
    output = private_output(output or ROOT / "build_native/characters/prepared" /
                            (pack.metadata["id"] + "-" + pack.fingerprint[:16]))
    skeleton = read_csf(originals[CJ_SKELETON])
    read_cmf(pack.mesh_bytes, skeleton)
    cfg, count = re.subn(rb"^mesh=[^\r\n]+", b"mesh=cj_native.cmf", originals[CJ_CFG], flags=re.MULTILINE)
    if count != 1:
        raise CharacterError("Original model configuration has no unique mesh reference")
    names = [f"_native/cj_native_{i}" for i in range(4)]
    companion = ("\r\n".join(names[:len(pack.mesh.submeshes)]) + "\r\nendmats\r\n").encode("ascii")
    pending = {"cj_native.cfg": cfg, "cj_native.cmf": pack.mesh_bytes, "cj_native.mat.txt": companion}
    materials = []
    for index in range(4):
        texture = pack.textures[index if len(pack.textures) > index else 0]
        pending[f"cj_native_{index}.png"] = texture
        materials.append({"submesh": index, "registered_name": names[index],
                          "diffuse_filename": f"_avatar/cj_native_{index}.png",
                          "texture_sha256": digest(texture), "texture_metadata": checked_png(texture)})
    report = {"resource": "BNMD:_avatar/cj_native.cfg", "mesh_sha256": digest(pack.mesh_bytes),
              "original_cfg_sha256": digest(originals[CJ_CFG]), "cfg_sha256": digest(cfg),
              "skeleton_sha256": digest(originals[CJ_SKELETON]), "materials": materials,
              "hardware": preflight_hardware(pack.mesh, skeleton),
              "animations": "Original relative CSF/CAF/mask paths and callbacks preserved byte-for-byte",
              "requires_native_material_binding": True}
    files = {output / "data/art/_avatar" / name: value for name, value in pending.items()}
    manifest = output / "cj_native.assets.json"
    files[manifest] = (json.dumps(report, indent=2) + "\n").encode("utf-8")
    for path, value in files.items():
        private_output(path)
        if path.exists() and bounded_read(path) != value:
            raise CharacterError(f"Prepared character output changed: {path}; use a new output directory")
    for path, value in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)
    character_assets(manifest, originals[CJ_CFG], originals[CJ_SKELETON])
    return manifest


def resolve_pack(value: str | Path = "cj") -> Pack:
    if str(value) == "cj":
        return validate_pack(ROOT / "assets/characters/cj")
    candidate = Path(value)
    if candidate.exists():
        return validate_pack(candidate)
    identifier(str(value))
    candidates = list((ROOT / "build_native/characters/library").glob(str(value) + "/character.json"))
    if len(candidates) != 1:
        raise CharacterError(f"Character not found: {value}")
    return validate_pack(candidates[0])


def selected_pack(state: Path | None = None) -> Pack:
    state = state or ROOT / "local/character.json"
    _no_links(state.absolute())
    if not state.exists():
        return resolve_pack("cj")
    data = json.loads(bounded_read(state, 16 * 1024))
    if not isinstance(data, dict) or data.get("schema_version") != 1 or not isinstance(data.get("pack"), str):
        raise CharacterError("Invalid selected-character settings; use Restore CJ")
    candidate = Path(os.path.abspath(ROOT / data["pack"]))
    if not candidate.is_relative_to(ROOT):
        raise CharacterError("Selected character settings point outside this project; use Restore CJ")
    pack = resolve_pack(candidate)
    if pack.fingerprint != data.get("sha256"):
        raise CharacterError("Selected character changed; select it again or use Restore CJ")
    return pack


def select_pack(pack: Pack, state: Path | None = None) -> dict:
    state = Path(os.path.abspath(state or ROOT / "local/character.json"))
    _no_links(state)
    if not pack.path.is_relative_to(ROOT):
        raise CharacterError("Install external packs into the character library before selecting them")
    data = {"schema_version": 1, "pack": pack.path.relative_to(ROOT).as_posix(),
            "sha256": pack.fingerprint, "name": pack.metadata["name"]}
    state.parent.mkdir(parents=True, exist_ok=True)
    temporary = state.with_name(state.name + "." + uuid.uuid4().hex + ".tmp")
    with temporary.open("x", encoding="utf-8") as output:
        output.write(json.dumps(data, indent=2) + "\n")
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, state)
    return data


def library() -> list[Pack]:
    result = [resolve_pack("cj")]
    for path in sorted((ROOT / "build_native/characters/library").glob("*/character.json")):
        result.append(validate_pack(path))
    return result
