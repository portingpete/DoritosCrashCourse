"""Adverse game-control contracts using only fake Win32 APIs; no OS input."""
from __future__ import annotations

import ctypes
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location(
    "native_game_control", Path(__file__).resolve().parents[1] / "tools/native_game_control.py")
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class FakeUser:
    def __init__(self):
        self.hwnd, self.foreground = 100, 200
        self.origin, self.cursor = (300, 400), (0, 0)
        self.capture, self.gui_flags, self.menu_owner, self.move_size = 0, 0, 0, 0
        self.held = set()
        self.dpi, self.dpi_changes = 17, []
        self.moves, self.raises, self.sent = [], [], []
        self.mouse_data = []
        self.insert_counts = []
        self.on_raise = self.on_move = None
        self.gui_available, self.hit_owner = True, self.hwnd

    def SetThreadDpiAwarenessContext(self, context):
        old, self.dpi = self.dpi, context
        self.dpi_changes.append(context)
        return old

    def IsWindow(self, hwnd):
        return hwnd == self.hwnd

    IsWindowVisible = IsWindow

    def IsIconic(self, hwnd):
        return False

    def GetForegroundWindow(self):
        return self.foreground

    def GetAsyncKeyState(self, key):
        return 0x8000 if key in self.held else 0

    def GetGUIThreadInfo(self, thread, pointer):
        assert thread == 0
        info = pointer._obj
        assert info.cbSize == ctypes.sizeof(control.GuiThreadInfo)
        info.hwndCapture, info.flags = self.capture, self.gui_flags
        info.hwndMenuOwner, info.hwndMoveSize = self.menu_owner, self.move_size
        return self.gui_available

    def GetClientRect(self, hwnd, pointer):
        pointer._obj.right, pointer._obj.bottom = 2560, 1440
        return True

    def ClientToScreen(self, hwnd, pointer):
        point = pointer._obj
        point.x += self.origin[0]
        point.y += self.origin[1]
        return True

    def SetWindowPos(self, *args):
        self.raises.append(args)
        if self.on_raise:
            self.on_raise()
        return True

    def WindowFromPoint(self, point):
        return self.hit_owner

    def SetCursorPos(self, x, y):
        self.cursor = (x, y)
        self.moves.append(self.cursor)
        if self.on_move:
            self.on_move()
        return True

    def GetCursorPos(self, pointer):
        pointer._obj.x, pointer._obj.y = self.cursor
        return True

    def SendInput(self, count, inputs, size):
        assert size == ctypes.sizeof(control.Input)
        events = ctypes.cast(inputs, ctypes.POINTER(control.Input))
        flags = tuple(events[index].mi.dwFlags for index in range(count))
        self.mouse_data.append(tuple(events[index].mi.mouseData for index in range(count)))
        assert all(events[index].type == 0 for index in range(count))
        self.sent.append(flags)
        inserted = self.insert_counts.pop(0) if self.insert_counts else count
        if inserted:
            self.foreground = self.hwnd
        return inserted


class FakeWindows(control.Windows):
    def __init__(self):
        self.user = FakeUser()
        self.executable = "owned.exe"
        self.created = 123
        self.keyboard_events_sent = self.mouse_events_sent = 0

    def idle_ms(self):
        return 90000

    def process(self, pid):
        return {"pid": pid, "exe": self.executable, "creation_time_100ns": self.created}

    def window(self, hwnd):
        return {"hwnd": hex(hwnd), "pid": 42, "class": control.TITLE_CLASS}


class ClickFocusContracts(unittest.TestCase):
    def setUp(self):
        self.api = FakeWindows()
        self.args = SimpleNamespace(pid=42, hwnd=100, creation_time_100ns=123,
                                    authorized_input=True, authorized_active_user=False)

    def rejects_without_click(self, match):
        with self.assertRaisesRegex(control.ControlError, match):
            self.api.click_focus(self.args)
        self.assertEqual(self.api.user.sent, [])
        self.assertEqual(self.api.mouse_events_sent, 0)
        self.assertEqual(self.api.keyboard_events_sent, 0)
        self.assertEqual(self.api.user.dpi, 17)

    def test_foreign_capture_rejected_before_any_cursor_move(self):
        self.api.user.capture = 200
        self.rejects_without_click("captured")
        self.assertEqual(self.api.user.moves, [])
        self.assertEqual(self.api.user.raises, [])

    def test_foreground_menu_and_move_size_states_refuse(self):
        for attribute, value in (("gui_flags", 2), ("gui_flags", 4), ("gui_flags", 8),
                                 ("gui_flags", 16), ("menu_owner", 200), ("move_size", 200)):
            with self.subTest(attribute=attribute, value=value):
                self.api = FakeWindows()
                setattr(self.api.user, attribute, value)
                self.rejects_without_click("menu or move/size")
                self.assertEqual(self.api.user.moves, [])

    def test_unavailable_foreground_queue_refuses(self):
        self.api.user.gui_available = False
        self.rejects_without_click("inspect foreground")

    def test_window_moves_but_hit_test_still_returns_same_hwnd(self):
        self.api.user.on_raise = lambda: setattr(self.api.user, "origin", (310, 400))
        self.rejects_without_click("client point changed")
        self.assertEqual(self.api.user.moves, [])

    def test_capture_changes_after_cursor_move_refuse_click(self):
        self.api.user.on_move = lambda: setattr(self.api.user, "capture", 200)
        self.rejects_without_click("captured")

    def test_each_mouse_button_and_modifier_can_become_held_late(self):
        for key in (1, 2, 4, 5, 6, 0x10, 0x11, 0x12, 0x5B, 0x5C):
            with self.subTest(key=key):
                self.api = FakeWindows()
                self.api.user.on_move = lambda: self.api.user.held.add(key)
                self.rejects_without_click("already held")

    def test_other_window_covers_point_after_cursor_move(self):
        self.api.user.on_move = lambda: setattr(self.api.user, "hit_owner", 200)
        self.rejects_without_click("covered")

    def test_creation_identity_changes_after_cursor_move(self):
        self.api.user.on_move = lambda: setattr(self.api, "created", 124)
        self.rejects_without_click("identity")

    def test_cursor_moves_away_after_programmed_move(self):
        self.api.user.on_move = lambda: setattr(self.api.user, "cursor", (900, 900))
        self.rejects_without_click("cursor moved")

    def test_success_inserts_one_serial_click_and_restores_dpi(self):
        result = self.api.click_focus(self.args)
        self.assertEqual(self.api.user.sent, [(2, 4)])
        self.assertEqual(result["mouse_events_sent"], 2)
        self.assertEqual(result["keyboard_events_sent"], 0)
        self.assertEqual(self.api.user.raises, [(100, 0, 0, 0, 0, 0, 0x13)])
        self.assertEqual(self.api.user.dpi_changes, [-4, 17])

    def test_zero_insert_does_not_release_a_button_we_never_pressed(self):
        self.api.user.insert_counts = [0]
        with self.assertRaisesRegex(control.ControlError, "complete mouse click"):
            self.api.click_focus(self.args)
        self.assertEqual(self.api.user.sent, [(2, 4)])
        self.assertEqual(self.api.mouse_events_sent, 0)
        self.assertEqual(self.api.user.dpi, 17)

    def test_partial_insert_releases_only_our_injected_down(self):
        self.api.user.insert_counts = [1, 1]
        with self.assertRaisesRegex(control.ControlError, "complete mouse click"):
            self.api.click_focus(self.args)
        self.assertEqual(self.api.user.sent, [(2, 4), (4,)])
        self.assertEqual(self.api.mouse_events_sent, 2)
        self.assertEqual(self.api.user.dpi, 17)

    def test_cleanup_failure_remains_explicit_and_restores_dpi(self):
        self.api.user.insert_counts = [1, 0]
        with self.assertRaisesRegex(control.ControlError, "mouse-up cleanup"):
            self.api.click_focus(self.args)
        self.assertEqual(self.api.user.sent, [(2, 4), (4,)])
        self.assertEqual(self.api.mouse_events_sent, 1)
        self.assertEqual(self.api.user.dpi, 17)


class MouseContracts(unittest.TestCase):
    def setUp(self):
        self.api = FakeWindows()
        self.api.user.foreground = self.api.user.hwnd
        self.args = SimpleNamespace(action="click", pid=42, hwnd=100, creation_time_100ns=123,
                                    authorized_input=True, authorized_active_user=False,
                                    x=640, y=360, button="left", ticks=1)

    def rejects(self, match, *, before_move=False):
        with self.assertRaisesRegex(control.ControlError, match):
            self.api.mouse(self.args)
        self.assertEqual(self.api.user.sent, [])
        self.assertEqual(self.api.mouse_events_sent, 0)
        self.assertEqual(self.api.keyboard_events_sent, 0)
        self.assertEqual(self.api.user.raises, [])
        self.assertEqual(self.api.user.dpi, 17)
        if before_move:
            self.assertEqual(self.api.user.moves, [])

    def test_authorization_is_required_before_cursor_movement(self):
        self.args.authorized_input = False
        self.rejects("authorization", before_move=True)

    def test_recent_user_input_requires_active_authorization(self):
        self.api.idle_ms = lambda: 100
        self.rejects("active-user authorization", before_move=True)
        self.args.authorized_active_user = True
        self.api.mouse(self.args)
        self.assertEqual(self.api.user.sent, [(2, 4)])

    def test_exact_creation_identity_is_required(self):
        self.args.creation_time_100ns = 122
        self.rejects("creation identity", before_move=True)

    def test_wrong_executable_is_rejected(self):
        self.api.process = lambda pid: {"pid": pid, "exe": "foreign.exe", "creation_time_100ns": 123}
        self.rejects("executable", before_move=True)

    def test_background_mouse_never_activates_or_moves(self):
        self.api.user.foreground = 200
        self.rejects("foreground", before_move=True)

    def test_dialog_target_is_rejected_even_when_owned(self):
        self.api.window = lambda hwnd: {"hwnd": hex(hwnd), "pid": 42,
                                      "class": control.SIGNIN_CLASS, "owner_hwnd": "0x65"}
        self.api.is_game_window = lambda record: True
        self.rejects("excluding dialogs", before_move=True)

    def test_outside_client_points_never_move_the_cursor(self):
        for x, y in ((-1, 0), (0, -1), (2560, 100), (100, 1440), (2**40, 100)):
            with self.subTest(x=x, y=y):
                self.api = FakeWindows()
                self.api.user.foreground = 100
                self.args.x, self.args.y = x, y
                self.rejects("inside", before_move=True)

    def test_covered_client_point_is_rejected_before_cursor_move(self):
        self.api.user.hit_owner = 200
        self.rejects("covered", before_move=True)

    def test_foreign_capture_blocks_cursor_movement(self):
        self.api.user.capture = 200
        self.rejects("captured", before_move=True)

    def test_own_capture_does_not_imply_other_window_input(self):
        self.api.user.capture = 100
        self.api.mouse(self.args)
        self.assertEqual(self.api.user.sent, [(2, 4)])

    def test_foreground_menu_or_move_size_rejected(self):
        for name, value in (("gui_flags", 2), ("gui_flags", 4), ("gui_flags", 8),
                            ("gui_flags", 16), ("menu_owner", 200), ("move_size", 200)):
            with self.subTest(name=name):
                self.api = FakeWindows()
                self.api.user.foreground = 100
                setattr(self.api.user, name, value)
                self.rejects("menu or move/size", before_move=True)

    def test_foreground_changes_after_cursor_move_abort_injection(self):
        self.api.user.on_move = lambda: setattr(self.api.user, "foreground", 200)
        self.rejects("foreground")

    def test_process_reuse_after_cursor_move_aborts_injection(self):
        self.api.user.on_move = lambda: setattr(self.api, "created", 124)
        self.rejects("identity")

    def test_title_class_reuse_after_cursor_move_aborts_injection(self):
        self.api.user.on_move = lambda: setattr(
            self.api, "window", lambda hwnd: {"hwnd": hex(hwnd), "pid": 42, "class": "ForeignClass"})
        self.rejects("class")

    def test_window_moves_after_cursor_move_abort_injection(self):
        self.api.user.on_move = lambda: setattr(self.api.user, "origin", (301, 400))
        self.rejects("client point changed")

    def test_foreign_capture_or_coverage_after_cursor_move_aborts_injection(self):
        for name in ("capture", "hit_owner"):
            with self.subTest(name=name):
                self.api = FakeWindows()
                self.api.user.foreground = 100
                self.api.user.on_move = lambda: setattr(self.api.user, name, 200)
                self.rejects("captured|covered")

    def test_each_held_button_or_modifier_aborts_injection(self):
        for key in (1, 2, 4, 5, 6, 0x10, 0x11, 0x12, 0x5B, 0x5C):
            with self.subTest(key=key):
                self.api = FakeWindows()
                self.api.user.foreground = 100
                self.api.user.on_move = lambda: self.api.user.held.add(key)
                self.rejects("already held")

    def test_cursor_changes_after_move_abort_injection(self):
        self.api.user.on_move = lambda: setattr(self.api.user, "cursor", (999, 888))
        self.rejects("cursor or mouse owner changed")

    def test_new_user_input_after_move_aborts_unauthorized_active_user(self):
        self.api.user.on_move = lambda: setattr(self.api, "idle_ms", lambda: 100)
        self.rejects("User input resumed")

    def test_move_changes_only_cursor_without_raise_or_sendinput(self):
        self.args.action = "move"
        result = self.api.mouse(self.args)
        self.assertEqual(self.api.user.moves, [(940, 760)])
        self.assertEqual(self.api.user.sent, [])
        self.assertEqual(self.api.user.raises, [])
        self.assertEqual(result["screen_point"], [940, 760])
        self.assertEqual(result["mouse_events_sent"], 0)
        self.assertEqual(self.api.user.dpi_changes, [-4, 17])

    def test_each_click_uses_one_complete_batch(self):
        for button, flags in (("left", (2, 4)), ("right", (8, 16)), ("middle", (32, 64))):
            with self.subTest(button=button):
                self.api = FakeWindows()
                self.api.user.foreground = 100
                self.args.button = button
                result = self.api.mouse(self.args)
                self.assertEqual(self.api.user.sent, [flags])
                self.assertEqual(result["mouse_events_sent"], 2)
                self.assertEqual(self.api.user.raises, [])

    def test_partial_insert_releases_matching_button(self):
        self.args.button = "right"
        self.api.user.insert_counts = [1, 1]
        with self.assertRaisesRegex(control.ControlError, "complete mouse click"):
            self.api.mouse(self.args)
        self.assertEqual(self.api.user.sent, [(8, 16), (16,)])
        self.assertEqual(self.api.mouse_events_sent, 2)
        self.assertEqual(self.api.user.dpi, 17)

    def test_zero_insert_does_not_release_unpressed_button(self):
        self.api.user.insert_counts = [0]
        with self.assertRaisesRegex(control.ControlError, "complete mouse click"):
            self.api.mouse(self.args)
        self.assertEqual(self.api.user.sent, [(2, 4)])
        self.assertEqual(self.api.mouse_events_sent, 0)

    def test_wheel_is_one_signed_bounded_event(self):
        self.args.action = "wheel"
        for ticks in (-8, -1, 1, 8):
            with self.subTest(ticks=ticks):
                self.api = FakeWindows()
                self.api.user.foreground = 100
                self.args.ticks = ticks
                result = self.api.mouse(self.args)
                self.assertEqual(self.api.user.sent, [(0x800,)])
                self.assertEqual(self.api.user.mouse_data, [((ticks * 120) & 0xFFFFFFFF,)])
                self.assertEqual(result["wheel_delta"], ticks * 120)

    def test_invalid_wheel_ticks_rejected_before_any_cursor_move(self):
        self.args.action = "wheel"
        for ticks in (-9, 0, 9):
            with self.subTest(ticks=ticks):
                self.args.ticks = ticks
                self.rejects("Wheel ticks", before_move=True)

    def test_keyboard_aliases_match_native_actions(self):
        self.assertEqual(control.KEYS["p"], 0x50)
        self.assertEqual(control.KEYS["r"], 0x52)
        self.assertEqual(control.KEYS["backspace"], 0x08)


if __name__ == "__main__":
    unittest.main()
