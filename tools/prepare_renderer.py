"""Prepare verified original shader inputs for the native title renderer."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath

from inspect_pak import PakError, SHADER_ROOT, _no_links, inspect_pak, read_directory, read_entry
from inspect_xex import inspect_xex

PACKAGE_SHA256 = "0819b4e9f90695b70e083690e98ac6f76f0df0cf2bbb817f196316c930a54fc3"
ROOT = Path(__file__).resolve().parents[1]


def prepare(game: Path, destination: Path) -> dict:
    inspect_xex(game / "default.xex", require_supported=True)
    package = game / "data.pak"
    initial = inspect_pak(package)
    if initial["sha256"] != PACKAGE_SHA256:
        raise PakError("Native renderer requires the supported original data.pak")
    # Decode and validate every input before touching outputs. Existing derived
    # inputs are reusable only when they still match the user's original bytes.
    decoded = [(entry.name, read_entry(package, entry, max_output=4 * 1024 * 1024))
               for entry in read_directory(package) if entry.name.startswith(SHADER_ROOT)]
    if inspect_pak(package) != initial:
        raise PakError("Original package changed while preparing renderer inputs")
    destination = destination.absolute()
    _no_links(destination)
    targets = []
    for name, content in decoded:
        target = destination.joinpath(*PurePosixPath(name).parts)
        _no_links(target)
        if target.exists() and (target.stat().st_size != len(content) or target.read_bytes() != content):
            raise PakError(f"Existing native renderer input differs from the original: {name}")
        targets.append((target, content))
    for target, content in targets:
        if target.exists():
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        _no_links(target)
        with target.open("xb") as output:
            output.write(content)
    return {"package_sha256": PACKAGE_SHA256, "count": len(decoded),
            "destination": str(destination),
            "inputs": [{"name": name, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()}
                       for name, content in decoded]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, default=ROOT / "game")
    parser.add_argument("--destination", type=Path, default=ROOT / "build_native/renderer_inputs")
    args = parser.parse_args()
    try:
        report = prepare(args.game_dir, args.destination)
    except (OSError, ValueError) as error:
        parser.exit(2, f"Renderer preparation rejected: {error}\n")
    print(json.dumps({key: value for key, value in report.items() if key != "inputs"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
