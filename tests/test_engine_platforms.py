from copy import deepcopy
from pathlib import Path
import math
import os
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from engine_assets import AssetError,Reader,SceneRecord,read_scene
from engine_world import diffuse_motion,platform_surface,prepare_world


def record(name,**changes):
    values={'Pos':[[1,2,3]],'Ang':[[25,35,15]],'Scale':[[7,2,3]],
            'UseForFloor':[True],'SlidablePlatform':[False],'IsConveyor':[True],
            'Friction':[.5],'SlideVelocity':[[1,2,3]],
            'Resources':['RVMP:model','CLP3:box','CLP3:box'],
            'ResourcesPos':[[0,0,0],[4,5,6],[-3,2,1]],
            'ResourcesAng':[[0,0,0],[7,11,13],[31,29,23]],
            'ResourcesScale':[[1,1,1],[2,3,4],[4,2,3]],
            'ResourcesBoneAttach':['','','']}
    values.update(changes)
    return SceneRecord('PLATFORM',name,values)


def fixture():
    links=lambda a,b:[SceneRecord('CHUNKLINK','in',{'LinkType':[0],'Pos':[a]}),
                      SceneRecord('CHUNKLINK','out',{'LinkType':[1],'Pos':[b]})]
    start='data/maps/start.wbm';end='data/maps/end.wbm'
    scenes={start:links([0,0,0],[10,1,2])+[
        SceneRecord('STARTPOS','start',{'Pos':[[0,0,0]]}),record('repeated')],
        end:links([2,3,4],[20,3,4])+[
            record('repeated',UseForFloor=[False],SlidablePlatform=[True],IsConveyor=[False],SlideVelocity=[[0,0,0]],
                   Resources=['WCOM:triangle'],ResourcesPos=[[0,0,0]],ResourcesAng=[[0,0,0]],ResourcesScale=[[1,1,1]],ResourcesBoneAttach=['']),
            record('attached',AttachTo=['parent']),record('moving',Vel=[[1,0,0]])],
        'data/templates/shader.wbr':[
            SceneRecord('SHADERMASTER','normal_a',{'MultilayerDiffuse':['body.jpg'],'MultilayerUVSpeed':[[0,0]],
                                                  'ShifterSpeed':[[9,9]]}),
            SceneRecord('SHADERMASTER','normal_b',{'MultilayerDiffuse':['belt.jpg'],'MultilayerUVSpeed':[[-1.25,0]],'MultilayerUVEffect':[0]})],
        'data/templates/texture.wbr':[],
        'data/templates/rendervertexmodel.wbr':[SceneRecord('RVMMASTER','model',{'FileName':['model.rvm']})],
        'data/templates/collisionmesh.wbr':[SceneRecord('WCOMMASTER','triangle',{'FileName':['triangle.wco']})],
        'data/templates/cprimitive3.wbr':[SceneRecord('CPRIMITIVE3MASTER','box',{'PrimitiveType':[2],
            'BoundingBoxMin':[[-.5,-.5,-.5]],'BoundingBoxMax':[[.5,.5,.5]]})]}
    catalog=[{'id':0,'name':'synthetic','theme':0,'chunks':[{'name':'common/end_chunk','file':end},
                                                              {'name':'common/start_chunk','file':start}]}]
    return scenes,catalog


def mesh():
    return {'vertices':[[0,0,0,0,1,0,0,0],[1,0,0,0,1,0,1,0],[0,0,1,0,1,0,0,1]],
            'indices':[0,1,2,0,2,1],
            'groups':[{'material':'normal_a','slot':0,'first':0,'count':3},
                      {'material':'normal_b','slot':1,'first':3,'count':3}], 'repairs':{}}


def collision(data):
    r=Reader(data)
    if r.take(4)!=b'DNC1' or r.u32()!=2:raise AssertionError('Not DNC1 version2')
    courses=[]
    for _ in range(r.count(15)):
        course={'id':r.u32(),'platforms':[],'triangles':[]}
        for _ in range(r.count(16384)):
            actor=r.string();chunk,ordinal,flags=r.unpack('III');friction=r.floats(1)[0]
            course['platforms'].append({'actor':actor,'chunk':chunk,'ordinal':ordinal,'flags':flags,
                                        'friction':friction,'slide_velocity':r.floats(3),'orientation':r.floats(16)})
        for _ in range(r.count(2000000)):
            positions=r.floats(9);owner=r.u32()
            if owner>=len(course['platforms']):raise AssertionError('Triangle has no owning platform')
            course['triangles'].append((positions,owner))
        courses.append(course)
    if r.position!=len(data):raise AssertionError('Trailing DNC data')
    return courses


def world_materials(data):
    r=Reader(data)
    if r.take(4)!=b'DNW1' or r.u32()!=2:raise AssertionError('Not DNW1 version2')
    materials=[]
    for _ in range(r.count(1024)):
        name,texture=r.string(),r.string();color=r.floats(4);lit,transparent=r.unpack('II')
        materials.append({'name':name,'texture':texture,'color':color,'lit':lit,'transparent':transparent,'uv_speed':r.floats(2)})
    for _ in range(r.count(2048)):
        r.string();vertices,indices,groups=r.unpack('III')
        r.take(vertices*32+indices*4+groups*12)
    for _ in range(r.count(15)):
        r.u32();r.floats(6)
        for _ in range(r.count(16384)):
            r.string();r.u32();r.floats(17)
    if r.position!=len(data):raise AssertionError('Trailing DNW data')
    return materials


def export(scenes,catalog,output,source=None):
    with patch('engine_world.read_static_mesh',return_value=mesh()),patch('engine_world.read_collision_mesh',return_value={
            'vertices':[[0,0,0],[2,0,0],[0,0,2]],'indices':[0,1,2]}):
        return prepare_world(source or (lambda _:b'synthetic-image'),scenes,catalog,output)


class EnginePlatformsTests(unittest.TestCase):
    def test_theme_shader_variants_select_matching_texture_motion_and_missing_fallback(self):
        scenes,catalog=fixture();prefix='props/obs/conv_a/'
        shaders=scenes['data/templates/shader.wbr']
        shaders[0].name=prefix+'usa/normal_a';shaders[1].name=prefix+'usa/normal_b'
        scenes['data/templates/rendervertexmodel.wbr'][0].values['ShadersList']=[
            prefix+'usa/normal_a;'+prefix+'usa/normal_b']
        for theme in ('eu','japan'):
            variant=deepcopy(shaders[1]);variant.name=prefix+theme+'/normal_b'
            variant.values['MultilayerDiffuse']=['belt-'+theme+'.jpg'];shaders.append(variant)
        # No EU/Japan normal_a exists here: preserve the authored USA fallback.
        catalog=[dict(deepcopy(catalog[0]),id=theme,theme=theme) for theme in range(3)]
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);export(scenes,catalog,output,lambda name:name.encode())
            materials=world_materials((output/'world.dnw').read_bytes())
            self.assertEqual([m['name'] for m in materials],[prefix+'usa/normal_a',prefix+'usa/normal_b',
                prefix+'usa/normal_a',prefix+'eu/normal_b',prefix+'usa/normal_a',prefix+'japan/normal_b'])
            for material,filename in zip(materials,['body.jpg','belt.jpg','body.jpg','belt-eu.jpg','body.jpg','belt-japan.jpg']):
                self.assertEqual((output/material['texture']).read_bytes(),('data/art/'+filename).encode())
                self.assertEqual(material['uv_speed'],[-1.25,0] if material['name'].endswith('/normal_b') else [0,0])

    def test_collision_ownership_flags_and_all_resource_triangles(self):
        scenes,catalog=fixture()
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);metadata=export(scenes,catalog,output)
            course=collision((output/'static-collision.dnc').read_bytes())[0]
            materials=world_materials((output/'world.dnw').read_bytes())
        self.assertEqual([(p['actor'],p['chunk'],p['ordinal']) for p in course['platforms']],
                         [('repeated',0,3),('repeated',1,2)])
        self.assertEqual([p['flags'] for p in course['platforms']],[5,2])
        self.assertEqual(course['platforms'][0]['friction'],.5)
        self.assertEqual(course['platforms'][0]['slide_velocity'],[1,3,2])
        self.assertEqual(len(course['triangles']),25)
        self.assertEqual([owner for _,owner in course['triangles']],[0]*24+[1])
        self.assertEqual(metadata['courses'][0]['static_platforms'],2)
        self.assertEqual(metadata['courses'][0]['conveyor_platforms'],1)
        self.assertEqual(metadata['courses'][0]['static_collision_triangles'],25)
        self.assertEqual([m['uv_speed'] for m in materials],[[0,0],[-1.25,0]])
        self.assertEqual(metadata['animated_materials'],[{'name':'normal_b','uv_speed':[-1.25,0]}])

    def test_noncommuting_actor_orientation_without_scale_position_or_resource_transform(self):
        scenes,catalog=fixture();values=scenes['data/maps/start.wbm'][3].values
        first=platform_surface(values,'platform',0,3)
        yaw,pitch,roll=map(math.radians,[25,35,15])
        source=[1,2,3]
        source=[math.cos(yaw)*source[0]+math.sin(yaw)*source[2],source[1],-math.sin(yaw)*source[0]+math.cos(yaw)*source[2]]
        source=[source[0],math.cos(pitch)*source[1]-math.sin(pitch)*source[2],math.sin(pitch)*source[1]+math.cos(pitch)*source[2]]
        source=[math.cos(roll)*source[0]-math.sin(roll)*source[1],math.sin(roll)*source[0]+math.cos(roll)*source[1],source[2]]
        expected=[source[0],source[2],source[1]];slide=first['slide_velocity'];matrix=first['orientation']
        actual=[sum(slide[row]*matrix[row*4+col] for row in range(3)) for col in range(3)]
        for a,b in zip(actual,expected):self.assertAlmostEqual(a,b)
        self.assertEqual([matrix[i] for i in (3,7,11,12,13,14,15)],[0,0,0,0,0,0,1])
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);export(scenes,catalog,output)
            original=collision((output/'static-collision.dnc').read_bytes())[0]
            values.update(Pos=[[-80,40,25]],Scale=[[.25,9,17]],ResourcesPos=[[0,0,0],[100,50,20],[10,20,30]],
                          ResourcesAng=[[0,0,0],[80,12,45],[120,30,40]])
            export(scenes,catalog,output)
            changed=collision((output/'static-collision.dnc').read_bytes())[0]
        self.assertEqual(original['platforms'][0],changed['platforms'][0])
        self.assertNotEqual(original['triangles'][0][0],changed['triangles'][0][0])

    def test_material_motion_matches_selected_diffuse_and_reports_unsupported_effects(self):
        selected={'MultilayerDiffuse':['chosen'],'MultilayerUVSpeed':[[-1.25,0]],
                  'LayersDiffuseName':['ignored'],'ShifterSpeed':[[9,9]]}
        self.assertEqual(diffuse_motion(selected),('chosen',[-1.25,0],0,''))
        selected['MultilayerDiffuse']=['']
        diffuse,speed,_,issue=diffuse_motion(selected)
        self.assertEqual((diffuse,speed),('ignored',[0,0]))
        self.assertIn('per-layer ShifterSpeed',issue)
        selected['LayersDiffuseName']=['','chosen'];selected['ShifterSpeed']=[[8,9],[0,0]]
        self.assertEqual(diffuse_motion(selected),('chosen',[0,0],0,''))
        selected['ShifterSpeed'][1]=[.1,0]
        self.assertIn('per-layer ShifterSpeed',diffuse_motion(selected)[3])
        selected['MultilayerDiffuse']=['chosen'];selected['MultilayerUVEffect']=[3]
        self.assertEqual(diffuse_motion(selected)[1],[0,0])
        self.assertIn('MultilayerUVEffect',diffuse_motion(selected)[3])
        scenes,catalog=fixture();scenes['data/templates/shader.wbr'][0].values={
            'LayersDiffuseName':['','body.jpg'],'ShifterSpeed':[[0,0],[.2,.3]],'LayersUVChannel':[0,0]}
        with tempfile.TemporaryDirectory() as folder:
            metadata=export(scenes,catalog,Path(folder))
        self.assertIn('normal_a',metadata['unsupported_material_motion'])

    def test_malformed_platform_and_uv_metadata_rejected_before_writing(self):
        changes=[lambda s:s['data/maps/start.wbm'][3].values.update(Friction=[float('nan')]),
                 lambda s:s['data/maps/start.wbm'][3].values.update(Friction=[-1]),
                 lambda s:s['data/maps/start.wbm'][3].values.update(IsConveyor=[1]),
                 lambda s:s['data/maps/start.wbm'][3].values.update(SlideVelocity=[[1,2]]),
                 lambda s:s['data/maps/start.wbm'][3].values.update(SlideVelocity=[[1,float('inf'),3]]),
                 lambda s:s['data/maps/start.wbm'][3].values.update(Ang=[[1,2]]),
                 lambda s:s['data/templates/shader.wbr'][1].values.update(MultilayerUVSpeed=[[1,float('nan')]]),
                 lambda s:s['data/templates/shader.wbr'][1].values.update(MultilayerUVSpeed=[[1]]),
                 lambda s:setattr(s['data/maps/start.wbm'][3],'name','bad\0name')]
        for change in changes:
            scenes,catalog=fixture();change(scenes)
            with tempfile.TemporaryDirectory() as folder:
                output=Path(folder)
                with self.assertRaises((AssetError,ValueError)):export(scenes,catalog,output)
                self.assertFalse((output/'static-collision.dnc').exists())
                self.assertFalse((output/'world.dnw').exists())

    @unittest.skipUnless(os.environ.get('DORITOS_PLATFORM_OWNED_TEST') and (ROOT/'game/data.pak').is_file(),
                         'Optional user-owned package test')
    def test_owned_all_courses_triangle_references_and_first_belts(self):
        from inspect_pak import read_directory,read_entry
        pak=ROOT/'game/data.pak';entries={e.name:e for e in read_directory(pak)}
        source=lambda name:read_entry(pak,entries[name])
        scenes={n:read_scene(source(n)) for n in entries if n.startswith('data/templates/') and n.endswith('.wbr')}
        tracks={r.name:r for r in scenes['data/templates/track.wbr']};chunks={r.name:r for r in scenes['data/templates/chunk.wbr']}
        catalog=[]
        for level in sorted(scenes['data/templates/levelconfig.wbr'],key=lambda r:r.values['LevelId'][0]):
            ordered=[]
            for name in tracks[level.values['Track'][0]].values['Chunk']:
                path='data/maps/'+chunks[name].values['Map'][0].removesuffix('.wxm')+'.wbm'
                if path not in scenes:scenes[path]=read_scene(source(path))
                ordered.append({'name':name,'file':path})
            catalog.append({'id':level.values['LevelId'][0],'name':level.name,'theme':level.values['Theme'][0],'chunks':ordered})
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);metadata=prepare_world(source,scenes,catalog,output)
            courses=collision((output/'static-collision.dnc').read_bytes());materials=world_materials((output/'world.dnw').read_bytes())
            # The original package names these variants /eu/, not /europe/.
            for country in ('usa','eu','japan'):
                name='props/obs/conv_a/'+country+'/normal_b'
                matching=[m for m in materials if m['name']==name]
                self.assertTrue(matching)
                shader=next(r for r in scenes['data/templates/shader.wbr'] if r.name==name)
                diffuse=shader.values['MultilayerDiffuse'][0]
                texture=next((r.values['DiffuseFile'][0] for r in scenes['data/templates/texture.wbr'] if r.name==diffuse),diffuse)
                expected=source('data/art/'+texture)
                for material in matching:
                    self.assertEqual((output/material['texture']).read_bytes(),expected)
                    self.assertEqual(material['uv_speed'],[-1.25,0])
        self.assertEqual([c['id'] for c in courses],list(range(15)))
        for course,summary in zip(courses,metadata['courses']):
            platforms=course['platforms'];owners=[owner for _,owner in course['triangles']]
            self.assertEqual(set(owners),set(range(len(platforms))))
            self.assertEqual(len({(p['chunk'],p['ordinal']) for p in platforms}),len(platforms))
            self.assertEqual(len(owners),summary['static_collision_triangles'])
            self.assertEqual(len(platforms),summary['static_platforms'])
        belts=[p for p in courses[0]['platforms'] if p['chunk']==2 and p['ordinal'] in (12,11,7,9)]
        self.assertEqual(len(belts),4)
        for platform in belts:
            self.assertEqual(platform['flags'],7);self.assertEqual(platform['slide_velocity'],[3,0,0]);self.assertEqual(platform['friction'],.5)
            rotated=[sum(platform['slide_velocity'][row]*platform['orientation'][row*4+col] for row in range(3)) for col in range(3)]
            self.assertAlmostEqual(rotated[0],3 if platform['ordinal'] in (12,11) else -3)
            self.assertAlmostEqual(rotated[1],0);self.assertAlmostEqual(rotated[2],0)
        self.assertTrue(any(m['name']=='props/obs/conv_a/usa/normal_b' and m['uv_speed']==[-1.25,0] for m in materials))
        self.assertTrue(all(m['uv_speed']==[0,0] for m in materials if m['name'].endswith('/normal_a')))


if __name__=='__main__':unittest.main()
