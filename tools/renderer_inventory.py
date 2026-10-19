#!/usr/bin/env python3
"""Inventory original shader metadata and generated imports, without emitting source."""
from __future__ import annotations
import argparse
import bisect
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import sys

try:
    from .inspect_pak import SHADER_ROOT, PakError, _no_links, inspect_pak, read_directory, read_entry
except ImportError:
    from inspect_pak import SHADER_ROOT, PakError, _no_links, inspect_pak, read_directory, read_entry


def shader_metadata(name: str, content: bytes) -> dict:
    lexical_content = content[:-1] if content.endswith(b"\0") else content
    binary = any(value < 9 or 13 < value < 32 for value in lexical_content)
    text = lexical_content.decode("utf-8", errors="replace")
    result = {"name": name, "decoded_size": len(content), "sha256": hashlib.sha256(content).hexdigest(),
              "content_kind": "binary_shader_cache" if binary else "text_shader_source",
              "profiles": sorted(set(re.findall(r"\b[vp]s_[1-5]_[0-9]\b", text)))}
    if not binary:
        result["terminal_nul"] = len(lexical_content) != len(content)
        functions = sorted(set(re.findall(
            r"(?m)^\s*(?:(?:static|inline)\s+)*[A-Za-z_]\w*(?:\d(?:x\d)?)?\s+([A-Za-z_]\w*)\s*\(", text)))
        result["function_names"] = functions
        result["entry_candidates"] = [function for function in functions if re.search(r"(?:^main|[pv]s[A-Z]|_VS$|_PS$|VS$|PS$)", function)]
        result["includes"] = sorted(set(re.findall(r'^\s*#\s*include\s*[<"]([^">]+)[">]', text, re.M)))
        result["conditional_symbols"] = sorted(set(re.findall(r"\b(?:ifdef|ifndef|defined)\s*\(?\s*([A-Za-z_]\w*)", text)))
        result["candidate_status"] = "lexical_candidates_require_engine_defines_vertex_layouts_and_bindings"
    elif PurePosixPath(name).name == "precache_win32_3.bsh":
        result["target_evidence"] = "win32_name_and_shader_model_strings; cache_record_format_not_yet_decoded"
    elif PurePosixPath(name).name == "precache_xenon.bsh":
        result["target_evidence"] = "xenon_name_and_shader_model_strings; cache_record_format_not_yet_decoded"
    return result


def import_inventory(mapping_path: Path) -> list[dict]:
    # The user-owned generated mapping is inspected, never copied into the project.
    text = mapping_path.read_text(encoding="utf-8")
    matches = re.findall(r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*__imp__(Vd\w+)\s*\}", text)
    return [{"address": address.upper().replace("0X", "0x"), "name": name,
             "status": "kernel_import_boundary_only; engine_draw_hook_not_identified"}
            for address, name in sorted(set(matches))]


def inventory(pak_path: Path, mapping_path: Path | None = None) -> dict:
    shaders = []
    for entry in read_directory(pak_path):
        if entry.name.startswith(SHADER_ROOT):
            shaders.append(shader_metadata(entry.name, read_entry(pak_path, entry, max_output=4 * 1024 * 1024)))
    result = {"package": inspect_pak(pak_path), "shaders": shaders,
              "shader_content_counts": dict(Counter(shader["content_kind"] for shader in shaders)),
              "limitations": ["No original shader source or bytecode is embedded in this inventory.",
                              "Shader availability does not identify the game's D3D device/vtable/draw boundaries.",
                              "Shader cache records, engine constants, vertex declarations and texture layouts still require verification."]}
    if mapping_path:
        result["graphics_imports"] = import_inventory(mapping_path)
    return result


def image_inventory(image_path: Path, metadata_path: Path, mapping_path: Path) -> dict:
    """Correlate verified image string addresses with bounded PPC address loads."""
    if image_path.stat().st_size > 128 * 1024 * 1024:
        raise PakError("Loaded image exceeds renderer inventory read limit")
    data = image_path.read_bytes()
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if metadata.get("image_sha256") != hashlib.sha256(data).hexdigest() or metadata.get("encryption") != 1:
        raise PakError("Renderer image does not match its encrypted-XEX loader metadata")
    image_base = metadata["image_base"]
    mapping = mapping_path.read_text(encoding="utf-8")
    functions = sorted(set(int(value, 16) for value in re.findall(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*sub_", mapping)))
    if not functions or functions[0] < image_base or functions[-1] >= image_base + len(data):
        raise PakError("Function mapping is outside the verified loaded image")
    names = {"DIRECTXSHADERMANAGER::LoadShaders", "LoadShaders", "RenderShader", "CreateTextureDesc",
             "ShadersList", "ShadersPriorityList", "ShaderVariations", "PixelShader", "VertexShader"}
    symbols = {}
    for match in re.finditer(rb"[ -~]{5,}", data):
        if match.group().decode("ascii") in names:
            address = image_base + match.start()
            symbols[address] = {"name": match.group().decode("ascii"), "address": f"0x{address:08X}", "references": []}
    for offset in range(functions[0] - image_base, functions[-1] - image_base, 4):
        instruction = struct.unpack_from(">I", data, offset)[0]
        if instruction >> 26 != 15 or (instruction >> 16) & 31:
            continue
        register = (instruction >> 21) & 31
        high = (instruction & 65535) << 16
        for gap in range(4, 36, 4):
            if offset + gap + 4 > len(data):
                break
            following = struct.unpack_from(">I", data, offset + gap)[0]
            opcode = following >> 26
            if opcode == 14 and (following >> 16) & 31 == register:
                low = following & 65535
                low -= 65536 if low & 32768 else 0
                value = (high + low) & 0xFFFFFFFF
                if value in symbols:
                    function = functions[bisect.bisect_right(functions, image_base + offset) - 1]
                    symbols[value]["references"].append({"instruction_address": f"0x{image_base+offset+gap:08X}",
                                                       "function_candidate": f"sub_{function:08X}", "instruction_gap": gap})
                break
            # Stop after instructions known to overwrite this high-half register.
            if opcode in {14, 15, *range(32, 48)} and (following >> 21) & 31 == register:
                break
            if opcode in {20, 21, 23, 24, 25, 26, 27, 28, 29} and (following >> 16) & 31 == register:
                break
    imports = [{"address": f"0x{entry['address']:08X}", "name": entry["name"]}
               for entry in metadata["imports"] if entry["kind"] == 1 and entry["name"].startswith("Vd")]
    return {"image_sha256": metadata["image_sha256"], "image_base": f"0x{image_base:08X}",
            "symbols": list(symbols.values()), "graphics_imports": imports,
            "reference_status": "lis/addi address constructions with nearest mapped function; candidate identities require verification"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pak", nargs="?", default="game/data.pak", type=Path)
    parser.add_argument("--mapping", type=Path)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build_native/renderer_inventory.json"))
    args = parser.parse_args(argv)
    try:
        result = inventory(args.pak, args.mapping)
        if args.image or args.metadata:
            if not args.image or not args.metadata or not args.mapping:
                raise PakError("Image inventory requires --image, --metadata and --mapping")
            result["loaded_image"] = image_inventory(args.image, args.metadata, args.mapping)
        _no_links(args.output.absolute())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        _no_links(args.output.parent.absolute())
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    except (OSError, PakError) as error:
        print(f"Renderer inventory rejected: {error}", file=sys.stderr)
        return 2
    print(f"Inventoried {len(result['shaders'])} shader entries: {args.output.absolute()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
