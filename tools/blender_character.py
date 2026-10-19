"""Blender-side character fitting/weight transfer; invoked by cal3d_character.py.

prepare-blend imports the user's FBX/glTF/OBJ and builds the real skeleton and
reference mesh. Fit the imported objects to the reference in the saved scene.
export-blend requires that fitted scene and either exact named weights or an
explicit surface-weight transfer. The game continues to use its original CAFs.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bpy
from mathutils import Matrix, Quaternion, Vector
from cal3d_character import (Bone, CharacterError, Mesh, Submesh, Vertex, digest,
                            metadata, private_output, read, read_cmf, read_csf,
                            save, write_cmf)
import json
from character_uv import blender_to_title_uv

REFERENCE = "DoritosOriginalReference"
RIG = "DoritosOriginalSkeleton"


def cal_matrix(bone: Bone) -> Matrix:
    # CalVector uses q^-1 * vector * q (Cal3D vector.cpp), opposite Blender's
    # quaternion action. Its stored bone-space transform is an inverse bind.
    x, y, z, w = bone.inverse_bind_rotation
    inverse = Quaternion((w, -x, -y, -z)).to_matrix().to_4x4()
    inverse.translation = Vector(bone.inverse_bind_translation)
    return inverse.inverted()


def active(obj):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def create_reference(skeleton, mesh, csf_path: Path, cmf_path: Path):
    armature = bpy.data.armatures.new(RIG)
    rig = bpy.data.objects.new(RIG, armature)
    bpy.context.collection.objects.link(rig)
    rig.show_in_front = True
    rig["doritos_csf_path"] = str(csf_path.absolute())
    rig["doritos_csf_sha256"] = digest(read(csf_path))
    active(rig)
    bpy.ops.object.mode_set(mode="EDIT")
    for index, bone in enumerate(skeleton.bones):
        edit = armature.edit_bones.new(bone.name)
        # A new EditBone has zero length; give it an axis before its matrix
        # setter, otherwise Blender substitutes a default rest orientation.
        edit.length = 0.025
        edit.matrix = cal_matrix(bone)
        edit["doritos_bone_id"] = index
    for bone in skeleton.bones:
        if bone.parent != -1:
            armature.edit_bones[bone.name].parent = armature.edit_bones[skeleton.bones[bone.parent].name]
    bpy.ops.object.mode_set(mode="OBJECT")
    positions, faces, vertices = [], [], []
    for submesh in mesh.submeshes:
        offset = len(positions)
        positions.extend(vertex.position for vertex in submesh.vertices)
        vertices.extend(submesh.vertices)
        faces.extend(tuple(index + offset for index in face) for face in submesh.faces)
    data = bpy.data.meshes.new(REFERENCE)
    data.from_pydata(positions, [], faces)
    data.update()
    obj = bpy.data.objects.new(REFERENCE, data)
    bpy.context.collection.objects.link(obj)
    obj.display_type = "WIRE"
    obj.hide_render = True
    obj["doritos_reference"] = True
    obj["doritos_cmf_path"] = str(cmf_path.absolute())
    obj["doritos_cmf_sha256"] = digest(read(cmf_path))
    for bone in skeleton.bones:
        obj.vertex_groups.new(name=bone.name)
    for index, vertex in enumerate(vertices):
        for bone_id, weight in vertex.influences:
            obj.vertex_groups[skeleton.bones[bone_id].name].add([index], weight, "REPLACE")
    deform = obj.modifiers.new("Original skeleton", "ARMATURE")
    deform.object = rig
    return rig, obj


def import_source(path: Path) -> list:
    path = path.absolute()
    extension = path.suffix.lower()
    before = set(bpy.data.objects)
    if extension in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=str(path))
    elif extension == ".fbx":
        bpy.ops.import_scene.fbx(filepath=str(path))
    elif extension == ".obj":
        bpy.ops.wm.obj_import(filepath=str(path))
    else:
        raise CharacterError("Source must be an actual FBX, glTF/GLB or OBJ model")
    # Blender's glTF importer can create an Icosphere mesh used solely as a
    # custom bone widget. It is not authored character geometry, even though
    # it is linked into the scene. Never fit, skin or export those widgets.
    bone_widgets = {bone.custom_shape for obj in bpy.data.objects if obj.type == "ARMATURE"
                    for bone in obj.pose.bones if bone.custom_shape}
    objects = [obj for obj in bpy.data.objects if obj not in before and obj.type == "MESH" and obj not in bone_widgets]
    for widget in bone_widgets:
        widget["doritos_export"] = False
        widget.hide_render = True
    if not objects:
        raise CharacterError("The supplied model contains no mesh")
    for obj in objects:
        obj["doritos_export"] = True
    return objects


def prepare(args):
    output = private_output(args.output)
    if output.exists():
        raise CharacterError("Refusing to replace an existing Blender conversion scene")
    skeleton = read_csf(read(args.skeleton))
    mesh = read_cmf(read(args.mesh), skeleton)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    rig, reference = create_reference(skeleton, mesh, args.skeleton, args.mesh)
    objects = import_source(args.source)
    scene = bpy.context.scene
    scene["doritos_source_path"] = str(args.source.absolute())
    scene["doritos_conversion_note"] = "Fit imported geometry to original reference, preserve UVs/materials; then export exact named or explicit surface weights."
    # No guessed rotation, scale, bone-name map or animation is applied.
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1
    for obj in objects:
        # Disable the source rig only in the conversion scene. Vertex-group
        # data remains available for a deliberate named-bone retarget.
        for modifier in obj.modifiers:
            if modifier.type == "ARMATURE":
                modifier.show_viewport = False
                modifier.show_render = False
    active(objects[0])
    bpy.ops.wm.save_as_mainfile(filepath=str(output), check_existing=False)
    print(json.dumps({"scene": str(output), "source_meshes": len(objects), "original_bones": len(skeleton.bones),
                      "fit_required": True, "original_bounds": metadata(skeleton, mesh)["bounds"]}))


def transfer_weights(obj, reference, skeleton):
    # Geometry must already have been fitted in the reviewed Blender scene.
    active(obj)
    obj.vertex_groups.clear()
    for bone in skeleton.bones:
        obj.vertex_groups.new(name=bone.name)
    modifier = obj.modifiers.new("Original surface skin weights", "DATA_TRANSFER")
    modifier.object = reference
    modifier.use_vert_data = True
    modifier.data_types_verts = {"VGROUP_WEIGHTS"}
    modifier.vert_mapping = "POLYINTERP_NEAREST"
    modifier.layers_vgroup_select_src = "ALL"
    modifier.layers_vgroup_select_dst = "NAME"
    modifier.mix_mode = "REPLACE"
    bpy.ops.object.modifier_apply(modifier=modifier.name)


def vertex_weights(obj, vertex, bone_ids):
    influences = []
    for group in vertex.groups:
        if group.weight <= 1e-8:
            continue
        name = obj.vertex_groups[group.group].name
        if name not in bone_ids:
            raise CharacterError(f"Unretargeted vertex group {name!r} on {obj.name}")
        influences.append((bone_ids[name], float(group.weight)))
    # Preserve the four strongest actual weights; normalize after truncation.
    influences.sort(key=lambda item: (-item[1], item[0]))
    influences = influences[:4]
    total = sum(weight for _, weight in influences)
    if total < 1e-8:
        raise CharacterError(f"Unweighted vertex {vertex.index} on {obj.name}")
    return tuple((bone, weight / total) for bone, weight in influences)


def image_source(image, output: Path) -> str:
    if image.packed_file:
        # GLB images may exist only in the imported scene. Extract identical
        # encoded bytes, preserving the artist's colors/alpha and file format.
        data = bytes(image.packed_file.data)
        if data.startswith(b"\x89PNG\r\n\x1a\n"):
            suffix = ".png"
        elif data.startswith(b"\xff\xd8"):
            suffix = ".jpg"
        elif data.startswith(b"DDS "):
            suffix = ".dds"
        else:
            raise CharacterError("Packed source image is not an observed PNG/JPEG/DDS format")
        path = output.parent / (output.stem + "_textures") / ("image_" + digest(data) + suffix)
        save(path, data)
        return str(path.absolute())
    return bpy.path.abspath(image.filepath) if image.filepath else ""


def export(args):
    rig, reference = bpy.data.objects.get(RIG), bpy.data.objects.get(REFERENCE)
    if not rig or not reference or not rig.get("doritos_csf_path"):
        raise CharacterError("Scene has no original Cal3D skeleton/reference")
    csf_bytes = read(Path(rig["doritos_csf_path"]))
    if digest(csf_bytes) != rig["doritos_csf_sha256"]:
        raise CharacterError("Original skeleton input changed since preparation")
    skeleton = read_csf(csf_bytes)
    bone_ids = {bone.name: index for index, bone in enumerate(skeleton.bones)}
    if set(rig.data.bones.keys()) != set(bone_ids):
        raise CharacterError("Conversion rig no longer matches the original bone identities")
    identity = Matrix.Identity(4)
    if any(abs(obj.matrix_world[row][column] - identity[row][column]) > 1e-6
           for obj in (rig, reference) for row in range(4) for column in range(4)):
        raise CharacterError("Original skeleton/reference must retain their original coordinate space")
    if any(obj.parent or obj.constraints or obj.animation_data for obj in (rig, reference)) or rig.data.animation_data:
        raise CharacterError("Original skeleton/reference must remain static and unconstrained")
    for authored in skeleton.bones:
        bone = rig.data.bones[authored.name]
        expected_parent = skeleton.bones[authored.parent].name if authored.parent != -1 else None
        actual_parent = bone.parent.name if bone.parent else None
        expected_matrix = cal_matrix(authored)
        if actual_parent != expected_parent or any(abs(bone.matrix_local[row][column] - expected_matrix[row][column]) > 1e-5
                                                   for row in range(4) for column in range(4)):
            raise CharacterError("Original skeleton rest matrices or hierarchy changed")
        pose = rig.pose.bones[authored.name]
        if pose.constraints or any(abs(pose.matrix_basis[row][column] - identity[row][column]) > 1e-6
                                   for row in range(4) for column in range(4)):
            raise CharacterError("Original skeleton must be in its unchanged rest pose for skin transfer")
    modifiers = list(reference.modifiers)
    if (len(modifiers) != 1 or modifiers[0].type != "ARMATURE" or modifiers[0].object != rig or
            modifiers[0].use_bone_envelopes or not modifiers[0].use_vertex_groups or modifiers[0].use_deform_preserve_volume or
            reference.data.shape_keys or reference.data.animation_data):
        raise CharacterError("Reference must have only its actual original skin deformation")
    original_bytes = read(Path(reference["doritos_cmf_path"]))
    if digest(original_bytes) != reference["doritos_cmf_sha256"]:
        raise CharacterError("Original reference mesh changed since preparation")
    original = read_cmf(original_bytes, skeleton)
    reference_vertices = [vertex for submesh in original.submeshes for vertex in submesh.vertices]
    if len(reference.data.vertices) != len(reference_vertices):
        raise CharacterError("Reference vertex count changed")
    original_faces, offset = [], 0
    for submesh in original.submeshes:
        original_faces.extend(tuple(index + offset for index in face) for face in submesh.faces)
        offset += len(submesh.vertices)
    if [tuple(polygon.vertices) for polygon in reference.data.polygons] != original_faces:
        raise CharacterError("Reference surface topology changed")
    for current, authored in zip(reference.data.vertices, reference_vertices):
        if any(abs(current.co[axis] - authored.position[axis]) > 1e-6 for axis in range(3)):
            raise CharacterError("Reference geometry must remain the original rest surface")
        actual_weights = sorted(vertex_weights(reference, current, bone_ids))
        authored_weights = sorted(authored.influences)
        if len(actual_weights) != len(authored_weights) or any(a[0] != b[0] or abs(a[1] - b[1]) > 1e-5
                                                             for a, b in zip(actual_weights, authored_weights)):
            raise CharacterError("Reference skin weights changed")
    objects = sorted((obj for obj in bpy.data.objects if obj.type == "MESH" and obj.get("doritos_export")), key=lambda obj: obj.name)
    if not objects:
        raise CharacterError("Scene has no selected conversion geometry")
    submeshes, materials = [], []
    for obj in objects:
        if args.weights == "surface":
            transfer_weights(obj, reference, skeleton)
        # Rest geometry is exported directly, never the source rig's animated
        # evaluated surface. Existing modifiers must be applied deliberately.
        if any(modifier.type != "ARMATURE" and modifier.show_viewport for modifier in obj.modifiers):
            raise CharacterError(f"Apply geometry modifiers before exporting {obj.name}")
        data = obj.data
        data.calc_loop_triangles()
        if not 1 <= len(data.uv_layers) <= 2:
            raise CharacterError(f"One or two actual UV sets required on {obj.name}")
        groups = {}
        normal_matrix = obj.matrix_world.to_3x3().inverted().transposed()
        for triangle in data.loop_triangles:
            # Material order is emitted explicitly for the engine .mat.txt.
            slot = triangle.material_index
            records = groups.setdefault(slot, ([], [], {}))
            vertices, faces, lookup = records
            face = []
            for loop_index in triangle.loops:
                loop = data.loops[loop_index]
                source = data.vertices[loop.vertex_index]
                position = tuple(obj.matrix_world @ source.co)
                normal = tuple((normal_matrix @ data.corner_normals[loop_index].vector).normalized())
                uv = tuple(blender_to_title_uv(layer.data[loop_index].uv) for layer in data.uv_layers)
                influences = vertex_weights(obj, source, bone_ids)
                # Split shared vertices only where UVs/normals differ.
                key = (loop.vertex_index, normal, uv)
                if key not in lookup:
                    lookup[key] = len(vertices)
                    vertices.append(Vertex(position, normal, -1, 0, uv, influences))
                face.append(lookup[key])
            if obj.matrix_world.to_3x3().determinant() < 0:
                face.reverse()
            faces.append(tuple(face))
        for slot, (vertices, faces, _) in sorted(groups.items()):
            submeshes.append(Submesh(-1, len(data.uv_layers), vertices, faces))
            material = obj.material_slots[slot].material if slot < len(obj.material_slots) else None
            images = sorted({image_source(node.image, args.output) for node in material.node_tree.nodes
                             if node.type == "TEX_IMAGE" and node.image} if material and material.use_nodes else set())
            diffuse_images = []
            if material and material.use_nodes:
                for node in material.node_tree.nodes:
                    if node.type == "BSDF_PRINCIPLED":
                        diffuse_images.extend(image_source(link.from_node.image, args.output)
                                              for link in node.inputs["Base Color"].links
                                              if link.from_node.type == "TEX_IMAGE" and link.from_node.image)
            materials.append({"submesh": len(submeshes) - 1, "object": obj.name,
                              "source_material": material.name if material else None,
                              "base_color": list(next((node.inputs["Base Color"].default_value for node in material.node_tree.nodes
                                                       if node.type == "BSDF_PRINCIPLED"), material.diffuse_color)
                                                 if material and material.use_nodes else
                                                 material.diffuse_color if material else (0.8, 0.8, 0.8, 1.0)),
                              "source_images": images, "direct_diffuse_images": sorted(set(diffuse_images))})
    mesh = Mesh(submeshes)
    binary = write_cmf(mesh, skeleton)
    read_cmf(binary, skeleton)
    save(args.output, binary)
    report = metadata(skeleton, mesh)
    report.update(mesh_sha256=digest(binary), original_skeleton_sha256=digest(csf_bytes),
                  uv_origin="title top; converted from Blender bottom by V=1-v, U/tiling retained",
                  weights_method=args.weights, materials=materials,
                  required_companion="<cfg basename>.mat.txt: one owned registered ShaderMaster name per physical submesh, then endmats")
    save(args.output.with_suffix(".manifest.json"), (json.dumps(report, indent=2) + "\n").encode())
    print(json.dumps({"mesh": str(args.output), "submeshes": len(submeshes), "vertices": sum(len(item.vertices) for item in submeshes),
                      "faces": sum(len(item.faces) for item in submeshes), "max_influences": report["max_influences"]}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare-blend", "export-blend"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--skeleton", type=Path)
    parser.add_argument("--mesh", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--weights", choices=("existing", "surface"), default="existing")
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    if args.command == "prepare-blend":
        prepare(args)
    else:
        export(args)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # Blender's default script exit code is zero; ensure CLI callers see
        # failure via --python-exit-code supplied by the parent launcher.
        raise
