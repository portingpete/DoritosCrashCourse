"""Read bounded, pointer-free Tongas scene records for the independent PC engine.

This reads asset serialization only. It never loads executable code or creates
an Xbox address space. Converted user assets remain in ignored build_native.
"""
from __future__ import annotations
import base64
from dataclasses import dataclass
import math
import struct


class AssetError(ValueError):
    pass


class Reader:
    def __init__(self, data: bytes):
        if len(data) > 64 * 1024 * 1024:
            raise AssetError('Asset exceeds 64 MiB')
        self.data, self.position = data, 0

    def take(self, size):
        if size < 0 or size > len(self.data) - self.position:
            raise AssetError(f'Truncated asset at {self.position}')
        value = self.data[self.position:self.position + size]
        self.position += size
        return value

    def unpack(self, fmt):
        return struct.unpack('<' + fmt, self.take(struct.calcsize('<' + fmt)))

    def u8(self): return self.unpack('B')[0]
    def u16(self): return self.unpack('H')[0]
    def u32(self): return self.unpack('I')[0]

    def count(self, maximum=65536):
        value = self.u32()
        if value > maximum:
            raise AssetError('Unbounded asset count')
        return value

    def string(self, wide=False):
        count = self.count()
        try:
            return self.take(count * (2 if wide else 1)).decode('utf-16le' if wide else 'utf-8')
        except UnicodeDecodeError as error:
            raise AssetError('Invalid asset string') from error

    def floats(self, count):
        result = self.unpack('f' * count)
        if not all(math.isfinite(value) for value in result):
            raise AssetError('Nonfinite asset number')
        return list(result)


@dataclass
class SceneRecord:
    type: str
    name: str
    values: dict


def read_static_mesh(data: bytes) -> dict:
    """Decode the observed RVM 102 asset layouts into PC vertex arrays.

    Only the four uncompressed layouts present in the supported package are
    accepted. Source coordinates are converted to the scene's Y-up convention.
    Material ranges are inclusive triangle ranges, not index counts.
    """
    reader = Reader(data)
    if reader.u8() != 102:
        raise AssetError('Unsupported static mesh version')
    layout, materials = reader.u32(), reader.u8()
    vertices, triangles = reader.u16(), reader.u16()
    extensions = reader.take(5)
    flags = reader.u32()
    reader.take(24 + 172)  # Authored bounds and unused editor metadata.
    allowed_extensions = {bytes([0, 0, int(layout != 5), int(layout in (3, 5, 6)), 0])}
    if layout == 6: allowed_extensions.add(bytes([0, 0, 1, 0, 0]))
    if (layout not in (2, 3, 5, 6) or not materials or not vertices or not triangles
            or extensions not in allowed_extensions or flags & ~0x108):
        raise AssetError('Unsupported static mesh layout or extensions')
    names = []
    for _ in range(materials):
        count = reader.u16() if flags & 0x100 else 32
        if not 1 <= count <= 4096: raise AssetError('Mesh material name outside budget')
        raw = reader.take(count)
        if b'\0' not in raw:
            raise AssetError('Unterminated mesh material name')
        try:
            names.append(raw.split(b'\0')[0].decode('utf-8'))
        except UnicodeDecodeError as error:
            raise AssetError('Invalid mesh material name') from error
    stride = {2: 32, 3: 36, 5: 32, 6: 44}[layout]
    points, alternate_uv, repaired = [], [], {'normals': 0, 'uv': 0}
    for _ in range(vertices):
        raw = reader.take(stride)
        position = list(struct.unpack_from('<3f', raw))
        normal = list(struct.unpack_from('<3f', raw, 12)) if layout != 5 else [0., 1., 0.]
        uv_offset = {2: 24, 3: 28, 5: 16, 6: 28}[layout]
        uv = list(struct.unpack_from('<2f', raw, uv_offset))
        uv2 = list(struct.unpack_from('<2f', raw, uv_offset+8)) if layout in (5,6) else uv[:]
        if not all(math.isfinite(v) for v in position):
            raise AssetError('Nonfinite static mesh position')
        # Some shipped meshes contain uninitialized unused UVs and normals on
        # collapsed faces. Never send those NaNs to the native GPU pipeline.
        if not all(math.isfinite(v) for v in uv):
            if not all(math.isfinite(v) for v in uv2):
                raise AssetError('No finite static mesh texture coordinates')
            uv = uv2[:]; repaired['uv'] += 1
        if not all(math.isfinite(v) for v in uv2): uv2 = uv[:]
        if not flags & 8:
            position[1], position[2] = position[2], position[1]
            if layout != 5: normal[1], normal[2] = normal[2], normal[1]
        points.append(position + normal + uv)
        alternate_uv.append(uv2)
    indices = list(reader.unpack('H' * (triangles * 3)))
    if any(index >= vertices for index in indices):
        raise AssetError('Static triangle index outside mesh')
    groups = []
    for slot, name in enumerate(names):
        first, last = reader.u16(), reader.u16()
        if last == 65535: continue
        if first > last:
            # Empty authored slots are represented by adjacent reversed bounds,
            # or the all-ones end marker. They draw no triangles.
            if last != 65535 and first != last + 1:
                raise AssetError('Invalid empty static material range')
            continue
        if last >= triangles:
            raise AssetError('Static material triangle range outside mesh')
        groups.append({'material': name, 'slot': slot, 'first': first * 3, 'count': (last-first+1)*3})
    if reader.position != len(data):
        raise AssetError('Unexpected static mesh extension or trailing data')
    derived = [[0., 0., 0.] for _ in points]
    for start in range(0, len(indices), 3):
        a,b,c = [points[index][:3] for index in indices[start:start+3]]
        ab,ac = [[v-u for u,v in zip(a,other)] for other in (b,c)]
        n = [ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]]
        for index in indices[start:start+3]:
            derived[index] = [x+y for x,y in zip(derived[index],n)]
    for point, normal in zip(points,derived):
        if layout == 5 or not all(math.isfinite(v) for v in point[3:6]):
            length = math.sqrt(sum(v*v for v in normal))
            point[3:6] = [v/length for v in normal] if length > 1e-12 else [0.,1.,0.]
            repaired['normals'] += 1
    return {'vertices': points, 'indices': indices, 'groups': groups, 'layout': layout,
            'alternate_uv': alternate_uv, 'repairs': repaired}


def read_collision_mesh(data: bytes) -> dict:
    """Read WCO 101 geometry and its serialized spatial tree as asset data."""
    reader=Reader(data)
    if reader.u8()!=101:raise AssetError('Unsupported collision mesh version')
    named=reader.u8()
    if named>1:raise AssetError('Invalid collision material flag')
    materials=[]
    if named:
        for _ in range(reader.u8()):
            raw=reader.take(32)
            if b'\0' not in raw:raise AssetError('Unterminated collision material name')
            try:materials.append(raw.split(b'\0')[0].decode())
            except UnicodeDecodeError as error:raise AssetError('Invalid collision material name') from error
    declared_vertices,triangles=reader.u16(),reader.u16()
    vertices=reader.u16();face_materials,normals=reader.u8(),reader.u8();tree_parameter=reader.u16()
    if (vertices!=declared_vertices or not 0<vertices<32768 or not 0<triangles<32768
            or face_materials>1 or normals>1 or tree_parameter!=32):
        raise AssetError('Invalid collision geometry counts or flags')
    points=[reader.floats(3) for _ in range(vertices)]
    if normals:
        # These authored normals are not required by the new collision core.
        reader.take(vertices*12)
    indices=[];face_material=[];nodes=[]
    def node(depth=0):
        if depth>64 or len(nodes)>=65535:raise AssetError('Collision tree outside depth/node budget')
        count,axis=reader.u16(),reader.u16();children=reader.u8();split=reader.floats(1)[0];bounds=reader.floats(6)
        if children>1 or children and axis>=3 or count>triangles-len(indices)//3:
            raise AssetError('Invalid collision spatial node')
        nodes.append({'axis':axis,'split':split,'bounds':bounds,'first':len(indices)//3,'count':count,'children':children})
        for _ in range(count):
            face=list(reader.unpack('3H'))
            material=reader.u16() if face_materials else 0
            if max(face)>=vertices or materials and material>=len(materials):raise AssetError('Collision face outside vertex/material table')
            indices.extend(face);face_material.append(material)
        if children:
            node(depth+1);node(depth+1)
    node()
    if len(indices)!=triangles*3 or reader.position!=len(data):raise AssetError('Collision mesh count mismatch or trailing data')
    return {'vertices':points,'indices':indices,'materials':materials,'face_materials':face_material,
            'nodes':nodes,'tree_parameter':tree_parameter}


def value(reader, kind):
    if kind in (0, 1): return reader.unpack('i')[0]
    if kind == 2:
        result = reader.u8()
        if result > 1: raise AssetError('Invalid asset boolean')
        return bool(result)
    if kind == 3: return reader.floats(1)[0]
    if kind in (4, 6): return reader.string(wide=kind == 4)
    if kind in (8, 9, 10, 11): return reader.floats({8: 2, 9: 3, 10: 4, 11: 3}[kind])
    if kind == 12: return list(reader.take(3))
    if kind == 13:
        flags = reader.u32()
        return {'flags': flags, 'blob': base64.b64encode(reader.take(reader.count())).decode()}
    if kind == 14:
        mode = reader.u8()
        if mode == 0: return None
        if mode not in (1, 2): raise AssetError('Unknown script asset mode')
        flags = reader.u32()
        return {'mode': mode, 'flags': flags,
                'blob': base64.b64encode(reader.take(reader.count())).decode()}
    raise AssetError(f'Unsupported scene property kind {kind}')


def read_scene(data: bytes) -> list[SceneRecord]:
    reader = Reader(data)
    if reader.u16() != 2:
        raise AssetError('Unsupported scene version')
    classes = {}
    for _ in range(reader.u16()):
        identity, name = reader.u16(), reader.string()
        version, count = reader.u16(), reader.u16()
        if identity in classes or count > 512 or not 1 <= version <= 8:
            raise AssetError('Invalid scene class table')
        properties = [(reader.string(), reader.u8()) for _ in range(count)]
        classes[identity] = (name, properties)
    records = []
    for _ in range(reader.u16()):
        identity, name = reader.u16(), reader.string()
        if identity not in classes: raise AssetError('Unknown scene record class')
        class_name, properties = classes[identity]
        values = {}
        for key, kind in properties:
            present = reader.take(1)
            if present == b'0': continue
            if present != b'1': raise AssetError(f'Invalid presence at {reader.position - 1}: {class_name}.{key}')
            count = reader.u16()
            values[key] = [value(reader, kind) for _ in range(count)]
        records.append(SceneRecord(class_name, name, values))
    if reader.take(5) != b'_bin\0' or reader.position != len(data):
        raise AssetError(f'Unparsed scene trailer at {reader.position}')
    return records


def read_animation(data: bytes, bone_count: int) -> dict:
    reader = Reader(data)
    if reader.take(4) != b'CAF\0':
        raise AssetError('Invalid animation magic')
    version = reader.u32()
    if version not in (1000, 1010):
        raise AssetError('Only observed Cal3D 1000/1010 animations are supported')
    flags = reader.u32() if version == 1010 else 0
    if flags not in (0, 2):
        raise AssetError('Unsupported animation extension flags')
    # Flag 2 precedes the ordinary duration/tracks with time + XYZ samples.
    # These are stored floats, not compressed quaternion keys.
    motion = [reader.floats(4) for _ in range(reader.count(100000))] if flags == 2 else []
    duration = reader.floats(1)[0]
    if not 0 < duration <= 3600:
        raise AssetError('Invalid animation duration')
    tracks, seen = [], set()
    for _ in range(reader.count(bone_count)):
        bone = reader.u32()
        if bone >= bone_count or bone in seen:
            raise AssetError('Invalid or duplicate animation bone')
        seen.add(bone)
        keys = [reader.floats(8) for _ in range(reader.count(100000))]
        if not keys or any(key[0] < 0 or key[0] > 3600 for key in keys):
            raise AssetError('Animation key time outside budget')
        # Cal3D inserts authored keyframes in time order. Older source files
        # can store the endpoint before the start. Preserve their values.
        keys.sort(key=lambda key: key[0])
        if any(b[0] <= a[0] for a, b in zip(keys, keys[1:])):
            raise AssetError('Animation keys must have increasing times')
        if any(abs(sum(q*q for q in key[4:8]) - 1) > .01 for key in keys):
            raise AssetError('Animation quaternion is not normalized')
        tracks.append({'bone': bone, 'keys': keys})
    if reader.position != len(data):
        raise AssetError('Unparsed animation bytes')
    return {'duration': duration, 'tracks': tracks, 'motion': motion}
