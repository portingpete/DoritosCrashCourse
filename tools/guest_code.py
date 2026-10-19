"""Inspect local AOT assembly annotations without saving proprietary source."""
from __future__ import annotations
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "build_native/generated"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address", type=lambda value: int(value, 16))
    parser.add_argument("--limit", type=int, default=180)
    args = parser.parse_args()
    mapping = (GENERATED / "ppc_func_mapping.cpp").read_text()
    entries = [(int(address, 16), name) for address, name in re.findall(r"\{ 0x([0-9A-Fa-f]+), (\w+) \}", mapping)]
    candidates = [entry for entry in entries if entry[0] <= args.address]
    if not candidates:
        parser.error("Address precedes every compiled entry")
    address, symbol = max(candidates)
    if symbol.startswith("__imp__"):
        print(f"0x{address:08X}: imported boundary {symbol}")
        return 0
    pattern = f"PPC_FUNC_IMPL(__imp__{symbol})"
    result = subprocess.run(["rg", "-l", "-F", pattern, str(GENERATED), "-g", "ppc_recomp.*.cpp"],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        parser.error(f"No local generated function for {symbol}")
    path = Path(result.stdout.splitlines()[0])
    data = path.read_text()
    begin = data.index(pattern)
    end = data.index("PPC_WEAK_FUNC", begin)
    comments = [line.strip()[3:] for line in data[begin:end].splitlines() if line.strip().startswith("// ")]
    print(f"0x{address:08X} {symbol}: {len(comments)} instruction annotations in {path.name}")
    for index, instruction in enumerate(comments[:args.limit]):
        print(f"  {index:4}: {instruction}")
    if len(comments) > args.limit:
        print(f"  ... {len(comments) - args.limit} more annotations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
