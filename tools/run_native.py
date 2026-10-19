"""Launch a native run and retain its exact logs and PID."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--leave-running", action="store_true",
                        help="Leave the exact game process alive when the observation duration ends")
    parser.add_argument("--executable", type=Path, default=ROOT / "build_native/native/Release/DoritosNative.exe")
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not 0 < args.seconds <= 600:
        parser.error("Diagnostic duration must be between 0 and 600 seconds")
    folder = ROOT / "build_native/run" / datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    folder.mkdir(parents=True, exist_ok=False)
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    command = [str(args.executable.resolve()), "--game-dir", str(ROOT / "game"), *arguments]
    start = time.perf_counter()
    with (folder / "runtime.log").open("wb") as stderr, (folder / "stdout.log").open("wb") as stdout:
        process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        print(json.dumps({"state": "running", "pid": process.pid, "command": command,
                          "log": str(folder / "runtime.log")}), flush=True)
        timed_out = False
        try:
            process.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            if not args.leave_running:
                # Popen retains the exact process handle; never use a name/wildcard kill.
                process.kill()
                process.wait(timeout=10)
    alive = process.poll() is None
    report = {"pid": process.pid, "exit_code": process.returncode, "timed_out": timed_out,
              "state": "running" if alive else "exited", "left_running": alive,
              "seconds": round(time.perf_counter() - start, 3), "command": command,
              "log": str(folder / "runtime.log"), "gameplay_verified": False}
    (folder / "run.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    log = (folder / "runtime.log").read_text(encoding="utf-8", errors="replace").splitlines()
    for line in log[-18:]:
        print(line, file=sys.stderr)
    return 0 if alive else (124 if timed_out else process.returncode)


if __name__ == "__main__":
    sys.exit(main())
