"""Request one real GPU capture from an opt-in, exact native title process."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def request(pid: int, created: int, executable: Path) -> dict:
    if os.name != "nt" or pid <= 0 or not 0 < created <= 0xFFFFFFFFFFFFFFFF:
        raise RuntimeError("Capture requires Windows and a positive PID/creation identity")
    expected = os.path.normcase(str(executable.resolve(strict=True)))
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "OpenProcess": (wintypes.HANDLE, [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]),
        "CloseHandle": (wintypes.BOOL, [wintypes.HANDLE]),
        "WaitForSingleObject": (wintypes.DWORD, [wintypes.HANDLE, wintypes.DWORD]),
        "QueryFullProcessImageNameW": (wintypes.BOOL, [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                     ctypes.POINTER(wintypes.DWORD)]),
        "GetProcessTimes": (wintypes.BOOL, [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4),
        "OpenEventW": (wintypes.HANDLE, [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]),
        "SetEvent": (wintypes.BOOL, [wintypes.HANDLE]),
    }
    for name, (result, arguments) in signatures.items():
        method = getattr(api, name)
        method.restype, method.argtypes = result, arguments
    process = api.OpenProcess(0x00101000, False, pid)  # SYNCHRONIZE | QUERY_LIMITED_INFORMATION.
    if not process:
        raise RuntimeError(f"Cannot query exact process {pid}")
    event = None
    try:
        def verify():
            if api.WaitForSingleObject(process, 0) != 0x102:
                raise RuntimeError("Exact capture process has exited or cannot be queried")
            image, length = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
            if not api.QueryFullProcessImageNameW(process, 0, image, ctypes.byref(length)):
                raise RuntimeError("Cannot read capture process executable identity")
            times = [wintypes.FILETIME() for _ in range(4)]
            if not api.GetProcessTimes(process, *(ctypes.byref(value) for value in times)):
                raise RuntimeError("Cannot read capture process creation identity")
            actual_created = times[0].dwLowDateTime | (times[0].dwHighDateTime << 32)
            if os.path.normcase(str(Path(image.value).resolve(strict=True))) != expected or actual_created != created:
                raise RuntimeError("Capture PID executable or creation identity does not match")
        verify()
        name = f"Local\\DoritosNativeCapture-{pid}-{created}"
        event = api.OpenEventW(0x0002, False, name)  # EVENT_MODIFY_STATE only.
        if not event:
            raise RuntimeError("Exact native process has no opt-in capture request event")
        verify()
        if not api.SetEvent(event):
            raise RuntimeError("Cannot signal exact native capture request event")
        return {"requested": True, "pid": pid, "creation_time_100ns": created, "event": name,
                "capture_completed": False, "note": "Capture is pending the next genuine drawn presentation"}
    finally:
        if event:
            api.CloseHandle(event)
        api.CloseHandle(process)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--creation-time-100ns", type=int, required=True)
    parser.add_argument("--exe", type=Path, default=ROOT / "build_native/native/Release/DoritosNative.exe")
    args = parser.parse_args()
    try:
        print(json.dumps({"ok": True, **request(args.pid, args.creation_time_100ns, args.exe)}, indent=2))
        return 0
    except (RuntimeError, OSError) as error:
        print(json.dumps({"ok": False, "error": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
