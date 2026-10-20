"""Convert owned Lua 5.1 asset chunks to the native PC serialization layout.

Only serialized string lengths change from 32 to 64 bits. Instructions, numeric
constants and byte strings retain their authored values. This is asset data;
no original machine code or runtime state is used.
"""
from __future__ import annotations
import base64
import hashlib
import math
from pathlib import Path
import struct

from engine_assets import AssetError, Reader
from inspect_pak import _no_links

LUA32_HEADER = bytes.fromhex('1b4c75615100010404040800')
LUA64_HEADER = bytes.fromhex('1b4c75615100010408040800')


class ChunkConverter:
    def __init__(self, data: bytes):
        self.reader = Reader(data)
        self.output = bytearray(LUA64_HEADER)
        self.functions = self.instructions = self.strings = 0

    def copy(self, size):
        raw = self.reader.take(size)
        self.output.extend(raw)
        return raw

    def count(self, maximum=65536):
        count = self.reader.count(maximum)
        self.output.extend(struct.pack('<I', count))
        return count

    def string(self):
        size = self.reader.count(1024 * 1024)
        raw = self.reader.take(size)
        if size and raw[-1] != 0:
            raise AssetError('Lua asset string lacks terminal NUL')
        self.output.extend(struct.pack('<Q', size) + raw)
        self.strings += 1

    def function(self, depth=0):
        self.functions += 1
        if depth > 32 or self.functions > 65536:
            raise AssetError('Lua asset prototype budget exceeded')
        self.string()
        self.copy(8)  # First/last source lines.
        upvalues, arguments, vararg, registers = self.copy(4)
        if not 2 <= registers <= 250 or arguments > registers or upvalues > 60 or vararg > 7:
            raise AssetError('Invalid Lua asset function header')
        count = self.count()
        self.instructions += count
        if self.instructions > 2 * 1024 * 1024:
            raise AssetError('Lua asset instruction budget exceeded')
        self.copy(count * 4)
        for _ in range(self.count()):
            kind = self.copy(1)[0]
            if kind == 0:
                pass
            elif kind == 1:
                if self.copy(1)[0] > 1:
                    raise AssetError('Invalid Lua asset boolean')
            elif kind == 3:
                if not math.isfinite(struct.unpack('<d', self.copy(8))[0]):
                    raise AssetError('Nonfinite Lua asset number')
            elif kind == 4:
                self.string()
            else:
                raise AssetError('Unsupported Lua asset constant type')
        for _ in range(self.count()):
            self.function(depth + 1)
        self.copy(self.count() * 4)  # Debug line table.
        for _ in range(self.count()):
            self.string()
            self.copy(8)  # Local lifetime instruction indices.
        for _ in range(self.count()):
            self.string()

    def convert(self):
        if self.reader.take(12) != LUA32_HEADER:
            raise AssetError('Unsupported Lua asset serialization layout')
        self.function()
        # The scene's counted script string may include one terminal NUL.
        tail = self.reader.take(len(self.reader.data) - self.reader.position)
        if tail not in (b'', b'\0'):
            raise AssetError('Trailing Lua asset data')
        return bytes(self.output)


def convert_chunk(data: bytes) -> bytes:
    return ChunkConverter(data).convert()


def prepare_scripts(scenes, output: Path):
    records, text_records = [], []
    functions = instructions = 0
    for source, actors in sorted(scenes.items()):
        for ordinal, actor in enumerate(actors):
            for field, values in actor.values.items():
                for index, value in enumerate(values):
                    if not isinstance(value, dict) or 'mode' not in value:
                        continue
                    location = [source, actor.type, actor.name, field, ordinal, index]
                    if value['mode'] == 1:
                        text_records.append(location)
                        continue
                    if value['mode'] != 2:
                        raise AssetError('Unsupported script asset mode')
                    raw = base64.b64decode(value['blob'], validate=True)
                    converter = ChunkConverter(raw)
                    chunk = converter.convert()
                    records.append((location, chunk))
                    functions += converter.functions
                    instructions += converter.instructions
    if len(records) > 16384:
        raise AssetError('Native script catalog outside budget')
    binary = bytearray(b'DNL1' + struct.pack('<II', 1, len(records)))
    for location, chunk in records:
        for name in location[:4]:
            raw = name.encode('utf-8')
            if len(raw) > 4096 or b'\0' in raw:
                raise AssetError('Invalid native script location')
            binary.extend(struct.pack('<I', len(raw)) + raw)
        binary.extend(struct.pack('<III', *location[4:], len(chunk)) + chunk)
    if len(binary) > 64 * 1024 * 1024:
        raise AssetError('Native script catalog exceeds 64 MiB')
    path = output / 'scripts.dnl'
    _no_links(path)
    path.write_bytes(binary)
    return {'schema': 1, 'binary_scripts': len(records), 'functions': functions,
            'instructions': instructions, 'bytes': len(binary),
            'text_scripts_not_converted': text_records,
            'sha256': hashlib.sha256(binary).hexdigest(),
            'scope': 'Native Lua 5.1 asset loading; scene host bindings and event execution are pending'}
