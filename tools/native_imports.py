"""Bind translated XEX imports to explicit native dispatch, preserving their identities."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def generate(root: Path, out: Path, metadata_path: Path, image_path: Path) -> dict:
    metadata = json.loads(metadata_path.read_text())
    if metadata["xex_sha256"] != sha256(root / "game/default.xex"):
        raise RuntimeError("XEX metadata does not describe the original source executable")
    if metadata["image_sha256"] != sha256(image_path):
        raise RuntimeError("Decoded image differs from XEX metadata")
    shared = (out / "ppc_recomp_shared.h").read_text()
    required = set(re.findall(r"PPC_EXTERN_FUNC\((__imp__\w+)\)", shared))
    imports = {}
    for entry in metadata["imports"]:
        if entry["kind"] != 1:
            continue
        symbol = "__imp__" + entry["name"]
        if symbol in imports and imports[symbol]["ordinal"] != entry["ordinal"]:
            raise RuntimeError(f"Ambiguous native import: {symbol}")
        imports[symbol] = entry
    missing = required - imports.keys()
    if missing:
        raise RuntimeError(f"Translated imports absent from source metadata: {sorted(missing)}")
    text = ('#include "ppc_context.h"\n'
            'void DoritosNativeImport(PPCContext&, uint8_t*, const char*, const char*, uint32_t);\n')
    for symbol in sorted(required):
        entry = imports[symbol]
        text += (f'PPC_FUNC({symbol}) {{ DoritosNativeImport(ctx, base, '
                 f'{json.dumps(entry["library"])}, {json.dumps(entry["name"])}, {entry["ordinal"]}u); }}\n')
    (out / "ppc_imports.cpp").write_text(text)
    callable_keys = {(entry["library"], entry["ordinal"]) for entry in metadata["imports"] if entry["kind"] == 1}
    data = [entry for entry in metadata["imports"] if entry["kind"] == 0
            and (entry["library"], entry["ordinal"]) not in callable_keys]
    header = '#pragma once\n#include <cstdint>\n'
    header += f'inline constexpr char kGameXexSha256[] = "{metadata["xex_sha256"]}";\n'
    header += f'inline constexpr char kGameImageSha256[] = "{metadata["image_sha256"]}";\n'
    header += f'inline constexpr uint32_t kGameImageBase = 0x{metadata["image_base"]:08X}u;\n'
    header += f'inline constexpr uint32_t kGameImageSize = {metadata["image_size"]}u;\n'
    header += f'inline constexpr uint32_t kGameEntryPoint = 0x{metadata["entry_point"]:08X}u;\n'
    header += 'struct GuestDataImport { uint32_t address; const char* library; const char* name; uint32_t ordinal; };\n'
    header += 'inline constexpr GuestDataImport kDataImports[] = {\n'
    for entry in data:
        header += (f'  {{0x{entry["address"]:08X}u, {json.dumps(entry["library"])}, '
                   f'{json.dumps(entry["name"])}, {entry["ordinal"]}u}},\n')
    header += '};\n'
    (out / "ppc_image_metadata.h").write_text(header)
    return {"function_imports": len(required), "data_imports": len(data),
            "runtime_verified": False}
