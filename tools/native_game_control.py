#!/usr/bin/env python3
"""Inspect the native title window or issue one guarded keyboard/mouse action.

Inspection is read-only. Press requires the exact reported PID/HWND and session
authorization. If user input is newer than 60 seconds, --authorized-active-user
also requires explicit permission for control while the user is at the PC.
Focus is a separate authorized action. Never launches, resizes, or sends a
series of gameplay actions. Move/click/wheel accept physical client-pixel
coordinates, require the exact process creation identity, and never focus or
raise a window. Their point must belong to the foreground title HWND.
Click-focus activates only the exact visible title window, after verifying
that the click point belongs to it. Keyboard taps retain their foreground guard.

Examples:
  py -3 tools/native_game_control.py inspect
  py -3 tools/native_game_control.py press --pid 1234 --hwnd 0x123456 --key enter --authorized-input
  py -3 tools/native_game_control.py click --pid 1234 --hwnd 0x123456 --creation-time-100ns 123456789 --x 640 --y 360 --authorized-input

SendInput is an OS keyboard event, with no per-window target. Every press is
guarded by executable identity, process creation time, class and foreground
checks. Losing focus aborts the tap; a final key-up releases our injected key
even on abort so it cannot remain held in the user's Windows session.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import sys
import time

TITLE_CLASS = "DoritosNativeTitleWindow"
SIGNIN_CLASS = "DoritosNativeSigninDialog"
DEFAULT_EXE = Path(__file__).resolve().parents[1] / "build_native/native/Release/DoritosNative.exe"
KEYS = {"enter": 0x0D, "space": 0x20, "escape": 0x1B, "tab": 0x09, "backspace": 0x08,
        "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27,
        **{key: ord(key.upper()) for key in "wasdqexypr"}}
MOUSE_BUTTON_FLAGS = {"left": (2, 4), "right": (8, 16), "middle": (32, 64)}


class ControlError(RuntimeError):
    pass


class LastInput(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]


class GuiThreadInfo(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                ("hwndActive", wintypes.HWND), ("hwndFocus", wintypes.HWND),
                ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND),
                ("rcCaret", wintypes.RECT)]


class KeyboardInput(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class MouseInput(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class InputUnion(ctypes.Union):
    _fields_ = [("ki", KeyboardInput), ("mi", MouseInput)]


class Input(ctypes.Structure):
    _anonymous_ = ("data",)
    _fields_ = [("type", wintypes.DWORD), ("data", InputUnion)]


class Windows:
    def __init__(self, executable: Path):
        if os.name != "nt":
            raise ControlError("This helper requires Windows")
        self.executable = os.path.normcase(str(executable.resolve(strict=True)))
        self.keyboard_events_sent = 0
        self.mouse_events_sent = 0
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "GetForegroundWindow": (wintypes.HWND, []),
            "SetForegroundWindow": (wintypes.BOOL, [wintypes.HWND]),
            "ShowWindowAsync": (wintypes.BOOL, [wintypes.HWND, ctypes.c_int]),
            "SetWindowPos": (wintypes.BOOL, [wintypes.HWND, wintypes.HWND, ctypes.c_int,
                                           ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]),
            "GetClientRect": (wintypes.BOOL, [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]),
            "ClientToScreen": (wintypes.BOOL, [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]),
            "WindowFromPoint": (wintypes.HWND, [wintypes.POINT]),
            "GetCursorPos": (wintypes.BOOL, [ctypes.POINTER(wintypes.POINT)]),
            "SetCursorPos": (wintypes.BOOL, [ctypes.c_int, ctypes.c_int]),
            "GetGUIThreadInfo": (wintypes.BOOL, [wintypes.DWORD, ctypes.POINTER(GuiThreadInfo)]),
            "SetThreadDpiAwarenessContext": (wintypes.HANDLE, [wintypes.HANDLE]),
            "GetWindowThreadProcessId": (wintypes.DWORD, [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]),
            "GetClassNameW": (ctypes.c_int, [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]),
            "GetWindowTextW": (ctypes.c_int, [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]),
            "GetWindow": (wintypes.HWND, [wintypes.HWND, wintypes.UINT]),
            "IsWindow": (wintypes.BOOL, [wintypes.HWND]),
            "IsWindowVisible": (wintypes.BOOL, [wintypes.HWND]),
            "IsIconic": (wintypes.BOOL, [wintypes.HWND]),
            "GetLastInputInfo": (wintypes.BOOL, [ctypes.POINTER(LastInput)]),
            "GetAsyncKeyState": (ctypes.c_short, [ctypes.c_int]),
            "SendInput": (wintypes.UINT, [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int]),
        }
        for name, (result, args) in signatures.items():
            method = getattr(self.user, name)
            method.restype, method.argtypes = result, args
        self.enum_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        self.user.EnumWindows.argtypes = [self.enum_callback, wintypes.LPARAM]
        self.user.EnumWindows.restype = wintypes.BOOL
        self.kernel.GetTickCount64.argtypes, self.kernel.GetTickCount64.restype = [], ctypes.c_ulonglong
        self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.kernel.OpenProcess.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes, self.kernel.CloseHandle.restype = [wintypes.HANDLE], wintypes.BOOL
        self.kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                        ctypes.POINTER(wintypes.DWORD)]
        self.kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        self.kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        self.kernel.GetProcessTimes.restype = wintypes.BOOL
        expected_size = 40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28
        if ctypes.sizeof(Input) != expected_size:
            raise ControlError("Unexpected native INPUT ABI")
        expected_gui_size = 72 if ctypes.sizeof(ctypes.c_void_p) == 8 else 48
        if ctypes.sizeof(GuiThreadInfo) != expected_gui_size:
            raise ControlError("Unexpected native GUITHREADINFO ABI")

    def idle_ms(self) -> int:
        last = LastInput(ctypes.sizeof(LastInput), 0)
        if not self.user.GetLastInputInfo(ctypes.byref(last)):
            raise ControlError("GetLastInputInfo failed")
        return (self.kernel.GetTickCount64() - last.dwTime) & 0xFFFFFFFF

    def process(self, pid: int) -> dict:
        process = self.kernel.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION only.
        if not process:
            raise ControlError(f"Cannot query exact process {pid}")
        try:
            image, length = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
            if not self.kernel.QueryFullProcessImageNameW(process, 0, image, ctypes.byref(length)):
                raise ControlError("Cannot read process executable identity")
            times = [wintypes.FILETIME() for _ in range(4)]
            if not self.kernel.GetProcessTimes(process, *(ctypes.byref(value) for value in times)):
                raise ControlError("Cannot read process creation identity")
            created = times[0].dwLowDateTime | (times[0].dwHighDateTime << 32)
            return {"pid": pid, "exe": os.path.normcase(str(Path(image.value).resolve(strict=True))),
                    "creation_time_100ns": created}
        finally:
            self.kernel.CloseHandle(process)

    def window(self, hwnd: int) -> dict:
        process = wintypes.DWORD()
        self.user.GetWindowThreadProcessId(hwnd, ctypes.byref(process))
        name = ctypes.create_unicode_buffer(256)
        self.user.GetClassNameW(hwnd, name, len(name))
        record = {"hwnd": hex(hwnd), "pid": process.value, "class": name.value,
                  "foreground": bool(hwnd and int(self.user.GetForegroundWindow() or 0) == hwnd)}
        if name.value == SIGNIN_CLASS:
            record["owner_hwnd"] = hex(int(self.user.GetWindow(hwnd, 4) or 0))  # GW_OWNER
        return record

    def is_game_window(self, record: dict) -> bool:
        if record["class"] == TITLE_CLASS:
            return True
        if record["class"] != SIGNIN_CLASS:
            return False
        owner_hwnd = int(record["owner_hwnd"], 0)
        if not owner_hwnd or not self.user.IsWindow(owner_hwnd):
            return False
        owner = self.window(owner_hwnd)
        return owner["class"] == TITLE_CLASS and owner["pid"] == record["pid"]

    def inspect(self, pid: int | None) -> dict:
        windows = []

        @self.enum_callback
        def visit(hwnd, _):
            record = self.window(int(hwnd))
            if self.is_game_window(record) and (pid is None or record["pid"] == pid):
                try:
                    identity = self.process(record["pid"])
                    if identity["exe"] == self.executable:
                        title = ctypes.create_unicode_buffer(256)
                        self.user.GetWindowTextW(hwnd, title, len(title))
                        windows.append({**record, **identity, "title": title.value,
                                        "visible": bool(self.user.IsWindowVisible(hwnd)),
                                        "minimized": bool(self.user.IsIconic(hwnd))})
                except (ControlError, OSError) as error:
                    windows.append({**record, "query_error": str(error)})
            return True

        if not self.user.EnumWindows(visit, 0):
            raise ControlError("EnumWindows failed")
        foreground = int(self.user.GetForegroundWindow() or 0)
        return {"action": "inspect", "idle_ms": self.idle_ms(), "expected_exe": self.executable,
                "foreground": self.window(foreground), "windows": windows}

    def guard(self, pid: int, hwnd: int, identity: dict, foreground=True, allow_minimized=False):
        if (not self.user.IsWindow(hwnd) or not self.user.IsWindowVisible(hwnd) or
                (self.user.IsIconic(hwnd) and not allow_minimized)
                or (foreground and int(self.user.GetForegroundWindow() or 0) != hwnd)):
            raise ControlError("Exact game HWND must be visible, restored, and foreground for input")
        record = self.window(hwnd)
        if record["pid"] != pid or not self.is_game_window(record) or self.process(pid) != identity:
            raise ControlError("Game PID, executable, creation identity or title class changed")

    def key_event(self, key: int, up: bool):
        event = Input(type=1)
        event.ki = KeyboardInput(key, 0, 2 if up else 0, 0, 0)
        if self.user.SendInput(1, ctypes.byref(event), ctypes.sizeof(Input)) != 1:
            raise ControlError("Windows rejected the keyboard event")
        self.keyboard_events_sent += 1

    def focus(self, args) -> dict:
        if not args.authorized_input:
            raise ControlError("Focus requires session authorization (--authorized-input)")
        initial_idle = self.idle_ms()
        if initial_idle < 60000 and not args.authorized_active_user:
            raise ControlError("User input is newer than 60 seconds; explicit active-user authorization is required")
        identity = self.process(args.pid)
        if identity["exe"] != self.executable or identity["creation_time_100ns"] != args.creation_time_100ns:
            raise ControlError("Exact focus PID executable or creation identity does not match")
        self.guard(args.pid, args.hwnd, identity, foreground=False, allow_minimized=True)
        if self.idle_ms() < 60000 and not args.authorized_active_user:
            raise ControlError("User input resumed before focusing the game")
        if self.user.IsIconic(args.hwnd):
            self.user.ShowWindowAsync(args.hwnd, 9)  # SW_RESTORE, exact verified game HWND.
            restore_deadline = time.perf_counter() + 0.5
            while self.user.IsIconic(args.hwnd) and time.perf_counter() < restore_deadline:
                self.guard(args.pid, args.hwnd, identity, foreground=False, allow_minimized=True)
                time.sleep(0.01)
            self.guard(args.pid, args.hwnd, identity, foreground=False)
        if not self.user.SetForegroundWindow(args.hwnd):
            raise ControlError("Windows declined foreground activation of the exact game window")
        # Activation may finish asynchronously. Poll the same visible window
        # without sending input or repeating the foreground request.
        activation_deadline = time.perf_counter() + 0.25
        while int(self.user.GetForegroundWindow() or 0) != args.hwnd:
            self.guard(args.pid, args.hwnd, identity, foreground=False)
            if time.perf_counter() >= activation_deadline:
                break
            time.sleep(0.01)
        self.guard(args.pid, args.hwnd, identity)
        return {"action": "focus", "pid": args.pid, "hwnd": hex(args.hwnd),
                "creation_time_100ns": args.creation_time_100ns,
                "initial_idle_ms": initial_idle, "keyboard_events_sent": 0}

    def click_focus(self, args) -> dict:
        # Keep all screen/cursor/hit-test coordinates in physical pixels even
        # when Python's manifest or a monitor's scale differs from the game.
        previous_dpi = self.user.SetThreadDpiAwarenessContext(-4)  # PER_MONITOR_AWARE_V2.
        if not previous_dpi:
            raise ControlError("Cannot establish physical-pixel click coordinates")
        try:
            return self._click_focus(args)
        finally:
            self.user.SetThreadDpiAwarenessContext(previous_dpi)

    def _click_focus(self, args) -> dict:
        if not args.authorized_input:
            raise ControlError("Click-focus requires session authorization")
        initial_idle = self.idle_ms()
        if initial_idle < 60000 and not args.authorized_active_user:
            raise ControlError("Recent user input requires explicit active-user authorization")
        identity = self.process(args.pid)
        if identity["exe"] != self.executable or identity["creation_time_100ns"] != args.creation_time_100ns:
            raise ControlError("Exact click-focus process identity does not match")
        self.guard(args.pid, args.hwnd, identity, foreground=False)
        if self.window(args.hwnd)["class"] != TITLE_CLASS:
            raise ControlError("Click-focus accepts only the title window, excluding dialogs")
        def held_input_guard():
            if any(self.user.GetAsyncKeyState(key) & 0x8000
                   for key in (1, 2, 4, 5, 6, 0x10, 0x11, 0x12, 0x5B, 0x5C)):
                raise ControlError("A mouse button or system modifier is already held")

        held_input_guard()

        def gui_input_guard():
            # GetCapture only observes this Python thread. Thread 0 instead
            # queries the actual foreground GUI queue, including foreign apps.
            info = GuiThreadInfo()
            info.cbSize = ctypes.sizeof(info)
            if not self.user.GetGUIThreadInfo(0, ctypes.byref(info)):
                raise ControlError("Cannot inspect foreground mouse capture")
            if info.hwndCapture and int(info.hwndCapture) != args.hwnd:
                raise ControlError("Another window has captured the mouse")
            if info.flags & 0x1E or info.hwndMenuOwner or info.hwndMoveSize:
                raise ControlError("A foreground menu or move/size loop is active")

        gui_input_guard()
        rect, point = wintypes.RECT(), wintypes.POINT(20, 20)
        if not self.user.GetClientRect(args.hwnd, ctypes.byref(rect)) or rect.right <= 40 or rect.bottom <= 40:
            raise ControlError("Title client area is unavailable")
        if not self.user.ClientToScreen(args.hwnd, ctypes.byref(point)):
            raise ControlError("Cannot locate the exact title client point")
        # Bring the verified window above normal windows without activation,
        # movement or resizing. A real click then supplies normal OS activation.
        if not self.user.SetWindowPos(args.hwnd, 0, 0, 0, 0, 0, 0x13):  # NOMOVE|NOSIZE|NOACTIVATE
            raise ControlError("Cannot expose the exact game window")

        def point_guard():
            self.guard(args.pid, args.hwnd, identity, foreground=False)
            current_rect, current_point = wintypes.RECT(), wintypes.POINT(20, 20)
            if (not self.user.GetClientRect(args.hwnd, ctypes.byref(current_rect)) or
                    current_rect.right <= 40 or current_rect.bottom <= 40 or
                    not self.user.ClientToScreen(args.hwnd, ctypes.byref(current_point)) or
                    (current_point.x, current_point.y) != (point.x, point.y)):
                raise ControlError("The title client point changed before the click")
            if int(self.user.WindowFromPoint(point) or 0) != args.hwnd:
                raise ControlError("The click point is covered or belongs to another window")

        point_guard()
        if self.idle_ms() < 60000 and not args.authorized_active_user:
            raise ControlError("User input resumed before the title click")
        gui_input_guard()
        held_input_guard()
        if not self.user.SetCursorPos(point.x, point.y):
            raise ControlError("Cannot place the cursor on the verified game point")
        point_guard()
        actual = wintypes.POINT()
        if not self.user.GetCursorPos(ctypes.byref(actual)) or (actual.x, actual.y) != (point.x, point.y):
            raise ControlError("The cursor moved before the guarded click")
        events = (Input * 2)()
        events[0].type = events[1].type = 0
        events[0].mi = MouseInput(0, 0, 0, 2, 0, 0)  # LEFTDOWN.
        events[1].mi = MouseInput(0, 0, 0, 4, 0, 0)  # LEFTUP.
        point_guard()
        gui_input_guard()
        held_input_guard()
        if (not self.user.GetCursorPos(ctypes.byref(actual)) or
                (actual.x, actual.y) != (point.x, point.y) or
                int(self.user.WindowFromPoint(actual) or 0) != args.hwnd):
            raise ControlError("The cursor or click owner changed before injection")
        # A single batch prevents physical or injected input interleaving
        # between our down/up. A proven one-event insert still needs release.
        sent = 0
        try:
            sent = int(self.user.SendInput(2, events, ctypes.sizeof(Input)))
            self.mouse_events_sent += sent
            if sent != 2:
                raise ControlError("Windows rejected the complete mouse click")
        finally:
            if sent == 1:
                if self.user.SendInput(1, ctypes.byref(events[1]), ctypes.sizeof(Input)) != 1:
                    raise ControlError("Windows rejected the partial click's mouse-up cleanup")
                self.mouse_events_sent += 1
        activation_deadline = time.perf_counter() + 0.25
        while int(self.user.GetForegroundWindow() or 0) != args.hwnd and time.perf_counter() < activation_deadline:
            self.guard(args.pid, args.hwnd, identity, foreground=False)
            time.sleep(0.01)
        self.guard(args.pid, args.hwnd, identity)
        return {"action": "click-focus", "pid": args.pid, "hwnd": hex(args.hwnd),
                "initial_idle_ms": initial_idle, "mouse_events_sent": self.mouse_events_sent,
                "keyboard_events_sent": 0}

    def press(self, args) -> dict:
        if not args.authorized_input:
            raise ControlError("Press requires explicit session authorization (--authorized-input)")
        initial_idle = self.idle_ms()
        if initial_idle < 60000 and not args.authorized_active_user:
            raise ControlError("User input is newer than 60 seconds; explicit active-user authorization is required")
        identity = self.process(args.pid)
        if identity["exe"] != self.executable:
            raise ControlError("Exact PID does not own the configured native executable")
        if args.creation_time_100ns is not None and identity["creation_time_100ns"] != args.creation_time_100ns:
            raise ControlError("Exact press process creation identity does not match")
        self.guard(args.pid, args.hwnd, identity)
        key = KEYS[args.key]
        if any(self.user.GetAsyncKeyState(value) & 0x8000 for value in (key, 0x10, 0x11, 0x12, 0x5B, 0x5C)):
            raise ControlError("A requested key or system modifier is already physically held")
        self.guard(args.pid, args.hwnd, identity)
        self.key_event(key, False)
        started = time.perf_counter()
        try:
            deadline = started + args.hold_ms / 1000
            while time.perf_counter() < deadline:
                self.guard(args.pid, args.hwnd, identity)
                time.sleep(min(0.01, max(0, deadline - time.perf_counter())))
            self.guard(args.pid, args.hwnd, identity)
        finally:
            self.key_event(key, True)
        return {"action": "press", "pid": args.pid, "hwnd": hex(args.hwnd), "key": args.key,
                "hold_ms": round((time.perf_counter() - started) * 1000), "initial_idle_ms": initial_idle,
                "authorized_active_user": args.authorized_active_user}

    def mouse(self, args) -> dict:
        # No activation, exposure or input is attempted while establishing the
        # physical coordinate system used by the exact visible client point.
        previous_dpi = self.user.SetThreadDpiAwarenessContext(-4)
        if not previous_dpi:
            raise ControlError("Cannot establish physical-pixel mouse coordinates")
        try:
            return self._mouse(args)
        finally:
            self.user.SetThreadDpiAwarenessContext(previous_dpi)

    def _mouse(self, args) -> dict:
        if args.action not in ("move", "click", "wheel"):
            raise ControlError("Unknown mouse action")
        if args.pid <= 0 or args.hwnd <= 0 or args.creation_time_100ns <= 0:
            raise ControlError("Mouse actions require a positive exact PID/HWND/creation identity")
        if args.action == "wheel" and (args.ticks == 0 or not -8 <= args.ticks <= 8):
            raise ControlError("Wheel ticks must be -8..8 excluding zero (maximum 960 delta)")
        if args.action == "click" and args.button not in MOUSE_BUTTON_FLAGS:
            raise ControlError("Unknown mouse button")
        if not args.authorized_input:
            raise ControlError("Mouse actions require session authorization (--authorized-input)")
        initial_idle = self.idle_ms()
        if initial_idle < 60000 and not args.authorized_active_user:
            raise ControlError("Recent user input requires explicit active-user authorization")
        identity = self.process(args.pid)
        if identity["exe"] != self.executable or identity["creation_time_100ns"] != args.creation_time_100ns:
            raise ControlError("Exact mouse process executable or creation identity does not match")

        def held_input_guard():
            if any(self.user.GetAsyncKeyState(key) & 0x8000
                   for key in (1, 2, 4, 5, 6, 0x10, 0x11, 0x12, 0x5B, 0x5C)):
                raise ControlError("A mouse button or system modifier is already held")

        def gui_input_guard():
            info = GuiThreadInfo()
            info.cbSize = ctypes.sizeof(info)
            if not self.user.GetGUIThreadInfo(0, ctypes.byref(info)):
                raise ControlError("Cannot inspect foreground mouse capture")
            if info.hwndCapture and int(info.hwndCapture) != args.hwnd:
                raise ControlError("Another window has captured the mouse")
            if info.flags & 0x1E or info.hwndMenuOwner or info.hwndMoveSize:
                raise ControlError("A foreground menu or move/size loop is active")

        def client_point():
            rect, point = wintypes.RECT(), wintypes.POINT(args.x, args.y)
            if not self.user.GetClientRect(args.hwnd, ctypes.byref(rect)):
                raise ControlError("Title client area is unavailable")
            if not rect.left <= args.x < rect.right or not rect.top <= args.y < rect.bottom:
                raise ControlError("Mouse point must be inside the exact title client area")
            if not self.user.ClientToScreen(args.hwnd, ctypes.byref(point)):
                raise ControlError("Cannot locate the exact title client point")
            return rect, point

        self.guard(args.pid, args.hwnd, identity)
        if self.window(args.hwnd)["class"] != TITLE_CLASS:
            raise ControlError("Mouse actions accept only the title window, excluding dialogs")
        held_input_guard()
        gui_input_guard()
        initial_rect, point = client_point()
        initial_geometry = (initial_rect.left, initial_rect.top, initial_rect.right, initial_rect.bottom,
                            point.x, point.y)

        def point_guard():
            self.guard(args.pid, args.hwnd, identity)
            # Keep the strict title-only rule on each recheck, including when a
            # HWND was destroyed and reused by another window in this process.
            if self.window(args.hwnd)["class"] != TITLE_CLASS:
                raise ControlError("Exact title class changed before the mouse action")
            current_rect, current_point = client_point()
            if (current_rect.left, current_rect.top, current_rect.right, current_rect.bottom,
                    current_point.x, current_point.y) != initial_geometry:
                raise ControlError("The title client point changed before the mouse action")
            if int(self.user.WindowFromPoint(point) or 0) != args.hwnd:
                raise ControlError("The mouse point is covered or belongs to another window")
            gui_input_guard()
            held_input_guard()
            if self.idle_ms() < 60000 and not args.authorized_active_user:
                raise ControlError("User input resumed before the mouse action")

        point_guard()
        if not self.user.SetCursorPos(point.x, point.y):
            raise ControlError("Cannot place the cursor on the verified game point")

        def cursor_guard():
            point_guard()
            actual = wintypes.POINT()
            if (not self.user.GetCursorPos(ctypes.byref(actual)) or
                    (actual.x, actual.y) != (point.x, point.y) or
                    int(self.user.WindowFromPoint(actual) or 0) != args.hwnd):
                raise ControlError("The cursor or mouse owner changed before injection")

        cursor_guard()
        if args.action == "click":
            events = (Input * 2)()
            down, up = MOUSE_BUTTON_FLAGS[args.button]
            events[0].type = events[1].type = 0
            events[0].mi = MouseInput(0, 0, 0, down, 0, 0)
            events[1].mi = MouseInput(0, 0, 0, up, 0, 0)
            cursor_guard()
            sent = 0
            try:
                sent = int(self.user.SendInput(2, events, ctypes.sizeof(Input)))
                self.mouse_events_sent += sent
                if sent != 2:
                    raise ControlError("Windows rejected the complete mouse click")
            finally:
                # Release only a down event whose insertion Windows reported.
                # Cleanup remains required on focus loss to avoid a stuck drag.
                if sent == 1:
                    if self.user.SendInput(1, ctypes.byref(events[1]), ctypes.sizeof(Input)) != 1:
                        raise ControlError("Windows rejected the partial click's mouse-up cleanup")
                    self.mouse_events_sent += 1
        elif args.action == "wheel":
            event = Input(type=0)
            event.mi = MouseInput(0, 0, (args.ticks * 120) & 0xFFFFFFFF, 0x800, 0, 0)
            cursor_guard()
            sent = int(self.user.SendInput(1, ctypes.byref(event), ctypes.sizeof(Input)))
            self.mouse_events_sent += sent
            if sent != 1:
                raise ControlError("Windows rejected the mouse wheel event")
        return {"action": args.action, "pid": args.pid, "hwnd": hex(args.hwnd),
                "creation_time_100ns": args.creation_time_100ns,
                "client_point": [args.x, args.y], "screen_point": [point.x, point.y],
                "initial_idle_ms": initial_idle, "authorized_active_user": args.authorized_active_user,
                "button": args.button if args.action == "click" else None,
                "wheel_delta": args.ticks * 120 if args.action == "wheel" else 0,
                "mouse_events_sent": self.mouse_events_sent, "keyboard_events_sent": 0}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="action", required=True)
    inspect = commands.add_parser("inspect", help="read-only foreground, window and last-input report")
    inspect.add_argument("--pid", type=int)
    press = commands.add_parser("press", help="one authorized foreground-only game key tap")
    press.add_argument("--pid", type=int, required=True)
    press.add_argument("--hwnd", type=lambda value: int(value, 0), required=True)
    press.add_argument("--creation-time-100ns", type=int)
    press.add_argument("--key", choices=KEYS, required=True)
    press.add_argument("--hold-ms", type=int, default=120)
    press.add_argument("--authorized-input", action="store_true")
    press.add_argument("--authorized-active-user", action="store_true",
                       help="use only after explicit user authorization while their input is recent")
    focus = commands.add_parser("focus", help="authorize foreground activation of the exact visible game window")
    focus.add_argument("--pid", type=int, required=True)
    focus.add_argument("--hwnd", type=lambda value: int(value, 0), required=True)
    focus.add_argument("--creation-time-100ns", type=int, required=True)
    focus.add_argument("--authorized-input", action="store_true")
    focus.add_argument("--authorized-active-user", action="store_true",
                       help="use only after explicit user authorization while their input is recent")
    click_focus = commands.add_parser("click-focus", help="one guarded client click to activate the exact title window")
    click_focus.add_argument("--pid", type=int, required=True)
    click_focus.add_argument("--hwnd", type=lambda value: int(value, 0), required=True)
    click_focus.add_argument("--creation-time-100ns", type=int, required=True)
    click_focus.add_argument("--authorized-input", action="store_true")
    click_focus.add_argument("--authorized-active-user", action="store_true")
    mouse_commands = []
    for action in ("move", "click", "wheel"):
        command = commands.add_parser(action, help=f"one authorized foreground-only title mouse {action}")
        command.add_argument("--pid", type=int, required=True)
        command.add_argument("--hwnd", type=lambda value: int(value, 0), required=True)
        command.add_argument("--creation-time-100ns", type=int, required=True)
        command.add_argument("--x", type=int, required=True, help="physical client-pixel x coordinate")
        command.add_argument("--y", type=int, required=True, help="physical client-pixel y coordinate")
        command.add_argument("--authorized-input", action="store_true")
        command.add_argument("--authorized-active-user", action="store_true")
        if action == "click":
            command.add_argument("--button", choices=MOUSE_BUTTON_FLAGS, default="left")
        if action == "wheel":
            command.add_argument("--ticks", type=int, required=True, help="signed wheel ticks, -8..8 excluding zero")
        mouse_commands.append(command)
    for command in (inspect, press, focus, click_focus, *mouse_commands):
        command.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    args = parser.parse_args(argv)
    api = None
    try:
        if args.action == "press" and (args.pid <= 0 or args.hwnd <= 0 or not 20 <= args.hold_ms <= 500):
            raise ControlError("Press requires a positive exact PID/HWND and hold 20..500 ms")
        if args.action in ("focus", "click-focus") and (args.pid <= 0 or args.hwnd <= 0 or args.creation_time_100ns <= 0):
            raise ControlError("Focus requires a positive exact PID/HWND/creation identity")
        if args.action in ("move", "click", "wheel"):
            if args.pid <= 0 or args.hwnd <= 0 or args.creation_time_100ns <= 0:
                raise ControlError("Mouse actions require a positive exact PID/HWND/creation identity")
            if args.x < 0 or args.y < 0:
                raise ControlError("Mouse client coordinates cannot be negative")
            if args.action == "wheel" and (args.ticks == 0 or not -8 <= args.ticks <= 8):
                raise ControlError("Wheel ticks must be -8..8 excluding zero (maximum 960 delta)")
        api = Windows(args.exe)
        result = (api.inspect(args.pid) if args.action == "inspect" else
                  api.click_focus(args) if args.action == "click-focus" else
                  api.focus(args) if args.action == "focus" else
                  api.mouse(args) if args.action in ("move", "click", "wheel") else api.press(args))
        print(json.dumps({"ok": True, **result}, indent=2))
        return 0
    except (ControlError, OSError) as error:
        print(json.dumps({"ok": False, "error": str(error),
                          "keyboard_events_sent": getattr(api, "keyboard_events_sent", 0),
                          "mouse_events_sent": getattr(api, "mouse_events_sent", 0)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
