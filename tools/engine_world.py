"""Prepare static course inspection data from the player's package.

This stage assembles authored chunk links and static resource transforms. It
does not execute scene scripts, simulate obstacles or claim gameplay parity.
"""
from __future__ import annotations
import hashlib
import math
import re
from pathlib import Path
import struct

from engine_assets import AssetError, read_static_mesh, read_collision_mesh
from inspect_pak import _no_links
from character_packs import solid_png


def matrix_product(a, b):
    return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def transform(position=(0,0,0), angles=(0,0,0), scale=(1,1,1)):
    # Scene ANG3 stores yaw about source Y, pitch about X, then roll about Z.
    # Its quaternion composition becomes Rz(roll)*Rx(pitch)*Ry(yaw) here.
    y,x,z = [math.radians(v) for v in angles]
    cx,sx,cy,sy,cz,sz = math.cos(x),math.sin(x),math.cos(y),math.sin(y),math.cos(z),math.sin(z)
    rx=[[1,0,0,0],[0,cx,-sx,0],[0,sx,cx,0],[0,0,0,1]]
    ry=[[cy,0,sy,0],[0,1,0,0],[-sy,0,cy,0],[0,0,0,1]]
    rz=[[cz,-sz,0,0],[sz,cz,0,0],[0,0,1,0],[0,0,0,1]]
    result=matrix_product(rz,matrix_product(rx,ry))
    for i in range(3):
        for j in range(3): result[i][j]*=scale[j]
        result[i][3]=position[i]
    return result


def scene_path(name):
    return 'data/maps/' + name.removesuffix('.wxm') + '.wbm'


def first(values, name, default=None):
    return values.get(name,[default])[0]


def finite_vector(value, size, label):
    if not isinstance(value,(list,tuple)) or len(value)!=size:
        raise AssetError(label+' has an invalid vector size')
    if any(isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v) or abs(v)>1e9 for v in value):
        raise AssetError(label+' contains a nonfinite or out-of-budget number')
    return list(value)


def platform_surface(values, actor, chunk, ordinal):
    """Keep velocity actor-local; the runtime rotates it with orientation only."""
    if not 0<=chunk<4096 or not 0<=ordinal<65536:raise AssetError('Platform authored identity outside budget')
    flags=0
    for field,bit in (('UseForFloor',1),('SlidablePlatform',2),('IsConveyor',4)):
        value=first(values,field)
        if not isinstance(value,bool):raise AssetError('Platform lacks authored boolean '+field)
        if value:flags|=bit
    friction=first(values,'Friction')
    if isinstance(friction,bool) or not isinstance(friction,(int,float)) or not math.isfinite(friction) or not 0<=friction<=1e9:
        raise AssetError('Platform friction outside budget')
    slide=finite_vector(first(values,'SlideVelocity'),3,'Platform SlideVelocity')
    angles=finite_vector(first(values,'Ang',[0,0,0]),3,'Platform orientation')
    basis=[[1,0,0,0],[0,0,1,0],[0,1,0,0],[0,0,0,1]]
    rotation=matrix_product(basis,matrix_product(transform(angles=angles),basis))
    return {'actor':actor,'chunk':chunk,'ordinal':ordinal,'flags':flags,'friction':friction,
            'slide_velocity':[slide[0],slide[2],slide[1]],
            'orientation':[rotation[row][col] for col in range(4) for row in range(4)]}


def diffuse_motion(shader):
    """Match UV motion to the diffuse image the current renderer selects.

    The native renderer implements plain multilayer UV translation. It does not
    implement the legacy per-layer shifter/effect programs or other UV channels.
    Return those omissions explicitly instead of applying an unrelated speed.
    """
    diffuse=first(shader,'MultilayerDiffuse','')
    if diffuse:
        speed=finite_vector(first(shader,'MultilayerUVSpeed',[0,0]),2,'Multilayer UV speed')
        effect=first(shader,'MultilayerUVEffect',0)
        if effect!=0:
            return diffuse,[0,0],0,'unsupported MultilayerUVEffect '+str(effect)
        return diffuse,speed,0,''
    names=shader.get('LayersDiffuseName',[])
    layer=next((i for i,name in enumerate(names) if name),None)
    if layer is None:return '',[0,0],0,''
    channels=shader.get('LayersUVChannel',[])
    channel=channels[layer] if layer<len(channels) else 0
    reasons=[]
    if channel!=0:reasons.append('unsupported selected diffuse UV channel '+str(channel))
    speeds=shader.get('ShifterSpeed',[])
    if layer<len(speeds) and any(finite_vector(speeds[layer],2,'Selected layer ShifterSpeed')):
        reasons.append('unsupported selected per-layer ShifterSpeed semantics')
    effects=shader.get('LayersUVEffect',[])
    if layer<len(effects) and effects[layer]!=0:
        reasons.append('unsupported selected LayersUVEffect '+str(effects[layer]))
    return names[layer],[0,0],channel,'; '.join(reasons)


def prepare_world(source, scenes, catalog, output: Path):
    templates={}
    for path,rows in scenes.items():
        if path.startswith('data/templates/'):
            templates[path.rsplit('/',1)[-1][:-4]]={r.name:r.values for r in rows}
    meshes, mesh_ids, materials, material_ids, textures = [], {}, [], {}, {}
    repairs={};collision_meshes={};excluded_collision=set()
    unresolved=set();unsupported_material_motion={}
    def material(name,theme):
        # Use an authored country variant only if that shader exists.
        shader_templates=templates['shader']
        theme_name=('usa','eu','japan')[theme]
        variant=name.replace('/usa/','/'+theme_name+'/')
        if variant in shader_templates: name=variant
        key=(name,theme)
        if key in material_ids:return material_ids[key]
        shader=shader_templates.get(name,{})
        diffuse,uv_speed,uv_channel,unsupported=diffuse_motion(shader)
        if unsupported:unsupported_material_motion[name]=unsupported
        texture=''
        if diffuse:
            descriptor=templates['texture'].get(diffuse,{})
            file=first(descriptor,'DiffuseFile',diffuse)
            rgb=re.fullmatch(r'rgb/w\d+h\d+r(\d+)g(\d+)b(\d+)',file)
            raw=None
            if rgb:
                color=[int(c) for c in rgb.groups()]
                if max(color)>255:raise AssetError('Invalid authored RGB texture')
                linear=[c/255/12.92 if c/255<=.04045 else ((c/255+.055)/1.055)**2.4 for c in color]
                raw=solid_png(linear+[1]);file+='-native.png'
            else:
                try:raw=source('data/art/'+file)
                except KeyError:unresolved.add('texture:'+file)
            if raw is not None:
                suffix=Path(file).suffix.lower()
                if suffix not in ('.png','.jpg','.jpeg','.dds'):raise AssetError('Unsupported native world image')
                texture='textures/'+hashlib.sha256(raw).hexdigest()+suffix
                textures[texture]=raw
        tint=first(shader,'DiffuseColor',[255,255,255])
        alpha=first(shader,'DiffuseAlpha',255)/255
        identity=len(materials);material_ids[key]=identity
        materials.append({'name':name,'texture':texture,'color':[v/255 for v in tint]+[alpha],
                          'lit':int(first(shader,'Lighting',True)),
                          'transparent':int(bool(first(shader,'Blend',0))),
                          'uv_channel':int(uv_channel),'uv_speed':uv_speed})
        return identity
    def mesh(name,theme):
        key=(name,theme)
        if key in mesh_ids:return mesh_ids[key]
        descriptor=templates['rendervertexmodel'].get(name,{})
        filename=first(descriptor,'FileName',name)
        try: original=read_static_mesh(source('data/art/'+filename))
        except KeyError:
            unresolved.add('mesh:'+filename);return None
        if any(original['repairs'].values()):repairs[filename]=original['repairs']
        override=next((v for v in descriptor.get('ShadersList',[]) if v),'').split(';')
        groups=[]
        for group in original['groups']:
            index=group['slot']
            shader=override[index] if index<len(override) and override[index] else group['material']
            groups.append({**group,'material':material(shader,theme)})
        # The engine uses Z-up, with native row-vector matrices throughout.
        vertices=[]
        for v in original['vertices']:
            vertices.append([v[0],v[2],v[1],v[3],v[5],v[4],v[6],v[7]])
        identity=len(meshes);mesh_ids[key]=identity
        meshes.append({'name':name,'vertices':vertices,'indices':original['indices'],'groups':groups})
        return identity
    def effective(record):
        values={}
        for field,names in record.values.items():
            if field.endswith('Template') and field!='IsTemplate' and names and names[0]:
                table=field[:-8].lower()
                base=templates.get(table,{}).get(names[0])
                if base is None:unresolved.add(field+':'+names[0])
                else:values.update(base)
        values.update(record.values)
        return values
    basis=[[1,0,0,0],[0,0,1,0],[0,1,0,0],[0,0,0,1]]
    courses=[];total_platforms=0
    for course in catalog:
        instances=[];collision=[];collision_platforms=[];platforms=[];spawn=None;cursor=None;chunk_positions=[];excluded=0
        # The serialized track list is a stack, from finish back to start.
        if course['chunks'][-1]['name']!='common/start_chunk' or course['chunks'][0]['name']!='common/end_chunk':
            raise AssetError('Unexpected course chunk order')
        for chunk_ordinal,chunk in enumerate(reversed(course['chunks'])):
            records=scenes[chunk['file']]
            links={first(r.values,'LinkType'):first(r.values,'Pos') for r in records if r.type=='CHUNKLINK'}
            if set(links)!={0,1}:raise AssetError('Course chunk needs unique start and end links')
            offset=[0.,0.,0.] if cursor is None else [a-b for a,b in zip(cursor,links[0])]
            cursor=[a+b for a,b in zip(offset,links[1])]
            chunk_positions.append({'name':chunk['name'],'offset':offset,'start':[a+b for a,b in zip(offset,links[0])],'end':cursor})
            for record_ordinal,record in enumerate(records):
                v=effective(record)
                platform_index=None
                if first(v,'IsTemplate',False):continue
                if record.type=='STARTPOS' and spawn is None:
                    p=[a+b for a,b in zip(first(v,'Pos',[0,0,0]),offset)];spawn=[p[0],p[2],p[1]]
                if first(v,'AttachTo',''):
                    # Dynamic/attached actors require their simulation hierarchy.
                    excluded+=1;continue
                actor=transform(first(v,'Pos',[0,0,0]),first(v,'Ang',[0,0,0]),first(v,'Scale',[1,1,1]))
                for i in range(3):actor[i][3]+=offset[i]
                for i,resource in enumerate(v.get('Resources',[])):
                    if not resource.startswith(('RVMP:','WCOM:','CLP3:')):continue
                    if not all(i<len(v.get(k,[])) for k in ('ResourcesPos','ResourcesAng','ResourcesScale')):
                        raise AssetError('Static resource transform array mismatch')
                    if any(abs(s)<1e-6 for s in v['ResourcesScale'][i]):continue
                    local=transform(v['ResourcesPos'][i],v['ResourcesAng'][i],v['ResourcesScale'][i])
                    if resource.startswith(('WCOM:','CLP3:')):
                        if record.type!='PLATFORM':continue
                        if any(first(v,'Vel',[0,0,0])) or first(v,'PathName','') or v.get('ResourcesBoneAttach',['']*len(v['Resources']))[i]:
                            excluded_collision.add(record.name);continue
                        if resource.startswith('WCOM:'):
                            name=resource[5:]
                            if name not in collision_meshes:
                                descriptor=templates['collisionmesh'].get(name,{})
                                collision_meshes[name]=read_collision_mesh(source('data/art/'+first(descriptor,'FileName',name)))
                            shape=collision_meshes[name]
                            vertices,indices=shape['vertices'],shape['indices']
                        else:
                            shape=templates['cprimitive3'].get(resource[5:],{})
                            if first(shape,'PrimitiveType',-1)!=2:
                                excluded_collision.add(resource);continue
                            low,high=first(shape,'BoundingBoxMin'),first(shape,'BoundingBoxMax')
                            if low is None or high is None:raise AssetError('Collision box has no authored bounds')
                            vertices=[[x,y,z] for x in (low[0],high[0]) for y in (low[1],high[1]) for z in (low[2],high[2])]
                            indices=[0,1,3,0,3,2,4,6,7,4,7,5,0,4,5,0,5,1,2,3,7,2,7,6,0,2,6,0,6,4,1,5,7,1,7,3]
                        matrix=matrix_product(basis,matrix_product(actor,local))
                        transformed=[[sum(matrix[row][col]*value for col,value in enumerate(point+[1])) for row in range(3)] for point in vertices]
                        if indices and platform_index is None:
                            if len(platforms)>=16384 or total_platforms>=65536:
                                raise AssetError('Native PLATFORM surface table outside budget')
                            platform_index=len(platforms)
                            platforms.append(platform_surface(v,record.name,chunk_ordinal,record_ordinal))
                            total_platforms+=1
                        for start in range(0,len(indices),3):
                            collision.append([transformed[j] for j in indices[start:start+3]])
                            collision_platforms.append(platform_index)
                        continue
                    opacity=first(v,'Opacity',1)*v.get('ResourcesOpacity',[1]*len(v['Resources']))[i]
                    if opacity<=0:continue
                    identity=mesh(resource[5:],course['theme'])
                    if identity is None:continue
                    world=matrix_product(basis,matrix_product(actor,matrix_product(local,basis)))
                    instances.append({'mesh':identity,'actor':record.name,'matrix':[world[j][k] for k in range(4) for j in range(4)],'opacity':opacity})
        if spawn is None:raise AssetError('Course has no authored player spawn')
        middle=chunk_positions[len(chunk_positions)//2]
        inspection=[(a+b)/2 for a,b in zip(middle['start'],middle['end'])]
        inspection=[inspection[0],inspection[2],inspection[1]]
        # Prefer an actual authored solid surface when the middle chunk is
        # predominantly animated actors that this stage cannot display yet.
        candidates=[]
        for instance in instances:
            m=meshes[instance['mesh']]
            if not any(word in m['name'] for word in ('/box_','/japanbox_','/conveyor_','_chunk_platform')):continue
            values=instance['matrix']
            center=[sum((min(v[axis] for v in m['vertices']),max(v[axis] for v in m['vertices'])))/2 for axis in range(3)]
            p=[sum(values[col*4+row]*value for col,value in enumerate(center+[1])) for row in range(3)]
            candidates.append((abs(p[0]-inspection[0]),p))
        if candidates:inspection=min(candidates,key=lambda item:item[0])[1]
        courses.append({'id':course['id'],'name':course['name'],'spawn':spawn,'inspection':inspection,'instances':instances,'collision':collision,
                        'platforms':platforms,'collision_platforms':collision_platforms,'chunks':chunk_positions,'excluded_attached':excluded})
    # DNW1 is an independent, bounded little-endian format with no guest pointers.
    binary=bytearray(b'DNW1'+struct.pack('<I',2))
    def string(value):
        raw=value.encode()
        if len(raw)>4096 or b'\0' in raw:raise AssetError('Native world string outside budget')
        binary.extend(struct.pack('<I',len(raw))+raw)
    binary.extend(struct.pack('<I',len(materials)))
    for m in materials:
        string(m['name']);string(m['texture']);binary.extend(struct.pack('<4fII2f',*m['color'],m['lit'],m['transparent'],*m['uv_speed']))
    binary.extend(struct.pack('<I',len(meshes)))
    for m in meshes:
        string(m['name']);binary.extend(struct.pack('<III',len(m['vertices']),len(m['indices']),len(m['groups'])))
        for v in m['vertices']:binary.extend(struct.pack('<8f',*v))
        binary.extend(struct.pack('<'+'I'*len(m['indices']),*m['indices']))
        for g in m['groups']:binary.extend(struct.pack('<III',g['material'],g['first'],g['count']))
    binary.extend(struct.pack('<I',len(courses)))
    for c in courses:
        binary.extend(struct.pack('<I6fI',c['id'],*c['spawn'],*c['inspection'],len(c['instances'])))
        for instance in c['instances']:
            string(instance['actor']);binary.extend(struct.pack('<I17f',instance['mesh'],*instance['matrix'],instance['opacity']))
    collision_binary=bytearray(b'DNC1'+struct.pack('<II',2,len(courses)))
    def collision_string(value):
        raw=value.encode()
        if len(raw)>4096 or b'\0' in raw:raise AssetError('Native platform actor name outside budget')
        collision_binary.extend(struct.pack('<I',len(raw))+raw)
    for c in courses:
        collision_binary.extend(struct.pack('<II',c['id'],len(c['platforms'])))
        for platform in c['platforms']:
            collision_string(platform['actor'])
            collision_binary.extend(struct.pack('<IIIf3f16f',platform['chunk'],platform['ordinal'],platform['flags'],platform['friction'],
                                                *platform['slide_velocity'],*platform['orientation']))
        collision_binary.extend(struct.pack('<I',len(c['collision'])))
        for triangle,platform in zip(c['collision'],c['collision_platforms']):
            collision_binary.extend(struct.pack('<9fI',*(value for point in triangle for value in point),platform))
    if len(binary)>64*1024*1024 or len(collision_binary)>64*1024*1024:
        raise AssetError('Native world or collision asset exceeds 64 MiB')
    for path,raw in {**textures,'world.dnw':binary,'static-collision.dnc':collision_binary}.items():
        target=output/path;_no_links(target);target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
    return {'schema':2,'world_version':2,'collision_version':2,'scope':'Static course geometry and owning PLATFORM surface metadata; dynamic obstacles remain excluded.',
            'meshes':len(meshes),'materials':len(materials),'textures':len(textures),'bytes':len(binary),
            'courses':[{k:v for k,v in c.items() if k not in ('instances','collision','platforms','collision_platforms')}|
                       {'instances':len(c['instances']),'static_collision_triangles':len(c['collision']),
                        'static_platforms':len(c['platforms']),'conveyor_platforms':sum(bool(p['flags']&4) for p in c['platforms'])} for c in courses],
            'collision_scope':'Queries against static PLATFORM box/mesh resources. Dynamic, attached and procedural collision is pending.',
            'excluded_collision':sorted(excluded_collision),
            'unsupported_material_motion':unsupported_material_motion,
            'animated_materials':[{'name':m['name'],'uv_speed':m['uv_speed']} for m in materials if any(m['uv_speed'])],
            'collision_bytes':len(collision_binary),'collision_sha256':hashlib.sha256(collision_binary).hexdigest(),
            'source_repairs':repairs,'unresolved':sorted(unresolved),'sha256':hashlib.sha256(binary).hexdigest()}
