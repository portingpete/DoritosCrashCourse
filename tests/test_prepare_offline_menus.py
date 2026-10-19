"""Offline-menu timing contracts from the user's private original package.

No original Lua chunk is embedded in the repository. The small executor below
supports only instructions used by the authored commands under test.
"""
from __future__ import annotations

from copy import deepcopy
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_offline_menus as menus
from inspect_pak import PakError, read_directory, read_entry

SKIP_CHUNK = 0x2B3BD
ACTIVATION_CHUNK = 0x2AE5D
LIVE_ACTIVATION_CHUNK = 0x1CDBF
LIVE_ACTIVATION_WORDS = [0x00000005, 0x00004041, 0x0100401C,
                         0x00008005, 0x0040C006, 0x82414009, 0x0080001E]
MAIN_MENU_CHILDREN = ["", "bt_return", "bt_achievements", "bt_leaderboards",
                      "bt_help", "bt_online", "bt_multiplayer", "bt_singleplayer", ""]
PRE_SKIP_GROUP_HASH = "a92b0a182956c358e75a65023462a8f6ee98a88be8975015f6fa3b064f3c5030"
PRE_SKIP_SCRIPTS_HASH = "37bfd8f3c789bf027097f5aeb1b992d67af283284278f55059b499f9c1ac42a7"
EXPECTED_EVENTS = [("cancel-delays",), ("close",),
                   ("display", "menumusic", "menus/menu_music"),
                   ("display", "gui", "loading/mainmenu")]
MENU_TILE_NAMES = [
    "menus/main/buttons/singleplayer", "menus/main/buttons/multiplayer",
    "menus/main/buttons/leaderboards", "menus/main/buttons/helpandoptions",
    "menus/main/buttons/achievements",
    "menus/help_and_options/buttons/how_to_play", "menus/help_and_options/buttons/controls",
    "menus/help_and_options/buttons/game_settings", "menus/help_and_options/buttons/credits",
    "pause/buttons/resume", "pause/buttons/back_to_main_menu", "pause/buttons/achievements",
    "pause/buttons/helpandoptions", "pause/buttons/leaderboards",
]


def named_tile_mouse_area(data, name):
    # Independent record decoding survives earlier variable-length navigation
    # patches. Presence/count markers separate the two vec2 properties.
    encoded = name.encode("ascii")
    header = b"0A\x00" + struct.pack("<I", len(encoded)) + encoded
    if data.count(header) != 1:
        raise AssertionError("Named tile record is ambiguous")
    start = data.index(header)
    end = data.find(b"0A\x00", start + len(header))
    if end < 0:
        end = data.index(b"0_bin\0", start + len(header))
    offset_property = b"1\x01\x00" + struct.pack("<2f", 0, 0) + b"1\x01\x00"
    positions = [start + at for at in range(len(header), end - start - len(offset_property) - 8)
                 if data[start + at:start + at + len(offset_property)] == offset_property]
    # The serialized tail uniquely identifies the adjacent clickable bounds;
    # optional group child arrays earlier in the record are not candidates.
    positions = [at for at in positions if data[at + len(offset_property) + 8:
                                               at + len(offset_property) + 12] == b"1\x01\x00\x00"]
    if len(positions) != 1:
        raise AssertionError("Named tile mouse properties are ambiguous")
    area = positions[0] + len(offset_property)
    if data[end - 11:end - 8] != b"1\x01\x00":
        raise AssertionError("Named tile rendered-size property differs")
    return area, struct.unpack_from("<2f", data, area), struct.unpack_from("<2f", data, end - 8)


def pause_child_transforms(data):
    # Original GUIGROUPMASTER descriptor order: Control resource strings,
    # ControlPos vec2 array, ControlSize vec2 array, ControlOrientation floats.
    name = b"pause/buttons/buttons"
    header = b"0A\x00" + struct.pack("<I", len(name)) + name
    if data.count(header) != 1:
        raise AssertionError("Pause parent record is ambiguous")
    start = data.index(header)
    end = data.index(b"0A\x00", start + len(header))
    resources = ["GPCT:screen_reference", "GGRP:pause/buttons/back_to_main_menu",
                 "GGRP:pause/buttons/helpandoptions", "GGRP:pause/buttons/achievements",
                 "GGRP:pause/buttons/leaderboards", "GGRP:pause/buttons/resume", "GGRP:empty"]
    array_header = b"1" + struct.pack("<H", len(resources))
    control_array = array_header + b"".join(struct.pack("<I", len(resource)) + resource.encode("ascii")
                                           for resource in resources)
    if data[start:end].count(control_array) != 1:
        raise AssertionError("Pause parent original Control resource array differs")
    position = data.index(control_array, start, end) + len(control_array)
    arrays = []
    for width in (2, 2, 1):
        size = len(resources) * width * 4
        if position + 3 + size > end or data[position:position + 3] != array_header:
            raise AssertionError("Pause parent bounded child transform array differs")
        position += 3
        values = struct.unpack_from("<" + str(len(resources) * width) + "f", data, position)
        arrays.append([values[index * width:(index + 1) * width] for index in range(len(resources))])
        position += size
    return {resource[5:]: (arrays[0][index], arrays[1][index], arrays[2][index][0])
            for index, resource in enumerate(resources) if resource.startswith("GGRP:pause/buttons/")}


class WaitRequest:
    def __init__(self, seconds):
        self.seconds = seconds


def execute_live_activation(function, globals_):
    # Independent Lua 5.1 decoding, with a host Wait represented by a yield.
    # Each next() runs until the coroutine gives the renderer a turn or exits.
    registers = [None] * 256
    pc = 0

    def rk(operand):
        return function.constants[operand & 255] if operand & 256 else registers[operand]

    for _ in range(32):
        if not 0 <= pc < len(function.words):
            raise AssertionError("Live activation left its instruction array")
        instruction = function.words[pc]
        pc += 1
        opcode, a = instruction & 63, instruction >> 6 & 255
        b, c = instruction >> 23 & 511, instruction >> 14 & 511
        bx = instruction >> 14 & 0x3FFFF
        if opcode == 0:  # MOVE
            registers[a] = registers[b]
        elif opcode == 1:  # LOADK
            registers[a] = function.constants[bx]
        elif opcode == 5:  # GETGLOBAL
            registers[a] = globals_[function.constants[bx]]
        elif opcode == 6:  # GETTABLE
            registers[a] = registers[b][rk(c)]
        elif opcode == 9:  # SETTABLE
            registers[a][rk(b)] = rk(c)
        elif opcode == 22:  # JMP; offset is relative to the following word.
            pc += bx - 131071
        elif opcode == 28:  # CALL, with fixed arguments and no results.
            if not b or c != 1:
                raise AssertionError("Unsupported Live-activation call")
            result = registers[a](*registers[a + 1:a + b])
            if isinstance(result, WaitRequest):
                yield result.seconds
        elif opcode == 30:  # RETURN, with no result in this command.
            if b != 1:
                raise AssertionError("Unsupported Live-activation return")
            return
        else:
            raise AssertionError(f"Unsupported Live-activation opcode {opcode}")
    raise AssertionError("Live activation exceeded the bounded instruction budget")


def live_host(*, group_visible=1.0, frame_visible=1.0, include_wait=True):
    events = []
    group = {"Visible": group_visible, "frame": {"Visible": frame_visible}}

    def wait(seconds):
        events.append(("wait", seconds))
        return WaitRequest(seconds)

    globals_ = {"me": group}
    if include_wait:
        globals_["Wait"] = wait
    return globals_, group, events


def initial_main_menu_visibility(data):
    # Locate the parent and its independently decoded child-name table. Both
    # survive navigation-string resizing, so this does not assume old offsets.
    parent_name = b"menus/main/buttons/buttons"
    parent = b"0A\x00" + struct.pack("<I", len(parent_name)) + parent_name
    names = b"1" + struct.pack("<H", len(MAIN_MENU_CHILDREN)) + b"".join(
        struct.pack("<I", len(name)) + name.encode("ascii") for name in MAIN_MENU_CHILDREN)
    if data.count(parent) != 1 or data.count(names) != 1:
        raise AssertionError("Main-menu parent/child-name record is ambiguous")
    parent_offset, names_offset = data.index(parent), data.index(names)
    if not 0 < names_offset - parent_offset < 1024:
        raise AssertionError("Main-menu child-name table left its parent record")
    position = names_offset + len(names)
    # The child names are followed by integer order, float scale, then boolean
    # visibility arrays. Verify their presence/count fields before advancing.
    header = b"1" + struct.pack("<H", len(MAIN_MENU_CHILDREN))
    for _ in range(2):
        if data[position:position + 3] != header:
            raise AssertionError("Main-menu child-property array differs")
        position += 3 + len(MAIN_MENU_CHILDREN) * 4
    if data[position:position + 3] != header:
        raise AssertionError("Main-menu initial visibility header differs")
    position += 3
    values = list(data[position:position + len(MAIN_MENU_CHILDREN)])
    if len(values) != len(MAIN_MENU_CHILDREN) or any(value not in (0, 1) for value in values):
        raise AssertionError("Main-menu initial visibility values differ")
    return position, dict(zip(MAIN_MENU_CHILDREN, values))


def execute_skip(function, globals_):
    # Independent Lua 5.1 field/VM semantics:
    # https://www.lua.org/source/5.1/lopcodes.h.html
    # https://www.lua.org/source/5.1/lvm.c.html
    # This is a bounded test oracle, not a runtime used by the native game.
    registers = [None] * 256
    pc = 0

    def rk(operand):
        return function.constants[operand & 255] if operand & 256 else registers[operand]

    for _ in range(128):
        if not 0 <= pc < len(function.words):
            raise AssertionError("Skip command left its instruction array")
        instruction = function.words[pc]
        pc += 1
        opcode, a = instruction & 63, instruction >> 6 & 255
        b, c = instruction >> 23 & 511, instruction >> 14 & 511
        bx = instruction >> 14 & 0x3FFFF
        if opcode == 1:  # LOADK
            registers[a] = function.constants[bx]
        elif opcode == 5:  # GETGLOBAL
            registers[a] = globals_[function.constants[bx]]
        elif opcode == 6:  # GETTABLE
            registers[a] = registers[b][rk(c)]
        elif opcode == 22:  # JMP; offset is relative to the following word.
            pc += bx - 131071
        elif opcode == 26:  # TEST; nil and false are the only false Lua values.
            truth = registers[a] is not None and registers[a] is not False
            if truth != bool(c):
                pc += 1
        elif opcode == 28:  # CALL, with fixed argument and result counts.
            if not b or c not in (1, 2):
                raise AssertionError("Unsupported open/multiple-result call")
            result = registers[a](*registers[a + 1:a + b])
            if c == 2:
                registers[a] = result
        elif opcode == 30:  # RETURN, with no result in this command.
            if b != 1:
                raise AssertionError("Unsupported skip-command return")
            return
        else:
            raise AssertionError(f"Unsupported skip-command opcode {opcode}")
    raise AssertionError("Skip command exceeded the bounded instruction budget")


def skip_host(is_dev, *, include_predicate=True):
    events, pending = [], ["microsoft", "snackstrong", "a2m", "esrb"]
    predicate_calls = []

    def predicate():
        predicate_calls.append(is_dev)
        return is_dev

    def cancel_delays():
        events.append(("cancel-delays",))
        pending.clear()

    def close():
        if pending:
            raise AssertionError("The logo owner closed before cancelling its delayed sequence")
        events.append(("close",))

    def display(layer, group):
        events.append(("display", layer, group))

    globals_ = {"me": {"CleanMyDelays": cancel_delays, "Close": close},
                "gui": {"DisplayGroup": display}}
    if include_predicate:
        globals_["IsDev"] = predicate
    return globals_, events, pending, predicate_calls


@unittest.skipUnless((ROOT / "game/data.pak").is_file(), "Private original package unavailable")
class PrivateOpeningLogoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        package = ROOT / "game/data.pak"
        with package.open("rb") as source:
            if hashlib.file_digest(source, "sha256").hexdigest() != menus.PAK_SHA256:
                raise AssertionError("Private test package differs from the supported original")
        entries = {entry.name: entry for entry in read_directory(package)}
        cls.group = read_entry(package, entries[menus.GROUP])
        cls.scripts = read_entry(package, entries[menus.SCRIPTS])
        cls.original = menus.chunks(cls.group)
        cls.output, cls.metadata = menus.prepare(cls.group, menus.GROUP)
        cls.modified = menus.chunks(cls.output)

    def paired_chunk(self, offset):
        # Earlier navigation-string edits can shift intervening chunks. Pair
        # by the unchanged record order, not by assuming all offsets survive.
        index = list(self.original).index(offset)
        return self.original[offset], list(self.modified.values())[index]

    def test_restoring_skip_visibility_and_tile_targets_recovers_prior_overlay(self):
        (original, body), (modified, changed_body) = self.paired_chunk(SKIP_CHUNK)
        self.assertEqual(len(self.original), len(self.modified))
        self.assertEqual(len(original.words), 21)
        self.assertEqual(modified.constants, original.constants)
        self.assertEqual(original.words[0], 0x00000005)
        self.assertEqual(modified.words[0], 0x80008016)
        self.assertEqual(modified.words[1:], original.words[1:])
        relative = original.word_offsets[0] - SKIP_CHUNK
        self.assertEqual(changed_body[:relative], body[:relative])
        self.assertEqual(changed_body[relative + 4:], body[relative + 4:])
        restored = bytearray(self.output)
        struct.pack_into("<I", restored, modified.word_offsets[0], original.words[0])
        (live_original, _), (live_modified, _) = self.paired_chunk(LIVE_ACTIVATION_CHUNK)
        struct.pack_into("<I", restored, live_modified.word_offsets[0], live_original.words[0])
        before_position, before_visibility = initial_main_menu_visibility(self.group)
        after_position, after_visibility = initial_main_menu_visibility(self.output)
        self.assertEqual(before_position + MAIN_MENU_CHILDREN.index("bt_online"), 0x4B87)
        self.assertEqual(before_visibility["bt_online"], 1)
        self.assertEqual(after_visibility["bt_online"], 0)
        restored[after_position + MAIN_MENU_CHILDREN.index("bt_online")] = 1
        for tile in MENU_TILE_NAMES:
            _, before_area, _ = named_tile_mouse_area(self.group, tile)
            after_area, _, _ = named_tile_mouse_area(self.output, tile)
            struct.pack_into("<2f", restored, after_area, *before_area)
        # This pinned pre-change whole-file digest also checks non-Lua records,
        # prior offline patches and every unrelated/protected Lua chunk.
        self.assertEqual(menus.digest(restored), PRE_SKIP_GROUP_HASH)
        scripts, _ = menus.prepare(self.scripts, menus.SCRIPTS)
        self.assertEqual(menus.digest(scripts), PRE_SKIP_SCRIPTS_HASH)

    def test_visible_tiles_have_full_interior_targets_and_unchanged_rendered_sizes(self):
        self.assertEqual(set(menus.TILE_MOUSE_AREAS), set(MENU_TILE_NAMES))
        for tile in MENU_TILE_NAMES:
            with self.subTest(tile=tile):
                _, old_area, old_size = named_tile_mouse_area(self.group, tile)
                _, new_area, new_size = named_tile_mouse_area(self.output, tile)
                self.assertAlmostEqual(old_area[0], 0.15)
                self.assertAlmostEqual(old_area[1], 1.5 if "/help_and_options/" in tile else 1)
                self.assertAlmostEqual(new_area[0], 0.235)
                self.assertAlmostEqual(new_area[1], 2.5)
                self.assertEqual(new_size, old_size)
                self.assertAlmostEqual(new_size[0], 0.8)
                self.assertAlmostEqual(new_size[1], 0.07)

                # Independent original 822B3D98 hit-test formula at scale 1:
                # local pointer / master Size lies in +/- MouseAreaSize / 2.
                def hit(dx, dy, dimensions):
                    return (abs(dx / old_size[0]) <= dimensions[0] / 2 and
                            abs(dy / old_size[1]) <= dimensions[1] / 2)

                for dx in (-0.08, 0, 0.08):
                    for dy in (-0.07, 0, 0.07):
                        self.assertTrue(hit(dx, dy, new_area), (tile, dx, dy))
                self.assertFalse(hit(0.08, 0, old_area), "Prior text strip misses visible tile sides")
                self.assertFalse(hit(0, 0.07, old_area), "Prior text strip misses visible tile top/bottom")
                self.assertFalse(hit(0.10, 0, new_area), "Neighboring horizontal tile stays outside")
                self.assertFalse(hit(0, 0.10, new_area), "Neighboring vertical tile stays outside")

        areas = [record for record in self.metadata["patches"]
                 if record["purpose"].startswith("fit native mouse target")]
        self.assertEqual(len(areas), 14)
        self.assertTrue(all(record["before_bytes"] == record["after_bytes"] == 8 for record in areas))

    def test_tile_target_rectangles_keep_measured_neighbor_gaps(self):
        # Genuine stable main/help geometry from native guest inspection.
        layouts = [
            [("singleplayer", .6911, .309), ("multiplayer", .6911, .5004),
             ("leaderboards", .8816, .5004), ("helpandoptions", .8818, .692),
             ("achievements", .6911, .692)],
            [("how_to_play", .4983, .3075), ("controls", .3075, .5),
             ("game_settings", .6892, .5), ("credits", .4987, .6925)],
        ]
        for prefix, layout in zip(("menus/main/buttons/", "menus/help_and_options/buttons/"), layouts):
            bounds = []
            for suffix, x, y in layout:
                _, area, size = named_tile_mouse_area(self.output, prefix + suffix)
                bounds.append((x - size[0] * area[0] / 2, y - size[1] * area[1] / 2,
                               x + size[0] * area[0] / 2, y + size[1] * area[1] / 2))
            for first in range(len(bounds)):
                for second in range(first + 1, len(bounds)):
                    a, b = bounds[first], bounds[second]
                    self.assertTrue(a[2] < b[0] or b[2] < a[0] or a[3] < b[1] or b[3] < a[1],
                                    (prefix, layout[first][0], layout[second][0]))

    def test_pause_tile_targets_keep_original_child_layout_and_neighbor_gaps(self):
        original = pause_child_transforms(self.group)
        modified = pause_child_transforms(self.output)
        self.assertEqual(modified, original)
        self.assertEqual(set(modified), {tile for tile in MENU_TILE_NAMES if tile.startswith("pause/")})
        bounds = {}
        for tile, ((x, y), (sx, sy), angle) in modified.items():
            # All five children share this parent, so its common translation
            # and invertible transform preserve separation. The actual child
            # arrays prove no individual scale/rotation changes this geometry.
            self.assertEqual((sx, sy, angle), (1.0, 1.0, 0.0))
            _, area, size = named_tile_mouse_area(self.output, tile)
            half_x, half_y = size[0] * area[0] / 2, size[1] * area[1] / 2
            bounds[tile] = (x - half_x, y - half_y, x + half_x, y + half_y)
        names = list(bounds)
        for first in range(len(names)):
            for second in range(first + 1, len(names)):
                a, b = bounds[names[first]], bounds[names[second]]
                self.assertTrue(a[2] < b[0] or b[2] < a[0] or a[3] < b[1] or b[3] < a[1],
                                (names[first], names[second]))

    def test_corrupt_named_tile_mouse_bounds_and_master_size_are_rejected(self):
        for tile, (start, end, area, _) in menus.TILE_MOUSE_AREAS.items():
            # Exercise each identity, presence/count, origin, extent and master
            # render-size guard after bypassing only the outer source digest.
            for location in (start + 7, area - 14, area - 13, area - 11,
                             area - 3, area - 2, area, area + 4, end - 8, end - 4):
                with self.subTest(tile=tile, original_byte=hex(location)):
                    changed = bytearray(self.group)
                    changed[location] ^= 1
                    with patch.dict(menus.INPUTS, {menus.GROUP: menus.digest(changed)}):
                        with self.assertRaisesRegex(PakError, "menu tile mouse-area record"):
                            menus.prepare(bytes(changed), menus.GROUP)

    def test_live_default_visibility_hides_only_online_before_activation(self):
        before_position, before = initial_main_menu_visibility(self.group)
        after_position, after = initial_main_menu_visibility(self.output)
        self.assertEqual(self.group[before_position - 3:before_position + 9],
                         b"1\x09\x00\x01\x00\x01\x01\x01\x01\x01\x01\x00")
        self.assertEqual(self.output[after_position - 3:after_position + 9],
                         b"1\x09\x00\x01\x00\x01\x01\x01\x00\x01\x01\x00")
        self.assertEqual({name: value for name, value in after.items() if name != "bt_online"},
                         {name: value for name, value in before.items() if name != "bt_online"})
        _, group, _ = live_host(group_visible=after["bt_online"])
        # The renderer can see this initial object before Lua is scheduled.
        self.assertEqual(group["Visible"], 0)
        self.assertEqual(group["frame"]["Visible"], 1)

    def test_live_activation_jump_skips_wait_and_loads_group(self):
        (original, _), (modified, _) = self.paired_chunk(LIVE_ACTIVATION_CHUNK)
        self.assertEqual(original.words, LIVE_ACTIVATION_WORDS)
        self.assertEqual(original.constants, ["Wait", 0.4, "me", "frame", "Visible", 1.0])
        self.assertEqual(modified.words[0], 0x80004016)
        word = modified.words[0]
        self.assertEqual(word & 63, 22)
        self.assertEqual(word >> 6 & 255, 0)
        self.assertEqual(1 + ((word >> 14 & 0x3FFFF) - 131071), 3)
        self.assertEqual(modified.words[1:4], original.words[1:4])
        self.assertEqual(modified.words[4], 0)  # MOVE r0,r0 retains me.
        self.assertEqual(modified.words[5:], original.words[5:])
        self.assertEqual(modified.constants[:-1], original.constants[:-1])
        self.assertEqual(modified.constants[-1], 0.0)

    def test_original_and_delayed_overlay_leave_visible_group_at_first_yield(self):
        (original, _), _ = self.paired_chunk(LIVE_ACTIVATION_CHUNK)
        delayed = deepcopy(original)
        delayed.words[4] = 0
        delayed.constants[5] = 0.0
        for name, function in (("original", original), ("delayed overlay", delayed)):
            with self.subTest(variant=name):
                globals_, group, events = live_host()
                coroutine = execute_live_activation(function, globals_)
                self.assertEqual(next(coroutine), 0.4)
                self.assertEqual(group["Visible"], 1.0)
                self.assertEqual(group["frame"]["Visible"], 1.0)
                self.assertEqual(events, [("wait", 0.4)])
                with self.assertRaises(StopIteration):
                    next(coroutine)
                self.assertEqual(group["Visible"], 1.0 if name == "original" else 0.0)
                self.assertEqual(group["frame"]["Visible"], 1.0)

    def test_immediate_live_hide_never_yields_or_requires_wait_on_reactivation(self):
        _, (modified, _) = self.paired_chunk(LIVE_ACTIVATION_CHUNK)
        _, initial = initial_main_menu_visibility(self.output)
        for starting_visibility in (1.0, initial["bt_online"]):
            with self.subTest(starting_visibility=starting_visibility):
                globals_, group, events = live_host(group_visible=starting_visibility, include_wait=False)
                for _ in range(3):
                    coroutine = execute_live_activation(modified, globals_)
                    with self.assertRaises(StopIteration):
                        next(coroutine)
                    self.assertEqual(group["Visible"], 0.0)
                    self.assertEqual(group["frame"]["Visible"], 1.0)
                    self.assertEqual(events, [])

    def test_corrupt_live_activation_and_initial_visibility_oracles_are_rejected(self):
        original = self.original[LIVE_ACTIVATION_CHUNK][0]
        for index in range(len(LIVE_ACTIVATION_WORDS)):
            with self.subTest(word=index):
                changed = bytearray(self.group)
                struct.pack_into("<I", changed, original.word_offsets[index], original.words[index] ^ 64)
                with patch.dict(menus.INPUTS, {menus.GROUP: menus.digest(changed)}):
                    with self.assertRaisesRegex(PakError, "Live-button activation"):
                        menus.prepare(bytes(changed), menus.GROUP)
        for index, replacement in ((0, b"WaiX"), (4, b"VisiblX")):
            with self.subTest(constant=index):
                changed = bytearray(self.group)
                location = original.constant_offsets[index] + 4
                changed[location:location + len(replacement)] = replacement
                with patch.dict(menus.INPUTS, {menus.GROUP: menus.digest(changed)}):
                    with self.assertRaisesRegex(PakError, "Live-button activation"):
                        menus.prepare(bytes(changed), menus.GROUP)
        changed = bytearray(self.group)
        struct.pack_into("<d", changed, original.constant_offsets[1], 0.5)
        with patch.dict(menus.INPUTS, {menus.GROUP: menus.digest(changed)}):
            with self.assertRaisesRegex(PakError, "Live-button activation"):
                menus.prepare(bytes(changed), menus.GROUP)
        for location in range(0x4B7F, 0x4B8B):
            with self.subTest(initial_property_byte=hex(location)):
                changed = bytearray(self.group)
                changed[location] ^= 1
                with patch.dict(menus.INPUTS, {menus.GROUP: menus.digest(changed)}):
                    with self.assertRaisesRegex(PakError, "main-menu control visibility"):
                        menus.prepare(bytes(changed), menus.GROUP)

    def test_jump_lands_on_cleanup_and_natural_sequence_and_a_binding_survive(self):
        (original, _), (modified, _) = self.paired_chunk(SKIP_CHUNK)
        word = modified.words[0]
        self.assertEqual(word & 63, 22)
        self.assertEqual(word >> 6 & 255, 0)
        self.assertEqual(1 + ((word >> 14 & 0x3FFFF) - 131071), 4)
        self.assertEqual(original.constants[1:4], ["me", "CleanMyDelays", "Close"])
        (_, natural), (_, after_natural) = self.paired_chunk(ACTIVATION_CHUNK)
        self.assertEqual(after_natural, natural)
        binding = struct.pack("<I", 17) + b"buta=me.RunCmd(0)"
        self.assertEqual(self.group[0x2B509:0x2B509 + len(binding)], binding)
        self.assertEqual(self.output[0x2B509:0x2B509 + len(binding)], binding)

    def test_original_predicate_blocks_retail_skip_and_allows_developer_skip(self):
        (original, _), _ = self.paired_chunk(SKIP_CHUNK)
        for is_dev in (False, True):
            with self.subTest(is_dev=is_dev):
                globals_, events, pending, predicates = skip_host(is_dev)
                execute_skip(original, globals_)
                self.assertEqual(predicates, [is_dev])
                self.assertEqual(events, EXPECTED_EVENTS if is_dev else [])
                self.assertEqual(pending, [] if is_dev else ["microsoft", "snackstrong", "a2m", "esrb"])

    def test_enabled_skip_cancels_closes_then_loads_music_and_menu_without_isdev(self):
        _, (modified, _) = self.paired_chunk(SKIP_CHUNK)
        for is_dev in (False, True):
            with self.subTest(is_dev=is_dev):
                globals_, events, pending, predicates = skip_host(is_dev)
                execute_skip(modified, globals_)
                self.assertEqual(events, EXPECTED_EVENTS)
                self.assertEqual(pending, [])
                self.assertEqual(predicates, [])
        globals_, events, pending, predicates = skip_host(False, include_predicate=False)
        execute_skip(modified, globals_)
        self.assertEqual(events, EXPECTED_EVENTS)
        self.assertEqual(pending, [])
        self.assertEqual(predicates, [])

    def test_corrupt_original_hash_and_exact_predicate_oracles_are_rejected(self):
        original = self.original[SKIP_CHUNK][0]
        changed = bytearray(self.group)
        struct.pack_into("<I", changed, original.word_offsets[-1], 0)
        with self.assertRaisesRegex(PakError, "supported original"):
            menus.prepare(bytes(changed), menus.GROUP)
        for index in range(4):
            with self.subTest(word=index):
                changed = bytearray(self.group)
                struct.pack_into("<I", changed, original.word_offsets[index], 0)
                # Bypass only the whole-source digest to exercise the narrower
                # exact skip-handler predicate, rather than its outer gate.
                with patch.dict(menus.INPUTS, {menus.GROUP: menus.digest(changed)}):
                    with self.assertRaises(PakError):
                        menus.prepare(bytes(changed), menus.GROUP)
        changed = bytearray(self.group)
        location = original.constant_offsets[0] + 4
        changed[location:location + 5] = b"IsDeX"
        with patch.dict(menus.INPUTS, {menus.GROUP: menus.digest(changed)}):
            with self.assertRaisesRegex(PakError, "opening-logo skip handler"):
                menus.prepare(bytes(changed), menus.GROUP)


def help_children(data):
    # Independent reader for this supported original parent's actual parallel
    # child arrays. Do not reuse the generator's offsets or decoding helpers.
    name = b"menus/help_and_options/buttons/buttons"
    header = b"0A\0" + struct.pack("<I", len(name)) + name
    start = data.index(header)
    end = data.index(b"0A\0", start + len(header))
    first = b"GGRP:menus/help_and_options/buttons/credits"
    position = data.index(struct.pack("<I", len(first)) + first, start, end) - 3
    count = struct.unpack_from("<H", data, position + 1)[0]
    if count not in (5, 6) or data[position] != ord("1"):
        raise AssertionError("Help control array contract differs")
    position += 3

    def string():
        nonlocal position
        length = struct.unpack_from("<I", data, position)[0]
        position += 4
        result = data[position:position + length].decode("ascii")
        position += length
        return result

    resources = [string() for _ in range(count)]
    fields = []
    for kind, width in (("f", 2), ("f", 2), ("f", 1), (None, 1), ("I", 1),
                        ("f", 1), ("B", 1), ("B", 1), ("I", 1)):
        if data[position:position + 3] != b"1" + struct.pack("<H", count):
            raise AssertionError("Help parallel child array differs")
        position += 3
        if kind is None:
            values = [string() for _ in range(count)]
        else:
            values = []
            for _ in range(count):
                values.append(struct.unpack_from("<" + str(width) + kind, data, position))
                position += width * struct.calcsize("<" + kind)
        fields.append(values)
    return {resource: tuple(values[index] for values in fields) for index, resource in enumerate(resources)}


@unittest.skipUnless((ROOT / "game/data.pak").is_file(), "Private original package unavailable")
class PrivateNativeVideoMenuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        package = ROOT / "game/data.pak"
        entries = {entry.name: entry for entry in read_directory(package)}
        group = read_entry(package, entries[menus.GROUP])
        cls.base, _ = menus.prepare(group, menus.GROUP)
        cls.output, cls.metadata = menus.prepare_video_menu(cls.base, menus.GROUP)
        cls.label = read_entry(package, entries[menus.LABELS])
        cls.strings = read_entry(package, entries[menus.STRINGS])

    def test_four_original_help_controls_preserved_and_fifth_is_navigable(self):
        original = help_children(self.base)
        modified = help_children(self.output)
        new_resource = "GGRP:" + menus.VIDEO_TILE
        self.assertEqual(len(original), 5)
        self.assertEqual(len(modified), 6)
        self.assertEqual({key: modified[key] for key in original}, original)
        video = modified[new_resource]
        self.assertEqual(video[3], "bt_video")
        self.assertAlmostEqual(video[0][0], 0.1907)
        self.assertAlmostEqual(video[0][1], 0.1925)
        self.assertEqual(video[1:], ((1, 1), (0,), "bt_video", (1,), (1,), (1,), (1,), (0,)))
        for tile, navigation in (("game_settings", "down=me.parent.bt_video.ForceMouseOver()"),
                                 ("credits", "right=me.parent.bt_video.ForceMouseOver()"),
                                 ("native_video_settings", "up=me.parent.bt_setings.ForceMouseOver()"),
                                 ("native_video_settings", "left=me.parent.bt_credits.ForceMouseOver()")):
            name = ("menus/help_and_options/buttons/" + tile).encode("ascii")
            start = self.output.index(b"0A\0" + struct.pack("<I", len(name)) + name)
            end = self.output.index(b"0A\0", start + len(name) + 7) if tile != "native_video_settings" else len(self.output) - 6
            self.assertIn(navigation.encode("ascii"), self.output[start:end])

    def test_original_lua_and_settings_actions_remain_byte_identical(self):
        before = list(menus.chunks(self.base).values())
        after = list(menus.chunks(self.output).values())
        self.assertEqual(len(after), len(before) + 4)
        self.assertEqual([body for _, body in after[:len(before)]], [body for _, body in before])
        settings = [body for function, body in before
                    if "menus/settings/actions/save_settings" in function.constants or
                    "menus/settings/actions/reset_to_defaults" in function.constants]
        self.assertEqual(len(settings), 2)
        self.assertTrue(all(body in self.output for body in settings))
        functions = [function for function, _ in after[-4:]]
        self.assertEqual(functions[1].constants, ["me", "RunCmd", 3.0])
        calls = []
        self.assertEqual(list(execute_live_activation(functions[1], {"me": {"RunCmd": calls.append}})), [])
        self.assertEqual(calls, [3])
        self.assertEqual(functions[2].constants[-1], menus.VIDEO_TOOLTIP)
        self.assertNotIn("Transition", functions[1].constants)

    def test_new_tile_has_full_hit_area_and_neighbor_gaps(self):
        _, area, size = named_tile_mouse_area(self.output, menus.VIDEO_TILE)
        self.assertAlmostEqual(area[0], 0.235)
        self.assertAlmostEqual(area[1], 2.5)
        self.assertAlmostEqual(size[0], 0.8)
        self.assertAlmostEqual(size[1], 0.07)
        children = help_children(self.output)
        half_x, half_y = area[0] * size[0] / 2, area[1] * size[1] / 2
        native = children["GGRP:" + menus.VIDEO_TILE][0]
        for key, fields in children.items():
            if key.startswith("GPCT:") or key == "GGRP:" + menus.VIDEO_TILE:
                continue
            other = fields[0]
            self.assertTrue(abs(native[0] - other[0]) > 2 * half_x or
                            abs(native[1] - other[1]) > 2 * half_y, key)

    def test_labels_and_strings_append_only_owned_native_records(self):
        label, _ = menus.prepare_video_menu(self.label, menus.LABELS)
        strings, _ = menus.prepare_video_menu(self.strings, menus.STRINGS)
        for original, modified, class_id, first_name, added in (
                (self.label, label, 0x47, "_dev/info", 1),
                (self.strings, strings, 0x58, "empty_text", 2)):
            # Independent location: the first class/name declaration follows
            # the little-endian object count, not an uncounted EOF list.
            first = struct.pack("<HI", class_id, len(first_name)) + first_name.encode("ascii")
            offset = original.index(first) - 2
            before = struct.unpack_from("<H", original, offset)[0]
            self.assertEqual(struct.unpack_from("<H", modified, offset)[0], before + added)
            restored = bytearray(modified[:len(original) - 6])
            struct.pack_into("<H", restored, offset, before)
            self.assertEqual(restored, original[:-6])
        self.assertTrue(label.endswith(b"0_bin\0"))
        self.assertTrue(strings.endswith(b"0_bin\0"))
        self.assertEqual(label.count(menus.VIDEO_LABEL.encode("ascii")), 2)
        self.assertEqual(strings.count(menus.VIDEO_LABEL.encode("ascii")), 1)
        self.assertIn("Video\r\nSettings".encode("utf-16le"), strings)

    def test_declared_object_count_includes_native_tile_and_omission_is_rejected(self):
        first = struct.pack("<HI", 0x41, 4) + b"Init"
        offset = self.base.index(first) - 2
        self.assertEqual(struct.unpack_from("<H", self.output, offset)[0],
                         struct.unpack_from("<H", self.base, offset)[0] + 1)
        changed = bytearray(self.output)
        struct.pack_into("<H", changed, offset, struct.unpack_from("<H", self.base, offset)[0])
        with self.assertRaises(PakError):
            menus._template_layout(bytes(changed))

    def test_video_source_mutation_or_double_derivation_is_rejected(self):
        for name, data in ((menus.GROUP, self.base), (menus.LABELS, self.label), (menus.STRINGS, self.strings)):
            with self.subTest(name=name):
                changed = bytearray(data); changed[-1] ^= 1
                with self.assertRaisesRegex(PakError, "supported input"):
                    menus.prepare_video_menu(bytes(changed), name)
                output, _ = menus.prepare_video_menu(data, name)
                with self.assertRaisesRegex(PakError, "supported input"):
                    menus.prepare_video_menu(output, name)


class OfflineMenuRejectionTests(unittest.TestCase):
    def test_unsupported_source_identity_and_name_fail_before_lua_parsing(self):
        with patch.object(menus, "chunks", side_effect=AssertionError("Parser must not run")):
            for name in (menus.GROUP, menus.SCRIPTS, "data/templates/unowned.wbr"):
                with self.subTest(name=name), self.assertRaisesRegex(PakError, "supported original"):
                    menus.prepare(b"unowned synthetic bytes", name)

    def test_unsupported_package_creates_no_output_and_does_not_read_its_directory(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build_native") as directory:
            root = Path(directory)
            package = root / "unsupported.pak"
            package.write_bytes(b"owned synthetic unsupported package")
            output = root / "candidate"
            with patch.object(sys, "argv", ["prepare_offline_menus", "--pak", str(package),
                                              "--output", str(output)]), \
                 patch.object(menus, "read_directory", side_effect=AssertionError("Directory must not be read")):
                with self.assertRaisesRegex(PakError, "Unsupported original package identity"):
                    menus.main()
            self.assertFalse(output.exists())
            self.assertEqual(package.read_bytes(), b"owned synthetic unsupported package")


if __name__ == "__main__":
    unittest.main()
