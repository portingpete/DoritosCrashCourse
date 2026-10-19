"""Append a captured original shader to the title's normal Xenon precache.

The stock cache and every stock variant remain unchanged. Only the existing
shader's variant count and its appended, genuine source-compiled record change.
The paired native compiler report is validated by the normal build validator;
this tool never compiles, masks keys, or substitutes native code for Xenon code.
All derived game data stays in ignored build_native storage.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile

from compile_title_variants import SOURCE_CAPTURE_SELECTION, need, pack_cache, parse_cache
from configure_shader_inputs import IDENTITIES, configure
from inspect_pak import PakError, _no_links, inspect_pak, read_directory, read_entry
from prepare_native_character import private_output

ROOT = Path(__file__).resolve().parents[1]
CACHE_NAME = "data/art/dxshaders/precache_xenon.bsh"


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def read_private(path: Path, maximum: int) -> bytes:
    path = private_output(path)
    need(path.is_file() and 0 < path.stat().st_size <= maximum, "Missing or excessive private shader input")
    with path.open("rb") as stream:
        value = stream.read(maximum + 1)
    need(0 < len(value) <= maximum, "Private shader input grew beyond its byte budget")
    return value


def merge_cache(original: bytes, extension: bytes) -> tuple[bytes, dict]:
    stock = parse_cache(original, True)
    need(pack_cache(stock, True) == original, "Original Xenon cache does not byte-roundtrip")
    additions = parse_cache(extension, True)
    name, key = SOURCE_CAPTURE_SELECTION
    need(len(additions) == 1 and additions[0]["name"] == name and
         len(additions[0]["variants"]) == 1 and additions[0]["variants"][0]["key"] == key,
         "Shader precache overlay only accepts the measured original source name/key")
    merged = copy.deepcopy(stock)
    matches = [shader for shader in merged if shader["name"] == name]
    need(len(matches) == 1, "Source shader must belong to an existing stock shader master")
    destination = matches[0]
    addition = additions[0]["variants"][0]
    need(all(variant["key"] != key for variant in destination["variants"]),
         "Shader precache overlay would replace an original feature key")
    destination["variants"].append(copy.deepcopy(addition))
    result = pack_cache(merged, True)
    # Undo only the new record in the decoded result. Exact re-encoding must
    # reproduce every stock header/name/order/parameter/code byte, not just IDs.
    restored = parse_cache(result, True)
    for shader in restored:
        if shader["name"] == name:
            need(shader["variants"][-1] == addition, "Appended source shader record changed")
            shader["variants"].pop()
    need(pack_cache(restored, True) == original and result[:12] == original[:12],
         "Derived shader precache changed original content or top-level header")
    # Locate the only changed stock field and retain a raw-byte preservation
    # proof as well as the independent parsed roundtrip above.
    offset = 12
    for shader in stock:
        name_size = struct.unpack_from(">I", original, offset)[0]
        count_offset = offset + 4 + name_size
        offset = count_offset + 4
        for variant in shader["variants"]:
            offset += 20 + 16 * len(variant["parameters"]) + len(variant["code"])
        if shader["name"] == name:
            append_offset = offset
            break
    appended_bytes = len(result) - len(original)
    need(result[:count_offset] == original[:count_offset] and
         result[count_offset + 4:append_offset] == original[count_offset + 4:append_offset] and
         result[append_offset + appended_bytes:] == original[append_offset:] and
         struct.unpack_from(">I", result, count_offset)[0] == len(destination["variants"]),
         "Shader precache changed bytes outside its count field and appended record")
    return result, {
        "name": CACHE_NAME, "input_sha256": digest(original), "output_sha256": digest(result),
        "input_bytes": len(original), "output_bytes": len(result),
        "stock_shader_masters": len(stock),
        "stock_variants": sum(len(shader["variants"]) for shader in stock),
        "owned_variants_added": 1, "shader_name": name, "key": list(key),
        "original_header_unchanged": True, "stock_variant_bytes_unchanged": True,
        "variant_count_offset": count_offset, "append_offset": append_offset,
        "append_bytes": appended_bytes, "append_sha256": digest(result[append_offset:append_offset + appended_bytes]),
        "original_source_code_sha256": digest(addition["code"]),
        "original_parameters": addition["parameters"],
        "loader": "original precache loader82387AE0; original ready-variant getter82387758",
    }


def prepare(pak: Path, manifest: Path, output: Path) -> dict:
    manifest = private_output(manifest)
    report_bytes = read_private(manifest, 8 * 1024 * 1024)
    report = json.loads(report_bytes)
    need(isinstance(report, dict) and report.get("source_extension_variants") == 1 and
         report.get("source_capture_count") == 1, "Shader overlay requires one verified paired source capture")
    # Reuse the frozen baseline/identity/reflection/pair validator unchanged.
    private_output(ROOT / "build_native")
    with tempfile.TemporaryDirectory(prefix="shader_overlay_validation_", dir=ROOT / "build_native") as folder:
        configure(manifest, Path(folder) / "shader_inputs.h")
    need(read_private(manifest, 8 * 1024 * 1024) == report_bytes, "Shader compiler report changed during validation")
    extension_path = private_output(Path(report["source_extension_path"]))
    need(extension_path.parent == manifest.parent, "Source cache must remain beside its paired compiler report")
    extension = read_private(extension_path, 2 * 1024 * 1024)
    need(digest(extension) == report["source_extension_sha256"], "Validated source cache changed before overlay generation")
    _no_links(pak.absolute())
    package = inspect_pak(pak)
    need(package["sha256"] == IDENTITIES["package_sha256"], "Unsupported original package identity")
    entries = {entry.name: entry for entry in read_directory(pak)}
    original = read_entry(pak, entries[CACHE_NAME])
    need(digest(original) == IDENTITIES["xenon_cache_sha256"], "Original shader cache identity changed")
    content, metadata = merge_cache(original, extension)
    need(metadata["stock_shader_masters"] == 12 and metadata["stock_variants"] == 352,
         "Original shader cache inventory differs from its supported exact identity")
    need(inspect_pak(pak)["sha256"] == package["sha256"], "Original package changed during overlay generation")
    output = private_output(output)
    target = private_output(output.joinpath(*CACHE_NAME.split("/")))
    sidecar = private_output(output / "shader_overlay_manifest.json")
    target.parent.mkdir(parents=True, exist_ok=True)
    _no_links(target.parent)
    target.write_bytes(content)
    need(digest(read_private(target, 16 * 1024 * 1024)) == metadata["output_sha256"], "Shader overlay output hash failed")
    result = {
        "policy": "append genuine captured original code through the normal title precache loader; exact native/source pair retained",
        "package_sha256": package["sha256"], "overlay_root": str(output),
        "paired_shader_report_sha256": digest(report_bytes),
        "source_extension_sha256": digest(extension), "files": [metadata],
    }
    sidecar.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pak", type=Path, default=ROOT / "game/data.pak")
    parser.add_argument("--manifest", type=Path, required=True, help="Verified paired native/source compiler metadata in build_native")
    parser.add_argument("--output", type=Path, default=ROOT / "build_native/native_shader_overlay_candidate")
    args = parser.parse_args()
    try:
        result = prepare(args.pak, args.manifest, args.output)
        print(json.dumps({"manifest": str(Path(result["overlay_root"]) / "shader_overlay_manifest.json"),
                          "stock_variants_preserved": result["files"][0]["stock_variants"],
                          "source_variants_added": 1, "output_sha256": result["files"][0]["output_sha256"]}, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, PakError) as error:
        print("Native shader precache overlay rejected: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
