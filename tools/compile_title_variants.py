#!/usr/bin/env python3
"""Compile exact missing Tongas feature keys with original portable HLSL.

Original assets, reflected game tables and compiled bytecode stay in ignored
build_native storage. The extension contains only missing keys and uses the
verified native cache format. Known native keys must reproduce their actual
parameter/register bindings before a source family is trusted. Optional bounded
source-loader observations pair exact original dynamically compiled Xenon bytes
with native compilation of the same measured key; original precaches stay intact.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import uuid

try:
    from .inspect_pak import PakError, SHADER_ROOT, _no_links, inspect_pak
    from .inspect_xex import XexError, inspect_xex
    from .compile_native_shaders import private_path
except ImportError:
    from inspect_pak import PakError, SHADER_ROOT, _no_links, inspect_pak
    from inspect_xex import XexError, inspect_xex
    from compile_native_shaders import private_path

ROOT = Path(__file__).absolute().parents[1]
IMAGE_SHA256 = "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7"
TABLES = ((0x820f5570,31),(0x820f5930,29),(0x820f5cb8,30))
PARAMETERS = (0x820f61c8,155)
SOURCES = {"composite": "composite.shader_0_2_12.txt", "multilayer": "multilayer.shader_0_0_159.txt"}
AVATAR_PIXEL = "billboard/avatarpose_xenon_ps"
AVATAR_VERTEX = "billboard/avatarpose_xenon_vs"
MAX_CACHE = 16*1024*1024
SOURCE_CAPTURE_IDENTITIES = {
    "source_xex_sha256": "6c4fa5a12ef2ad51db4809d5d654b487300a1ace06c2cdf6cb14258fd273dfc1",
    "package_sha256": "0819b4e9f90695b70e083690e98ac6f76f0df0cf2bbb817f196316c930a54fc3",
    "image_sha256": IMAGE_SHA256,
}
# This is the first measured ordinary BoneModel source variant. New source
# requests require their own original-loader evidence; no nearby key is used.
SOURCE_CAPTURE_SELECTION = ("multilayer/vs", (8388614, 0, 75))

def need(condition: bool, message: str):
    if not condition:
        raise ValueError(message)

class Reader:
    def __init__(self,data: bytes,big: bool=False):
        self.data,self.position,self.order = data,0,">" if big else "<"
    def take(self,count: int) -> bytes:
        need(0 <= count <= len(self.data)-self.position,"Truncated shader cache")
        value = self.data[self.position:self.position+count]
        self.position += count
        return value
    def word(self) -> int:
        return struct.unpack(self.order+"I",self.take(4))[0]

def parse_cache(data: bytes,big: bool=False) -> list[dict]:
    need(12 <= len(data) <= MAX_CACHE,"Invalid shader cache size")
    reader = Reader(data,big)
    need(reader.word() == 3 and reader.word() == 0,"Unsupported shader cache header")
    count = reader.word()
    need(1 <= count <= 256,"Invalid shader count")
    shaders,names,total = [],set(),0
    for _ in range(count):
        length = reader.word()
        need(2 <= length <= 256,"Invalid shader name length")
        name = reader.take(length)
        need(name[-1] == 0 and all(33 <= b <= 126 and b not in (58,92) for b in name[:-1]),"Invalid shader name")
        name = name[:-1].decode("ascii")
        need(name.casefold() not in names,"Duplicate shader name")
        names.add(name.casefold())
        need(name.casefold().endswith(("/vs","/ps","_vs","_ps")),"Invalid shader stage")
        variants,keys = [],set()
        count = reader.word()
        need(1 <= count <= 4096-total,"Invalid shader variant count")
        total += count
        for _ in range(count):
            key = tuple(reader.word() for _ in range(3))
            need(key not in keys,"Duplicate shader key")
            keys.add(key)
            parameter_count = reader.word()
            need(parameter_count <= 1024,"Excessive shader parameters")
            params,ids = [],set()
            for _ in range(parameter_count):
                item = tuple(reader.word() for _ in range(4))
                ident,register,register_count,reserved = item
                need(ident not in ids and 0 < register_count <= 4096-register and reserved == 0,"Invalid shader parameter")
                ids.add(ident)
                params.append(item)
            length = reader.word()
            need(8 <= length <= 1024*1024 and length%4 == 0,"Invalid shader bytecode length")
            code = reader.take(length)
            if not big:
                validate_tokens(code,name.casefold().endswith(("/vs","_vs")),strict_sm3=False)
            variants.append({"key":key,"parameters":params,"code":code})
        shaders.append({"name":name,"variants":variants})
    need(reader.position == len(data),"Trailing shader cache data")
    return shaders

def validate_tokens(code: bytes,vertex: bool,strict_sm3: bool=True) -> list[bytes]:
    need(8 <= len(code) <= 1024*1024 and len(code)%4 == 0,"Invalid native shader bytecode size")
    words = struct.unpack("<"+"I"*(len(code)//4),code)
    need((words[0] >> 16) == (0xfffe if vertex else 0xffff) and
         ((words[0] >> 8)&255) in ((3,) if strict_sm3 else (2,3)),
         "Native bytecode is not the requested shader stage/profile")
    position,comments = 1,[]
    while position < len(words):
        token = words[position]
        position += 1
        opcode = token & 65535
        if opcode == 65535:
            need(token == 65535 and position == len(words),"Shader END is not terminal")
            return comments
        length = (token >> 16)&32767 if opcode == 65534 else (token >> 24)&15
        need(length <= len(words)-position,"Shader instruction exceeds bytecode")
        if opcode == 65534:
            comments.append(code[position*4:(position+length)*4])
        position += length
    raise ValueError("Shader bytecode has no END")

def reflect_ctab(code: bytes,vertex: bool,parameter_names: list[str]) -> list[tuple[int,int,int,int]]:
    tables = [comment[4:] for comment in validate_tokens(code,vertex) if comment[:4] == b"CTAB"]
    need(len(tables) == 1,"Native shader must contain exactly one CTAB")
    data = tables[0]
    need(len(data) >= 28,"Truncated CTAB header")
    size,creator,version,count,info,flags,target = struct.unpack_from("<7I",data)
    need(size == 28 and version == struct.unpack_from("<I",code)[0] and count <= 1024,"Invalid CTAB header")
    def field(offset: int,length: int) -> bytes:
        need(0 <= offset <= len(data) and 0 <= length <= len(data)-offset,"CTAB field exceeds comment")
        return data[offset:offset+length]
    def string(offset: int) -> str:
        field(offset,1)
        end = data.find(b"\0",offset,min(len(data),offset+512))
        need(end >= offset,"Unterminated CTAB string")
        return data[offset:end].decode("ascii")
    string(creator); string(target)
    field(info,count*20)
    names = {name:index for index,name in enumerate(parameter_names)}
    need(len(names) == len(parameter_names),"Duplicate original parameter names")
    params,ids = [],set()
    for index in range(count):
        name,set_,register,register_count,reserved,type_,default = struct.unpack_from("<I4H2I",data,info+index*20)
        name = string(name)
        field(type_,16)
        need(register_count > 0,"Invalid CTAB constant record")
        if default:
            field(default,register_count*(4 if set_ == 0 else 16))
        if set_ == 3: # Original importer sub_82387140 omits samplers.
            continue
        need(reserved == 0,"Unverified CTAB float-constant flags")
        need(set_ == 2,"Native title bridge has no verified boolean/integer constant upload")
        need(name in names,"Unknown original shader constant: "+name)
        ident = names[name]
        need(ident not in ids,"Duplicate reflected parameter ID")
        need(register+register_count <= (256 if vertex else 224),"Native SM3 float register range exceeded")
        ids.add(ident)
        params.append((ident,register,register_count,0))
    return sorted(params)

def read_table(image: bytes,address: int,count: int) -> list[str]:
    offset = address-0x82000000
    need(0 <= offset <= len(image) and count*30 <= len(image)-offset,"Original shader table outside image")
    result = []
    for index in range(count):
        cell = image[offset+index*30:offset+(index+1)*30]
        need(0 in cell,"Original shader name has no terminator")
        name = cell[:cell.index(0)].decode("ascii")
        need(name and all(char.isalnum() or char == '_' for char in name),"Invalid original shader identifier")
        result.append(name)
    return result

def feature_defines(key: tuple[int,int,int],tables: list[list[str]],vertex: bool) -> list[str]:
    need(len(key) == len(tables) == 3,"Invalid feature key dimension")
    values = ["SHVER_SH3","_VERTEX_SHADER" if vertex else "_PIXEL_SHADER","_MAXBONES=48"]
    for word,table in zip(key,tables):
        need(0 <= word < (1<<len(table)),"Feature key selects an unverified original bit")
        values.extend(name for bit,name in enumerate(table) if word&(1<<bit))
    return list(dict.fromkeys(values))

def isolate_avatar_pixel(source: bytes) -> bytes:
    """Guard the unreachable Xbox vertex helper for a native pixel compilation.

    The original function is outside both stage guards, and therefore Microsoft
    parses its Xenon-only vfetch assembly even for main_PS. This transformation
    changes only its availability in the pixel stage; it does not substitute a
    different avatar shader or change active pixel expressions.
    """
    text = source.decode("utf-8")
    matches = list(re.finditer(r"\bSKINVERTEX\s+SkinVertex\s*\(",text))
    need(len(matches) == 1,"Unverified original avatar skin helper declaration")
    begin = matches[0].start()
    body = text.find('{',matches[0].end())
    need(body >= 0,"Original avatar skin helper has no body")
    depth,end = 1,body+1
    while depth and end < len(text):
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    need(depth == 0,"Original avatar skin helper is unterminated")
    need("vfetch" in text[body:end],"Original avatar skin helper changed")
    pixel = text.find("#ifdef _PIXEL_SHADER",end)
    need(pixel >= end and "SkinVertex" not in text[pixel:],"Avatar pixel stage references the vertex-only skin helper")
    need("main_PS" in text[pixel:],"Original avatar pixel entry changed")
    return (text[:begin]+"#ifdef _VERTEX_SHADER\n"+text[begin:end]+"\n#endif\n"+text[end:]).encode("utf-8")

def native_avatar_vertex(source: bytes) -> bytes:
    """Replace three indexed Xbox vertex fetches with actual palette constants.

    Original constructor/update/draw 82390E18/82391A68/821BC868 establish 72
    bones, each three float4 rows, uploaded in stream1 with stride48. Ordinary
    native per-vertex stream binding cannot express its blend-index fetch.
    Graphics must explicitly upload those exact rows into this uniform.
    """
    guarded = isolate_avatar_pixel(source).decode("utf-8")
    begin = guarded.index("SKINVERTEX SkinVertex")
    body = guarded.index('{',begin)
    depth,end = 1,body+1
    while depth and end < len(guarded):
        if guarded[end] == '{': depth += 1
        elif guarded[end] == '}': depth -= 1
        end += 1
    need(depth == 0,"Original avatar helper bounds changed")
    original = guarded[begin:end]
    need(all(re.search(r"vfetch\s+fetch"+str(index+1)+r",\s*blendIndex,\s*texcoord"+str(index+6),original)
             for index in range(3)),"Original avatar bone row fetch semantics changed")
    need("g_skinMatrix" not in guarded,"Original avatar source already defines a different skin uniform")
    replacement = """uniform float4 g_skinMatrix[216];
SKINVERTEX SkinVertex(float4 P, float3 N, int4 blendIndex, float4 blendWeight)
{
    SKINVERTEX Out = (SKINVERTEX)0;
    float4 skinningMatrix[3] = { float4(0,0,0,0), float4(0,0,0,0), float4(0,0,0,0) };
    for (int i = 0; i < 4; ++i)
    {
        if (blendWeight[i] > 0)
        {
            int row = blendIndex[i] * 3;
            skinningMatrix[0] += blendWeight[i] * g_skinMatrix[row];
            skinningMatrix[1] += blendWeight[i] * g_skinMatrix[row+1];
            skinningMatrix[2] += blendWeight[i] * g_skinMatrix[row+2];
        }
    }
    Out.P.x = dot(skinningMatrix[0],P);
    Out.P.y = dot(skinningMatrix[1],P);
    Out.P.z = dot(skinningMatrix[2],P);
    Out.P.w = 1;
    Out.N.x = dot(skinningMatrix[0].xyz,N);
    Out.N.y = dot(skinningMatrix[1].xyz,N);
    Out.N.z = dot(skinningMatrix[2].xyz,N);
    return Out;
}"""
    return (guarded[:begin]+replacement+guarded[end:]).encode("utf-8")

def validate_source_capture(data: dict, code: bytes) -> dict:
    """Validate an actual source-loader observation, never synthesize its code."""
    need(isinstance(data,dict) and data.get("schema") == 1 and
         data.get("provenance") == "original_source_loader", "Invalid source shader capture schema")
    for key,expected in SOURCE_CAPTURE_IDENTITIES.items():
        need(data.get(key) == expected, "Source shader capture has wrong original identity: "+key)
    need(data.get("loader_return_address") == 0x82387638 and
         data.get("compiler_address") == 0x82648da8 and data.get("stage") == "vertex",
         "Source shader capture has an unverified original compiler/stage/caller")
    name,key = data.get("shader_name"),data.get("key")
    need(isinstance(key,list) and len(key) == 3 and all(type(word) is int for word in key) and
         (name,tuple(key)) == SOURCE_CAPTURE_SELECTION, "Source shader capture has an unmeasured name/key")
    addresses = [data.get(field) for field in ("master_address","variant_address","variant_owner_address")]
    need(all(type(address) is int and 0 < address <= 0xffffffff and address%4 == 0 for address in addresses) and
         addresses[0] == addresses[2] and addresses[0] != addresses[1],
         "Source shader capture does not preserve its actual owner")
    need(12 <= len(code) <= 1024*1024 and len(code)%4 == 0, "Invalid source Xenon bytecode size")
    _,header,instructions = struct.unpack_from(">3I",code)
    need(12 <= header <= len(code) and header%4 == instructions%4 == 0 and
         instructions == len(code)-header, "Source Xenon bytecode header differs from its actual allocation")
    need(type(data.get("bytecode_bytes")) is int and data["bytecode_bytes"] == len(code) and
         data.get("bytecode_sha256") == hashlib.sha256(code).hexdigest(),
         "Source Xenon bytecode differs from its captured bytes/hash")
    parameters = data.get("parameters")
    need(isinstance(parameters,list) and len(parameters) <= PARAMETERS[1], "Invalid original source parameter count")
    ids,validated = set(),[]
    for item in parameters:
        need(isinstance(item,list) and len(item) == 4 and all(type(word) is int for word in item),
             "Invalid original source parameter record")
        ident,register,count,reserved = item
        # ID154 is the title's custom renderer-parameter sentinel; its fourth
        # lane is an index rather than a reserved word. That route is excluded.
        need(0 <= ident < PARAMETERS[1]-1 and ident not in ids and 0 <= register < 256 and
             0 < count <= 256-register and reserved == 0, "Invalid original source float register binding")
        ids.add(ident)
        validated.append(tuple(item))
    bone = [item for item in validated if item[0] == 81]
    need(len(bone) == 1 and bone[0][2] == 192, "Source ordinary skin shader lacks its full48bone binding")
    return {"name":name,"variant":{"key":tuple(key),"parameters":validated,"code":code},"capture":data}


def read_source_capture(path: Path) -> dict:
    path = private_path(path)
    need(path.is_file() and 0 < path.stat().st_size <= 65536, "Invalid source shader capture manifest size")
    raw = path.read_bytes()
    data = json.loads(raw)
    need(isinstance(data,dict), "Source shader capture must be an object")
    name = data.get("bytecode_file")
    need(isinstance(name,str) and re.fullmatch(r"[A-Za-z0-9_-]+\.xenon\.bin",name) is not None,
         "Source shader bytecode filename must be a local capture basename")
    code_path = private_path(path.parent/name)
    need(code_path.is_file() and 12 <= code_path.stat().st_size <= 1024*1024, "Source shader bytecode capture missing/excessive")
    result = validate_source_capture(data,code_path.read_bytes())
    result.update(capture_path=str(path),capture_sha256=hashlib.sha256(raw).hexdigest())
    return result


def pack_cache(shaders: list[dict],big: bool=False) -> bytes:
    order = ">" if big else "<"
    chunks = [struct.pack(order+"3I",3,0,len(shaders))]
    for shader in shaders:
        name = shader["name"].encode("ascii")+b"\0"
        chunks.extend((struct.pack(order+"I",len(name)),name,struct.pack(order+"I",len(shader["variants"]))))
        for variant in shader["variants"]:
            chunks.append(struct.pack(order+"4I",*variant["key"],len(variant["parameters"])))
            chunks.extend(struct.pack(order+"4I",*item) for item in variant["parameters"])
            code = variant["code"]
            chunks.extend((struct.pack(order+"I",len(code)),code))
    value = b"".join(chunks)
    parse_cache(value,big)
    return value

def run(args) -> dict:
    gate = inspect_xex(args.xex,require_supported=True)
    inventory = json.loads(args.inventory.read_text(encoding="utf-8"))
    package = inspect_pak(args.pak)
    need(package["sha256"] == inventory["package"]["sha256"],"Original package changed after inventory")
    image = args.image.read_bytes()
    need(hashlib.sha256(image).hexdigest() == IMAGE_SHA256,"Original decoded image identity changed")
    tables = [read_table(image,*item) for item in TABLES]
    parameter_names = read_table(image,*PARAMETERS)
    assets = private_path(args.assets)
    records = {record["name"]:record for record in inventory["shaders"]}
    def checked(name: str) -> Path:
        path = assets/name
        _no_links(path)
        need(path.is_file(),"Verified original shader input missing: "+name)
        need(path.stat().st_size <= MAX_CACHE,"Original shader input too large")
        record = records[SHADER_ROOT+name]
        need(hashlib.sha256(path.read_bytes()).hexdigest() == record["sha256"],"Original shader input identity changed: "+name)
        return path
    # Check every extracted dependency before native compiler includes may read it.
    for name,record in records.items():
        if name.startswith(SHADER_ROOT) and (assets/name[len(SHADER_ROOT):]).exists():
            checked(name[len(SHADER_ROOT):])
    xenon_path,native_path = checked("precache_xenon.bsh"),checked("precache_win32_3.bsh")
    xenon = parse_cache(xenon_path.read_bytes(),True)
    native = parse_cache(native_path.read_bytes())
    existing = {(shader["name"].casefold(),variant["key"]):variant for shader in native for variant in shader["variants"]}
    original_keys = {(shader["name"].casefold(),variant["key"]) for shader in xenon for variant in shader["variants"]}
    capture_paths = getattr(args,"source_capture",[])
    need(len(capture_paths) <= 1, "Only one measured source shader capture is currently supported")
    captures = [read_source_capture(path) for path in capture_paths]
    capture_keys = set()
    for capture in captures:
        item = (capture["name"].casefold(),capture["variant"]["key"])
        need(item not in original_keys and item not in existing and item not in capture_keys,
             "Source shader capture would replace or duplicate an existing feature key")
        capture_keys.add(item)
        feature_defines(item[1],tables,True)
    compiler = args.compiler.absolute()
    need(compiler.is_file(),"Native shader compiler missing")
    _no_links(compiler)
    output = private_path(args.output)/uuid.uuid4().hex
    output.mkdir(parents=True,exist_ok=False)
    _no_links(output)
    avatar_pixel_source = avatar_vertex_source = None
    if args.avatar_pixel:
        original = checked("avatarpose_xenon.sha")
        avatar_pixel_source = output/"avatarpose_xenon_pixel.sha"
        avatar_pixel_source.write_bytes(isolate_avatar_pixel(original.read_bytes()))
    if args.avatar_vertex:
        original = checked("avatarpose_xenon.sha")
        avatar_vertex_source = output/"avatarpose_xenon_vertex.sha"
        avatar_vertex_source.write_bytes(native_avatar_vertex(original.read_bytes()))
    tasks,unmapped = [],[]
    for shader in xenon:
        family = shader["name"].split('/')[0].casefold()
        for variant in shader["variants"]:
            item = (shader["name"].casefold(),variant["key"])
            if family not in SOURCES:
                if item[0] == AVATAR_PIXEL and avatar_pixel_source is not None:
                    tasks.append((shader,variant,avatar_pixel_source,existing.get(item),"avatarpose_xenon.sha"))
                    continue
                if item[0] == AVATAR_VERTEX and avatar_vertex_source is not None:
                    tasks.append((shader,variant,avatar_vertex_source,existing.get(item),"avatarpose_xenon.sha"))
                    continue
                if item not in existing:
                    unmapped.append({"name":shader["name"],"key":variant["key"],"reason":"No verified portable original source/entry mapping"})
                continue
            tasks.append((shader,variant,checked(SOURCES[family]),existing.get(item),SOURCES[family]))
    for capture in captures:
        family = capture["name"].split('/')[0]
        tasks.append(({"name":capture["name"]},capture["variant"],checked(SOURCES[family]),None,SOURCES[family]))
    def compile_one(task):
        shader,variant,source,known,original_name = task
        key = variant["key"]
        vertex = shader["name"].casefold().endswith(("/vs","_vs"))
        defines = feature_defines(key,tables,vertex)
        stem = shader["name"].replace('/','_')+"_"+"_".join(str(word) for word in key)
        target = output/(stem+".bin")
        command = [str(compiler),str(source),"main_VS" if vertex else "main_PS","vs_3_0" if vertex else "ps_3_0",str(target)]
        for define in defines:
            command.extend(("--define",define))
        process = subprocess.run(command,cwd=assets,encoding="utf-8",errors="replace",stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=60)
        record = {"name":shader["name"],"key":key,"defines":defines,"known_native_key":known is not None,
                  "source_sha256":records[SHADER_ROOT+original_name]["sha256"],"exit_code":process.returncode,
                  "diagnostics":process.stderr[:16384]}
        for capture in captures:
            if capture["name"].casefold() == shader["name"].casefold() and capture["variant"]["key"] == key:
                record.update(source_capture=capture["capture"],source_capture_path=capture["capture_path"],
                              source_capture_sha256=capture["capture_sha256"],
                              original_parameters=variant["parameters"])
        if original_name == "avatarpose_xenon.sha":
            record.update(source_transform="Replace actual72bone indexed vfetch rows with216float4 uniform" if vertex else
                          "Exclude original unreachable SkinVertex vfetch helper only in pixel stage",
                          native_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
        if process.returncode:
            record.update(status="compile_failed")
            return record,None
        try:
            code = target.read_bytes()
            metadata = json.loads(process.stdout)
            need(metadata["compiled_bytes"] == len(code),"Native compiler output size differs")
            parameters = reflect_ctab(code,vertex,parameter_names)
            if known is not None:
                need(parameters == sorted(known["parameters"]),"Known native key reflected bindings differ from original PC cache")
            else:
                original = {item[0]:item for item in variant["parameters"]}
                regular = parameters
                if shader["name"].casefold() == AVATAR_VERTEX:
                    bone = [item for item in parameters if item[0] == 81]
                    need(len(bone) == 1 and bone[0][2] == 216 and 81 not in original,"Native avatar bone uniform contract differs")
                    regular = [item for item in parameters if item[0] != 81]
                    record.update(bone_palette_binding={"parameter_id":81,"first_register":bone[0][1],"register_count":216,
                        "bones":72,"row_bytes":16,"bone_bytes":48,"source_bytes":3456,"source_endian":"big",
                        "source_contract":"Original stream1 palette, not Xenon guest shader constants",
                        "requires":"Explicit native palette upload before draw; validate every weighted blend index <72"})
                need(all(item[0] in original and item[2] == original[item[0]][2] for item in regular),"Compiled native constants cannot map exact original Xenon constant upload ranges")
            record.update(status="verified",parameters=parameters,bytecode_path=str(target),
                          bytecode_sha256=hashlib.sha256(code).hexdigest(),compiled_bytes=len(code))
            return record,{"key":key,"parameters":parameters,"code":code}
        except (ValueError,UnicodeError) as error:
            record.update(status="reflection_rejected",reason=str(error))
            return record,None
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        compiled = list(executor.map(compile_one,tasks))
    trusted = {}
    for shader in xenon:
        name = shader["name"].casefold()
        checks = [record for record,_ in compiled if record["name"].casefold() == name and record["known_native_key"]]
        trusted[name] = bool(checks) and all(record["status"] == "verified" for record in checks)
    if args.avatar_pixel or args.avatar_vertex:
        # Its original portable PC counterpart is absent. Trust only the exact
        # original pixel branch, unchanged by the stage guard, after all portable
        # stage checks validate the shared feature/parameter table interpretation.
        baseline = all(trusted[name] for name in ("composite/vs","composite/ps","multilayer/vs","multilayer/ps"))
        for name,enabled in ((AVATAR_PIXEL,args.avatar_pixel),(AVATAR_VERTEX,args.avatar_vertex)):
            if enabled:
                checks = [record for record,_ in compiled if record["name"].casefold() == name]
                trusted[name] = baseline and len(checks) == 6 and all(record["status"] == "verified" for record in checks)
    extension = []
    for shader in xenon:
        name = shader["name"].casefold()
        variants = []
        for record,value in compiled:
            if record["name"].casefold() == name and not record["known_native_key"] and value is not None and trusted[name]:
                variants.append(value)
        if variants:
            extension.append({"name":shader["name"],"variants":variants})
    source_extension = []
    for capture in captures:
        verified = [value for record,value in compiled if record.get("source_capture") == capture["capture"] and
                    value is not None and trusted[capture["name"].casefold()]]
        if verified:
            need(len(verified) == 1, "Duplicate compiled source shader result")
            source_extension.append({"name":capture["name"],"variants":[capture["variant"]]})
    cache_path = output/"native_missing_variants.bsh"
    cache = pack_cache(extension) if extension else None
    if cache is not None:
        cache_path.write_bytes(cache) # Unique per-run directory, no existing output.
    source_path = output/"original_source_variants.bsh"
    source_cache = pack_cache(source_extension,True) if source_extension else None
    if source_cache is not None:
        source_path.write_bytes(source_cache)
    need(inspect_pak(args.pak)["sha256"] == package["sha256"],"Original package changed during compilation")
    result = {"source_xex_sha256":gate["sha256"],"package_sha256":package["sha256"],"image_sha256":IMAGE_SHA256,
              "compiler_sha256":hashlib.sha256(compiler.read_bytes()).hexdigest(),"compiler":"Windows D3DCompile, flags 0",
              "native_cache_sha256":hashlib.sha256(native_path.read_bytes()).hexdigest(),
              "xenon_cache_sha256":hashlib.sha256(xenon_path.read_bytes()).hexdigest(),
              "define_tables":[{"address":address,"names":table} for (address,_),table in zip(TABLES,tables)],
              "parameter_table_address":PARAMETERS[0],"parameter_names":parameter_names,
              "trusted_shader_stages":trusted,"results":[record for record,_ in compiled],"unmapped":unmapped,
              "extension_variants":sum(len(shader["variants"]) for shader in extension),
              "extension_path":str(cache_path) if cache is not None else None,
              "extension_sha256":hashlib.sha256(cache).hexdigest() if cache is not None else None,
              "source_capture_count":len(captures),
              "source_extension_variants":sum(len(shader["variants"]) for shader in source_extension),
              "source_extension_path":str(source_path) if source_cache is not None else None,
              "source_extension_sha256":hashlib.sha256(source_cache).hexdigest() if source_cache is not None else None,
              "limitations":["Native AvatarPose vertex shaders require the documented actual bone palette upload; system avatar families remain unavailable.",
                             "Native bytecode must still create on the actual hardware and receive verified draw inputs."]}
    report = output/"metadata.json"
    report.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"extension_variants":result["extension_variants"],"extension_path":result["extension_path"],
                      "trusted_shader_stages":trusted,"unmapped_count":len(unmapped),"report":str(report)}))
    return result

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler",type=Path,required=True)
    parser.add_argument("--xex",type=Path,default=ROOT/"game/default.xex")
    parser.add_argument("--pak",type=Path,default=ROOT/"game/data.pak")
    parser.add_argument("--inventory",type=Path,default=ROOT/"build_native/renderer_inventory.json")
    parser.add_argument("--image",type=Path,default=ROOT/"build_native/game_image.bin")
    parser.add_argument("--assets",type=Path,default=ROOT/"build_native/renderer_inputs/data/art/dxshaders")
    parser.add_argument("--output",type=Path,default=ROOT/"build_native/native_shader_variants")
    parser.add_argument("--jobs",type=int,default=4,choices=range(1,9))
    parser.add_argument("--avatar-pixel",action="store_true",help="Compile exact original avatarPose main_PS, excluding unreachable Xenon vertex helper")
    parser.add_argument("--avatar-vertex",action="store_true",help="Compile exact avatarPose indexed bone rows into explicit native216float4 palette uniform")
    parser.add_argument("--source-capture",type=Path,action="append",default=[],help="Bounded original source-loader JSON observation in ignored build_native; preserves its exact Xenon bytes")
    args = parser.parse_args()
    try:
        result = run(args)
        expected = ["composite/vs","composite/ps","multilayer/vs","multilayer/ps"]
        if args.avatar_pixel: expected.append(AVATAR_PIXEL)
        if args.avatar_vertex: expected.append(AVATAR_VERTEX)
        return 0 if (all(result["trusted_shader_stages"].get(name,False) for name in expected) and
                     result["source_extension_variants"] == result["source_capture_count"]) else 3
    except (OSError,ValueError,KeyError,PakError,XexError,subprocess.TimeoutExpired) as error:
        print("Exact native shader compilation stopped: "+str(error),file=sys.stderr)
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
