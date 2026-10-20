"""Export authored STARTPOS/TRIGGER records without guest pointers or code.

DNS1 uses little-endian numbers, counted UTF-8 strings and bounded arrays. The
course and chunk order matches engine_world. Script references point to the
unchanged source location used by engine_scripts, including inherited values.
"""
from __future__ import annotations
import hashlib
import math
from pathlib import Path
import struct

from engine_assets import AssetError
from engine_world import first, matrix_product, transform
from inspect_pak import _no_links

SCRIPT_FIELDS = ('Command', 'OnRespawnScript', 'OnActivate',
                 'LeftTriggerCommand', 'AllTouchersLeftTriggerCommand')
BASIS = [[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]]
MAX_BYTES = 64 * 1024 * 1024


def _vector(value, size=3):
    if not isinstance(value, (list, tuple)) or len(value) != size:
        raise AssetError('Native scene vector has an invalid size')
    result = [float(v) for v in value]
    if not all(math.isfinite(v) and abs(v) <= 1e9 for v in result):
        raise AssetError('Native scene number outside budget')
    return result


def _native(value):
    value = _vector(value)
    return [value[0], value[2], value[1]]


def _matrix(value):
    # Python column matrices are transposed for DirectX row-vector storage.
    return [value[row][col] for col in range(4) for row in range(4)]


def _signed(value):
    if isinstance(value, bool) or not isinstance(value, int) or not -2147483648 <= value <= 2147483647:
        raise AssetError('Native scene integer outside budget')
    return value


def _unsigned(value):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xffffffff:
        raise AssetError('Native scene unsigned integer outside budget')
    return value


def _boolean(value):
    if not isinstance(value, bool):
        raise AssetError('Native scene boolean is not authored as boolean')
    return value


def scene_data(scenes, catalog):
    """Return bounded export records; this does not infer checkpoint behavior."""
    templates = {}
    binary_scripts = {}
    for source, rows in sorted(scenes.items()):
        if source.startswith('data/templates/'):
            table = {}
            for ordinal, record in enumerate(rows):
                if record.name in table:
                    raise AssetError('Duplicate native scene template: ' + record.name)
                table[record.name] = (source, ordinal, record)
            templates[source.rsplit('/', 1)[-1].rsplit('.', 1)[0]] = table
        # Keep exactly the traversal used by prepare_scripts, including scripts
        # in non-gameplay records and in templates.
        for ordinal, record in enumerate(rows):
            for field, values in record.values.items():
                for index, value in enumerate(values):
                    if isinstance(value, dict) and value.get('mode') == 2:
                        location = (source, record.type, record.name, field, ordinal, index)
                        binary_scripts[location] = len(binary_scripts)
    if len(binary_scripts) > 16384:
        raise AssetError('Native scene script catalog outside budget')

    def effective(source, ordinal, record, active=()):
        key = (source, ordinal)
        if key in active or len(active) >= 16:
            raise AssetError('Cyclic native scene template inheritance')
        values, origins = {}, {}
        for field, names in record.values.items():
            if field.endswith('Template') and field != 'IsTemplate' and names and names[0]:
                table = templates.get(field[:-8].lower(), {})
                if names[0] not in table:
                    raise AssetError('Unresolved native scene template: ' + field + ':' + names[0])
                base_values, base_origins = effective(*table[names[0]], active + (key,))
                values.update(base_values)
                origins.update(base_origins)
        for field, field_values in record.values.items():
            values[field] = field_values
            origins[field] = (source, record.type, record.name, field, ordinal)
        return values, origins

    courses, exclusions, text_scripts = [], [], []
    total_records = 0
    if not 1 <= len(catalog) <= 15 or len({c['id'] for c in catalog}) != len(catalog):
        raise AssetError('Native scene course catalog outside budget')
    for course in catalog:
        identity = _unsigned(course['id'])
        if identity > 14:
            raise AssetError('Native scene course ID outside budget')
        if not 2 <= len(course['chunks']) <= 128:
            raise AssetError('Native scene chunk catalog outside budget')
        if course['chunks'][-1]['name'] != 'common/start_chunk' or course['chunks'][0]['name'] != 'common/end_chunk':
            raise AssetError('Unexpected native scene chunk order')
        result = {'id': identity, 'name': course['name'], 'chunks': [], 'records': []}
        cursor = None
        for chunk_id, chunk in enumerate(reversed(course['chunks'])):
            source = chunk['file']
            records = scenes[source]
            links = {}
            for record in records:
                if record.type == 'CHUNKLINK':
                    kind = first(record.values, 'LinkType')
                    if kind in links:
                        raise AssetError('Native scene chunk has duplicate links')
                    links[kind] = _vector(first(record.values, 'Pos'))
            if set(links) != {0, 1}:
                raise AssetError('Native scene chunk needs unique start and end links')
            offset = [0., 0., 0.] if cursor is None else [a-b for a, b in zip(cursor, links[0])]
            cursor = [a+b for a, b in zip(offset, links[1])]
            result['chunks'].append({'id': chunk_id, 'serialized_ordinal': len(course['chunks'])-1-chunk_id,
                                     'name': chunk['name'], 'source': source, 'offset': _native(offset),
                                     'start': _native([a+b for a, b in zip(offset, links[0])]), 'end': _native(cursor)})
            for ordinal, record in enumerate(records):
                if record.type not in ('STARTPOS', 'TRIGGER'):
                    continue
                values, origins = effective(source, ordinal, record)
                if first(values, 'IsTemplate', False):
                    continue
                required = ('Pos', 'Ang', 'Scale', 'Enabled', 'Mode', 'MaxCommands',
                            'CommandCounter', 'WaitTime', 'UseTimePerActor', 'SendToToucher')
                if any(not values.get(field) for field in required):
                    raise AssetError('Native scene actor lacks authored fields: ' + record.name)
                position = _vector(first(values, 'Pos'))
                angles = _vector(first(values, 'Ang'))
                scale = _vector(first(values, 'Scale'))
                actor = transform(position, angles, scale)
                for axis in range(3):
                    actor[axis][3] += offset[axis]
                native_actor = matrix_product(BASIS, matrix_product(actor, BASIS))
                item = {'id': len(result['records']), 'chunk': chunk_id, 'ordinal': ordinal,
                        'source': source, 'type': record.type, 'name': record.name,
                        'position': _native([a+b for a, b in zip(position, offset)]),
                        'transform': _matrix(native_actor),
                        'enabled': _boolean(first(values, 'Enabled')),
                        'mode': _signed(first(values, 'Mode')),
                        'max_commands': _signed(first(values, 'MaxCommands')),
                        'command_counter': _signed(first(values, 'CommandCounter')),
                        'wait_time': _vector(first(values, 'WaitTime'), 2),
                        'use_time_per_actor': _boolean(first(values, 'UseTimePerActor')),
                        'send_to_toucher': _boolean(first(values, 'SendToToucher')),
                        'resources': list(values.get('Resources', [])), 'scripts': [], 'boxes': []}
                reasons = []
                if first(values, 'AttachTo', ''):
                    reasons.append('attached actor')
                if first(values, 'PathName', ''):
                    reasons.append('path-driven actor')
                for field in ('Vel', 'AngVel', 'Aceleration', 'AngAceleration'):
                    if any(_vector(first(values, field, [0, 0, 0]))):
                        reasons.append('dynamic ' + field)
                if any(abs(v) < 1e-6 for v in scale):
                    reasons.append('singular actor scale')
                for field in SCRIPT_FIELDS:
                    for index, value in enumerate(values.get(field, [])):
                        if value is None:
                            continue
                        if not isinstance(value, dict) or value.get('mode') not in (1, 2):
                            raise AssetError('Unsupported native scene script reference')
                        origin = origins[field]
                        location = origin + (index,)
                        reference = dict(zip(('source', 'type', 'actor', 'field', 'ordinal', 'index'), location))
                        reference.update(mode=value['mode'], flags=_unsigned(value.get('flags', 0)),
                                         catalog_index=binary_scripts.get(location, -1))
                        if value['mode'] == 2 and reference['catalog_index'] < 0:
                            raise AssetError('Native scene binary script has no catalog entry')
                        if value['mode'] == 1:
                            text_scripts.append([identity, chunk_id, item['id'], *location])
                            reasons.append('unsupported text script ' + field)
                        item['scripts'].append(reference)
                resources = item['resources']
                if len(resources) > 512 or len(item['scripts']) > 128:
                    raise AssetError('Native scene actor arrays outside budget')
                for index, name in enumerate(resources):
                    if not isinstance(name, str):
                        raise AssetError('Native scene resource name is not text')
                    if not name.startswith('CLP3:'):
                        continue
                    table = templates.get('cprimitive3', {})
                    if name[5:] not in table:
                        raise AssetError('Unresolved native scene collision primitive: ' + name)
                    primitive, _ = effective(*table[name[5:]])
                    kind = _unsigned(first(primitive, 'PrimitiveType'))
                    if kind != 2:
                        reasons.append('unsupported collision primitive ' + name)
                        continue
                    fields = ('ResourcesPos', 'ResourcesAng', 'ResourcesScale')
                    if any(index >= len(values.get(field, [])) for field in fields):
                        raise AssetError('Native scene resource transform array mismatch')
                    bone_attach = values.get('ResourcesBoneAttach', [])
                    if index < len(bone_attach) and bone_attach[index]:
                        reasons.append('bone-attached collision resource ' + name)
                    resource_scale = _vector(values['ResourcesScale'][index])
                    if any(abs(v) < 1e-6 for v in resource_scale):
                        reasons.append('singular collision resource ' + name)
                    local = transform(_vector(values['ResourcesPos'][index]),
                                      _vector(values['ResourcesAng'][index]), resource_scale)
                    minimum = _native(first(primitive, 'BoundingBoxMin'))
                    maximum = _native(first(primitive, 'BoundingBoxMax'))
                    if any(a >= b for a, b in zip(minimum, maximum)):
                        raise AssetError('Native scene collision box has invalid bounds')
                    item['boxes'].append({'resource': index, 'name': name, 'primitive_type': kind,
                                          'collision_mask': _unsigned(first(primitive, 'CollisionMask')),
                                          'collision_group': _unsigned(first(primitive, 'CollisionGroup')),
                                          'minimum': minimum, 'maximum': maximum,
                                          'transform': _matrix(matrix_product(BASIS, matrix_product(actor, matrix_product(local, BASIS))))})
                item['supported'] = not reasons
                item['exclusion'] = '; '.join(dict.fromkeys(reasons))
                if reasons:
                    item['boxes'] = []
                    exclusions.append({'course': identity, 'chunk': chunk_id, 'record': item['id'],
                                       'source': source, 'ordinal': ordinal, 'name': record.name,
                                       'reason': item['exclusion']})
                result['records'].append(item)
                total_records += 1
                if len(result['records']) > 4096 or total_records > 65536:
                    raise AssetError('Native scene record catalog outside budget')
        courses.append(result)
    return courses, exclusions, text_scripts


def encode_scene(courses):
    binary = bytearray(b'DNS1' + struct.pack('<II', 1, len(courses)))

    def string(value):
        raw = value.encode('utf-8')
        if len(raw) > 4096 or b'\0' in raw:
            raise AssetError('Native scene string outside budget')
        binary.extend(struct.pack('<I', len(raw)) + raw)

    def floats(values):
        if not all(math.isfinite(v) and abs(v) <= 1e9 for v in values):
            raise AssetError('Native scene export number outside budget')
        binary.extend(struct.pack('<' + 'f'*len(values), *values))

    for course in courses:
        binary.extend(struct.pack('<I', course['id']))
        string(course['name'])
        binary.extend(struct.pack('<I', len(course['chunks'])))
        for chunk in course['chunks']:
            binary.extend(struct.pack('<II', chunk['id'], chunk['serialized_ordinal']))
            string(chunk['name']); string(chunk['source'])
            floats(chunk['offset'] + chunk['start'] + chunk['end'])
        binary.extend(struct.pack('<I', len(course['records'])))
        for record in course['records']:
            binary.extend(struct.pack('<III', record['id'], record['chunk'], record['ordinal']))
            for field in ('source', 'type', 'name'):
                string(record[field])
            floats(record['position'] + record['transform'])
            binary.extend(struct.pack('<Iiii', record['enabled'], record['mode'], record['max_commands'], record['command_counter']))
            floats(record['wait_time'])
            binary.extend(struct.pack('<III', record['use_time_per_actor'], record['send_to_toucher'], record['supported']))
            string(record['exclusion'])
            binary.extend(struct.pack('<I', len(record['resources'])))
            for resource in record['resources']:
                string(resource)
            binary.extend(struct.pack('<I', len(record['scripts'])))
            for reference in record['scripts']:
                for field in ('source', 'type', 'actor', 'field'):
                    string(reference[field])
                binary.extend(struct.pack('<IIIIi', reference['ordinal'], reference['index'], reference['mode'],
                                          reference['flags'], reference['catalog_index']))
            binary.extend(struct.pack('<I', len(record['boxes'])))
            for box in record['boxes']:
                binary.extend(struct.pack('<IIII', box['resource'], box['primitive_type'], box['collision_mask'], box['collision_group']))
                string(box['name'])
                floats(box['minimum'] + box['maximum'] + box['transform'])
        if len(binary) > MAX_BYTES:
            raise AssetError('Native scene asset exceeds 64 MiB')
    return binary


def prepare_scene(scenes, catalog, output: Path):
    courses, exclusions, text_scripts = scene_data(scenes, catalog)
    binary = encode_scene(courses)
    target = output / 'scene.dns'
    _no_links(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(binary)
    return {'schema': 1, 'file': 'scene.dns', 'bytes': len(binary),
            'sha256': hashlib.sha256(binary).hexdigest(),
            'courses': [{'id': c['id'], 'chunks': len(c['chunks']), 'records': len(c['records']),
                         'start_positions': sum(r['type'] == 'STARTPOS' for r in c['records']),
                         'triggers': sum(r['type'] == 'TRIGGER' for r in c['records']),
                         'boxes': sum(len(r['boxes']) for r in c['records'])} for c in courses],
            'excluded_records': exclusions, 'text_scripts_not_supported': text_scripts,
            'scope': 'Authored STARTPOS/TRIGGER fields, resolved script references and static CLP3 boxes; unsupported dynamic/attached records are explicitly excluded.'}
