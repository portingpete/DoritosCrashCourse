from pathlib import Path
import base64
import copy
import json
import os
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from engine_assets import AssetError, SceneRecord
from engine_scene import prepare_scene, scene_data
from engine_scripts import LUA32_HEADER, prepare_scripts
from engine_world import matrix_product, transform


def script():
    # Independently authored, valid Lua prototype returning a string.
    name, text = b'native-test\0', b'checkpoint\0'
    body = (struct.pack('<I', len(name)) + name + struct.pack('<II4B', 0, 0, 0, 0, 2, 2) +
            struct.pack('<I2I', 2, 1, 0x0100001e) + struct.pack('<IBI', 1, 4, len(text)) + text +
            struct.pack('<4I', 0, 0, 0, 0))
    return {'mode': 2, 'flags': 1, 'blob': base64.b64encode(LUA32_HEADER + body).decode()}


def actor_values(**changes):
    values = {'Pos': [[1, 2, 3]], 'Ang': [[0, 0, 0]], 'Scale': [[1, 1, 1]],
              'Enabled': [True], 'Mode': [1], 'MaxCommands': [1], 'CommandCounter': [1],
              'WaitTime': [[0, 0]], 'UseTimePerActor': [False], 'SendToToucher': [False]}
    values.update(changes)
    return values


def fixture():
    # A second course reuses both chunks; identities must remain unambiguous.
    start, end = 'data/maps/start.wbm', 'data/maps/end.wbm'
    links = lambda low, high: [SceneRecord('CHUNKLINK', 'in', {'LinkType': [0], 'Pos': [low]}),
                              SceneRecord('CHUNKLINK', 'out', {'LinkType': [1], 'Pos': [high]})]
    resources = {'Resources': ['RVMP:decor', '', 'CLP3:arbitrary/volume'],
                 'ResourcesPos': [[0, 0, 0], [0, 0, 0], [0, 7, 0]],
                 'ResourcesAng': [[0, 0, 0]] * 3, 'ResourcesScale': [[1, 1, 1]] * 2 + [[1, 15, 2]],
                 'ResourcesBoneAttach': ['', '', '']}
    scenes = {
        # Sort before maps so the inherited script index cannot be guessed from
        # course record order. Also include unrelated scripts in the catalog.
        'data/aaa.wbr': [SceneRecord('ACTOR', 'unrelated', {'OnActivate': [script()]})],
        start: links([3, 4, 5], [13, 8, 9]) +
               [SceneRecord('STARTPOS', 'not-named-start', actor_values(Mode=[0]))],
        end: links([2, 3, 4], [22, 3, 4]) +
             [SceneRecord('STARTPOS', 'not-named-finish', actor_values(
                 StartPosTemplate=['checkpoint'], Ang=[[90, 0, 0]], Command=[script()]))],
        'data/templates/startpos.wbr': [SceneRecord('STARTPOSMASTER', 'checkpoint',
                                                   actor_values(Command=[script()], OnRespawnScript=[script()], **resources))],
        'data/templates/cprimitive3.wbr': [SceneRecord('CPRIMITIVE3MASTER', 'arbitrary/volume', {
            'PrimitiveType': [2], 'CollisionMask': [1], 'CollisionGroup': [512],
            'BoundingBoxMin': [[-1, -2, -3]], 'BoundingBoxMax': [[4, 5, 6]]})]}
    catalog = [{'id': i, 'name': 'course-' + str(i), 'chunks': [
        {'name': 'common/end_chunk', 'file': end}, {'name': 'common/start_chunk', 'file': start}]} for i in range(2)]
    return scenes, catalog


def script_locations(path):
    """Read the actual DNL1 location table to certify the shared numeric IDs."""
    data, offset = path.read_bytes(), 12
    if data[:8] != b'DNL1' + struct.pack('<I', 1):
        raise AssertionError('Not a script catalog')
    result = []
    for _ in range(struct.unpack_from('<I', data, 8)[0]):
        strings = []
        for _ in range(4):
            size = struct.unpack_from('<I', data, offset)[0]
            offset += 4
            strings.append(data[offset:offset+size].decode())
            offset += size
        ordinal, index, size = struct.unpack_from('<III', data, offset)
        offset += 12 + size
        result.append(tuple(strings + [ordinal, index]))
    if offset != len(data):
        raise AssertionError('Trailing script data')
    return result


class EngineSceneTests(unittest.TestCase):
    def test_course_order_offsets_native_bounds_and_authored_identity(self):
        scenes, catalog = fixture()
        courses, exclusions, text = scene_data(scenes, catalog)
        self.assertFalse(exclusions)
        self.assertFalse(text)
        course = courses[0]
        self.assertEqual([c['name'] for c in course['chunks']], ['common/start_chunk', 'common/end_chunk'])
        self.assertEqual([c['serialized_ordinal'] for c in course['chunks']], [1, 0])
        # CHUNKLINK end[13,8,9] minus next start[2,3,4], then Y/Z swap.
        self.assertEqual(course['chunks'][1]['offset'], [11, 5, 5])
        self.assertEqual(course['records'][1]['position'], [12, 8, 7])
        self.assertEqual(course['records'][1]['resources'], ['RVMP:decor', '', 'CLP3:arbitrary/volume'])
        box = course['records'][1]['boxes'][0]
        self.assertEqual((box['resource'], box['collision_mask'], box['collision_group']), (2, 1, 512))
        self.assertEqual(box['minimum'], [-1, -3, -2])
        self.assertEqual(box['maximum'], [4, 6, 5])
        # Authored 90-degree source yaw keeps the checkpoint's local Y vertical;
        # native Z is actor height7 + resource offset7, not a name heuristic.
        self.assertAlmostEqual(box['transform'][14], 14)
        self.assertAlmostEqual(box['transform'][10], 15)
        self.assertEqual([r['ordinal'] for r in course['records']], [2, 2])
        identities = {(c['id'], r['chunk'], r['id']) for c in courses for r in c['records']}
        self.assertEqual(len(identities), 4)

    def test_resolved_template_scripts_match_actual_script_catalog(self):
        scenes, catalog = fixture()
        with tempfile.TemporaryDirectory() as folder:
            prepare_scripts(scenes, Path(folder))
            locations = script_locations(Path(folder) / 'scripts.dnl')
            courses, _, _ = scene_data(scenes, catalog)
            references = courses[0]['records'][1]['scripts']
            by_field = {r['field']: r for r in references}
            self.assertEqual(by_field['Command']['source'], 'data/maps/end.wbm')
            self.assertEqual(by_field['OnRespawnScript']['source'], 'data/templates/startpos.wbr')
            self.assertEqual(by_field['OnRespawnScript']['type'], 'STARTPOSMASTER')
            for reference in references:
                location = tuple(reference[k] for k in ('source', 'type', 'actor', 'field', 'ordinal', 'index'))
                self.assertEqual(locations[reference['catalog_index']], location)

    def test_noncommuting_resource_transform_uses_shared_world_convention(self):
        scenes, catalog = fixture()
        actor = scenes['data/maps/end.wbm'][2]
        actor.values['Ang'] = [[23, 41, 17]]
        actor.values['Scale'] = [[2, 3, 4]]
        actor.values['ResourcesAng'] = [[0, 0, 0], [0, 0, 0], [11, 7, 31]]
        courses, _, _ = scene_data(scenes, catalog)
        box = courses[0]['records'][1]['boxes'][0]
        basis = [[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]]
        matrix = transform([12, 7, 8], [23, 41, 17], [2, 3, 4])
        matrix = matrix_product(basis, matrix_product(matrix, matrix_product(transform([0, 7, 0], [11, 7, 31], [1, 15, 2]), basis)))
        # Certify the frame conversion on an asymmetric local point. This would
        # fail if actor/resource composition or transpose order was reversed.
        point = [1, 3, 2, 1]
        expected = [sum(matrix[row][col] * point[col] for col in range(4)) for row in range(3)]
        actual = [sum(box['transform'][col*4+row] * point[col] for col in range(4)) for row in range(3)]
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b)

    def test_attached_dynamic_text_and_nonbox_records_are_explicitly_excluded(self):
        for change, reason in [({'AttachTo': ['parent']}, 'attached actor'),
                               ({'Vel': [[1, 0, 0]]}, 'dynamic Vel'),
                               ({'PathName': ['rail']}, 'path-driven actor'),
                               ({'OnActivate': [{'mode': 1, 'flags': 0, 'blob': ''}]}, 'unsupported text script')]:
            scenes, catalog = fixture()
            scenes['data/maps/end.wbm'][2].values.update(change)
            courses, exclusions, _ = scene_data(scenes, catalog)
            self.assertEqual(len(exclusions), 2)
            record = courses[0]['records'][1]
            self.assertFalse(record['supported'])
            self.assertIn(reason, record['exclusion'])
            self.assertEqual(record['boxes'], [])
            self.assertEqual(record['resources'][-1], 'CLP3:arbitrary/volume')
        scenes, catalog = fixture()
        scenes['data/templates/cprimitive3.wbr'][0].values['PrimitiveType'] = [1]
        self.assertIn('unsupported collision primitive', scene_data(scenes, catalog)[1][0]['reason'])

    def test_invalid_authored_inputs_fail_before_writing(self):
        changes = [lambda s, c: s['data/maps/start.wbm'].append(copy.deepcopy(s['data/maps/start.wbm'][0])),
                   lambda s, c: s['data/maps/end.wbm'][2].values.update(StartPosTemplate=['missing']),
                   lambda s, c: s['data/maps/end.wbm'][2].values.update(Pos=[[float('nan'), 0, 0]]),
                   lambda s, c: s['data/maps/end.wbm'][2].values.update(ResourcesScale=[]),
                   lambda s, c: s['data/maps/end.wbm'][2].values.update(MaxCommands=[2**32]),
                   lambda s, c: s['data/templates/cprimitive3.wbr'][0].values.update(BoundingBoxMax=[[-2, -3, -4]]),
                   lambda s, c: s['data/maps/end.wbm'][2].values.update(Resources=['x'*4097])]
        for change in changes:
            scenes, catalog = fixture()
            change(scenes, catalog)
            with tempfile.TemporaryDirectory() as folder:
                with self.assertRaises(AssetError):
                    prepare_scene(scenes, catalog, Path(folder))
                self.assertFalse((Path(folder) / 'scene.dns').exists())

    def test_export_is_deterministic_and_bounded(self):
        scenes, catalog = fixture()
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            metadata = prepare_scene(scenes, catalog, output)
            first_export = (output / 'scene.dns').read_bytes()
            self.assertEqual(first_export[:12], b'DNS1' + struct.pack('<II', 1, 2))
            self.assertEqual(metadata['courses'][0]['start_positions'], 2)
            self.assertEqual(metadata['bytes'], len(first_export))
            prepare_scene(dict(reversed(list(scenes.items()))), catalog, output)
            self.assertEqual(first_export, (output / 'scene.dns').read_bytes())

    @unittest.skipUnless(os.environ.get('DORITOS_SCENE_ASSET_SOURCE'), 'Optional user-owned scene input')
    def test_owned_all_courses_script_references_and_finish_masks(self):
        source = Path(os.environ['DORITOS_SCENE_ASSET_SOURCE'])
        scenes = {path: [SceneRecord(**record) for record in rows] for path, rows in
                  json.loads((source / 'source-scenes.json').read_text()).items()}
        catalog = json.loads((source / 'manifest.json').read_text())['courses']
        courses, exclusions, text = scene_data(scenes, catalog)
        self.assertEqual([c['id'] for c in courses], list(range(15)))
        self.assertEqual(exclusions, [])
        self.assertEqual(text, [])
        locations = script_locations(source / 'scripts.dnl')
        for course in courses:
            self.assertEqual(sum(r['type'] == 'STARTPOS' and r['mode'] == 0 for r in course['records']), 1)
            self.assertEqual(sum(b['collision_group'] == 512 for r in course['records'] for b in r['boxes']), 1)
            for record in course['records']:
                for reference in record['scripts']:
                    self.assertEqual(locations[reference['catalog_index']], tuple(reference[k] for k in
                                     ('source', 'type', 'actor', 'field', 'ordinal', 'index')))


if __name__ == '__main__':
    unittest.main()
