from pathlib import Path
import struct
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from engine_assets import AssetError,read_animation,read_scene,read_static_mesh,read_collision_mesh
from engine_world import transform,matrix_product


def string(text):
    data=text.encode(); return struct.pack('<I',len(data))+data


def scene():
    return (struct.pack('<3H',2,1,5)+string('ACTOR')+struct.pack('<2H',1,2)+
            string('Pos')+bytes([9])+string('Name')+bytes([6])+struct.pack('<2H',1,5)+string('instance')+
            b'1'+struct.pack('<H3f',1,1,2,3)+b'1'+struct.pack('<H',1)+string('Native actor')+b'_bin\0')


def animation(flags=0):
    extension=struct.pack('<I4f',1,0,1,2,3) if flags==2 else b''
    return (b'CAF\0'+struct.pack('<2I',1010,flags)+extension+struct.pack('<fIII',1,1,0,2)+
            struct.pack('<8f',0,0,0,0,0,0,0,1)+struct.pack('<8f',1,1,0,0,0,0,0,1))


def static_mesh():
    header=struct.pack('<BIBHH5BI',102,2,1,3,1,0,0,1,0,0,0)+bytes(196)
    vertices=b''.join(struct.pack('<8f',*p,0,0,1,0,0) for p in ((0,0,0),(1,0,0),(0,1,0)))
    return header+b'native/material\0'.ljust(32,b'\0')+vertices+struct.pack('<5H',0,1,2,0,0)


def collision_mesh():
    header=struct.pack('<2B3H2BH',101,0,3,1,3,0,0,32)
    points=struct.pack('<9f',0,0,0,1,0,0,0,1,0)
    node=struct.pack('<HHB7f3H',1,3,0,0,0,0,0,1,1,0,0,1,2)
    return header+points+node


class EngineAssetsTests(unittest.TestCase):
    def test_collision_triangle_tree_and_truncation(self):
        data=collision_mesh();mesh=read_collision_mesh(data)
        self.assertEqual(mesh['indices'],[0,1,2]);self.assertEqual(len(mesh['nodes']),1)
        for size in range(len(data)):
            with self.assertRaises(AssetError):read_collision_mesh(data[:size])
        for offset,fmt,value in ((0,'B',102),(4,'H',2),(6,'H',4),(len(data)-2,'H',3)):
            bad=bytearray(data);struct.pack_into('<'+fmt,bad,offset,value)
            with self.assertRaises(AssetError):read_collision_mesh(bytes(bad))
        with self.assertRaises(AssetError):read_collision_mesh(data+b'x')
    def test_static_geometry_axes_material_ranges_and_bounds(self):
        data=static_mesh();mesh=read_static_mesh(data)
        self.assertEqual(mesh['vertices'][2][:6],[0,0,1,0,1,0])
        self.assertEqual(mesh['groups'],[{'material':'native/material','slot':0,'first':0,'count':3}])
        for size in range(len(data)):
            with self.assertRaises(AssetError):read_static_mesh(data[:size])
        for offset,fmt,value in ((0,'B',103),(1,'I',8),(len(data)-10,'H',3),(247,'f',float('nan'))):
            broken=bytearray(data);struct.pack_into('<'+fmt,broken,offset,value)
            with self.assertRaises(AssetError):read_static_mesh(bytes(broken))
        with self.assertRaises(AssetError):read_static_mesh(data+b'x')

    def test_static_authored_degenerate_normals_are_rebuilt(self):
        data=bytearray(static_mesh());struct.pack_into('<3f',data,259,*([float('nan')]*3))
        mesh=read_static_mesh(bytes(data))
        self.assertEqual(mesh['repairs'],{'normals':1,'uv':0})
        self.assertEqual(mesh['vertices'][0][3:6],[0,-1,0])
        struct.pack_into('<2H',data,len(data)-4,0,65535)
        self.assertEqual(read_static_mesh(bytes(data))['groups'],[])

    def test_scene_resource_scale_rotation_and_translation_composition(self):
        local=transform((2,0,0),(0,0,90),(2,1,1))
        actor=transform((10,20,30))
        world=matrix_product(actor,local)
        point=[sum(world[i][j]*v for j,v in enumerate((1,0,0,1))) for i in range(4)]
        for a,b in zip(point,(12,22,30,1)):self.assertAlmostEqual(a,b)

    def test_scene_values_have_no_addresses_or_execution_context(self):
        record=read_scene(scene())[0]
        self.assertEqual((record.type,record.name),('ACTOR','instance'))
        self.assertEqual(record.values,{'Pos':[[1,2,3]],'Name':['Native actor']})

    def test_scene_truncation_and_trailing_bytes_rejected(self):
        data=scene()
        for size in range(len(data)):
            with self.assertRaises(AssetError): read_scene(data[:size])
        with self.assertRaises(AssetError): read_scene(data+b'x')

    def test_animation_track_and_motion_extension(self):
        for flags in (0,2):
            clip=read_animation(animation(flags),1)
            self.assertEqual(clip['duration'],1)
            self.assertEqual(clip['tracks'][0]['keys'][-1],[1,1,0,0,0,0,0,1])
            self.assertEqual(clip['motion'],[[0,1,2,3]] if flags else [])

    def test_animation_bones_times_quaternions_and_truncation_rejected(self):
        data=animation()
        for size in range(len(data)):
            with self.assertRaises(AssetError): read_animation(data[:size],1)
        for offset,fmt,value in ((8,'I',1),(20,'I',2),(60,'f',-1),(56,'f',2)):
            corrupt=bytearray(data); struct.pack_into('<'+fmt,corrupt,offset,value)
            with self.assertRaises(AssetError): read_animation(bytes(corrupt),1)
        with self.assertRaises(AssetError): read_animation(data+b'x',1)

    @unittest.skipUnless((ROOT/'game/data.pak').is_file(),'Private original package not present')
    def test_all_original_scene_and_animation_assets_parse_exactly(self):
        from inspect_pak import read_directory,read_entry
        counts={'.wbm':0,'.wbr':0,'.caf':0,'.rvm':0,'.wco':0}
        for entry in read_directory(ROOT/'game/data.pak'):
            suffix=Path(entry.name).suffix
            if suffix not in counts: continue
            with self.subTest(asset=entry.name):
                data=read_entry(ROOT/'game/data.pak',entry)
                readers={'.caf':lambda d:read_animation(d,256),'.rvm':read_static_mesh,'.wco':read_collision_mesh}
                readers.get(suffix,read_scene)(data)
            counts[suffix]+=1
        self.assertEqual(counts,{'.wbm':183,'.wbr':56,'.caf':106,'.rvm':153,'.wco':36})


if __name__=='__main__': unittest.main()
