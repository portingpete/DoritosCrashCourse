"""Export the certified HAMMER slice from user-owned assets, without pointers.

DNA1 v1 is little endian. Strings are u32 byte length + UTF-8, without NUL.
Header: magic,version; materials in DNWv2 order; models; fifteen courses.
Model: cfg,path,submeshCount,materialIndices,eventCount,(animation,time,command).
Course: id,instanceCount. Instance: chunk,ordinal,name,modelIndex,actorWorld16f,
modelLocal16f,animation,animationRuntimeResourceId,loop,startMoving,wait,
initialWait,scriptCount,(field,catalogIndex,source,type,name,ordinal,index),
boxCount,(runtimeResourceId,boneIndex,local16f,min3f,max3f,group,mask).
All counts, IDs and booleans are u32; all real values are f32.

DNE models preserve raw CSF/CAF transforms, inverse binds, CMF positions,
normals and UVs. Faces reverse winding, matching original Cal loading mode 5.
For column matrices B swaps source Y/Z, F mirrors Cal X, and C maps raw Cal
(x,y,z) to (x,z,-y). Exported row matrices are the transpose of:
actorWorld=B*actor*B; modelLocal=B*resource*F*C; boxLocal=F*boxLocalSource.
Draw: rawSkin * modelLocal * actorWorld. Box: local * absoluteRawBone *
modelLocal * actorWorld. Box bounds retain the original CLP source axes.
The bone attachment does not use an inverse bind transform.
"""
from __future__ import annotations

import hashlib
import math
from pathlib import Path, PurePosixPath
import re
import struct

from cal3d_character import read_cmf, read_csf
from character_packs import solid_png
from engine_assets import AssetError, read_animation
from engine_world import diffuse_motion, finite_vector, first, matrix_product, transform
from inspect_pak import _no_links

B = [[1,0,0,0],[0,0,1,0],[0,1,0,0],[0,0,0,1]]
F = [[-1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]]
C = [[1,0,0,0],[0,0,1,0],[0,-1,0,0],[0,0,0,1]]
MAX_BYTES = 64 * 1024 * 1024


def _row(matrix):
    return [matrix[row][col] for col in range(4) for row in range(4)]


def actor_matrices(position, angles, scale, resource_position, resource_angles, resource_scale, offset=(0,0,0)):
    """The explicit raw-Cal -> source -> native conversion, applied once."""
    vectors = [finite_vector(v,3,'HAMMER transform') for v in
               (position,angles,scale,resource_position,resource_angles,resource_scale,offset)]
    position,angles,scale,rp,ra,rs,offset = vectors
    # Original attachment scale/quaternion composition is not a general affine
    # hierarchy. Only the certified unit actor and model-resource scales apply.
    if scale != [1,1,1] or rs != [1,1,1]:
        raise AssetError('Unsupported HAMMER actor/model resource scale')
    actor = transform([a+b for a,b in zip(position,offset)], angles, scale)
    resource = transform(rp,ra,rs)
    return (_row(matrix_product(B,matrix_product(actor,B))),
            _row(matrix_product(B,matrix_product(resource,matrix_product(F,C)))))


def box_matrix(position, angles, scale):
    values=[finite_vector(v,3,'HAMMER box transform') for v in (position,angles,scale)]
    if any(v<=0 for v in values[2]):raise AssetError('HAMMER box scale must be positive')
    return _row(matrix_product(F,transform(*values)))


def _number(value, label, minimum=-1e9, maximum=1e9):
    if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value) or not minimum<=value<=maximum:
        raise AssetError(label+' outside budget')
    return value


def _uint(value,label,maximum=0xffffffff):
    if isinstance(value,bool) or not isinstance(value,int) or not 0<=value<=maximum:
        raise AssetError(label+' outside budget')
    return value


def _bool(value,label):
    if not isinstance(value,bool):raise AssetError(label+' must be an authored boolean')
    return value


def _path(value):
    if not isinstance(value,str) or '\\' in value or ':' in value or not value:
        raise AssetError('Invalid HAMMER relative asset path')
    path=PurePosixPath(value)
    if path.is_absolute() or any(p in ('','.','..') for p in value.split('/')):
        raise AssetError('Invalid HAMMER relative asset path')
    return str(path)


def parse_cfg(raw, name):
    """Keep executable timed commands as data, never execute them in Python."""
    if len(raw)>1024*1024:raise AssetError('HAMMER CFG outside budget')
    try:text=raw.decode('utf-8-sig')
    except UnicodeDecodeError as error:raise AssetError('HAMMER CFG is not UTF-8') from error
    skeleton=None;meshes=[];clips={};events=[]
    for line in text.splitlines():
        line=line.strip()
        if not line or line.startswith('#'):continue
        if '=' not in line:raise AssetError('Unsupported HAMMER CFG directive')
        field,value=[p.strip() for p in line.split('=',1)]
        if field=='skeleton':
            if skeleton is not None:raise AssetError('Duplicate HAMMER CFG skeleton')
            skeleton=_path(value)
        elif field=='mesh':meshes.append(_path(value))
        elif field=='animation':
            if ':' not in value:raise AssetError('HAMMER animation lacks authored name')
            key,path=value.split(':',1)
            if not key or key in clips:raise AssetError('Duplicate or empty HAMMER animation')
            clips[key]=_path(path)
        elif field=='run':
            match=re.fullmatch(r'(\S+)\s+(\S+)\s+(.+)',value)
            if not match:raise AssetError('Invalid HAMMER timed command')
            animation,time,command=match.groups()
            try:time=float(time)
            except ValueError as error:raise AssetError('Invalid HAMMER event time') from error
            events.append({'animation':animation,'time':_number(time,'HAMMER event time',0,3600),'command':command})
        else:raise AssetError('Unsupported HAMMER CFG directive '+field)
    if skeleton is None or not 1<=len(meshes)<=4 or not 1<=len(clips)<=128 or len(events)>1024:
        raise AssetError('HAMMER CFG resource counts outside budget')
    if any(e['animation'] not in clips for e in events):raise AssetError('HAMMER event references missing animation')
    parent=PurePosixPath(_path(name)).parent
    # The package directory is canonical lowercase; original CFG leaf names
    # commonly use uppercase CSF/CMF/CAF extensions on the case-insensitive VFS.
    return {'skeleton':str(parent/skeleton).lower(),'meshes':[str(parent/p).lower() for p in meshes],
            'clips':{n:str(parent/p).lower() for n,p in clips.items()},'events':events}


class _Writer:
    def __init__(self,magic):self.data=bytearray(magic)
    def u32(self,*values):
        self.data.extend(struct.pack('<'+'I'*len(values),*[_uint(v,'HAMMER integer') for v in values]))
    def f32(self,*values):
        self.data.extend(struct.pack('<'+'f'*len(values),*[_number(v,'HAMMER float') for v in values]))
    def string(self,value):
        if not isinstance(value,str):raise AssetError('HAMMER string is not text')
        raw=value.encode('utf-8')
        if len(raw)>4096 or b'\0' in raw:raise AssetError('HAMMER string outside budget')
        self.u32(len(raw));self.data.extend(raw)
    def finish(self):
        if len(self.data)>MAX_BYTES:raise AssetError('HAMMER bundle exceeds 64 MiB')
        return bytes(self.data)


def encode_model(name,skeleton,submeshes,clips):
    if not 1<=len(skeleton.bones)<=256 or not 1<=len(submeshes)<=4:
        raise AssetError('HAMMER model counts outside native budget')
    if sum(len(m.vertices) for m in submeshes)>64000 or sum(len(m.faces) for m in submeshes)>64000:
        raise AssetError('HAMMER geometry exceeds native model budget')
    w=_Writer(b'DNE1');w.u32(2,len(skeleton.bones),len(submeshes));w.string(name)
    for bone in skeleton.bones:
        w.data.extend(struct.pack('<i',bone.parent))
        w.f32(*bone.translation,*bone.rotation,*bone.inverse_bind_translation,*bone.inverse_bind_rotation)
    for index,mesh in enumerate(submeshes):
        if mesh.uv_sets!=1:raise AssetError('Unsupported HAMMER model UV set count')
        w.u32(index,len(mesh.vertices),len(mesh.faces)*3)
        for vertex in mesh.vertices:
            influences=list(vertex.influences)+[(0,0)]*(4-len(vertex.influences))
            w.f32(*vertex.position,*vertex.normal,*vertex.uv[0])
            w.u32(*(bone for bone,_ in influences));w.f32(*(weight for _,weight in influences))
        for a,b,c in mesh.faces:w.u32(a,c,b) # Original Cal loading mode4.
    w.u32(len(clips))
    for name,clip in clips.items():
        if clip['motion']:raise AssetError('Unsupported HAMMER root-motion extension')
        w.string(name);w.f32(clip['duration']);w.u32(len(clip['tracks']))
        for track in clip['tracks']:
            w.u32(track['bone'],len(track['keys']))
            for key in track['keys']:w.f32(*key)
        w.u32(0)
    w.u32(0) # Generic prop model has no course catalog.
    return w.finish()


class _Templates:
    def __init__(self,scenes):
        self.tables={};self.scripts={}
        for source,rows in sorted(scenes.items()):
            if source.startswith('data/templates/'):
                table={}
                for ordinal,record in enumerate(rows):
                    if record.name in table:raise AssetError('Duplicate HAMMER template')
                    table[record.name]=(source,ordinal,record)
                self.tables[source.rsplit('/',1)[-1][:-4]]=table
            for ordinal,record in enumerate(rows):
                for field,values in record.values.items():
                    for index,value in enumerate(values):
                        if isinstance(value,dict) and value.get('mode')==2:
                            self.scripts[(source,record.type,record.name,field,ordinal,index)]=len(self.scripts)
        if len(self.scripts)>16384:raise AssetError('HAMMER script catalog outside budget')
    def effective(self,source,ordinal,record,active=()):
        key=(source,ordinal)
        if key in active or len(active)>=16:raise AssetError('Cyclic HAMMER template inheritance')
        values={};origins={}
        for field,names in record.values.items():
            if field.endswith('Template') and field!='IsTemplate' and names and names[0]:
                value=self.tables.get(field[:-8].lower(),{}).get(names[0])
                if value is None:raise AssetError('Unresolved HAMMER template '+str(names[0]))
                base,where=self.effective(*value,active+(key,));values.update(base);origins.update(where)
        for field,array in record.values.items():
            values[field]=array;origins[field]=(source,record.type,record.name,field,ordinal)
        return values,origins
    def descriptor(self,table,name):
        entry=self.tables.get(table,{}).get(name)
        if entry is None:raise AssetError('Missing HAMMER '+table+' descriptor '+name)
        return self.effective(*entry)[0]
    def references(self,values,origins):
        result=[]
        for field in ('OnPlayAnimationScript','OnPlayerHit'):
            for index,value in enumerate(values.get(field,[])):
                if value is None:continue
                if not isinstance(value,dict) or value.get('mode')!=2:
                    raise AssetError('Unsupported HAMMER text or unknown script '+field)
                source,kind,name,origin_field,ordinal=origins[field]
                location=(source,kind,name,origin_field,ordinal,index)
                result.append({'field':field,'catalog_index':self.scripts[location],
                               'source':source,'type':kind,'name':name,'ordinal':ordinal,'index':index})
        if len(result)>8:raise AssetError('HAMMER script reference count outside budget')
        return result


class _Materials:
    def __init__(self,source,templates):
        self.source=source;self.templates=templates;self.items=[];self.ids={};self.files={};self.unsupported={}
    def add(self,name,theme):
        variant=name.replace('/usa/','/'+('usa','eu','japan')[theme]+'/')
        if variant in self.templates.tables.get('shader',{}):name=variant
        key=(name,theme)
        if key in self.ids:return self.ids[key]
        shader=self.templates.descriptor('shader',name)
        diffuse,speed,channel,unsupported=diffuse_motion(shader)
        if unsupported:self.unsupported[name]=unsupported
        texture=''
        if diffuse:
            descriptor=self.templates.descriptor('texture',diffuse)
            filename=first(descriptor,'DiffuseFile',diffuse)
            rgb=re.fullmatch(r'rgb/w\d+h\d+r(\d+)g(\d+)b(\d+)',filename)
            if rgb:
                color=[int(c) for c in rgb.groups()]
                if max(color)>255:raise AssetError('Invalid HAMMER RGB texture')
                linear=[c/255/12.92 if c/255<=.04045 else ((c/255+.055)/1.055)**2.4 for c in color]
                raw=solid_png(linear+[1]);filename+='-native.png'
            else:raw=self.source('data/art/'+_path(filename))
            suffix=Path(filename).suffix.lower()
            if suffix not in ('.png','.jpg','.jpeg','.dds'):raise AssetError('Unsupported HAMMER texture format')
            texture='textures/'+hashlib.sha256(raw).hexdigest()+suffix;self.files[texture]=raw
        color=finite_vector(first(shader,'DiffuseColor',[255,255,255]),3,'HAMMER material tint')
        alpha=_number(first(shader,'DiffuseAlpha',255),'HAMMER alpha',0,255)/255
        identity=len(self.items)
        if identity>=4096:raise AssetError('HAMMER material count outside budget')
        self.ids[key]=identity
        self.items.append({'name':name,'texture':texture,'color':[v/255 for v in color]+[alpha],
                           'lit':int(first(shader,'Lighting',True)),
                           'transparent':int(bool(first(shader,'Blend',0))),'uv_speed':speed})
        return identity


def encode_actors(materials,models,courses):
    if len(courses)!=15 or sorted(c['id'] for c in courses)!=list(range(15)):
        raise AssetError('HAMMER catalog must contain all fifteen course IDs')
    if len(materials)>4096 or len(models)>256:raise AssetError('HAMMER asset table outside budget')
    w=_Writer(b'DNA1');w.u32(1,len(materials))
    for m in materials:
        w.string(m['name']);w.string(m['texture']);w.f32(*m['color']);w.u32(m['lit'],m['transparent']);w.f32(*m['uv_speed'])
    w.u32(len(models))
    for m in models:
        w.string(m['name']);w.string(m['path']);w.u32(len(m['materials']),*m['materials']);w.u32(len(m['events']))
        for e in m['events']:w.string(e['animation']);w.f32(e['time']);w.string(e['command'])
    w.u32(len(courses));total=0
    for course in courses:
        total+=len(course['instances'])
        if len(course['instances'])>4096 or total>16384:raise AssetError('HAMMER instance count outside budget')
        w.u32(course['id'],len(course['instances']))
        for actor in course['instances']:
            w.u32(actor['chunk'],actor['ordinal']);w.string(actor['name']);w.u32(actor['model'])
            w.f32(*actor['actor_world'],*actor['model_local']);w.string(actor['animation'])
            w.u32(actor['animation_resource'],int(actor['loop']),int(actor['start_moving']));w.f32(actor['wait'],actor['initial_wait'])
            w.u32(len(actor['scripts']))
            for r in actor['scripts']:
                w.string(r['field']);w.u32(r['catalog_index'])
                for field in ('source','type','name'):w.string(r[field])
                w.u32(r['ordinal'],r['index'])
            w.u32(len(actor['boxes']))
            for box in actor['boxes']:
                w.u32(box['resource'],box['bone']);w.f32(*box['local'],*box['minimum'],*box['maximum']);w.u32(box['group'],box['mask'])
    return w.finish()


def prepare_actors(source,scenes,catalog,output:Path):
    """Export only proved square HAMMER variants; report every omitted record."""
    if len(catalog)!=15 or sorted(c['id'] for c in catalog)!=list(range(15)):
        raise AssetError('HAMMER catalog must contain all fifteen course IDs')
    templates=_Templates(scenes);materials=_Materials(source,templates)
    models=[];model_ids={};files={};courses=[];exclusions=[];unsupported_events=[];total=0
    def model(name,theme):
        key=(name,theme)
        if key in model_ids:return model_ids[key]
        descriptor=templates.descriptor('bonedmodel',name)
        cfg_name=_path(first(descriptor,'FileName',name))
        cfg_raw=source('data/art/'+cfg_name);cfg=parse_cfg(cfg_raw,cfg_name)
        skeleton=read_csf(source('data/art/'+cfg['skeleton']))
        submeshes=[]
        for filename in cfg['meshes']:submeshes.extend(read_cmf(source('data/art/'+filename),skeleton).submeshes)
        if len(submeshes)!=1 or submeshes[0].material!=-1:
            raise AssetError('Unsupported HAMMER submesh/material slot mapping')
        clips={name:read_animation(source('data/art/'+filename),len(skeleton.bones)) for name,filename in cfg['clips'].items()}
        for event in cfg['events']:
            if event['time']>clips[event['animation']]['duration']+.001:raise AssetError('HAMMER event exceeds animation duration')
        shaders=next((v for v in descriptor.get('ShadersList',[]) if v),'').split(';')
        if len(submeshes)>len(shaders) or any(not shaders[i] for i in range(len(submeshes))):
            raise AssetError('HAMMER submesh lacks authored shader override')
        indices=[materials.add(shaders[i],theme) for i in range(len(submeshes))]
        binary=encode_model(name,skeleton,submeshes,clips)
        path='actors/'+hashlib.sha256(binary).hexdigest()+'.dne';files[path]=binary
        identity=len(models)
        if identity>=256:raise AssetError('HAMMER model count outside budget')
        models.append({'name':name,'path':path,'materials':indices,'events':cfg['events'],
                       'bones':[bone.name for bone in skeleton.bones],'clips':list(clips),
                       'skeleton':cfg['skeleton'],'mesh_sources':cfg['meshes'],'animation_sources':cfg['clips'],
                       'cfg_sha256':hashlib.sha256(cfg_raw).hexdigest(),
                       'vertices':sum(len(m.vertices) for m in submeshes),'triangles':sum(len(m.faces) for m in submeshes)})
        model_ids[key]=identity
        unsupported_events.extend({'model':identity,**e,'reason':'Timed original command is retained; native command execution is unsupported'} for e in cfg['events'])
        return identity
    for course in catalog:
        theme=_uint(course['theme'],'HAMMER theme',2);result={'id':course['id'],'instances':[]};cursor=None
        if not 2<=len(course['chunks'])<=128 or course['chunks'][-1]['name']!='common/start_chunk' or course['chunks'][0]['name']!='common/end_chunk':
            raise AssetError('Unexpected HAMMER course chunk order')
        for chunk_id,chunk in enumerate(reversed(course['chunks'])):
            rows=scenes[chunk['file']];links={}
            for record in rows:
                if record.type=='CHUNKLINK':
                    kind=first(record.values,'LinkType')
                    if kind in links:raise AssetError('Duplicate HAMMER chunk link')
                    links[kind]=finite_vector(first(record.values,'Pos'),3,'HAMMER chunk link')
            if set(links)!={0,1}:raise AssetError('HAMMER chunk needs unique start/end links')
            offset=[0.,0.,0.] if cursor is None else [a-b for a,b in zip(cursor,links[0])]
            cursor=[a+b for a,b in zip(offset,links[1])]
            for ordinal,record in enumerate(rows):
                if record.type!='HAMMER':continue
                _uint(ordinal,'HAMMER record ordinal',65535)
                values,origins=templates.effective(chunk['file'],ordinal,record)
                if first(values,'IsTemplate',False):continue
                identity={'course':course['id'],'chunk':chunk_id,'ordinal':ordinal,'name':record.name,'source':chunk['file']}
                reasons=[]
                if not first(values,'Animation',''):reasons.append('Empty/script-controlled animation is outside square HAMMER slice')
                if first(values,'HammerType')!=1:reasons.append('Unsupported HammerType')
                if first(values,'Physic',False):reasons.append('Physic actor simulation is unsupported')
                if first(values,'AnimSpeed')!=1:reasons.append('Unsupported nonunit AnimSpeed')
                if first(values,'StartFirstAnim',False):reasons.append('Automatic first animation is unsupported')
                if first(values,'AttachTo','') or first(values,'PathName',''):reasons.append('Actor attachment/path simulation is unsupported')
                if any(any(finite_vector(first(values,f,[0,0,0]),3,'HAMMER '+f)) for f in ('Vel','AngVel','Aceleration','AngAceleration')):
                    reasons.append('Actor velocity/acceleration simulation is unsupported')
                resources=values.get('Resources',[])
                if not 1<=len(resources)<=24:raise AssetError('HAMMER resources outside budget')
                if any(len(values.get(f,[]))!=len(resources) for f in ('ResourcesPos','ResourcesAng','ResourcesScale','ResourcesBoneAttach')):
                    raise AssetError('HAMMER resource transform array mismatch')
                bnmd=[i for i,r in enumerate(resources) if r.startswith('BNMD:')]
                if len(bnmd)!=1:reasons.append('HAMMER needs exactly one boned model')
                if any(not r.startswith(('BNMD:','CLP3:')) for r in resources):reasons.append('Unsupported HAMMER resource kind')
                if list(first(values,'Scale',[]))!=[1,1,1]:reasons.append('Unsupported actor scale')
                if len(bnmd)==1 and values['ResourcesScale'][bnmd[0]]!=[1,1,1]:reasons.append('Unsupported model resource scale')
                if reasons:
                    exclusions.append({**identity,'reason':'; '.join(reasons)});continue
                index=bnmd[0];runtime=len(resources)-1-index
                if values['ResourcesBoneAttach'][index]:raise AssetError('HAMMER model itself is bone-attached')
                animation_resource=_uint(first(values,'AnimationResourceId'),'HAMMER animation resource',23)
                if runtime!=animation_resource:raise AssetError('HAMMER animation resource does not resolve to model')
                try:refs=templates.references(values,origins)
                except AssetError as error:
                    exclusions.append({**identity,'reason':str(error)});continue
                mid=model(resources[index][5:],theme);description=models[mid]
                animation=first(values,'Animation')
                if animation not in description['clips']:raise AssetError('HAMMER references missing animation')
                actor_world,model_local=actor_matrices(first(values,'Pos'),first(values,'Ang'),first(values,'Scale'),
                    values['ResourcesPos'][index],values['ResourcesAng'][index],values['ResourcesScale'][index],offset)
                boxes=[]
                for i,resource in enumerate(resources):
                    if not resource.startswith('CLP3:'):continue
                    attach=values['ResourcesBoneAttach'][i]
                    match=re.fullmatch(r'(\d+),(.+)',attach)
                    if not match or int(match[1])!=runtime or match[2] not in description['bones']:
                        raise AssetError('HAMMER box bone attachment does not resolve')
                    primitive=templates.descriptor('cprimitive3',resource[5:])
                    if first(primitive,'PrimitiveType')!=2:raise AssetError('Unsupported HAMMER primitive type')
                    if first(primitive,'RegisterInManager') is not True or first(primitive,'CollisionTest')!=0 or first(primitive,'BoundingBoxMode')!=2:
                        raise AssetError('Unsupported HAMMER collision registration/test/bounds mode')
                    low=finite_vector(first(primitive,'BoundingBoxMin'),3,'HAMMER box minimum')
                    high=finite_vector(first(primitive,'BoundingBoxMax'),3,'HAMMER box maximum')
                    if any(a>=b for a,b in zip(low,high)):raise AssetError('Invalid HAMMER box bounds')
                    boxes.append({'resource':len(resources)-1-i,'bone':description['bones'].index(match[2]),
                                  'local':box_matrix(values['ResourcesPos'][i],values['ResourcesAng'][i],values['ResourcesScale'][i]),
                                  'minimum':low,'maximum':high,'group':_uint(first(primitive,'CollisionGroup'),'HAMMER group'),
                                  'mask':_uint(first(primitive,'CollisionMask'),'HAMMER mask')})
                if not boxes:raise AssetError('HAMMER has no authored collision boxes')
                item={**identity,'model':mid,'actor_world':actor_world,'model_local':model_local,'animation':animation,
                      'animation_resource':runtime,'loop':_bool(first(values,'AnimationLoop'),'HAMMER loop'),
                      'start_moving':_bool(first(values,'StartMoving'),'HAMMER StartMoving'),
                      'wait':_number(first(values,'AnimationWaitTime'),'HAMMER wait',0,3600),
                      'initial_wait':_number(first(values,'AnimationInitialWaitTime'),'HAMMER initial wait',0,3600),
                      'scripts':refs,'boxes':boxes,'activation':first(values,'Activation'),'deactivation':first(values,'Deactivation'),
                      'actor_group':first(values,'ActorGroupName',''),'hammer_type':first(values,'HammerType'),
                      'hammer_force':first(values,'HammerForce'),'source_template':first(values,'HammerTemplate','')}
                result['instances'].append(item);total+=1
        courses.append(result)
    binary=encode_actors(materials.items,models,courses)
    for path,raw in {**materials.files,**files,'actors.dna':binary}.items():
        target=output/path;_no_links(target);target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
    return {'schema':1,'file':'actors.dna','bytes':len(binary),'sha256':hashlib.sha256(binary).hexdigest(),
            'instances':total,'materials':materials.items,'models':models,'courses':courses,'excluded_records':exclusions,
            'unsupported_timed_commands':unsupported_events,'unsupported_material_motion':materials.unsupported,
            'pending_runtime':['Original activation/deactivation and checkpoint actor-group lifecycle',
                               'Original HAMMER player-hit response, crash actions and OnPlayerHit FX',
                               'CFG timed command execution (commands are retained verbatim)'],
            'scope':'Authored square HAMMER animation controls, raw-Cal models and absolute-bone attached collision boxes; explicit exclusions for other variants.'}
