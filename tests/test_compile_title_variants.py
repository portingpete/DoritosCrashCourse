"""Bounded cache/CTAB contracts; fixtures contain no original game data."""
import struct
import unittest
import copy
import hashlib
import json
from pathlib import Path
import tempfile
from tools.compile_title_variants import (SOURCE_CAPTURE_IDENTITIES, feature_defines, isolate_avatar_pixel,
    native_avatar_vertex, pack_cache, parse_cache, reflect_ctab, validate_tokens, validate_source_capture)
from tools.configure_shader_inputs import IDENTITIES, configure


def shader_code(name="g_test",register_set=2,register=4,count=2,reserved=0):
    # Offsets are relative to CTAB header after its four-character tag.
    table = bytearray(64)
    table.extend(name.encode("ascii")+b"\0")
    creator = len(table); table.extend(b"fixture\0")
    target = len(table); table.extend(b"vs_3_0\0")
    struct.pack_into("<7I",table,0,28,creator,0xfffe0300,1,28,0,target)
    struct.pack_into("<I4H2I",table,28,64,register_set,register,count,reserved,48,0)
    struct.pack_into("<8H",table,48,1,3,1,4,1,0,0,0)
    table.extend(b"\0"*((-len(table))%4))
    comment = b"CTAB"+table
    return struct.pack("<2I",0xfffe0300,((len(comment)//4)<<16)|0xfffe)+comment+struct.pack("<I",65535)


def source_capture():
    # Opaque synthetic Xenon instructions, with the original bounded header ABI.
    code = struct.pack(">3I",0x102a1101,12,16)+b"\x11\x22\x33\x44"*4
    data = {"schema":1,"provenance":"original_source_loader",**SOURCE_CAPTURE_IDENTITIES,
            "loader_return_address":0x82387638,"compiler_address":0x82648da8,
            "master_address":0x11000000,"variant_address":0x11001000,"variant_owner_address":0x11000000,
            "shader_name":"multilayer/vs","stage":"vertex","key":[8388614,0,75],
            "parameters":[[81,8,192,0]],"bytecode_file":"multilayer_vs_8388614_0_75.xenon.bin",
            "bytecode_bytes":len(code),"bytecode_sha256":hashlib.sha256(code).hexdigest()}
    return data,code


class ShaderContracts(unittest.TestCase):
    def test_reflection_uses_original_parameter_ids(self):
        self.assertEqual(reflect_ctab(shader_code(),True,["g_unused","g_test"]),[(1,4,2,0)])
        self.assertEqual(reflect_ctab(shader_code(register_set=3,reserved=2),True,["g_unused"]),[])

    def test_unknown_register_types_and_names_rejected(self):
        for register_set in (0,1,4,65535):
            with self.assertRaises(ValueError):
                reflect_ctab(shader_code(register_set=register_set),True,["g_test"])
        for code in (shader_code(name="g_unknown"),shader_code(count=0),
                     shader_code(register=255,count=2),shader_code(reserved=1)):
            with self.assertRaises(ValueError):
                reflect_ctab(code,True,["g_test"])

    def test_truncation_and_offset_bounds(self):
        code = shader_code()
        for end in range(len(code)):
            with self.assertRaises(ValueError):
                reflect_ctab(code[:end],True,["g_test"])
        for location in (4,12,16,24,28,40,44):
            # Code header/comment/FourCC occupy 12 bytes before CTAB header.
            corrupt = bytearray(code)
            struct.pack_into("<I",corrupt,12+location,0xffffffff)
            with self.assertRaises(ValueError):
                reflect_ctab(corrupt,True,["g_test"])

    def test_cache_roundtrip_and_bounds(self):
        value = [{"name":"fixture/vs","variants":[{"key":(1,2,4),"parameters":[(7,4,2,0)],"code":shader_code()}]}]
        cache = pack_cache(value)
        self.assertEqual(parse_cache(cache),value)
        for end in range(len(cache)):
            with self.assertRaises(ValueError):
                parse_cache(cache[:end])
        with self.assertRaises(ValueError):
            parse_cache(cache+b"\0")
        for mutation in (value*2,[{"name":"fixture/vs","variants":value[0]["variants"]*2}]):
            with self.assertRaises(ValueError):
                pack_cache(mutation)

    def test_exact_feature_bits_and_duplicate_macro(self):
        tables = [["_A","_POINTSPRITE"],["_B","_POINTSPRITE"],["_C"]]
        self.assertEqual(feature_defines((3,2,1),tables,True),
                         ["SHVER_SH3","_VERTEX_SHADER","_MAXBONES=48","_A","_POINTSPRITE","_C"])
        with self.assertRaises(ValueError):
            feature_defines((4,0,0),tables,False)

    def test_avatar_pixel_keeps_active_source_unchanged(self):
        prefix = "struct SKINVERTEX { float4 P; };\n"
        helper = "SKINVERTEX SkinVertex(float4 P) { asm { vfetch row, index, texcoord6; }; return row; }"
        pixel = "\n#ifdef _PIXEL_SHADER\nfloat4 main_PS() { return float4(1,2,3,4); }\n#endif\n"
        source = (prefix+helper+pixel).encode()
        self.assertEqual(isolate_avatar_pixel(source),
                         (prefix+"#ifdef _VERTEX_SHADER\n"+helper+"\n#endif\n"+pixel).encode())
        for invalid in (source.replace(b"vfetch",b"other"),source.replace(b"main_PS()",b"SkinVertex main_PS()"),
                        source.replace(b"}; return row; }",b"}; return row;")):
            with self.assertRaises(ValueError):
                isolate_avatar_pixel(invalid)

    def test_avatar_vertex_requires_three_original_palette_rows(self):
        helper = "SKINVERTEX SkinVertex(float4 P) { asm { " + " ".join(
            f"vfetch fetch{i+1}, blendIndex, texcoord{i+6};" for i in range(3)) + " }; return row; }"
        pixel = "\n#ifdef _PIXEL_SHADER\nfloat4 main_PS() { return float4(1,2,3,4); }\n#endif\n"
        source = (helper+pixel).encode()
        transformed = native_avatar_vertex(source)
        self.assertIn(b"uniform float4 g_skinMatrix[216];",transformed)
        self.assertNotIn(b"vfetch",transformed)
        self.assertTrue(transformed.endswith(pixel.encode()))
        for invalid in (source.replace(b"texcoord8",b"texcoord9"),b"float g_skinMatrix;\n"+source):
            with self.assertRaises(ValueError):
                native_avatar_vertex(invalid)

    def test_sm3_profile_terminal_end_and_comment_bounds(self):
        for code,vertex in ((shader_code(),False),(shader_code()+b"\0"*4,True)):
            with self.assertRaises(ValueError):
                validate_tokens(code,vertex)
        corrupt = bytearray(shader_code())
        struct.pack_into("<I",corrupt,4,0x7ffffffe)
        with self.assertRaises(ValueError):
            validate_tokens(corrupt,True)

    def test_source_capture_preserves_actual_code_key_and_binding(self):
        data,code = source_capture()
        result = validate_source_capture(data,code)
        original = [{"name":result["name"],"variants":[result["variant"]]}]
        encoded = pack_cache(original,True)
        self.assertEqual(parse_cache(encoded,True),original)
        self.assertEqual(result["variant"]["code"],code)
        self.assertEqual(result["variant"]["key"],(8388614,0,75))
        self.assertEqual(result["variant"]["parameters"],[(81,8,192,0)])
        # The runtime continues to compare these exact opaque original bytes.
        with self.assertRaises(ValueError):
            validate_source_capture(data,code[:-4]+b"\0"*4)

    def test_source_capture_rejects_unmeasured_scope_owner_and_registers(self):
        data,code = source_capture()
        mutations = {"schema":2,"provenance":"cached_loader","stage":"pixel",
                     "loader_return_address":0x82387d4c,"compiler_address":0x82648ab8,
                     "shader_name":"composite/vs","key":[8388614,0,203],
                     "variant_owner_address":0x11002000,"master_address":True,
                     "image_sha256":"0"*64,"bytecode_bytes":len(code)+4,
                     "parameters":[[81,8,191,0]]}
        for field,value in mutations.items():
            changed = copy.deepcopy(data); changed[field] = value
            with self.subTest(field=field),self.assertRaises(ValueError):
                validate_source_capture(changed,code)
        for parameters in ([[81,65,192,0]],[[81,8,192,1]],[[154,0,1,0],[81,8,192,0]],
                           [[81,8,192,0],[81,8,192,0]],[[81,-1,192,0]],[[81,8,True,0]]):
            changed = copy.deepcopy(data); changed["parameters"] = parameters
            with self.subTest(parameters=parameters),self.assertRaises(ValueError):
                validate_source_capture(changed,code)
        broken = bytearray(code); struct.pack_into(">I",broken,8,20)
        changed = copy.deepcopy(data); changed["bytecode_sha256"] = hashlib.sha256(broken).hexdigest()
        with self.assertRaises(ValueError):
            validate_source_capture(changed,broken)

    def test_source_extension_configuration_requires_exact_native_pair(self):
        data,code = source_capture()
        original_variant = validate_source_capture(data,code)["variant"]
        native_code = shader_code(register=0,count=192)
        native_variant = {"key":original_variant["key"],"parameters":[(81,0,192,0)],"code":native_code}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_path,native_path = root/"original.bsh",root/"native.bsh"
            source_bytes = pack_cache([{"name":"multilayer/vs","variants":[original_variant]}],True)
            native_bytes = pack_cache([{"name":"multilayer/vs","variants":[native_variant]}])
            source_path.write_bytes(source_bytes); native_path.write_bytes(native_bytes)
            manifest = {**IDENTITIES,"trusted_shader_stages":{name:True for name in
                ("composite/vs","composite/ps","multilayer/vs","multilayer/ps")},
                "extension_path":str(native_path),"extension_sha256":hashlib.sha256(native_bytes).hexdigest(),
                "extension_variants":1,"source_extension_path":str(source_path),
                "source_extension_sha256":hashlib.sha256(source_bytes).hexdigest(),
                "source_extension_variants":1,"source_capture_count":1,
                "parameter_names":["g_unused"+str(i) for i in range(81)]+["g_test"],
                "results":[{"name":"multilayer/vs","key":[8388614,0,75],"status":"verified","exit_code":0,
                            "known_native_key":False,"bytecode_sha256":hashlib.sha256(native_code).hexdigest(),
                            "parameters":[[81,0,192,0]],"source_capture":data,
                            "original_parameters":[[81,8,192,0]]}]}
            path = root/"metadata.json"; path.write_text(json.dumps(manifest))
            header = root/"inputs.h"; configure(path,header)
            self.assertIn('sourcePath[] = L',header.read_text())
            self.assertIn(hashlib.sha256(source_bytes).hexdigest(),header.read_text())
            changed = copy.deepcopy(manifest)
            changed["results"][0]["original_parameters"][0][1] = 9
            path.write_text(json.dumps(changed))
            with self.assertRaises(ValueError): configure(path,header)
            changed = copy.deepcopy(manifest)
            changed["source_extension_sha256"] = "0"*64
            path.write_text(json.dumps(changed))
            with self.assertRaises(ValueError): configure(path,header)
            changed = copy.deepcopy(manifest)
            changed["source_capture_count"] = 0
            path.write_text(json.dumps(changed))
            with self.assertRaises(ValueError): configure(path,header)

    def test_original_source_loader_instruction_oracle(self):
        image_path = Path(__file__).parents[1]/"build_native/game_image.bin"
        if not image_path.is_file(): self.skipTest("Private original decoded image unavailable")
        image = image_path.read_bytes()
        self.assertEqual(hashlib.sha256(image).hexdigest(),SOURCE_CAPTURE_IDENTITIES["image_sha256"])
        words = {0x82387164:0x7c7e1b78,0x82387170:0x91430008,0x82387178:0x9123000c,
                 0x82387180:0x91030010,0x82387184:0x920309e8,0x82387360:0x482c1a49,
                 0x82387604:0x806f0000,0x82387608:0x81700078,0x82387618:0x7d4903a6,
                 0x82387630:0x4e800421,0x82387634:0x48038ff5,0x82387638:0x907e0024,
                 0x822acb68:0x8063000c,0x822acb6c:0x4e800020}
        for address,word in words.items():
            with self.subTest(address=hex(address)):
                self.assertEqual(struct.unpack_from(">I",image,address-0x82000000)[0],word)


if __name__ == "__main__":
    unittest.main()
