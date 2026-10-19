"""Derive a bounded offline-menu overlay from the user's supported original package.

The original package is never modified. Derived WBR files belong beneath the
ignored build_native directory and must not be redistributed with this tool.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct

from inspect_pak import PakError, _no_links, read_directory, read_entry

ROOT = Path(__file__).resolve().parents[1]
PAK_SHA256 = "0819b4e9f90695b70e083690e98ac6f76f0df0cf2bbb817f196316c930a54fc3"
GROUP = "data/templates/guigroup.wbr"
SCRIPTS = "data/templates/scripts.wbr"
INPUTS = {
    GROUP: "6d73541913bcbec5ca3d1ce1ecddab7672565a547c861c3b1dd82c04246a43cc",
    SCRIPTS: "64e59781f59ea557dc6d879f704ad3b082a48d275a548d2de1497dabeb5e9501",
}
LABELS = "data/templates/guilabel.wbr"
STRINGS = "data/templates/unistrings.wbr"
VIDEO_INPUTS = {
    GROUP: "be8dc480be10e84d0606ec50fd00e447c4c31a7c4b67f421a3773297b1c1a26f",
    LABELS: "f04670af3d255487df7312af9c3c431d381988089b55533f8de97e16941ad0d1",
    STRINGS: "67568bfc166f1700d64ad37c21c0e016b1d6d3502958f9505c1769b1db71fb6f",
}
VIDEO_TILE = "menus/help_and_options/buttons/native_video_settings"
VIDEO_LABEL = "menus/help_and_options/native_video_settings"
VIDEO_TOOLTIP = "menus/help_and_options/tooltips/native_video_settings"
LUA_HEADER = bytes.fromhex("1b4c75615100010404040800")
RETURN = 0x0080001E

# Original GUIGROUP records verified against GUICLICKABLEMASTER's
# MouseAreaOffset/MouseAreaSize descriptors at 0x82A93238/0x82A93278.
# Each tuple is (record start, next record start, MouseAreaSize value, height).
# These are the visible blue tiles; zero-area containers and other controls
# retain their authored bounds. All fourteen masters have Size (.8, .07).
TILE_MOUSE_AREAS = {
    "menus/main/buttons/singleplayer": (0x406A, 0x488A, 0x4795, 1.0),
    "menus/main/buttons/multiplayer": (0x605F, 0x695A, 0x6858, 1.0),
    "menus/main/buttons/leaderboards": (0x695A, 0x7146, 0x7054, 1.0),
    "menus/main/buttons/helpandoptions": (0x7146, 0x7871, 0x7775, 1.0),
    "menus/main/buttons/achievements": (0x7871, 0x7DFD, 0x7D0C, 1.0),
    "menus/help_and_options/buttons/how_to_play": (0xA064, 0xA648, 0xA559, 1.5),
    "menus/help_and_options/buttons/controls": (0xA8E1, 0xAF19, 0xAE29, 1.5),
    "menus/help_and_options/buttons/game_settings": (0xAF19, 0xB57E, 0xB48C, 1.5),
    "menus/help_and_options/buttons/credits": (0xB57E, 0xBDF3, 0xBD00, 1.5),
    "pause/buttons/resume": (0x30EF0, 0x3156A, 0x3144F, 1.0),
    "pause/buttons/back_to_main_menu": (0x3156A, 0x31B06, 0x31A65, 1.0),
    "pause/buttons/achievements": (0x31B06, 0x32024, 0x31F7C, 1.0),
    "pause/buttons/helpandoptions": (0x32024, 0x325C3, 0x3251F, 1.0),
    "pause/buttons/leaderboards": (0x325C3, 0x32C38, 0x32B92, 1.0),
}
TILE_MOUSE_SIZE = (0.235, 2.5)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass
class Function:
    words: list[int]
    word_offsets: list[int]
    constants: list[object]
    constant_offsets: list[int]


class Reader:
    def __init__(self, data: bytes, position: int):
        self.data, self.position = data, position

    def take(self, size: int) -> bytes:
        if size < 0 or size > len(self.data) - self.position:
            raise PakError("Truncated original Lua chunk")
        result = self.data[self.position:self.position + size]
        self.position += size
        return result

    def count(self) -> int:
        result = struct.unpack("<I", self.take(4))[0]
        if result > 65536:
            raise PakError("Unbounded original Lua record")
        return result

    def string(self) -> str:
        size = self.count()
        if not size:
            return ""
        value = self.take(size)
        if value[-1] != 0:
            raise PakError("Original Lua string lacks its terminal NUL")
        return value[:-1].decode("utf-8")

    def function(self, depth: int = 0) -> Function:
        if depth > 16:
            raise PakError("Unbounded original Lua prototype nesting")
        self.string()
        self.take(8)  # Original first/last source lines.
        self.take(4)  # Original upvalues/arguments/varargs/register capacity.
        word_count = self.count()
        word_offsets = [self.position + index * 4 for index in range(word_count)]
        words = [struct.unpack("<I", self.take(4))[0] for _ in range(word_count)]
        constants, constant_offsets = [], []
        for _ in range(self.count()):
            kind = self.take(1)[0]
            constant_offsets.append(self.position)
            if kind == 0:
                constants.append(None)
            elif kind == 1:
                value = self.take(1)[0]
                if value > 1:
                    raise PakError("Invalid original Lua boolean")
                constants.append(bool(value))
            elif kind == 3:
                constants.append(struct.unpack("<d", self.take(8))[0])
            elif kind == 4:
                constants.append(self.string())
            else:
                raise PakError("Unknown original Lua constant type")
        for _ in range(self.count()):
            self.function(depth + 1)
        self.take(self.count() * 4)
        for _ in range(self.count()):
            self.string()
            self.take(8)
        for _ in range(self.count()):
            self.string()
        return Function(words, word_offsets, constants, constant_offsets)


def chunks(data: bytes) -> dict[int, tuple[Function, bytes]]:
    result = {}
    for match in re.finditer(re.escape(LUA_HEADER), data):
        offset = match.start()
        reader = Reader(data, offset + len(LUA_HEADER))
        function = reader.function()
        # WBR stores these chunks as counted strings including a terminal NUL.
        if offset < 4 or struct.unpack_from("<I", data, offset - 4)[0] != reader.position - offset + 1 or data[reader.position] != 0:
            raise PakError("Original WBR Lua counted-string contract differs")
        result[offset] = function, data[offset:reader.position]
    if not result:
        raise PakError("Original WBR has no verified Lua chunks")
    return result


def prepare(data: bytes, name: str) -> tuple[bytes, dict]:
    if name not in INPUTS or digest(data) != INPUTS[name]:
        raise PakError("Offline-menu source differs from its supported original")
    original = chunks(data)
    patches: list[tuple[int, bytes, bytes, str]] = []
    changed_chunks: set[int] = set()

    def word(chunk: int, index: int, before: int, after: int, purpose: str):
        function = original[chunk][0]
        if function.words[index] != before:
            raise PakError("Original menu instruction oracle differs")
        patches.append((function.word_offsets[index], struct.pack("<I", before), struct.pack("<I", after), purpose))
        changed_chunks.add(chunk)

    if name == GROUP:
        # The Xbox templates only hit-test a narrow text-sized strip. Native
        # pointer targets cover the tile interior, with gaps between adjacent
        # .1905-wide/.1914-high tile centers. Do not change the master Size:
        # it also determines rendering and normalization of these hit regions.
        for tile, (start, end, area, height) in TILE_MOUSE_AREAS.items():
            tile_name = tile.encode("ascii")
            header = b"0A\x00" + struct.pack("<I", len(tile_name)) + tile_name
            size = b"1\x01\x00" + struct.pack("<2f", 0.8, 0.07)
            original_area = struct.pack("<2f", 0.15, height)
            properties = (b"1\x01\x00" + struct.pack("<2f", 0.0, 0.0) +
                          b"1\x01\x00" + original_area)
            if (not start < area - 14 < area + 8 < end or
                    data[start:start + len(header)] != header or
                    data.count(header) != 1 or data[end:end + 3] != b"0A\x00" or
                    data[end - len(size):end] != size or
                    data[area - 14:area + 8] != properties or
                    data[start:end].count(properties) != 1):
                raise PakError(f"Original menu tile mouse-area record differs: {tile}")
            patches.append((area, original_area, struct.pack("<2f", *TILE_MOUSE_SIZE),
                            f"fit native mouse target to visible tile interior: {tile}"))

        # The shipped logos group already binds A to RunCmd(0), whose skip
        # handler cancels its pending delays before opening the normal menu.
        # Enable that authored handler by jumping past its developer-only
        # predicate; the timed activation sequence remains byte-identical.
        function = original[0x2B3BD][0]
        if (len(function.words) != 21 or function.constants != [
                "IsDev", "me", "CleanMyDelays", "Close", "gui", "DisplayGroup",
                "menumusic", "menus/menu_music", "loading/mainmenu"] or
                function.words[1:4] != [0x0080801C, 0x0000001A, 0x8003C016]):
            raise PakError("Original opening-logo skip handler differs")
        word(0x2B3BD, 0, 0x00000005, 0x80008016, "enable authored A-button opening-logo skip")

        function = original[0xE241][0]
        if function.constants[-2:] != ["gui", "AwardGamerPictures"]:
            raise PakError("Original menu-screen gamer-picture call differs")
        word(0xE241, 14, 0x00028005, RETURN, "omit unavailable Xbox dashboard picture award")
        producer_chunks = {0x623F, 0x19DEE, 0x1A846, 0x1B278, 0x22433, 0x2CB7A, 0x2D221, 0x40A70}
        writes = 0
        for offset, (function, _) in original.items():
            for index, instruction in enumerate(function.words):
                if instruction & 63 != 7:
                    continue
                constant = instruction >> 14
                if constant >= len(function.constants) or function.constants[constant] != "postingGameInLive":
                    continue
                if offset not in producer_chunks or index == 0 or instruction >> 6 & 255:
                    raise PakError("Unverified original Live-posting producer")
                word(offset, index - 1, 0x00800002, 0x00000002, "use authored offline Continue posting flag")
                writes += 1
        if writes != 14:
            raise PakError("Original Live-posting producer inventory differs")

        # The parent instantiates bt_online as visible before its activation
        # coroutine runs. Hide child 5 in its original nine-control visibility
        # array, so even the first rendered frame cannot expose the Live tile.
        visibility = b"1\x09\x00\x01\x00\x01\x01\x01\x01\x01\x01\x00"
        if data[0x4B7F:0x4B8B] != visibility:
            raise PakError("Original main-menu control visibility differs")
        patches.append((0x4B87, b"\x01", b"\x00", "instantiate Live button hidden before activation"))

        function = original[0x1CDBF][0]
        if (function.constants != ["Wait", 0.4, "me", "frame", "Visible", 1.0] or
                function.words != [0x00000005, 0x00004041, 0x0100401C,
                                   0x00008005, 0x0040C006, 0x82414009, RETURN]):
            raise PakError("Original Live-button activation differs")
        # JMP sBx=2 goes from word 0 to word 3, skipping Wait(0.4) and
        # hiding synchronously on every activation rather than after a yield.
        word(0x1CDBF, 0, 0x00000005, 0x80004016, "hide Live button without yielding to its original delay")
        word(0x1CDBF, 4, 0x0040C006, 0, "target Live button group visibility")
        number = function.constant_offsets[5]
        patches.append((number, struct.pack("<d", 1.0), struct.pack("<d", 0.0), "hide Live button group"))
        word(0x1CF65, 0, 0x00000007, RETURN, "disable hidden Live button action")

        navigation = {
            0x47AF: (180, "bt_help"),
            0x706E: (177, "bt_help"),
            0x778F: (187, "bt_leaderboards"),
        }
        for offset, (size, replacement) in navigation.items():
            if struct.unpack_from("<I", data, offset)[0] != size:
                raise PakError("Original main-menu navigation string size differs")
            before = data[offset + 4:offset + 4 + size]
            text = before.decode("ascii")
            if text.count("bt_online") != (2 if offset == 0x47AF else 1):
                raise PakError("Original main-menu Live-button navigation differs")
            after = text.replace("bt_online", replacement).encode("ascii")
            patches.append((offset, struct.pack("<I", size) + before, struct.pack("<I", len(after)) + after,
                            "rewire main-menu keyboard navigation away from hidden Live button"))
    else:
        function = original[0xB54E][0]
        if function.constants[7] != "CreateMatch" or function.constants[10] != "postingGameInLive":
            raise PakError("Original startgame offline-launch source differs")
        word(0xB54E, 20, 0x00028145, 0x00000142, "pass authored offline Continue argument to CreateMatch")

    output = bytearray(data)
    previous_end = 0
    for offset, before, after, _ in sorted(patches):
        if offset < previous_end or data[offset:offset + len(before)] != before:
            raise PakError("Menu patch overlaps or lacks its exact original bytes")
        previous_end = offset + len(before)
    for offset, before, after, _ in sorted(patches, reverse=True):
        output[offset:offset + len(before)] = after
    result = bytes(output)
    changed = list(chunks(result).values())
    if len(changed) != len(original):
        raise PakError("Menu patch changed the authored Lua chunk inventory")
    protected = []
    for (offset, (function, body)), (_, new_body) in zip(original.items(), changed, strict=True):
        if offset not in changed_chunks and body != new_body:
            raise PakError("Menu patch changed unrelated authored Lua bytecode")
        strings = [value for value in function.constants if isinstance(value, str)]
        if "ShowAchievementUI" in strings or "Leaderboards" in strings or any(value.startswith("GGRP:menus/achievements/") for value in strings):
            if body != new_body:
                raise PakError("Menu patch changed preserved achievement/leaderboard content")
            protected.append({"original_chunk_offset": f"0x{offset:X}", "bytes": len(body), "sha256": digest(body)})
    return result, {"name": name, "input_sha256": digest(data), "output_sha256": digest(result),
                    "input_bytes": len(data), "output_bytes": len(result), "lua_chunks": len(original),
                    "unchanged_lua_chunks": len(original) - len(changed_chunks),
                    "patches": [{"offset": f"0x{offset:X}", "before_bytes": len(before), "after_bytes": len(after), "purpose": purpose}
                                for offset, before, after, purpose in sorted(patches)],
                    "preserved_achievement_leaderboard_chunks": protected}


def _counted(value: str, *, nul: bool = False) -> bytes:
    encoded = value.encode("ascii") + (b"\0" if nul else b"")
    return struct.pack("<I", len(encoded)) + encoded


def _record(data: bytes, kind: bytes, name: str) -> tuple[int, int]:
    header = b"0" + kind + b"\0" + _counted(name)
    if data.count(header) != 1:
        raise PakError(f"Native video menu source record is ambiguous: {name}")
    start = data.index(header)
    end = data.find(b"0" + kind + b"\0", start + len(header))
    if end < 0:
        end = data.find(b"0_bin\0", start + len(header))
    if end < 0:
        raise PakError("Native video menu source record is unterminated")
    return start, end


def _replace_once(data: bytes, before: bytes, after: bytes) -> bytes:
    if data.count(before) != 1:
        raise PakError("Native video menu field oracle differs")
    return data.replace(before, after, 1)


def _template_layout(data: bytes) -> tuple[int, list[tuple[str, int, int]]]:
    # Original WBR v2 single-class header and property kinds from 8225DE18.
    # Read every record, including script values, so a counted declaration
    # cannot silently be appended after the importer's object inventory.
    reader = Reader(data, 0)
    if struct.unpack("<2H", reader.take(4)) != (2, 1):
        raise PakError("Native video menu template header differs")
    class_id = struct.unpack("<H", reader.take(2))[0]

    def string(*, wide: bool = False) -> str:
        size = reader.count()
        value = reader.take(size * (2 if wide else 1))
        return value.decode("utf-16le" if wide else "ascii")

    class_name = string()
    version, properties_count = struct.unpack("<2H", reader.take(4))
    if (class_name, version) not in (("GUIGROUPMASTER", 1), ("GUILABELMASTER", 2), ("UNISTRINGSMASTER", 2)) or properties_count > 64:
        raise PakError("Native video menu template class differs")
    kinds = []
    for _ in range(properties_count):
        string(); kinds.append(reader.take(1)[0])
    count_offset = reader.position
    count = struct.unpack("<H", reader.take(2))[0]
    if count > 4096:
        raise PakError("Native video menu template record count is unbounded")
    records = []
    widths = {0: 4, 1: 4, 2: 1, 3: 4, 8: 8, 12: 3}
    for _ in range(count):
        start = reader.position
        if struct.unpack("<H", reader.take(2))[0] != class_id:
            raise PakError("Native video menu template record class differs")
        name = string()
        for kind in kinds:
            present = reader.take(1)
            if present == b"0":
                continue
            if present != b"1":
                raise PakError("Native video menu template property presence differs")
            values = struct.unpack("<H", reader.take(2))[0]
            if values > 4096:
                raise PakError("Native video menu template value count is unbounded")
            for _ in range(values):
                if kind in widths:
                    reader.take(widths[kind])
                elif kind in (4, 6):
                    string(wide=kind == 4)
                elif kind == 14:
                    command = reader.take(1)
                    if command == b"\0":
                        continue
                    if command != b"\2" or reader.count() != 1:
                        raise PakError("Native video menu template script kind differs")
                    # Lua bytecode includes NULs and is not an ASCII resource.
                    reader.take(reader.count())
                else:
                    raise PakError("Native video menu template property kind differs")
        records.append((name, start, reader.position))
    if reader.take(5) != b"_bin\0" or reader.position != len(data):
        raise PakError("Native video menu counted template trailer differs")
    return count_offset, records


def _append_records(data: bytes, records: bytes, added: int = 1) -> bytes:
    if not data.endswith(b"0_bin\0") or data.count(b"0_bin\0") != 1:
        raise PakError("Native video menu template terminator differs")
    count_offset, original = _template_layout(data)
    output = bytearray(data[:-6] + records + data[-6:])
    struct.pack_into("<H", output, count_offset, len(original) + added)
    result = bytes(output)
    _, modified = _template_layout(result)
    if len(modified) != len(original) + added or any(
            a[0] != b[0] or data[a[1]:a[2]] != result[b[1]:b[2]] for a, b in zip(original, modified)):
        raise PakError("Native video menu append changed an existing declaration")
    return result


def _native_video_command() -> bytes:
    # A private, newly authored Lua 5.1 command: me.RunCmd(3). The exact named
    # native master/index pair is intercepted by the host; no existing script
    # or existing Game Settings action is replaced.
    words = [0x00000005, 0x00404006, 0x00008041, 0x0100401C, RETURN]
    constants = b"\4" + _counted("me", nul=True) + b"\4" + _counted("RunCmd", nul=True) + b"\3" + struct.pack("<d", 3.0)
    return (LUA_HEADER + struct.pack("<III", 0, 0, 0) + bytes((0, 0, 2, 2)) +
            struct.pack("<I5II", len(words), *words, 3) + constants + struct.pack("<IIII", 0, 0, 0, 0))


def _help_parent_with_video(data: bytes) -> bytes:
    name = "menus/help_and_options/buttons/buttons"
    start, end = _record(data, b"A", name)
    record = data[start:end]
    resources = ["GGRP:menus/help_and_options/buttons/credits", "GGRP:menus/help_and_options/buttons/game_settings",
                 "GGRP:menus/help_and_options/buttons/controls", "GGRP:menus/help_and_options/buttons/how_to_play",
                 "GPCT:screen_reference"]
    old_resources = b"1\5\0" + b"".join(_counted(value) for value in resources)
    if record.count(old_resources) != 1:
        raise PakError("Native video menu original Help controls differ")
    begin = record.index(old_resources)
    position = begin + len(old_resources)
    arrays = []
    # Authored GUIGROUPMASTER ControlPos/Size/Orientation/Name/ZSort/Opacity/
    # Visible/Enabled/Track arrays, all with one entry per Control resource.
    for width in (8, 8, 4, None, 4, 4, 1, 1, 4):
        if record[position:position + 3] != b"1\5\0":
            raise PakError("Native video menu Help child-array contract differs")
        position += 3
        values = []
        for _ in resources:
            if width is None:
                size = struct.unpack_from("<I", record, position)[0]
                if size > 128 or size > len(record) - position - 4:
                    raise PakError("Native video menu Help child name is unbounded")
                values.append(record[position:position + 4 + size]); position += 4 + size
            else:
                values.append(record[position:position + width]); position += width
        arrays.append(values)
    expected_names = ["bt_credits", "bt_setings", "bt_controls", "bt_howtoplay", ""]
    if arrays[3] != [_counted(value) for value in expected_names] or any(
            values != [struct.pack("<2f", 1, 1)] * 4 + [struct.pack("<2f", 1, 0.98)]
            for values in arrays[1:2]):
        raise PakError("Native video menu Help child identity/scale oracle differs")
    new_values = [struct.pack("<2f", 0.1907, 0.1925), struct.pack("<2f", 1, 1), struct.pack("<f", 0),
                  _counted("bt_video"), struct.pack("<I", 1), struct.pack("<f", 1), b"\1", b"\1", struct.pack("<I", 0)]
    expanded = b"1\6\0" + b"".join(_counted(value) for value in resources[:-1] + ["GGRP:" + VIDEO_TILE, resources[-1]])
    for values, new_value in zip(arrays, new_values, strict=True):
        expanded += b"1\6\0" + b"".join(values[:-1] + [new_value, values[-1]])
    return data[:start] + record[:begin] + expanded + record[position:] + data[end:]


def prepare_video_menu(data: bytes, name: str) -> tuple[bytes, dict]:
    """Add an owned native entry after the independently checked offline patch.

    Keeping this as a second derivation preserves the existing whole-file and
    unrelated Lua oracles. The four original Help controls and their scripts
    stay intact; only the two adjacent keyboard links reach the fifth tile.
    """
    if name not in VIDEO_INPUTS or digest(data) != VIDEO_INPUTS[name]:
        raise PakError("Native video menu source differs from its supported input")
    if name == GROUP:
        source_name = "menus/help_and_options/buttons/how_to_play"
        start, end = _record(data, b"A", source_name)
        tile = data[start:end]
        tile = _replace_once(tile, _counted(source_name), _counted(VIDEO_TILE))
        tile = _replace_once(tile, _counted("GLBL:menus/help_and_options/how_to_play"), _counted("GLBL:" + VIDEO_LABEL))
        functions = list(chunks(tile).items())
        if len(functions) != 4 or functions[1][1][0].constants[-1] != "GGRP:menus/help_and_options/how_to_play":
            raise PakError("Native video menu original tile callback differs")
        old_body = functions[1][1][1]
        tile = _replace_once(tile, struct.pack("<I", len(old_body) + 1) + old_body + b"\0",
                             struct.pack("<I", len(_native_video_command()) + 1) + _native_video_command() + b"\0")
        hover_body = functions[2][1][1]
        new_hover = _replace_once(hover_body, _counted("menus/help_and_options/tooltips/how_to_play", nul=True),
                                  _counted(VIDEO_TOOLTIP, nul=True))
        tile = _replace_once(tile, struct.pack("<I", len(hover_body) + 1) + hover_body + b"\0",
                             struct.pack("<I", len(new_hover) + 1) + new_hover + b"\0")
        old_navigation = ("up=me.parent.bt_credits.ForceMouseOver()\r\n" "down=me.parent.bt_credits.ForceMouseOver()\r\n"
                          "left=me.parent.bt_controls.ForceMouseOver()\r\n" "right=me.parent.bt_setings.ForceMouseOver()")
        new_navigation = ("up=me.parent.bt_setings.ForceMouseOver()\r\n" "down=me.parent.bt_howtoplay.ForceMouseOver()\r\n"
                          "left=me.parent.bt_credits.ForceMouseOver()\r\n" "right=me.parent.bt_controls.ForceMouseOver()")
        tile = _replace_once(tile, _counted(old_navigation), _counted(new_navigation))
        result = _help_parent_with_video(data)
        for tile_name, direction, target, replacement in (
                ("menus/help_and_options/buttons/game_settings", "down", "bt_credits", "bt_video"),
                ("menus/help_and_options/buttons/credits", "right", "bt_setings", "bt_video")):
            first, last = _record(result, b"A", tile_name)
            record = result[first:last]
            before = f"{direction}=me.parent.{target}.ForceMouseOver()".encode("ascii")
            # Navigation is one counted string, so resize the entire value,
            # not its interior while leaving the old byte count behind.
            match = re.search(rb"up=me\.parent\.[^\0]+?ForceMouseOver\(\)(?:\r\n(?:down|left|right)=me\.parent\.[^\0]+?ForceMouseOver\(\)){3}", record)
            if not match or record.count(before) != 1:
                raise PakError("Native video menu adjacent navigation oracle differs")
            old = match.group().decode("ascii")
            new = old.replace(before.decode("ascii"), f"{direction}=me.parent.{replacement}.ForceMouseOver()")
            record = _replace_once(record, _counted(old), _counted(new))
            result = result[:first] + record + result[last:]
        result = _append_records(result, tile)
        before_chunks = list(chunks(data).values())
        after_chunks = list(chunks(result).values())
        if len(after_chunks) != len(before_chunks) + 4 or any(a[1] != b[1] for a, b in zip(before_chunks, after_chunks)):
            raise PakError("Native video menu changed an existing Lua chunk")
        additions = {"tile": VIDEO_TILE, "instance": "bt_video", "label": VIDEO_LABEL,
                     "tooltip": VIDEO_TOOLTIP, "original_help_controls_preserved": 4, "appended_lua_chunks": 4}
    elif name == LABELS:
        source_name = "menus/help_and_options/how_to_play"
        start, end = _record(data, b"G", source_name)
        label = data[start:end]
        old_name = _counted(source_name)
        if label.count(old_name) != 2:
            raise PakError("Native video menu source label identity differs")
        label = label.replace(old_name, _counted(VIDEO_LABEL))
        result = _append_records(data, label)
        additions = {"label": VIDEO_LABEL, "original_labels_preserved": True}
    else:
        # Verify the existing simple localized record shape before authoring
        # the two new English strings. All original localized text is retained.
        start, end = _record(data, b"X", "menus/help_and_options/how_to_play")
        expected = b"0X\0" + _counted("menus/help_and_options/how_to_play") + b"01\1\0\0" + b"1\1\0" + struct.pack("<I", 11) + "How to Play".encode("utf-16le")
        if data[start:end] != expected:
            raise PakError("Native video menu localized string record differs")
        records = b""
        for key, value in ((VIDEO_LABEL, "Video\r\nSettings"), (VIDEO_TOOLTIP, "Native PC display, performance and image settings")):
            encoded = value.encode("utf-16le")
            records += b"0X\0" + _counted(key) + b"01\1\0\0" + b"1\1\0" + struct.pack("<I", len(encoded) // 2) + encoded
        result = _append_records(data, records, 2)
        additions = {"strings": [VIDEO_LABEL, VIDEO_TOOLTIP], "original_localized_text_preserved": True}
    return result, {"name": name, "input_sha256": digest(data), "output_sha256": digest(result),
                    "input_bytes": len(data), "output_bytes": len(result), "native_video_entry": additions}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pak", type=Path, default=ROOT / "game/data.pak")
    parser.add_argument("--output", type=Path, default=ROOT / "build_native/offline_menu_override")
    args = parser.parse_args()
    with args.pak.open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != PAK_SHA256:
            raise PakError("Unsupported original package identity")
    entries = {entry.name: entry for entry in read_directory(args.pak)}
    results = []
    for name in INPUTS:
        content, metadata = prepare(read_entry(args.pak, entries[name]), name)
        if name == GROUP:
            content, video_metadata = prepare_video_menu(content, name)
            metadata.update(output_sha256=digest(content), output_bytes=len(content),
                            lua_chunks=metadata["lua_chunks"] + 4,
                            native_video_entry=video_metadata["native_video_entry"])
        results.append((name, content, metadata))
    results.extend((name, *prepare_video_menu(read_entry(args.pak, entries[name]), name)) for name in (LABELS, STRINGS))
    output = args.output.absolute()
    _no_links(output)
    report = {"policy": "offline menus; original package/licensing retained; no Xbox Live identity or dashboard upload claimed",
              "package_sha256": PAK_SHA256, "overlay_root": str(output), "files": []}
    for name, content, metadata in results:
        path = output.joinpath(*name.split("/"))
        _no_links(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        if digest(path.read_bytes()) != metadata["output_sha256"]:
            raise PakError("Derived offline menu failed its output hash check")
        report["files"].append(metadata)
    manifest = output / "manifest.json"
    _no_links(manifest)
    manifest.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"manifest": str(manifest), "files": [{"name": item["name"], "sha256": item["output_sha256"],
                      "unchanged_lua_chunks": item.get("unchanged_lua_chunks")} for item in report["files"]]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
