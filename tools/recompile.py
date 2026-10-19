"""Translate the verified encrypted original XEX; refuse stale or incomplete ordinary builds."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tomllib
from native_imports import generate as generate_imports
from recover_offset_switches import recover as recover_offset_switches

ROOT = Path(__file__).resolve().parents[1]
XENON = ROOT / "third_party/XenonRecomp"
SOURCE_SHA256 = "6c4fa5a12ef2ad51db4809d5d654b487300a1ace06c2cdf6cb14258fd273dfc1"


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_source() -> None:
    path = ROOT / "game/default.xex"
    data = path.read_bytes()
    if data[:4] != b"XEX2" or len(data) < 24:
        raise RuntimeError("Original executable is not a valid XEX2 file")
    count = struct.unpack_from(">I", data, 20)[0]
    if 24 + count * 8 > len(data):
        raise RuntimeError("Truncated optional XEX headers")
    options = dict(struct.iter_unpack(">II", data[24:24 + count * 8]))
    offset = options.get(0x3FF, -1)
    if offset < 24 or offset + 8 > len(data):
        raise RuntimeError("Missing XEX file-format information")
    encryption = struct.unpack_from(">H", data, offset + 4)[0]
    if encryption != 1:
        raise RuntimeError("STOP: default.xex is not the regular encrypted original; restore your original dump")
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise RuntimeError("Original encrypted XEX differs from the verified Doritos revision")


def sources() -> list[Path]:
    files = [ROOT / name for name in (
        "config/doritos.toml", "config/switch_tables.toml", "runtime/guest/ppc_context.template.h",
        "game/default.xex", "tools/native_imports.py", "tools/recompile.py", "tools/setup_generator.py",
        "tools/recover_offset_switches.py")]
    files += sorted((ROOT / "tools/patches").glob("*.patch"))
    for directory in ("XenonRecomp", "XenonAnalyse", "XenonUtils"):
        for suffix in ("*.cpp", "*.h"):
            files += sorted((XENON / directory).glob(suffix))
    files += [XENON / f"XenonUtils/xbox/{name}_table.inc" for name in ("xboxkrnl", "xam")]
    return files


def verify_control_flow(path: Path) -> None:
    """An unbound C++ label is invalid generated code, even in diagnostic builds."""
    text = path.read_text()
    starts = list(re.finditer(r"^(?:PPC_FUNC_IMPL|PPC_WEAK_FUNC|PPC_FUNC)\((\w+)\)\s*\{", text, re.MULTILINE))
    for index, match in enumerate(starts):
        body = text[match.end():starts[index + 1].start() if index + 1 < len(starts) else len(text)]
        references = set(re.findall(r"\bgoto\s+(loc_[0-9A-Fa-f]+)\s*;", body))
        definitions = set(re.findall(r"^\s*(loc_[0-9A-Fa-f]+)\s*:", body, re.MULTILINE))
        missing = references - definitions
        if missing:
            raise RuntimeError(f"Invalid generated control flow in {path.name}:{match[1]}: undefined labels {sorted(missing)}")


def verify(out: Path, allow_incomplete: bool = False) -> int:
    validate_source()
    manifest = json.loads((out / "manifest.json").read_text())
    if manifest.get("format") != 1 or manifest.get("runtime_verified") is not False:
        raise RuntimeError("Unknown AOT manifest format or invalid runtime claim")
    diagnostics = manifest["semantic_diagnostics"]
    if not isinstance(diagnostics, int) or diagnostics < 0 or manifest["profile"] != (
            "diagnostic" if diagnostics else "complete-translation"):
        raise RuntimeError("AOT manifest profile disagrees with semantic accounting")
    if set(manifest["inputs"]) != {p.relative_to(ROOT).as_posix() for p in sources()}:
        raise RuntimeError("AOT source inventory changed; regenerate")
    for group in ("inputs", "outputs"):
        origin = ROOT if group == "inputs" else out
        for name, expected in manifest[group].items():
            path = (origin / name).resolve()
            if not path.is_relative_to(origin.resolve()) or not path.is_file() or sha256(path) != expected:
                raise RuntimeError(f"Changed AOT {group}: {name}; regenerate")
    generator = (ROOT / manifest["generator_path"]).resolve()
    if not generator.is_relative_to(XENON) or sha256(generator) != manifest["generator_sha256"]:
        raise RuntimeError("Generator binary changed; regenerate")
    if manifest["semantic_diagnostics"] and not allow_incomplete:
        raise RuntimeError(f'{manifest["semantic_diagnostics"]} unresolved semantic diagnostics; ordinary build refused')
    print(f'AOT verified: {len(manifest["outputs"])} files, {manifest["semantic_diagnostics"]} semantic diagnostics; runtime remains unverified')
    return 0


def toml_value(value) -> str:
    if isinstance(value, dict):
        return "{ " + ", ".join(f"{key} = {toml_value(item)}" for key, item in value.items()) + " }"
    if isinstance(value, list):
        return "[" + ", ".join(toml_value(item) for item in value) + "]"
    return json.dumps(value)


def analyse(out: Path, executable: Path | None, metadata: Path, image: Path) -> tuple[dict, Path]:
    source = json.loads(metadata.read_text())
    if source["xex_sha256"] != SOURCE_SHA256 or source["image_sha256"] != sha256(image):
        raise RuntimeError("Helper analysis image is not the verified decoded executable")
    data = image.read_bytes()
    word = lambda n: struct.pack(">I", n)
    patterns = {}
    for name, instruction, tail in (
        ("savegprlr_14", 0xF8010000, (0x9181FFF8, 0x4E800020)),
        ("restgprlr_14", 0xE8010000, (0x8181FFF8, 0x7D8803A6, 0x4E800020)),
        ("savefpr_14", 0xD80C0000, (0x4E800020,)),
        ("restfpr_14", 0xC80C0000, (0x4E800020,))):
        offset = -0x98 if "gpr" in name else -0x90
        patterns[name] = b"".join(word(instruction | (reg << 21) | ((offset + (reg - 14) * 8) & 0xFFFF))
                                  for reg in range(14, 32)) + b"".join(map(word, tail))
    for name, first, last, instruction in (
        ("savevmx_14", 14, 32, 0x7C0B61CE), ("restvmx_14", 14, 32, 0x7C0B60CE),
        ("savevmx_64", 64, 128, 0x100B61C3), ("restvmx_64", 64, 128, 0x100B60C3)):
        offset = -0x120 if first == 14 else -0x400
        parts = []
        for reg in range(first, last):
            parts.append(word(0x39600000 | ((offset + (reg - first) * 16) & 0xFFFF)))
            extension = ((reg >> 5) & 3) << 2 if first == 64 else 0
            parts.append(word(instruction | ((reg & 31) << 21) | extension))
        patterns[name] = b"".join(parts) + word(0x4E800020)
    helpers = {}
    for name, pattern in patterns.items():
        start = data.find(pattern)
        if start < 0 or start % 4 or data.find(pattern, start + 4) >= 0:
            raise RuntimeError(f"Helper {name} is absent, misaligned or ambiguous")
        helpers[name + "_address"] = source["image_base"] + start
    (out / "helper_addresses.json").write_text(json.dumps(helpers, indent=2) + "\n")
    checked = tomllib.loads((ROOT / "config/switch_tables.toml").read_text()).get("switch", [])
    recovered = []
    if executable:
        path = out / "analysed_switch_tables.toml"
        result = subprocess.run([str(executable), str(ROOT / "game/default.xex"), str(path)], cwd=ROOT)
        if result.returncode:
            raise RuntimeError(f"Switch analysis failed ({result.returncode})")
        recovered = tomllib.loads(path.read_text()).get("switch", [])
    # Explicit title corrections take precedence over fresh automatic analysis.
    tables = {entry["base"]: entry for entry in recovered}
    tables.update({entry["base"]: entry for entry in recover_offset_switches(data, source["image_base"])})
    tables.update({entry["base"]: entry for entry in checked})
    path = out / "switch_tables.toml"
    path.write_text("# Verified image analysis plus explicit title corrections\n" + "".join(
        "\n[[switch]]\n" + "\n".join(f"{key} = {toml_value(value)}" for key, value in entry.items()) + "\n"
        for _, entry in sorted(tables.items())))
    print(f"Verified {len(helpers)} register helpers; {len(tables)} explicit/recovered switch tables")
    return helpers, path


def write_config(out: Path, helpers: dict, switches: Path) -> Path:
    source = ROOT / "config/doritos.toml"
    config = tomllib.loads(source.read_text())
    main = config["main"]
    for name, address in helpers.items():
        if name in main and main[name] != address:
            raise RuntimeError(f"Configured helper {name} differs from verified instruction sequence")
    main.update(helpers)
    for key in ("file_path", "switch_table_file_path"):
        main[key] = os.path.relpath((source.parent / main[key]).resolve(), out).replace("\\", "/")
    main["out_directory_path"] = "."
    main["switch_table_file_path"] = switches.name
    text = "[main]\n" + "\n".join(f"{key} = {toml_value(value)}" for key, value in main.items()) + "\n"
    for hook in config.get("midasm_hook", []):
        text += "\n[[midasm_hook]]\n" + "\n".join(f"{key} = {toml_value(value)}" for key, value in hook.items()) + "\n"
    path = out / "generation.toml"
    path.write_text(text)
    return path


def regenerate(out: Path, executable: Path, metadata: Path, image: Path, allow_incomplete: bool,
               analyser: Path | None = None) -> int:
    validate_source()
    out.mkdir(parents=True, exist_ok=True)
    previous = {p.name: (p.read_bytes(), p.stat().st_atime_ns, p.stat().st_mtime_ns)
                for p in out.glob("ppc_*") if p.is_file()}
    helpers, switches = analyse(out, analyser, metadata, image)
    run_config = write_config(out, helpers, switches)
    log_path = out / "generation.log"
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run([str(executable), str(run_config), str(ROOT / "runtime/guest/ppc_context.template.h")],
                                stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    log = log_path.read_text(encoding="utf-8")
    diagnostics = re.findall(r"Semantic diagnostics: (\d+)", log)
    units = re.findall(r"^Translation units: (\d+)\s*$", log, re.MULTILINE)
    if result.returncode not in (0, 1) or not diagnostics or not units:
        raise RuntimeError(f"Generator failed or lacks semantic accounting ({result.returncode}); see {log_path}")
    diagnostic_count = int(diagnostics[-1])
    if result.returncode != bool(diagnostic_count):
        raise RuntimeError("Generator exit status disagrees with semantic accounting")
    native = generate_imports(ROOT, out, metadata, image)
    chunks = [f"ppc_recomp.{index}.cpp" for index in range(int(units[-1]))]
    names = chunks + ["ppc_func_mapping.cpp", "ppc_imports.cpp"]
    generated = [out / name for name in names] + sorted(out.glob("ppc_*.h"))
    for path in generated:
        if re.search(r"// ERROR(?:\s|:)", path.read_text()):
            raise RuntimeError(f"Silent code-generation failure: {path}")
        if path.suffix == ".cpp":
            verify_control_flow(path)
    source_list = out / "sources.cmake"
    source_list.write_text("set(DORITOS_PPC_SOURCES\n" + "".join(
        f'  "${{DORITOS_GENERATED_DIR}}/{name}"\n' for name in names) + ")\n")
    manifest = {"format": 1, "profile": "diagnostic" if diagnostic_count else "complete-translation",
                "runtime_verified": False, "semantic_diagnostics": diagnostic_count,
                "native_imports": native,
                "generator_path": executable.relative_to(ROOT).as_posix(), "generator_sha256": sha256(executable),
                "inputs": {p.relative_to(ROOT).as_posix(): sha256(p) for p in sources()},
                "outputs": {p.name: sha256(p) for p in [*generated, source_list, run_config, switches,
                                                       out / "helper_addresses.json"]}}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for path in generated:
        old = previous.get(path.name)
        if old and old[0] == path.read_bytes():
            os.utime(path, ns=(old[1], old[2]))
    return verify(out, allow_incomplete)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generator", type=Path)
    parser.add_argument("--analyser", type=Path, help="Pinned XenonAnalyse executable for automatic switch analysis")
    parser.add_argument("--metadata", type=Path, default=ROOT / "build_native/game_image.json")
    parser.add_argument("--image", type=Path, default=ROOT / "build_native/game_image.bin")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--allow-incomplete", action="store_true")
    args = parser.parse_args()
    if args.verify:
        return verify(args.output.resolve(), args.allow_incomplete)
    if not args.generator:
        parser.error("--generator is required when regenerating")
    return regenerate(args.output.resolve(), args.generator.resolve(), args.metadata.resolve(), args.image.resolve(),
                      args.allow_incomplete, args.analyser.resolve() if args.analyser else None)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, KeyError, ValueError, RuntimeError) as exc:
        print(f"AOT gate: {exc}", file=sys.stderr)
        sys.exit(1)
