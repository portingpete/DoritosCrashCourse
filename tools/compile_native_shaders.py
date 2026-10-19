#!/usr/bin/env python3
"""Check original package HLSL using the native Windows D3DCompile helper.

This is a compiler milestone, not a renderer: layouts, engine-selected variants,
constant uploads, texture formats and real draw hooks are not inferred here.
Extracted source and compiled bytecode stay in the ignored build_native tree.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import uuid

try:
    from .inspect_pak import SHADER_ROOT, PakError, _no_links, inspect_pak, read_directory, read_entry, safe_name
    from .inspect_xex import XexError, inspect_xex
except ImportError:
    from inspect_pak import SHADER_ROOT, PakError, _no_links, inspect_pak, read_directory, read_entry, safe_name
    from inspect_xex import XexError, inspect_xex

PROJECT = Path(__file__).absolute().parents[1]
MAX_SHADER = 4 * 1024 * 1024
# These functions and stage signatures were checked directly in original HLSL.
# SM3 is an explicit native compilation target, corroborated by the package's
# precache_win32_3.bsh target strings; per-entry original cache records are unknown.
CHECKS = [
    ("anaglyph.sha", "vsRenderToFullScreen", "vs_3_0"),
    ("anaglyph.sha", "psCompose", "ps_3_0"),
    ("tdbloom.sha", "vsRenderToFullScreen", "vs_3_0"),
    ("tdbloom.sha", "psBrightFilter", "ps_3_0"),
    ("tdbloom.sha", "psScale", "ps_3_0"),
    ("tdbloom.sha", "psCompose", "ps_3_0"),
]


def private_path(path: Path) -> Path:
    if ".." in path.parts:
        raise PakError("Native shader artifact path contains parent traversal")
    path = path.absolute()
    _no_links(path)
    if not path.is_relative_to(PROJECT / "build_native"):
        raise PakError("Native shader artifacts must remain inside the ignored build_native directory")
    return path


def prepare_sources(pak: Path, asset_root: Path, inventory: dict) -> tuple[dict[str, Path], list[dict]]:
    """Extract checked sources and their observed includes without overwriting."""
    entries = {entry.name.casefold(): entry for entry in read_directory(pak)}
    records = {record["name"].casefold(): record for record in inventory["shaders"]}
    pending = [SHADER_ROOT + name for name, _, _ in CHECKS]
    contents: dict[str, bytes] = {}
    while pending:
        requested = pending.pop()
        if requested.casefold() in contents:
            continue
        entry = entries.get(requested.casefold())
        if not entry or not entry.name.startswith(SHADER_ROOT):
            raise PakError(f"Original shader dependency is missing: {requested}")
        metadata = records.get(entry.name.casefold())
        if not metadata or metadata["content_kind"] != "text_shader_source":
            raise PakError(f"Dependency is not verified original HLSL: {entry.name}")
        content = read_entry(pak, entry, max_output=MAX_SHADER)
        if hashlib.sha256(content).hexdigest() != metadata["sha256"]:
            raise PakError(f"Original shader changed after inventory: {entry.name}")
        contents[entry.name.casefold()] = content
        text = content.decode("utf-8")
        for include in re.findall(r'^\s*#\s*include\s*[<"]([^">]+)[">]', text, re.M):
            include = safe_name(include)
            dependency = str(PurePosixPath(entry.name).parent / include)
            if not dependency.startswith(SHADER_ROOT):
                raise PakError("Shader include escapes the original shader directory")
            pending.append(dependency)
    paths = {}
    extracted = []
    for key, content in sorted(contents.items()):
        entry = entries[key]
        target = asset_root.joinpath(*PurePosixPath(safe_name(entry.name)).parts)
        _no_links(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        _no_links(target)
        if target.exists():
            if not target.is_file() or target.stat().st_size != len(content) or target.read_bytes() != content:
                raise PakError(f"Refusing to replace existing shader input: {target}")
        else:
            with target.open("xb") as stream:
                stream.write(content)
        paths[entry.name] = target
        extracted.append({"name": entry.name, "sha256": hashlib.sha256(content).hexdigest(),
                          "decoded_bytes": len(content), "path": str(target)})
    return paths, extracted


def compile_checks(compiler: Path, paths: dict[str, Path], inventory: dict, output: Path) -> list[dict]:
    metadata = {record["name"]: record for record in inventory["shaders"]}
    results = []
    for source_name, entry, profile in CHECKS:
        name = SHADER_ROOT + source_name
        if entry not in metadata[name].get("entry_candidates", []):
            raise PakError(f"Entry is absent from original shader inventory: {name}:{entry}")
        bytecode = output / f"{Path(source_name).stem}.{entry}.{profile}.bin"
        command = [str(compiler), str(paths[name]), entry, profile, str(bytecode)]
        process = subprocess.run(command, cwd=paths[name].parent, text=True, encoding="utf-8",
                                 errors="replace", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        record = {"source": name, "entry": entry, "profile": profile, "defines": {},
                  "source_sha256": metadata[name]["sha256"], "exit_code": process.returncode,
                  "status": "compiled" if process.returncode == 0 else "failed",
                  "diagnostics": process.stderr[:MAX_SHADER]}
        if process.returncode == 0:
            result = json.loads(process.stdout)
            data = bytecode.read_bytes()
            expected_token = 0xFFFE0300 if profile.startswith("vs") else 0xFFFF0300
            if len(data) < 8 or int.from_bytes(data[:4], "little") != expected_token or result["compiled_bytes"] != len(data):
                raise PakError("Compiler output is not the requested native SM3 bytecode")
            record.update(result)
            record.update({"bytecode_path": str(bytecode), "bytecode_sha256": hashlib.sha256(data).hexdigest()})
        else:
            record["compiler_output"] = process.stdout[:MAX_SHADER]
        results.append(record)
    return results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True, help="Built NativeShaderCompile.exe")
    parser.add_argument("--pak", type=Path, default=PROJECT / "game/data.pak")
    parser.add_argument("--xex", type=Path, default=PROJECT / "game/default.xex")
    parser.add_argument("--inventory", type=Path, default=PROJECT / "build_native/renderer_inventory.json")
    parser.add_argument("--assets", type=Path, default=PROJECT / "build_native/native_assets")
    parser.add_argument("--output-directory", type=Path, default=PROJECT / "build_native/native_shaders")
    parser.add_argument("--report", type=Path, default=PROJECT / "build_native/native_shader_report.json")
    args = parser.parse_args(argv)
    try:
        # Apply the user's encrypted-XEX prerequisite before touching derived files.
        xex = inspect_xex(args.xex, require_supported=True)
        inventory = json.loads(args.inventory.read_text(encoding="utf-8"))
        package = inspect_pak(args.pak)
        if package["sha256"] != inventory["package"]["sha256"]:
            raise PakError("Package changed after shader inventory")
        compiler = args.compiler.absolute()
        if not compiler.is_file():
            raise PakError(f"Native shader compiler is missing: {compiler}")
        assets = private_path(args.assets)
        output_root = private_path(args.output_directory)
        report = private_path(args.report)
        paths, extracted = prepare_sources(args.pak, assets, inventory)
        output = output_root / uuid.uuid4().hex
        output.mkdir(parents=True, exist_ok=False)
        _no_links(output)
        results = compile_checks(compiler, paths, inventory, output)
        result = {"compiler": "Windows D3DCompile", "source_xex_sha256": xex["sha256"],
                  "package_sha256": package["sha256"], "extracted_inputs": extracted,
                  "compiled_count": sum(item["status"] == "compiled" for item in results),
                  "failed_count": sum(item["status"] == "failed" for item in results), "results": results,
                  "profile_evidence": "Requested native SM3 check; original Win32 cache contains vs_3_0/ps_3_0 strings. Exact cache-entry matches are unverified.",
                  "limitations": ["No engine-selected feature defines are claimed; these checks use the original default branches.",
                                  "Compiled shaders alone do not implement game rendering or verify resource bindings.",
                                  "Draw hooks, constant uploads, textures and vertex declarations still require verification."]}
        report.parent.mkdir(parents=True, exist_ok=True)
        _no_links(report)
        report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"compiled_count": result["compiled_count"], "failed_count": result["failed_count"], "report": str(report)}))
        return 0 if result["failed_count"] == 0 else 3
    except (OSError, ValueError, PakError, XexError, subprocess.TimeoutExpired) as error:
        print(f"Native shader check stopped: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
