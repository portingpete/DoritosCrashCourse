"""Convert user-owned assets into the independent engine's pointer-free format."""
from __future__ import annotations
import argparse
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import struct
import sys

from cal3d_character import private_output, read_csf
from character_packs import ROOT, resolve_pack, original_inputs, digest
from engine_assets import AssetError, read_animation, read_scene
from inspect_pak import read_directory, read_entry, _no_links
from engine_world import prepare_world
from engine_gameplay import movement_data
from engine_scripts import prepare_scripts
from engine_environment import prepare_environment
from engine_scene import prepare_scene


def prepare(pak: Path, output: Path, character='cj'):
    output = private_output(output)
    _no_links(output)
    originals = original_inputs(pak)
    skeleton = read_csf(originals['data/art/_avatar/p_test.csf'])
    pack = resolve_pack(character)
    entries = {e.name: e for e in read_directory(pak)}
    source = lambda name: read_entry(pak, entries[name])
    scenes = {name: read_scene(source(name)) for name in entries if name.endswith(('.wbm', '.wbr'))}
    levels = sorted(scenes['data/templates/levelconfig.wbr'], key=lambda r: r.values['LevelId'][0])
    tracks = {r.name: r for r in scenes['data/templates/track.wbr']}
    chunks = {r.name: r for r in scenes['data/templates/chunk.wbr']}
    if [r.values['LevelId'][0] for r in levels] != list(range(15)):
        raise AssetError('Original course catalog must contain all 15 unique IDs')
    catalog = []
    for level in levels:
        track = tracks[level.values['Track'][0]]
        ordered = []
        for name in track.values['Chunk']:
            path = 'data/maps/' + chunks[name].values['Map'][0].removesuffix('.wxm') + '.wbm'
            if path not in scenes: raise AssetError('Missing original course chunk: ' + path)
            ordered.append({'name': name, 'file': path, 'actors': len(scenes[path])})
        catalog.append({'id': level.values['LevelId'][0], 'name': level.name,
                        'theme': level.values['Theme'][0], 'stage':level.values['Stage'][0], 'chunks': ordered})
    movement_settings, movement_clips, movement_binary = movement_data(scenes)
    clips = {key: read_animation(source('data/art/_avatar/' + filename.lower() + '.caf'), len(skeleton.bones))
             for key, filename in movement_clips.items()}
    output.mkdir(parents=True, exist_ok=True)
    _no_links(output / 'movement.dnp')
    (output / 'movement.dnp').write_bytes(movement_binary)
    for index, image in enumerate(pack.textures):
        target = output / f'diffuse-{index}.png'
        _no_links(target)
        target.write_bytes(image)
    binary = bytearray(b'DNE1' + struct.pack('<III', 2, len(skeleton.bones), len(pack.mesh.submeshes)))
    def string(value):
        encoded = value.encode('utf-8')
        binary.extend(struct.pack('<I', len(encoded)) + encoded)
    string(pack.metadata['name'])
    for bone in skeleton.bones:
        binary.extend(struct.pack('<i14f', bone.parent, *bone.translation, *bone.rotation,
                                  *bone.inverse_bind_translation, *bone.inverse_bind_rotation))
    for index, submesh in enumerate(pack.mesh.submeshes):
        binary.extend(struct.pack('<III', index if len(pack.textures) > index else 0,
                                  len(submesh.vertices), len(submesh.faces) * 3))
        for vertex in submesh.vertices:
            influences = list(vertex.influences) + [(0, 0)] * (4-len(vertex.influences))
            binary.extend(struct.pack('<8f4I4f', *vertex.position, *vertex.normal, *vertex.uv[0],
                                      *(bone for bone, weight in influences), *(weight for bone, weight in influences)))
        for face in submesh.faces: binary.extend(struct.pack('<3I', *face))
    binary.extend(struct.pack('<I', len(clips)))
    for name, clip in clips.items():
        string(name)
        binary.extend(struct.pack('<fI', clip['duration'], len(clip['tracks'])))
        for track in clip['tracks']:
            binary.extend(struct.pack('<II', track['bone'], len(track['keys'])))
            for key in track['keys']: binary.extend(struct.pack('<8f', *key))
        binary.extend(struct.pack('<I',len(clip['motion'])))
        for key in clip['motion']:binary.extend(struct.pack('<4f',*key))
    binary.extend(struct.pack('<I', len(catalog)))
    for level in catalog:
        binary.extend(struct.pack('<III', level['id'], level['theme'], len(level['chunks'])))
        string(level['name'])
        for chunk in level['chunks']:
            string(chunk['name'])
            binary.extend(struct.pack('<I', chunk['actors']))
    target = output / 'character.dne'
    _no_links(target)
    target.write_bytes(binary)
    world = prepare_world(source, scenes, catalog, output)
    scripts = prepare_scripts(scenes, output)
    environment = prepare_environment(source, scenes, catalog, output)
    scene = prepare_scene(scenes, catalog, output)
    manifest = {'schema': 1, 'engine': 'independent-native', 'character': pack.metadata['name'],
                'source_package_sha256': hashlib.sha256(pak.read_bytes()).hexdigest(),
                'pack_sha256': pack.fingerprint, 'asset_sha256': digest(binary),
                'bones': len(skeleton.bones), 'scenes_parsed': len(scenes), 'courses': catalog,
                'animations': {name: {'source': movement_clips[name], 'duration': c['duration'], 'tracks': len(c['tracks']),
                                      'motion_keys': len(c['motion'])} for name,c in clips.items()},
                'movement_settings': movement_settings,
                'movement_sha256': digest(movement_binary),
                'runtime_uses_xex': False, 'runtime_uses_guest_memory': False,
                'world': world,
                'scripts': scripts,
                'environment': environment,
                'scene': scene,
                'scope': 'Native character animation, static geometry and basic course movement; remaining actions, dynamic actors and full gameplay pending'}
    for name in ('manifest.json', 'source-scenes.json'):
        _no_links(output / name)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / 'source-scenes.json').write_text(json.dumps({name: [asdict(r) for r in rows] for name,rows in scenes.items()}), encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pak', type=Path, default=ROOT / 'game/data.pak')
    parser.add_argument('--output', type=Path, default=ROOT / 'build_native/engine_inputs')
    parser.add_argument('--character', default='cj')
    args = parser.parse_args()
    try:
        result = prepare(args.pak, args.output, args.character)
        print(json.dumps({k:v for k,v in result.items() if k not in ('courses',)}, indent=2))
    except (ValueError, OSError, KeyError) as error:
        print('Native engine assets: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
