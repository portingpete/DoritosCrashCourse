"""Audit project build inputs and the native executable's DLL dependencies."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def audit_sources() -> list[str]:
    failures = []
    patterns = (
        r'#\s*include\s*[<"](?:rex/|xenia/)',
        r'\b(?:rexglue_setup_target|REX_DEFINE_APP|PPC_EXTERN_IMPORT)\s*\(',
        r'\b(?:rex::|xe::cpu::)',
        r'find_package\s*\(\s*(?:rexglue|xenia)',
    )
    paths = [ROOT / "CMakeLists.txt"]
    for folder in ("app", "runtime", "renderer"):
        paths += sorted((ROOT / folder).rglob("*.cpp")) + sorted((ROOT / folder).rglob("*.h"))
    for path in paths:
        data = path.read_text(encoding="utf-8")
        for pattern in patterns:
            if re.search(pattern, data, re.I):
                failures.append(f"Disallowed runtime dependency in {path.relative_to(ROOT)}")
    return failures


def audit_executable(path: Path, llvm_readobj: Path) -> list[str]:
    result = subprocess.run([str(llvm_readobj), "--file-headers", "--coff-imports", str(path)],
                            check=True, capture_output=True, text=True)
    if "IMAGE_FILE_MACHINE_AMD64" not in result.stdout:
        raise RuntimeError("Executable is not native Windows x64")
    dependencies = re.findall(r"Name:\s*([^\s]+\.dll)", result.stdout, re.I)
    if not dependencies:
        raise RuntimeError("Executable dependency inventory is empty")
    forbidden = [name for name in dependencies if re.search(r"rexglue|xenia", name, re.I)]
    if forbidden:
        raise RuntimeError(f"Disallowed runtime DLL dependencies: {forbidden}")
    return sorted(set(dependencies), key=str.lower)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--llvm-readobj", type=Path,
                        default=Path(r"C:\Program Files\LLVM\bin\llvm-readobj.exe"))
    args = parser.parse_args()
    failures = audit_sources()
    if failures:
        raise RuntimeError("; ".join(failures))
    dependencies = audit_executable(args.executable, args.llvm_readobj) if args.executable else []
    print(json.dumps({"ok": True, "runtime_sdk": "native Win32", "cpu_execution": "compiled AOT C++",
                      "source_dependency_scan": "passed", "dll_dependencies": dependencies,
                      "gameplay_verified": False}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Native audit failed: {error}", file=sys.stderr)
        sys.exit(1)
