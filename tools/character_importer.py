#!/usr/bin/env python3
"""Import, validate, select and launch native Doritos Crash Course characters."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

from cal3d_character import CharacterError, private_output
from character_packs import (ROOT, Pack, bounded_read, checked_png, digest, identifier, library,
                             original_inputs, prepare_pack, resolve_pack, select_pack, selected_pack,
                             solid_png, validate_pack, write_pack)
from inspect_pak import PakError, _no_links
from prepare_native_character import CJ_CFG, CJ_SKELETON, CJ_ASSET_NAMES


def find_blender(explicit: Path | None = None) -> Path:
    candidates = [explicit, os.environ.get("BLENDER"), shutil.which("blender")]
    foundation = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Blender Foundation"
    candidates += sorted(foundation.glob("Blender */blender.exe"), reverse=True)
    for item in candidates:
        if item and Path(item).is_file():
            return Path(item).absolute()
    raise CharacterError("Blender is needed for custom models. Install Blender or choose its executable; bundled CJ needs no Blender")


def run(command: list[str], log=print):
    log("Running " + Path(command[0]).name)
    creation = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    with subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace", creationflags=creation) as process:
        for line in process.stdout:
            log(line.rstrip())
        if process.wait():
            raise CharacterError(f"{Path(command[0]).name} failed; see the conversion log above")


def new_job() -> Path:
    token = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
    path = private_output(ROOT / "build_native/characters/jobs" / token)
    path.mkdir()
    return path


def install_pack(pack: Pack, pack_id: str | None = None) -> Pack:
    pack_id = identifier(pack_id or pack.metadata["id"])
    if pack_id == "cj":
        raise CharacterError("CJ is the bundled default; choose a different ID for an imported character")
    metadata = {**pack.metadata, "id": pack_id}
    encoded = (json.dumps(metadata, indent=2) + "\n").encode("utf-8")
    target = private_output(ROOT / "build_native/characters/library" / pack_id)
    if target.exists():
        current = validate_pack(target)
        if current.fingerprint == digest(encoded):
            return current
        raise CharacterError("A different character already uses this ID; choose a new ID")
    target.mkdir()
    (target / "model.cmf").write_bytes(pack.mesh_bytes)
    for index, value in enumerate(pack.textures):
        (target / f"diffuse-{index}.png").write_bytes(value)
    # A library entry becomes visible only when its complete manifest is published.
    temporary = target / "character.json.tmp"
    temporary.write_bytes(encoded)
    os.replace(temporary, target / "character.json")
    return validate_pack(target)


def import_model(source: Path, name: str | None = None, blender: Path | None = None,
                 pak: Path | None = None, pack_id: str | None = None, auto_fit=True,
                 yaw=0.0, scale=1.0, weights="surface", prepare_only=False, log=print) -> dict:
    source = Path(os.path.abspath(source))
    _no_links(source)
    if source.suffix.lower() not in (".glb", ".gltf", ".fbx", ".obj", ".blend") or not source.is_file():
        raise CharacterError("Choose a GLB, glTF, FBX, OBJ or an importer-created Blender fitting scene")
    if source.stat().st_size > 64 * 1024 * 1024:
        raise CharacterError("Model source exceeds 64 MiB")
    if not math.isfinite(yaw) or not math.isfinite(scale) or not .001 <= scale <= 1000:
        raise CharacterError("Rotation must be finite and scale must be between 0.001 and 1000")
    executable = find_blender(blender)
    originals = original_inputs(pak or ROOT / "game/data.pak")
    job = new_job()
    reference = job / "reference"
    reference.mkdir()
    for resource, content in originals.items():
        (reference / Path(resource).name).write_bytes(content)
    target = job / "converted.cmf"
    command = [str(executable), "--background", "--factory-startup", "--disable-autoexec", "--python-exit-code", "1"]
    if source.suffix.lower() == ".blend":
        command.append(str(source))
    command += ["--python", str(ROOT / "tools/blender_import_character.py"), "--",
                "--source", str(source), "--skeleton", str(reference / Path(CJ_SKELETON).name),
                "--reference", str(reference / "dummymodel.cmf"), "--output", str(target),
                "--yaw", str(yaw), "--scale", str(scale), "--weights", weights]
    if not auto_fit:
        command.append("--no-auto-fit")
    if prepare_only:
        command.append("--prepare-only")
    run(command, log)
    if prepare_only:
        return {"fitting_scene": str(target.with_suffix(".blend")), "prepared_only": True}
    report = json.loads(bounded_read(target.with_suffix(".manifest.json"), 256 * 1024))
    materials = report["materials"]
    if not 1 <= len(materials) <= 4:
        raise CharacterError("Model has more than four material/submesh groups; merge or bake them in the saved Blender scene")
    textures = []
    for material in materials:
        diffuse = material["direct_diffuse_images"]
        if len(diffuse) == 1:
            value = bounded_read(Path(diffuse[0]))
            checked_png(value)
        elif not diffuse and not material["source_images"]:
            value = solid_png(material["base_color"])
        else:
            raise CharacterError("Use one image connected directly to each material's Base Color, or a solid base color; bake complex shaders in Blender")
        textures.append(value)
    name = name or source.stem
    if pack_id is None:
        import re
        slug = re.sub(r"[^a-z0-9-]+", "-", name.lower()).strip("-")[:32]
        if not slug or not slug[0].isalpha():
            slug = "character-" + slug
        pack_id = slug + "-" + uuid.uuid4().hex[:8]
    if pack_id == "cj":
        raise CharacterError("Use a different ID; the original bundled CJ remains the default")
    staged = write_pack(job / "pack", name, pack_id, bounded_read(target), textures,
                        author="User-supplied model", license="Original model's terms apply",
                        conversion={"source_filename": source.name, "weights": weights,
                                    "auto_fit": auto_fit, "yaw_degrees": yaw, "scale_multiplier": scale})
    installed = install_pack(staged)
    return {"id": installed.metadata["id"], "name": installed.metadata["name"],
            "pack": str(installed.path), "sha256": installed.fingerprint,
            "fitting_scene": str(target.with_suffix(".blend")), "hardware": report["hardware"]}


def overlay_files(folder: Path) -> dict[str, Path]:
    folder = Path(os.path.abspath(folder))
    _no_links(folder)
    if not folder.is_relative_to(ROOT / "build_native") or not folder.is_dir():
        raise CharacterError("The base overlay must be a prepared directory beneath build_native")
    result, stack = {}, [folder]
    while stack:
        current = stack.pop()
        for item in current.iterdir():
            _no_links(item)
            if item.is_dir():
                stack.append(item)
            elif item.is_file():
                result[item.relative_to(folder).as_posix()] = item
            else:
                raise CharacterError("Unexpected non-file in base overlay")
    if len(result) > 1000 or sum(p.stat().st_size for p in result.values()) > 512 * 1024 * 1024:
        raise CharacterError("Base overlay exceeds its copy budget")
    required = ("manifest.json", "character_manifest.json", "material_manifest.json", "texture_manifest.json",
                "data/templates/guigroup.wbr", "data/templates/bonedmodel.wbr", "data/templates/shader.wbr",
                "data/templates/texture.wbr", *CJ_ASSET_NAMES)
    if any(name not in result for name in required):
        raise CharacterError("Native base overlay is missing; run tools/build.ps1 first")
    return result


def build_overlay(pack: Pack, pak: Path | None = None, base: Path | None = None, log=print) -> Path:
    pak = pak or ROOT / "game/data.pak"
    base = Path(os.path.abspath(base or ROOT / "build_native/offline_menu_override"))
    sources = overlay_files(base)
    assets = prepare_pack(pack, pak)
    if all(bounded_read(sources[name]) == bounded_read(assets.parent / name) for name in CJ_ASSET_NAMES):
        return base
    base_hashes = {name: digest(bounded_read(path)) for name, path in sorted(sources.items())}
    token = digest(json.dumps({"pipeline": 1, "base": base_hashes, "pack": pack.fingerprint}, sort_keys=True).encode())[:24]
    output = private_output(ROOT / "build_native/characters/overlays" / (pack.metadata["id"] + "-" + token))
    ready = output / "character_selection.json"
    if ready.exists():
        state = json.loads(bounded_read(ready, 256 * 1024))
        if state.get("pack_sha256") != pack.fingerprint or set(state.get("files", {})) != set(sources):
            raise CharacterError("Character overlay cache manifest changed")
        for name, expected in state["files"].items():
            if digest(bounded_read(output / name)) != expected:
                raise CharacterError("Character overlay cache changed; reimport with a new character ID")
        return output
    output.mkdir(parents=True, exist_ok=True)
    for name, path in sources.items():
        target = private_output(output / name)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    for script in ("prepare_native_character.py", "prepare_native_materials.py", "prepare_native_textures.py"):
        run([sys.executable, str(ROOT / "tools" / script), "--pak", str(pak),
             "--character-assets", str(assets), "--output", str(output)], log)
    hashes = {name: digest(bounded_read(output / name)) for name in sources}
    ready.write_text(json.dumps({"schema_version": 1, "name": pack.metadata["name"],
                                "pack_sha256": pack.fingerprint, "files": hashes}, indent=2) + "\n", encoding="utf-8")
    return output


def launch(native_arguments: list[str], character=None, state=None, executable=None, base=None, game=None) -> int:
    native_arguments = list(native_arguments)
    if native_arguments[:1] == ["--"]:
        native_arguments.pop(0)
    for flag in ("--character", "--character-state"):
        while flag in native_arguments:
            index = native_arguments.index(flag)
            if index + 1 >= len(native_arguments):
                raise CharacterError(f"{flag} needs a value")
            value = native_arguments[index + 1]
            if flag == "--character":
                character = value
            else:
                state = Path(value)
            del native_arguments[index:index + 2]
    executable = Path(executable or ROOT / "build_native/native/Release/DoritosNative.exe")
    if not executable.is_file():
        raise CharacterError("Native executable has not been built; run tools/build.ps1 first")
    command = [str(executable), "--game-dir", str(game or ROOT / "game")]
    if "--asset-override-dir" not in native_arguments:
        pack = resolve_pack(character) if character else selected_pack(state)
        overlay = build_overlay(pack, base=base, pak=Path(game or ROOT / "game") / "data.pak")
        print("Character: " + pack.metadata["name"], flush=True)
        command += ["--asset-override-dir", str(overlay)]
    # Preserve the caller's cwd: isolated testers keep their own saves there.
    return subprocess.run(command + native_arguments).returncode


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="List bundled CJ and installed character packs")
    check = commands.add_parser("validate"); check.add_argument("character", nargs="?", default="cj")
    prepare = commands.add_parser("prepare", help="Generate local CFG/material inputs from the user's game")
    prepare.add_argument("--character", default="cj"); prepare.add_argument("--pak", type=Path, default=ROOT / "game/data.pak")
    prepare.add_argument("--output", type=Path)
    select = commands.add_parser("select"); select.add_argument("character"); select.add_argument("--state", type=Path)
    install = commands.add_parser("install-pack"); install.add_argument("path", type=Path); install.add_argument("--id")
    convert = commands.add_parser("import-model")
    convert.add_argument("source", type=Path); convert.add_argument("--name"); convert.add_argument("--id")
    convert.add_argument("--blender", type=Path); convert.add_argument("--pak", type=Path)
    convert.add_argument("--no-auto-fit", action="store_true"); convert.add_argument("--yaw", type=float, default=0)
    convert.add_argument("--scale", type=float, default=1); convert.add_argument("--weights", choices=("surface", "existing"), default="surface")
    convert.add_argument("--prepare-only", action="store_true"); convert.add_argument("--activate", action="store_true")
    play = commands.add_parser("launch")
    play.add_argument("--character"); play.add_argument("--state", type=Path); play.add_argument("--exe", type=Path)
    play.add_argument("--base-overlay", type=Path); play.add_argument("--game", type=Path)
    play.add_argument("native_arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.command == "list":
        result = [{"id": p.metadata["id"], "name": p.metadata["name"], "pack": str(p.path),
                   "default": p.metadata["id"] == "cj"} for p in library()]
    elif args.command == "validate":
        pack = resolve_pack(args.character)
        result = {"valid": True, "name": pack.metadata["name"], "sha256": pack.fingerprint,
                  "hardware": __import__("cal3d_character").preflight_hardware(pack.mesh)}
    elif args.command == "prepare":
        result = {"character_assets": str(prepare_pack(resolve_pack(args.character), args.pak, args.output))}
    elif args.command == "select":
        result = select_pack(resolve_pack(args.character), args.state)
    elif args.command == "install-pack":
        pack = install_pack(validate_pack(args.path), args.id)
        result = {"id": pack.metadata["id"], "name": pack.metadata["name"], "pack": str(pack.path)}
    elif args.command == "import-model":
        result = import_model(args.source, args.name, args.blender, args.pak, args.id, not args.no_auto_fit,
                              args.yaw, args.scale, args.weights, args.prepare_only)
        if args.activate and not args.prepare_only:
            select_pack(resolve_pack(result["pack"]))
    else:
        return launch(args.native_arguments, args.character, args.state, args.exe, args.base_overlay, args.game)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CharacterError, PakError, OSError, ValueError, KeyError) as error:
        print("Character importer: " + str(error), file=sys.stderr)
        raise SystemExit(1)
