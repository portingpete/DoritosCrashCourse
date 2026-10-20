"""Convert original Stage assets into bounded independent environment bundles.

Only authored RVMP geometry is drawn by this conversion. Decals, primitive
water, skeletal actors and effects are inventoried without invented meshes.
The companion DNW1 file uses the existing static-world native reader.
"""
from __future__ import annotations

from collections import Counter
import hashlib
import math
from pathlib import Path
import re
import struct

from character_packs import solid_png
from engine_assets import AssetError, read_static_mesh
from engine_world import first, matrix_product, scene_path, transform
from inspect_pak import _no_links, safe_name


_BASIS = [[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]]
_CAMERA_FIELDS = ('CameraDistance', 'CameraLookAt', 'CameraPitch', 'CameraYaw',
                  'CameraDistance2', 'CameraLookAt2', 'CameraPitch2', 'CameraYaw2', 'StartFov')


def _required(values, name):
    rows = values.get(name, [])
    if len(rows) != 1:
        raise AssetError('Missing or repeated environment field: ' + name)
    return rows[0]


def _number(value, low=-1000000, high=1000000):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
        raise AssetError('Environment number outside budget')
    return float(value)


def _integer(value, low, high):
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise AssetError('Environment integer outside budget')
    return value


def _flag(value):
    if not isinstance(value, bool):
        raise AssetError('Environment flag is not Boolean')
    return int(value)


def _vector(value, size=3, low=-1000000, high=1000000):
    if not isinstance(value, (list, tuple)) or len(value) != size:
        raise AssetError('Invalid environment vector')
    return [_number(v, low, high) for v in value]


def _rgb(value):
    return [_integer(v, 0, 255) / 255 for v in _vector_int(value)]


def _vector_int(value):
    if not isinstance(value, (list, tuple)) or len(value) != 3:
        raise AssetError('Invalid environment color')
    return value


def _native_vector(value):
    x, y, z = _vector(value)
    return [x, z, y]


def _matrix(values, resource_index=None):
    actor = transform(_vector(_required(values, 'Pos')), _vector(_required(values, 'Ang')),
                      _vector(_required(values, 'Scale')))
    if resource_index is not None:
        arrays = [values.get(name, []) for name in ('ResourcesPos', 'ResourcesAng', 'ResourcesScale')]
        if any(len(array) != len(values['Resources']) for array in arrays):
            raise AssetError('Environment resource transform arrays do not match')
        local = transform(*(_vector(array[resource_index]) for array in arrays))
        actor = matrix_product(actor, local)
    result = matrix_product(_BASIS, matrix_product(actor, _BASIS))
    scale = [sum(result[i][j] ** 2 for i in range(3)) for j in range(3)]
    if min(scale) < 1e-12:
        raise AssetError('Singular environment resource transform')
    return [_number(result[j][k]) for k in range(4) for j in range(4)]


class _Writer:
    def __init__(self, magic):
        self.data = bytearray(magic + struct.pack('<I', 1))

    def pack(self, fmt, *values):
        self.data.extend(struct.pack('<' + fmt, *values))
        if len(self.data) > 64 * 1024 * 1024:
            raise AssetError('Environment bundle exceeds budget')

    def string(self, value):
        if not isinstance(value, str) or '\0' in value:
            raise AssetError('Invalid environment string')
        raw = value.encode('utf-8')
        if len(raw) > 4096:
            raise AssetError('Environment string exceeds budget')
        self.pack('I', len(raw))
        self.data.extend(raw)


def _templates(scenes):
    result = {}
    for path, records in scenes.items():
        if not path.startswith('data/templates/'):
            continue
        table = {}
        for record in records:
            if record.name in table:
                raise AssetError('Duplicate environment template: ' + record.name)
            table[record.name] = record.values
        result[path.rsplit('/', 1)[-1][:-4]] = table
    return result


def _effective(record, templates):
    values = {}
    for field, rows in record.values.items():
        if field.endswith('Template') and field != 'IsTemplate' and rows and rows[0]:
            table = field[:-8].lower()
            if rows[0] not in templates.get(table, {}):
                raise AssetError('Missing environment actor template: ' + rows[0])
            values.update(templates[table][rows[0]])
    values.update(record.values)
    return values


def _settings(path, records, templates):
    levels = [r for r in records if r.type == 'LEVEL' and not first(r.values, 'IsTemplate', False)]
    if len(levels) != 1:
        raise AssetError('Environment stage needs one authored LEVEL')
    level = levels[0].values
    config = _required(level, 'GameConfig') or 'default'
    if config not in templates.get('gameconfig', {}):
        raise AssetError('Environment has no authored camera profile')
    camera = templates['gameconfig'][config]
    camera_data = {field: _required(camera, field) for field in _CAMERA_FIELDS}
    for field in ('CameraLookAt', 'CameraLookAt2'):
        camera_data[field] = _native_vector(camera_data[field])
    for field in ('CameraDistance', 'CameraDistance2'):
        camera_data[field] = _number(camera_data[field], .001, 100000)
    for field in ('CameraPitch', 'CameraYaw', 'CameraPitch2', 'CameraYaw2'):
        camera_data[field] = _number(camera_data[field], -360, 360)
    camera_data['StartFov'] = _number(camera_data['StartFov'], 1, 179)
    fog_start = _number(_required(level, 'FogStart'), 0, 100000)
    fog_end = _number(_required(level, 'FogEnd'), fog_start, 100000)
    return {'source': path, 'background': _rgb(_required(level, 'BGColor')),
            'fog_enabled': _flag(_required(level, 'FogEnabled')), 'fog_color': _rgb(_required(level, 'FogColor')),
            'fog_start': fog_start, 'fog_end': fog_end,
            'start_fov': _number(_required(level, 'StartFov'), 1, 179),
            'camera_source': 'data/templates/gameconfig.wbr#' + config, 'camera': camera_data, 'lights': []}


def _light(actor, values, index, name, templates):
    if name not in templates.get('light', {}):
        raise AssetError('Missing environment light template: ' + name)
    data = templates['light'][name]
    return {'actor': actor, 'resource': name,
            'type': _integer(_required(data, 'Type'), 0, 3),
            'light_mask': _integer(_required(data, 'LightMask'), 0, 0xffffffff),
            'render_pass_mask': _integer(_required(data, 'RenderPassMask'), 0, 0xffffffff),
            'color': _rgb(_required(data, 'Color')), 'specular': _rgb(_required(data, 'Specular')),
            'diffuse_factor': _number(_required(data, 'DiffuseFactor'), 0, 1000),
            'specular_factor': _number(_required(data, 'SpecularFactor'), 0, 1000),
            'range': _number(_required(data, 'Range'), .001, 100000),
            'cast_shadows': _flag(_required(data, 'CastShadows')),
            'attenuation': [_number(_required(data, 'Attenuation' + str(i)), 0, 1000000) for i in range(3)],
            'matrix': _matrix(values, index)}


def _metadata(stages, courses):
    out = _Writer(b'DNV1')
    out.pack('I', len(stages))
    for stage in stages:
        out.pack('I', stage['theme']);out.string(stage['source'])
        out.pack('3fI3f3f', *stage['background'], stage['fog_enabled'], *stage['fog_color'],
                 stage['fog_start'], stage['fog_end'], stage['start_fov'])
        out.string(stage['camera_source']);c = stage['camera']
        out.pack('13f', c['CameraDistance'], *c['CameraLookAt'], c['CameraPitch'], c['CameraYaw'],
                 c['CameraDistance2'], *c['CameraLookAt2'], c['CameraPitch2'], c['CameraYaw2'], c['StartFov'])
        out.pack('I', len(stage['lights']))
        for light in stage['lights']:
            out.string(light['actor']);out.string(light['resource'])
            out.pack('III9fI19f', light['type'], light['light_mask'], light['render_pass_mask'],
                     *light['color'], *light['specular'], light['diffuse_factor'], light['specular_factor'],
                     light['range'], light['cast_shadows'], *light['attenuation'], *light['matrix'])
    out.pack('I', len(courses))
    for course in courses: out.pack('II', course['id'], course['stage_index'])
    return out.data


def _world(materials, meshes, courses):
    out = _Writer(b'DNW1')
    out.pack('I', len(materials))
    for material in materials:
        out.string(material['name']);out.string(material['texture'])
        out.pack('4fII', *material['color'], material['lit'], material['transparent'])
    out.pack('I', len(meshes))
    for mesh in meshes:
        out.string(mesh['name']);out.pack('III', len(mesh['vertices']), len(mesh['indices']), len(mesh['groups']))
        for vertex in mesh['vertices']: out.pack('8f', *vertex)
        out.pack('I' * len(mesh['indices']), *mesh['indices'])
        for group in mesh['groups']: out.pack('III', group['material'], group['first'], group['count'])
    out.pack('I', len(courses))
    for course in courses:
        # DNW1's unused inspection anchor is the actual course spawn, never a
        # synthetic location. Stages retain their authored world placement.
        out.pack('I6fI', course['id'], *course['spawn'], *course['spawn'], len(course['instances']))
        for instance in course['instances']:
            out.string(instance['actor'])
            out.pack('I17f', instance['mesh'], *instance['matrix'], instance['opacity'])
    return out.data


def prepare_environment(source, scenes, catalog, output: Path):
    """Write environment.dnv and environment-world.dnw alongside world.dnw.

    `catalog` must have IDs, themes and original chunk paths, as produced by
    prepare_engine_assets. Stage selection comes from each original LEVELCONFIG
    record, and is checked against catalog['stage'] when that field is supplied.
    """
    output = Path(output)
    _no_links(output)
    templates = _templates(scenes)
    level_records = scenes.get('data/templates/levelconfig.wbr', [])
    levels = {_integer(_required(r.values, 'LevelId'), 0, 14): r for r in level_records}
    if len(level_records) != 15 or len(levels) != 15 or len(catalog) != 15 or sorted(c['id'] for c in catalog) != list(range(15)):
        raise AssetError('Environment requires all fifteen original course IDs')
    materials, meshes, textures = [], [], {}
    material_ids, mesh_ids = {}, {}
    repairs, degraded_materials = {}, {}
    stages, stage_ids, courses = [], {}, []
    unsupported = Counter();unsupported_resources = set();scripted = set()

    def material(name):
        if name in material_ids:return material_ids[name]
        if name not in templates.get('shader', {}):
            raise AssetError('Missing environment shader: ' + name)
        shader = templates['shader'][name]
        diffuse = first(shader, 'MultilayerDiffuse', '') or next((v for v in shader.get('LayersDiffuseName', []) if v), '')
        texture = ''
        if diffuse:
            descriptor = templates.get('texture', {}).get(diffuse, {})
            filename = first(descriptor, 'DiffuseFile', diffuse)
            rgb = re.fullmatch(r'rgb/w\d+h\d+r(\d+)g(\d+)b(\d+)', filename)
            if rgb:
                colors = [_integer(int(v), 0, 255) / 255 for v in rgb.groups()]
                linear = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4 for v in colors]
                raw = solid_png(linear + [1]);filename += '-native.png'
            else:
                safe_name(filename)
                raw = source('data/art/' + filename)
            suffix = Path(filename).suffix.lower()
            if suffix not in ('.png', '.jpg', '.jpeg', '.dds'):
                raise AssetError('Unsupported environment texture format')
            texture = 'textures/' + hashlib.sha256(raw).hexdigest() + suffix
            textures[texture] = raw
        tint = _rgb(_required(shader, 'DiffuseColor'))
        alpha = _integer(_required(shader, 'DiffuseAlpha'), 0, 255) / 255
        identity = len(materials);material_ids[name] = identity
        materials.append({'name': name, 'texture': texture, 'color': tint + [alpha],
                          'lit': _flag(_required(shader, 'Lighting')),
                          'transparent': int(bool(_required(shader, 'Blend')))})
        # Preserve precise unsupported shader state in private conversion evidence.
        limitations = {}
        for key in ('Type', 'Layers', 'LayersUVEffect', 'LayersUVChannel', 'Blend', 'AlphaTest', 'ZWrite', 'ZTest', 'Culling'):
            if key in shader:limitations[key] = shader[key]
        degraded_materials[name] = limitations
        if len(materials) > 1024:raise AssetError('Environment material budget exceeded')
        return identity

    def mesh(name):
        if name in mesh_ids:return mesh_ids[name]
        descriptor = templates.get('rendervertexmodel', {}).get(name, {})
        filename = first(descriptor, 'FileName', name);safe_name(filename)
        original = read_static_mesh(source('data/art/' + filename))
        if any(original['repairs'].values()):repairs[filename] = original['repairs']
        overrides = next((v for v in descriptor.get('ShadersList', []) if v), '').split(';')
        groups = []
        for group in original['groups']:
            slot = group['slot']
            shader = overrides[slot] if slot < len(overrides) and overrides[slot] else group['material']
            groups.append({**group, 'material': material(shader)})
        vertices = [[v[0], v[2], v[1], v[3], v[5], v[4], v[6], v[7]] for v in original['vertices']]
        identity = len(meshes);mesh_ids[name] = identity
        meshes.append({'name': name, 'vertices': vertices, 'indices': original['indices'], 'groups': groups})
        if (len(meshes) > 2048 or sum(len(m['vertices']) for m in meshes) > 1000000
                or sum(len(m['indices']) for m in meshes) > 4000000):
            raise AssetError('Environment geometry budget exceeded')
        return identity

    for course in sorted(catalog, key=lambda c: c['id']):
        level = levels[course['id']]
        theme = _integer(_required(level.values, 'Theme'), 0, 2)
        if course['theme'] != theme:raise AssetError('Course/environment theme mismatch')
        stage_name = _required(level.values, 'Stage');safe_name(stage_name)
        if 'stage' in course and course['stage'] != stage_name:
            raise AssetError('Course/environment stage mismatch')
        path = scene_path(stage_name)
        if path not in scenes:raise AssetError('Missing original environment stage: ' + path)
        if path not in stage_ids:
            if len(stages) >= 3:raise AssetError('Environment stage budget exceeded')
            stage_ids[path] = len(stages)
            records = scenes[path];stage = _settings(path, records, templates);stage['theme'] = theme
            instances = []
            for record in records:
                values = _effective(record, templates)
                if first(values, 'IsTemplate', False):continue
                resources = values.get('Resources', [])
                if len(resources) > 256:raise AssetError('Environment actor resource budget exceeded')
                for field in ('ResourcesPos', 'ResourcesAng', 'ResourcesScale', 'ResourcesBoneAttach'):
                    if resources and len(values.get(field, [])) != len(resources):
                        raise AssetError('Environment resource arrays do not match: ' + field)
                if any(values.get(k, [None])[0] for k in ('OnActivate', 'OnDeactivate', 'OnStepScript')):
                    scripted.add(path + '#' + record.name)
                for index, resource in enumerate(resources):
                    if not resource:continue
                    category = resource.split(':', 1)[0]
                    if category not in ('RVMP', 'LGHT'):
                        unsupported[category] += 1;unsupported_resources.add(resource);continue
                    attached = first(values, 'AttachTo', '') or (values.get('ResourcesBoneAttach', [''] * len(resources))[index])
                    moving = any(first(values, 'Vel', [0, 0, 0])) or any(first(values, 'AngVel', [0, 0, 0])) or first(values, 'PathName', '')
                    if attached or moving:
                        unsupported['attached' if attached else 'moving'] += 1;unsupported_resources.add(resource);continue
                    if category == 'LGHT':
                        stage['lights'].append(_light(record.name, values, index, resource[5:], templates));continue
                    if any(abs(s) < 1e-6 for s in _vector(values['ResourcesScale'][index])):
                        unsupported['zero_scale'] += 1;continue
                    opacity = _number(_required(values, 'Opacity'), 0, 1)
                    resource_opacity = values.get('ResourcesOpacity', [1] * len(resources))
                    if len(resource_opacity) != len(resources):raise AssetError('Environment opacity arrays do not match')
                    opacity *= _number(resource_opacity[index], 0, 1)
                    if opacity <= 0:continue
                    instances.append({'actor': record.name, 'mesh': mesh(resource[5:]), 'matrix': _matrix(values, index), 'opacity': opacity})
            if len(instances) > 16384 or len(stage['lights']) > 64:raise AssetError('Environment stage instance budget exceeded')
            stage['instances'] = instances;stages.append(stage)
        stage_index = stage_ids[path]
        if stages[stage_index]['theme'] != theme:raise AssetError('A stage cannot belong to multiple themes')
        # A course starts at its serialized final (start) chunk in prepare_world.
        start_path = course['chunks'][-1]['file']
        spawns = [r for r in scenes[start_path] if r.type == 'STARTPOS' and not first(r.values, 'IsTemplate', False)]
        if not spawns:raise AssetError('Environment geometry needs the original course spawn')
        spawn = _native_vector(_required(_effective(spawns[0], templates), 'Pos'))
        courses.append({'id': course['id'], 'stage_index': stage_index, 'spawn': spawn,
                        'instances': stages[stage_index]['instances']})
    if sorted(s['theme'] for s in stages) != [0, 1, 2]:
        raise AssetError('Environment requires three distinct original themes')
    if not materials or not meshes:raise AssetError('Environment contains no supported original static geometry')
    metadata = _metadata(stages, courses);geometry = _world(materials, meshes, courses)
    for name, raw in {**textures, 'environment.dnv': metadata, 'environment-world.dnw': geometry}.items():
        target = output / name;_no_links(target);target.parent.mkdir(parents=True, exist_ok=True);target.write_bytes(raw)
    return {'schema': 1, 'scope': 'Authored environment settings/lights and initial RVMP poses; no full visual parity',
            'metadata_sha256': hashlib.sha256(metadata).hexdigest(), 'geometry_sha256': hashlib.sha256(geometry).hexdigest(),
            'metadata_bytes': len(metadata), 'geometry_bytes': len(geometry), 'meshes': len(meshes),
            'materials': len(materials), 'textures': len(textures),
            'stages': [{k: v for k, v in stage.items() if k != 'instances'} | {'instances': len(stage['instances'])} for stage in stages],
            'courses': [{'id': c['id'], 'stage_index': c['stage_index']} for c in courses],
            'unsupported_categories': dict(sorted(unsupported.items())), 'unsupported_resources': sorted(unsupported_resources),
            'scripted_actors_initial_pose_only': sorted(scripted), 'source_repairs': repairs,
            'material_state_not_fully_rendered': degraded_materials,
            'remaining': ['DECL billboard/decal geometry and animation', 'RPRI procedural ocean and water shader',
                          'BNMD skeletal stage actors', 'PAEM/EMIT/FXFX particle and scripted effects',
                          'SNFX environment audio', 'stage triggers and dynamic/attached actor behavior',
                          'material blend modes, alpha test, depth/culling, UV effects and multiple layers',
                          'shadow maps, specular lighting, dynamic camera framing and post processing']}
