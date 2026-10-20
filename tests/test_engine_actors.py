from copy import deepcopy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from cal3d_character import Bone,Skeleton,Mesh,Submesh,Vertex,write_csf,write_cmf
from engine_assets import AssetError,Reader,SceneRecord
from engine_actors import actor_matrices,box_matrix,encode_actors,parse_cfg,prepare_actors


def multiply(a,b):
    return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def column(row):return [[row[j*4+i] for j in range(4)] for i in range(4)]


def point(matrix,value):return [sum(matrix[i][k]*value[k] for k in range(4)) for i in range(4)]


def authored(position,angles,scale):
    # Independently expanded source Y-yaw, X-pitch, Z-roll; deliberately avoid
    # using engine_world.transform or the production basis helper in the oracle.
    y,x,z=map(math.radians,angles)
    cy,sy,cx,sx,cz,sz=math.cos(y),math.sin(y),math.cos(x),math.sin(x),math.cos(z),math.sin(z)
    return [[(cz*cy-sz*sx*sy)*scale[0],-sz*cx*scale[1],(cz*sy+sz*sx*cy)*scale[2],position[0]],
            [(sz*cy+cz*sx*sy)*scale[0],cz*cx*scale[1],(sz*sy-cz*sx*cy)*scale[2],position[1]],
            [-cx*sy*scale[0],sx*scale[1],cx*cy*scale[2],position[2]],[0,0,0,1]]


def fixture():
    links=lambda a,b:[SceneRecord('CHUNKLINK','in',{'LinkType':[0],'Pos':[a]}),
                      SceneRecord('CHUNKLINK','out',{'LinkType':[1],'Pos':[b]})]
    script={'mode':2,'blob':'unused: references retain original catalog positions'}
    master={'Animation':['SQUASH'],'AnimationResourceId':[0],'AnimationLoop':[True],
            'AnimationWaitTime':[1.25],'AnimationInitialWaitTime':[.01],'StartMoving':[False],
            'OnPlayAnimationScript':[None],'OnPlayerHit':[script],
            'Resources':['CLP3:box','CLP3:box','BNMD:props/hammer.cfg'],
            'ResourcesPos':[[-3.8,0,0],[-4,0,0],[0,0,1.65]],
            'ResourcesAng':[[0,0,0],[0,0,0],[180,0,0]],
            'ResourcesScale':[[8,1.2,1.4],[8,1.2,2],[1,1,1]],
            'ResourcesBoneAttach':['0,Bone02','0,Bone02',''],'HammerType':[1],'HammerForce':[[10,5]],
            'AnimSpeed':[1.0],'StartFirstAnim':[False],'Physic':[False]}
    actor={'HammerTemplate':['square'],'Pos':[[17,7,0]],'Ang':[[0,0,0]],'Scale':[[1,1,1]],
           'IsTemplate':[False],'Activation':[2],'Deactivation':[0]}
    scenes={'data/maps/start.wbm':links([0,0,0],[10,1,2])+[SceneRecord('HAMMER','same',deepcopy(actor))],
            'data/maps/end.wbm':links([2,3,4],[20,3,4])+[SceneRecord('HAMMER','same',deepcopy(actor))],
            'data/templates/hammer.wbr':[SceneRecord('HAMMERMASTER','square',master)],
            'data/templates/bonedmodel.wbr':[SceneRecord('BONEDMODELMASTER','props/hammer.cfg',
                {'FileName':['props/hammer.cfg'],'ShadersList':['props/usa/n;']})],
            'data/templates/cprimitive3.wbr':[SceneRecord('CPRIMITIVE3MASTER','box',{'PrimitiveType':[2],
                'BoundingBoxMin':[[-.5,-.5,-.5]],'BoundingBoxMax':[[.5,.5,.5]],'CollisionGroup':[33216],'CollisionMask':[1],
                'RegisterInManager':[True],'CollisionTest':[0],'BoundingBoxMode':[2]})],
            'data/templates/shader.wbr':[SceneRecord('SHADERMASTER','props/'+theme+'/n',{
                'MultilayerDiffuse':[theme],'MultilayerUVSpeed':[[0,0]],'DiffuseColor':[[255,255,255]]})
                for theme in ('usa','eu','japan')],
            'data/templates/texture.wbr':[SceneRecord('TEXTUREMASTER',theme,{'DiffuseFile':[theme+'.png']})
                for theme in ('usa','eu','japan')]}
    catalog=[{'id':i,'name':'course'+str(i),'theme':i//5,'chunks':[
        {'name':'common/end_chunk','file':'data/maps/end.wbm'},
        {'name':'common/start_chunk','file':'data/maps/start.wbm'}]} for i in range(15)]
    skeleton=Skeleton([Bone('Root',(1,2,3),(0,0,0,1),(-1,-2,-3),(0,0,0,1),-1,(1,)),
                       Bone('Bone02',(2,0,0),(0,0,0,1),(-3,-2,-3),(0,0,0,1),0,())])
    vertices=[Vertex(p,(0,0,1),-1,0,((i/2,0),),((1,1),))
              for i,p in enumerate(((0,0,0),(1,0,0),(0,1,0)))]
    mesh=Mesh([Submesh(-1,1,vertices,[(0,1,2)])])
    clip=b'CAF\0'+struct.pack('<IfI',1000,1.,1)+struct.pack('<II8f',1,1,0,2,0,0,0,0,0,1)
    files={'data/art/props/hammer.cfg':b'skeleton=hammer.csf\nanimation=SQUASH:hammer.caf\nmesh=hammer.cmf\nrun=SQUASH 0.13 me.Resources[3]="SNFX:hit.wav"\n',
           'data/art/props/hammer.csf':write_csf(skeleton),'data/art/props/hammer.cmf':write_cmf(mesh,skeleton),
           'data/art/props/hammer.caf':clip,
           **{'data/art/'+t+'.png':t.encode() for t in ('usa','eu','japan')}}
    return scenes,catalog,files


def decode(data):
    r=Reader(data)
    if r.take(4)!=b'DNA1' or r.u32()!=1:raise AssertionError('Wrong actors format')
    materials=[]
    for _ in range(r.count(4096)):
        name,texture=r.string(),r.string();color=r.floats(4);lit,transparent=r.unpack('II');speed=r.floats(2)
        materials.append({'name':name,'texture':texture,'color':color,'lit':lit,'transparent':transparent,'uv_speed':speed})
    models=[]
    for _ in range(r.count(256)):
        name,path=r.string(),r.string();indices=[r.u32() for _ in range(r.count(4))]
        events=[(r.string(),r.floats(1)[0],r.string()) for _ in range(r.count(1024))]
        if any(i>=len(materials) for i in indices):raise AssertionError('Missing actor material')
        models.append({'name':name,'path':path,'materials':indices,'events':events})
    courses=[]
    for _ in range(r.count(15)):
        course={'id':r.u32(),'instances':[]}
        for _ in range(r.count(4096)):
            chunk,ordinal=r.unpack('II');name=r.string();model=r.u32()
            actor_world,model_local=r.floats(16),r.floats(16);animation=r.string()
            resource,loop,start=r.unpack('III');wait,initial=r.floats(2)
            scripts=[]
            for _ in range(r.count(8)):
                field,index=r.string(),r.u32();source,kind,actor=r.string(),r.string(),r.string();origin,element=r.unpack('II')
                scripts.append((field,index,source,kind,actor,origin,element))
            boxes=[]
            for _ in range(r.count(24)):
                rid,bone=r.unpack('II');local=r.floats(16);low,high=r.floats(3),r.floats(3);group,mask=r.unpack('II')
                boxes.append({'resource':rid,'bone':bone,'local':local,'low':low,'high':high,'group':group,'mask':mask})
            if model>=len(models):raise AssertionError('Missing actor model')
            course['instances'].append({'chunk':chunk,'ordinal':ordinal,'name':name,'model':model,
                'actor_world':actor_world,'model_local':model_local,'animation':animation,'resource':resource,
                'loop':loop,'start':start,'wait':wait,'initial':initial,'scripts':scripts,'boxes':boxes})
        courses.append(course)
    if sorted(c['id'] for c in courses)!=list(range(15)) or r.position!=len(data):raise AssertionError('Wrong course catalog or trailing data')
    return materials,models,courses


class EngineActorsTests(unittest.TestCase):
    def test_raw_cal_absolute_bone_and_noncommuting_basis_composition(self):
        actor_position=[11,13,17];actor_angles=[23,31,47];offset=[2,3,5]
        resource_position=[7,19,29];resource_angles=[53,61,71]
        local_position=[-3.8,2,1];local_angles=[17,37,43];local_scale=[8,1.2,1.4]
        actor,resource=actor_matrices(actor_position,actor_angles,[1,1,1],resource_position,resource_angles,[1,1,1],offset)
        local=box_matrix(local_position,local_angles,local_scale)
        bone=authored([2,5,7],[11,29,41],[1,1,1])
        exported=multiply(column(actor),multiply(column(resource),multiply(bone,column(local))))
        a=authored([13,16,22],actor_angles,[1,1,1]);r=authored(resource_position,resource_angles,[1,1,1])
        l=authored(local_position,local_angles,local_scale)
        # Independently expand the original loader, mirror and attachment path
        # on asymmetric corners. C is only left of the absolute raw bone.
        for x in (-.5,.5):
            for y in (-.5,.5):
                for z in (-.5,.5):
                    p=point(l,[x,y,z,1]);p[0]=-p[0]
                    p=point(bone,p);p=[p[0],p[2],-p[1],1];p[0]=-p[0]
                    p=point(a,point(r,p));expected=[p[0],p[2],p[1],1]
                    actual=point(exported,[x,y,z,1])
                    for left,right in zip(actual,expected):self.assertAlmostEqual(left,right,places=6)
        with self.assertRaisesRegex(AssetError,'scale'):
            actor_matrices(actor_position,actor_angles,[2,1,1],resource_position,resource_angles,[1,1,1])

    def test_bundle_authored_id_resources_template_scripts_events_and_theme(self):
        scenes,catalog,files=fixture()
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);metadata=prepare_actors(files.__getitem__,scenes,catalog,output)
            materials,models,courses=decode((output/'actors.dna').read_bytes())
            self.assertEqual(metadata['instances'],30);self.assertFalse(metadata['excluded_records'])
            self.assertEqual(len(models),3);self.assertEqual(len({m['path'] for m in models}),1)
            for country,course in zip(('usa','eu','japan'),courses[::5]):
                actor=course['instances'][0];model=models[actor['model']];material=materials[model['materials'][0]]
                self.assertEqual(material['name'],'props/'+country+'/n')
                self.assertEqual((output/material['texture']).read_bytes(),country.encode())
                self.assertEqual(actor['resource'],0);self.assertEqual([b['resource'] for b in actor['boxes']],[2,1])
                self.assertEqual([b['bone'] for b in actor['boxes']],[1,1])
                self.assertEqual([(b['group'],b['mask']) for b in actor['boxes']],[(33216,1)]*2)
                self.assertEqual(actor['scripts'],[('OnPlayerHit',0,'data/templates/hammer.wbr','HAMMERMASTER','square',0,0)])
                self.assertAlmostEqual(model['events'][0][1],.13)
                self.assertEqual(model['events'][0][2],'me.Resources[3]="SNFX:hit.wav"')
            self.assertEqual([(a['chunk'],a['ordinal'],a['name']) for a in courses[0]['instances']],[(0,2,'same'),(1,2,'same')])
            self.assertNotEqual(courses[0]['instances'][0]['actor_world'],courses[0]['instances'][1]['actor_world'])
            self.assertEqual(metadata['courses'][0]['instances'][0]['activation'],2)
            self.assertEqual(len(metadata['unsupported_timed_commands']),3)

    def test_dne_raw_rest_keys_uv_and_mode4_winding(self):
        scenes,catalog,files=fixture()
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);metadata=prepare_actors(files.__getitem__,scenes,catalog,output)
            r=Reader((output/metadata['models'][0]['path']).read_bytes())
            self.assertEqual(r.take(4),b'DNE1');self.assertEqual(r.unpack('III'),(2,2,1));r.string()
            self.assertEqual(r.unpack('i'),(-1,));root=r.floats(14)
            self.assertEqual(root[:3],[1,2,3]);self.assertEqual(root[3:7],[0,0,0,1]);self.assertEqual(root[7:10],[-1,-2,-3])
            r.take(60);self.assertEqual(r.unpack('III'),(0,3,3))
            first=r.floats(8);self.assertEqual(first,[0,0,0,0,0,1,0,0]);r.take(32+2*64)
            self.assertEqual(r.unpack('III'),(0,2,1))
            self.assertEqual(r.u32(),1);self.assertEqual(r.string(),'SQUASH');self.assertEqual(r.floats(1),[1]);self.assertEqual(r.u32(),1)
            self.assertEqual(r.unpack('II'),(1,1));self.assertEqual(r.floats(8),[0,2,0,0,0,0,0,1]);self.assertEqual(r.unpack('II'),(0,0))
            self.assertEqual(r.position,len(r.data))

    def test_script_reference_catalog_includes_other_records_and_actor_override(self):
        scenes,catalog,files=fixture();blob={'mode':2,'blob':'identity only'}
        scenes['data/maps/end.wbm'].insert(0,SceneRecord('OTHER','library',{'Script':[blob]}))
        scenes['data/maps/start.wbm'][2].values['OnPlayAnimationScript']=[blob]
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);prepare_actors(files.__getitem__,scenes,catalog,output)
            _,_,courses=decode((output/'actors.dna').read_bytes())
        scripts=courses[0]['instances'][0]['scripts']
        self.assertEqual(scripts[0],('OnPlayAnimationScript',1,'data/maps/start.wbm','HAMMER','same',2,0))
        self.assertEqual(scripts[1],('OnPlayerHit',2,'data/templates/hammer.wbr','HAMMERMASTER','square',0,0))
        self.assertEqual(courses[0]['instances'][1]['ordinal'],3)

    def test_unsupported_actor_variants_are_explicit_and_retained_by_identity(self):
        for changes,reason in [({'Animation':['']},'Empty'),({'Physic':[True]},'Physic'),
                               ({'AnimSpeed':[.92]},'AnimSpeed'),({'HammerType':[0]},'HammerType')]:
            scenes,catalog,files=fixture();scenes['data/maps/start.wbm'][2].values.update(changes)
            with tempfile.TemporaryDirectory() as folder:
                metadata=prepare_actors(files.__getitem__,scenes,catalog,Path(folder))
            self.assertEqual(metadata['instances'],15);self.assertEqual(len(metadata['excluded_records']),15)
            self.assertIn(reason,metadata['excluded_records'][0]['reason'])
            self.assertEqual(metadata['excluded_records'][0]['ordinal'],2)

    def test_malformed_cfg_bounds_links_arrays_and_attachment_rejected(self):
        for raw in (b'skeleton=../evil\nmesh=x\nanimation=A:y',b'skeleton=x\nmesh=y\nanimation=A:z\nrun=A nan cmd',
                    b'skeleton=x\nmesh=y\nanimation=A:z\nrun=B 0 cmd',b'skeleton=x\nmesh=y\nanimation=A:z\nscale=2'):
            with self.assertRaises(AssetError):parse_cfg(raw,'props/hammer.cfg')
        for change in [lambda s:s['data/templates/hammer.wbr'][0].values.update(ResourcesBoneAttach=['0,missing','0,Bone02','']),
                       lambda s:s['data/templates/hammer.wbr'][0].values.update(ResourcesAng=[[0,0,0]]),
                       lambda s:s['data/templates/cprimitive3.wbr'][0].values.update(BoundingBoxMin=[[1,1,1]]),
                       lambda s:s['data/maps/start.wbm'].append(SceneRecord('CHUNKLINK','duplicate',{'LinkType':[0],'Pos':[[0,0,0]]})),
                       lambda s:s['data/templates/hammer.wbr'][0].values.update(AnimationWaitTime=[float('nan')])]:
            scenes,catalog,files=fixture();change(scenes)
            with tempfile.TemporaryDirectory() as folder:
                with self.assertRaises(AssetError):prepare_actors(files.__getitem__,scenes,catalog,Path(folder))
                self.assertFalse((Path(folder)/'actors.dna').exists())
        with self.assertRaises(AssetError):encode_actors([],[],[{'id':0,'instances':[]}])

    @unittest.skipUnless(os.environ.get('DORITOS_ACTORS_OWNED_TEST') and (ROOT/'game/data.pak').is_file(),
                         'Optional user-owned package test')
    def test_owned_first_usa_hammer_and_complete_catalog(self):
        from inspect_pak import read_directory,read_entry
        from engine_assets import read_scene
        pak=ROOT/'game/data.pak';entries={e.name:e for e in read_directory(pak)}
        source=lambda name:read_entry(pak,entries[name])
        scenes={n:read_scene(source(n)) for n in entries if n.endswith(('.wbm','.wbr'))}
        tracks={r.name:r for r in scenes['data/templates/track.wbr']};chunks={r.name:r for r in scenes['data/templates/chunk.wbr']}
        catalog=[]
        for level in sorted(scenes['data/templates/levelconfig.wbr'],key=lambda r:r.values['LevelId'][0]):
            ordered=[{'name':n,'file':'data/maps/'+chunks[n].values['Map'][0].removesuffix('.wxm')+'.wbm'}
                     for n in tracks[level.values['Track'][0]].values['Chunk']]
            catalog.append({'id':level.values['LevelId'][0],'name':level.name,'theme':level.values['Theme'][0],'chunks':ordered})
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);metadata=prepare_actors(source,scenes,catalog,output)
            materials,models,courses=decode((output/'actors.dna').read_bytes())
            first=next(a for a in courses[0]['instances'] if a['chunk']==4 and a['ordinal']==10)
            self.assertEqual(first['name'],'obstacle/sq_hammer_001');self.assertEqual(first['actor_world'][12:15],[121,0,6])
            self.assertEqual(first['animation'],'SQUASH');self.assertEqual(first['wait'],1.25)
            self.assertEqual([(b['resource'],b['bone']) for b in first['boxes']],[(2,6),(1,6)])
            for b in first['boxes']:self.assertEqual((b['group'],b['mask']),(33216,1))
            model=models[first['model']];self.assertEqual(metadata['models'][first['model']]['vertices'],819)
            self.assertEqual(metadata['models'][first['model']]['triangles'],1002)
            self.assertAlmostEqual(model['events'][0][1],.13)
            for c in courses:self.assertEqual(len({(a['chunk'],a['ordinal']) for a in c['instances']}),len(c['instances']))
            for country in ('usa','eu','japan'):
                matching=[m for m in materials if m['name']=='props/obs/sq_hammer/'+country+'/n']
                self.assertTrue(matching)
                shader=next(r for r in scenes['data/templates/shader.wbr'] if r.name==matching[0]['name'])
                diffuse=shader.values['MultilayerDiffuse'][0]
                texture=next(r.values['DiffuseFile'][0] for r in scenes['data/templates/texture.wbr'] if r.name==diffuse)
                self.assertEqual((output/matching[0]['texture']).read_bytes(),source('data/art/'+texture))


if __name__=='__main__':unittest.main()
