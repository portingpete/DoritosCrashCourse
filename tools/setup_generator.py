"""Fetch pinned XenonRecomp and apply the audited Darkness generator fixes."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "third_party/XenonRecomp"
UPSTREAM = "https://github.com/hedge-dev/XenonRecomp.git"
COMMIT = "c5bfd90d87f2ed0db8cff5c19ea3aff0e161e527"
PATCHES = tuple(ROOT / "tools/patches" / name for name in (
    "xenonrecomp-darkness.patch", "xenonrecomp-corrections.patch", "xenonrecomp-doritos.patch",
    "xenonrecomp-switch-order.patch", "xenonrecomp-switch-bounds.patch", "xenonrecomp-lfdux.patch"))


def git(directory: Path, *arguments: str, check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(["git", "-C", str(directory), *arguments],
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and result.returncode:
        raise RuntimeError(f"git {' '.join(arguments)} failed:\n{result.stderr.strip()}")
    return result


def install_patches(destination: Path) -> None:
    names = set()
    for patch in PATCHES:
        for entry in filter(None, git(destination, "apply", "--numstat", "-z", str(patch)).stdout.split("\0")):
            added, removed, name = entry.split("\t", 2)
            if added == "-" or removed == "-" or not (destination / name).resolve().is_relative_to(destination):
                raise RuntimeError(f"Unsupported generator patch path: {name}")
            names.add(name)
    with tempfile.TemporaryDirectory(prefix="doritos-generator-") as temporary:
        staging = Path(temporary)
        versions = {}
        for name in sorted(names):
            original = git(destination, "show", f"{COMMIT}:{name}").stdout.encode("utf-8")
            path = staging / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(original)
            versions[name] = {original}
        for patch in PATCHES:
            # Scratch files are never made into a Git checkout/worktree.
            git(staging, "-c", "core.autocrlf=false", "apply", "--no-index", "--whitespace=nowarn", str(patch))
            for name in names:
                versions[name].add((staging / name).read_bytes())
        updates = []
        for name in sorted(names):
            path = destination / name
            current = path.read_bytes()
            normalized = current.replace(b"\r\n", b"\n")
            desired = (staging / name).read_bytes()
            if normalized not in versions[name]:
                raise RuntimeError(f"Local generator edits conflict with patches: {path}; your edits are preserved")
            if normalized != desired:
                updates.append((path, desired.replace(b"\n", b"\r\n") if b"\r\n" in current else desired))
        for path, content in updates:
            path.write_bytes(content)
        print("Installed generator patches." if updates else "Generator patches verified.")


def setup(destination: Path, cache: Path | None = None) -> None:
    destination = destination.resolve()
    for patch in PATCHES:
        if not patch.is_file():
            raise RuntimeError(f"Missing patch: {patch}")
    if not destination.exists() or not any(destination.iterdir()):
        destination.parent.mkdir(parents=True, exist_ok=True)
        source = str(cache.resolve()) if cache else UPSTREAM
        git(destination.parent, "clone", "--no-checkout", "--no-hardlinks", "--", source, str(destination))
        git(destination, "remote", "set-url", "origin", UPSTREAM)
        git(destination, "checkout", "--detach", COMMIT)
    actual = git(destination, "rev-parse", "--show-toplevel", check=False)
    if actual.returncode or Path(actual.stdout.strip()).resolve() != destination:
        raise RuntimeError(f"Expected a separate XenonRecomp checkout: {destination}")
    head = git(destination, "rev-parse", "HEAD").stdout.strip()
    if head != COMMIT:
        raise RuntimeError(f"Generator revision {head} differs from pinned {COMMIT}; checkout preserved")
    install_patches(destination)
    git(destination, "submodule", "update", "--init", "--recursive")
    print(f"Pinned XenonRecomp ready: {destination}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=DESTINATION)
    parser.add_argument("--source-cache", type=Path, help="Optional local Git cache; network setup remains supported")
    args = parser.parse_args()
    try:
        setup(args.destination, args.source_cache)
    except (OSError, RuntimeError) as exc:
        print(f"Generator setup: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
