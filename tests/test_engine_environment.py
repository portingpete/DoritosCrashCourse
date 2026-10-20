from copy import deepcopy
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from engine_assets import AssetError, Reader, SceneRecord, read_scene
from engine_environment import prepare_environment


def synthetic_mesh():
    header = struct.pack('<BIBHH5BI', 102, 2, 1, 3, 1, 0, 0, 1, 0, 0, 0) + bytes(196)
    vertices = b''.join(struct.pack('<8f', *p, 0, 0, 1, 0, 0) for p in ((0, 0, 0), (1, 0, 0), (0, 1, 0)))
    return header + b'native/material\0'.ljust(32, b'\0') + vertices + struct.pack('<5H', 0, 1, 2, 0, 0)


def fixture():
    camera = {'CameraDistance': [8.0], 'CameraLookAt': [[2.5, .5, 0]], 'CameraPitch': [70.], 'CameraYaw': [195.],
              'CameraDistance2': [9.], 'CameraLookAt2': [[3., .75, 1.]], 'CameraPitch2': [75.], 'CameraYaw2': [190.], 'StartFov': [60.]}
    light = {'Type': [1], 'LightMask': [3], 'RenderPassMask': [8], 'Color': [[192, 255, 255]], 'Specular': [[224, 224, 224]],
             'DiffuseFactor': [1.25], 'SpecularFactor': [2.], 'Range': [10.], 'CastShadows': [True],
             'Attenuation0': [0.], 'Attenuation1': [.1], 'Attenuation2': [0.]}
    actor = {'Pos': [[10, 20, 30]], 'Ang': [[0, 0, 0]], 'Scale': [[2, 1, 1]],
             'Resources': ['RVMP:env/synthetic.rvm', 'LGHT:native/light', 'DECL:native/decal'],
             'ResourcesPos': [[1, 2, 3], [1, 2, 3], [0, 0, 0]], 'ResourcesAng': [[0, 0, 0]] * 3,
             'ResourcesScale': [[1, 1, 1]] * 3, 'ResourcesBoneAttach': ['', '', ''],
             'ResourcesOpacity': [.5, 1, 1], 'Opacity': [.75], 'IsTemplate': [False]}
    scenes = {'data/templates/gameconfig.wbr': [SceneRecord('GAMECONFIGMASTER', 'default', camera)],
              'data/templates/light.wbr': [SceneRecord('LIGHTMASTER', 'native/light', light)],
              'data/templates/shader.wbr': [SceneRecord('SHADERMASTER', 'native/material', {
                  'DiffuseColor': [[128, 255, 64]], 'DiffuseAlpha': [255], 'Lighting': [False], 'Blend': [0],
                  'LayersDiffuseName': ['', 'rgb/w1h1r32g64b128'], 'AlphaTest': [64]})],
              'data/templates/texture.wbr': [], 'data/templates/rendervertexmodel.wbr': [],
              'data/templates/levelconfig.wbr': [],
              'data/maps/start.wbm': [SceneRecord('STARTPOS', 'start', {'Pos': [[-3, 2, 1]]})]}
    catalog = []
    for index in range(15):
        theme = index // 5
        stage = 'environment/' + ('usa', 'europe', 'japan')[theme] + '_env.wxm'
        scenes['data/templates/levelconfig.wbr'].append(SceneRecord('LEVELCONFIGMASTER', str(index), {
            'LevelId': [index], 'Stage': [stage], 'Theme': [theme]}))
        catalog.append({'id': index, 'theme': theme, 'stage': stage, 'chunks': [{'file': 'data/maps/start.wbm'}]})
        path = 'data/maps/' + stage[:-4] + '.wbm'
        scenes[path] = [SceneRecord('LEVEL', 'level', {
            'GameConfig': [''], 'BGColor': [[5 * theme, 0, 21 * theme]], 'FogEnabled': [theme == 2],
            'FogColor': [[128, 128, 128]], 'FogStart': [30.], 'FogEnd': [100.], 'StartFov': [70.]}),
            SceneRecord('ACTOR', 'env/static', deepcopy(actor))]
    return scenes, catalog


def metadata(data):
    r = Reader(data)
    if r.take(4) != b'DNV1' or r.u32() != 1:raise AssertionError('bad independent environment header')
    stages = []
    for _ in range(r.u32()):
        stage = {'theme': r.u32(), 'source': r.string(), 'background': r.floats(3), 'fog_enabled': r.u32(),
                 'fog_color': r.floats(3), 'fog': r.floats(2), 'fov': r.floats(1)[0],
                 'camera_source': r.string(), 'camera': r.floats(13), 'lights': []}
        for _ in range(r.u32()):
            stage['lights'].append({'actor': r.string(), 'resource': r.string(), 'flags': r.unpack('3I'),
                                    'properties': r.floats(9), 'shadows': r.u32(), 'attenuation': r.floats(3),
                                    'matrix': r.floats(16)})
        stages.append(stage)
    courses = [r.unpack('II') for _ in range(r.u32())]
    if r.position != len(data):raise AssertionError('unconsumed metadata')
    return stages, courses


def geometry(data):
    r = Reader(data)
    if r.take(4) != b'DNW1' or r.u32() != 1:raise AssertionError('bad independent world header')
    materials = [{'name': r.string(), 'texture': r.string(), 'values': r.unpack('4fII')} for _ in range(r.u32())]
    meshes = []
    for _ in range(r.u32()):
        name = r.string();nv, ni, ng = r.unpack('3I')
        meshes.append({'name': name, 'vertices': [r.floats(8) for _ in range(nv)],
                       'indices': r.unpack('I' * ni), 'groups': [r.unpack('3I') for _ in range(ng)]})
    courses = []
    for _ in range(r.u32()):
        identity = r.u32();anchors = r.floats(6)
        instances = [{'actor': r.string(), 'mesh': r.u32(), 'matrix': r.floats(16), 'opacity': r.floats(1)[0]}
                     for _ in range(r.u32())]
        courses.append({'id': identity, 'anchors': anchors, 'instances': instances})
    if r.position != len(data):raise AssertionError('unconsumed geometry')
    return materials, meshes, courses


class EnvironmentTests(unittest.TestCase):
    def test_authored_settings_shared_meshes_and_static_native_basis(self):
        scenes, catalog = fixture()
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            manifest = prepare_environment(lambda _: synthetic_mesh(), scenes, catalog, output)
            stages, mapping = metadata((output / 'environment.dnv').read_bytes())
            materials, meshes, courses = geometry((output / 'environment-world.dnw').read_bytes())
            self.assertEqual(mapping, [(i, i // 5) for i in range(15)])
            self.assertEqual(len(stages), 3)
            self.assertEqual((stages[0]['fov'], stages[0]['camera'][0]), (70, 8))
            self.assertEqual(stages[0]['camera'][1:4], [2.5, 0, .5])
            self.assertAlmostEqual(stages[2]['background'][0], 10 / 255)
            self.assertTrue(stages[2]['fog_enabled'])
            light = stages[0]['lights'][0]
            self.assertEqual(light['flags'], (1, 3, 8))
            self.assertEqual(light['matrix'][12:15], [12, 33, 22])
            self.assertEqual(light['properties'][6:], [1.25, 2, 10])
            self.assertEqual(len(meshes), 1)
            self.assertEqual(meshes[0]['vertices'][2][:3], [0, 1, 0])
            self.assertEqual(courses[14]['anchors'], [-3, 1, 2, -3, 1, 2])
            self.assertEqual(courses[14]['instances'][0]['opacity'], .375)
            self.assertEqual(courses[0]['instances'][0]['matrix'][12:15], [12, 33, 22])
            self.assertEqual(manifest['unsupported_categories'], {'DECL': 3})
            self.assertEqual((manifest['meshes'], manifest['materials'], manifest['textures']), (1, 1, 1))
            self.assertTrue((output / materials[0]['texture']).is_file())
            before = (output / 'environment.dnv').read_bytes()
            self.assertEqual(manifest, prepare_environment(lambda _: synthetic_mesh(), scenes, catalog, output))
            self.assertEqual(before, (output / 'environment.dnv').read_bytes())

    def test_missing_or_invalid_authoritative_fields_fail_before_writing(self):
        changes = [lambda s, c: s['data/maps/environment/usa_env.wbm'][0].values.pop('BGColor'),
                   lambda s, c: s['data/maps/environment/usa_env.wbm'][0].values.update(StartFov=[float('nan')]),
                   lambda s, c: s['data/maps/environment/usa_env.wbm'][0].values.update(FogEnd=[20]),
                   lambda s, c: s['data/templates/gameconfig.wbr'][0].values.update(CameraDistance=[0]),
                   lambda s, c: s['data/templates/levelconfig.wbr'][0].values.update(Stage=['../outside.wxm']),
                   lambda s, c: s['data/templates/levelconfig.wbr'].append(s['data/templates/levelconfig.wbr'][0]),
                   lambda s, c: c[0].update(theme=1),
                   lambda s, c: s['data/maps/environment/usa_env.wbm'][1].values.update(ResourcesPos=[]),
                   lambda s, c: s['data/maps/environment/usa_env.wbm'][1].values.update(Scale=[[0, 1, 1]])]
        for change in changes:
            with self.subTest(change=changes.index(change)), tempfile.TemporaryDirectory() as folder:
                scenes, catalog = fixture();change(scenes, catalog)
                with self.assertRaises(ValueError):prepare_environment(lambda _: synthetic_mesh(), scenes, catalog, Path(folder))
                self.assertEqual(list(Path(folder).iterdir()), [])

    def test_animated_and_attached_resources_are_reported_without_static_instances(self):
        scenes, catalog = fixture()
        records = scenes['data/maps/environment/usa_env.wbm']
        moving = deepcopy(records[1]);moving.name = 'moving';moving.values['Vel'] = [[1, 0, 0]]
        attached = deepcopy(records[1]);attached.name = 'attached';attached.values['AttachTo'] = ['parent']
        records.extend([moving, attached])
        with tempfile.TemporaryDirectory() as folder:
            manifest = prepare_environment(lambda _: synthetic_mesh(), scenes, catalog, Path(folder))
        self.assertEqual(manifest['stages'][0]['instances'], 1)
        self.assertEqual(manifest['unsupported_categories'], {'DECL': 5, 'attached': 2, 'moving': 2})

    @unittest.skipUnless((ROOT / 'game/data.pak').is_file(), 'Private original package not present')
    def test_owned_package_stage_selection_geometry_and_authoritative_settings(self):
        from inspect_pak import read_directory, read_entry
        pak = ROOT / 'game/data.pak'
        entries = {e.name: e for e in read_directory(pak)}
        source = lambda name: read_entry(pak, entries[name])
        scenes = {n: read_scene(source(n)) for n in entries if n.startswith('data/templates/') and n.endswith('.wbr')}
        tracks = {r.name: r for r in scenes['data/templates/track.wbr']}
        chunks = {r.name: r for r in scenes['data/templates/chunk.wbr']}
        catalog = []
        for level in scenes['data/templates/levelconfig.wbr']:
            stage = level.values['Stage'][0]
            stage_path = 'data/maps/' + stage[:-4] + '.wbm'
            if stage_path not in scenes:scenes[stage_path] = read_scene(source(stage_path))
            course_chunks = []
            for name in tracks[level.values['Track'][0]].values['Chunk']:
                path = 'data/maps/' + chunks[name].values['Map'][0][:-4] + '.wbm'
                course_chunks.append({'file': path})
            start = course_chunks[-1]['file']
            if start not in scenes:scenes[start] = read_scene(source(start))
            catalog.append({'id': level.values['LevelId'][0], 'theme': level.values['Theme'][0], 'stage': stage, 'chunks': course_chunks})
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            manifest = prepare_environment(source, scenes, catalog, output)
            stages, mapping = metadata((output / 'environment.dnv').read_bytes())
            materials, meshes, courses = geometry((output / 'environment-world.dnw').read_bytes())
            self.assertEqual([s['fov'] for s in stages], [70, 70, 70])
            self.assertEqual([s['fog_enabled'] for s in stages], [0, 0, 0])
            self.assertEqual([len(s['lights']) for s in stages], [3, 3, 3])
            self.assertEqual([len(c['instances']) for c in courses[:5]], [manifest['stages'][0]['instances']] * 5)
            self.assertEqual(mapping, [(i, i // 5) for i in range(15)])
            self.assertEqual(stages[2]['source'], 'data/maps/environment/japan_env.wbm')
            self.assertAlmostEqual(stages[2]['background'][2], 21 / 255)
            self.assertGreater(len(meshes), 10)
            self.assertGreater(len(materials), 10)
            self.assertGreater(manifest['unsupported_categories']['DECL'], 100)
            self.assertEqual(manifest['metadata_sha256'], hashlib.sha256((output / 'environment.dnv').read_bytes()).hexdigest())


if __name__ == '__main__':unittest.main()
