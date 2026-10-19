"""Background Blender adapter for the character importer's quick/manual modes."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bpy
from mathutils import Matrix, Vector
import blender_character as conversion
from cal3d_character import CharacterError, digest, private_output, read, read_cmf, read_csf, save
from character_packs import checked_png


def fit(objects, reference, yaw: float, scale: float, auto_fit: bool):
    rotation = Matrix.Rotation(math.radians(yaw), 4, "Z")
    points = [rotation @ obj.matrix_world @ vertex.co for obj in objects for vertex in obj.data.vertices]
    if not points or not all(math.isfinite(x) for point in points for x in point):
        raise CharacterError("Source geometry is empty or nonfinite")
    low = Vector(tuple(min(point[axis] for point in points) for axis in range(3)))
    high = Vector(tuple(max(point[axis] for point in points) for axis in range(3)))
    matrix = rotation
    if auto_fit:
        target = [vertex.co for vertex in reference.data.vertices]
        target_low = Vector(tuple(min(p[axis] for p in target) for axis in range(3)))
        target_high = Vector(tuple(max(p[axis] for p in target) for axis in range(3)))
        if high.z - low.z < 1e-8:
            raise CharacterError("Source model has no vertical height; choose an upright rest pose")
        factor = (target_high.z - target_low.z) / (high.z - low.z) * scale
        source_anchor = Vector(((low.x + high.x) / 2, (low.y + high.y) / 2, low.z))
        target_anchor = Vector(((target_low.x + target_high.x) / 2, (target_low.y + target_high.y) / 2, target_low.z))
        matrix = Matrix.Translation(target_anchor) @ Matrix.Scale(factor, 4) @ Matrix.Translation(-source_anchor) @ rotation
    else:
        matrix = Matrix.Scale(scale, 4) @ rotation
    for obj in objects:
        transform = matrix @ obj.matrix_world
        obj.data = obj.data.copy()
        obj.data.transform(transform)
        if transform.to_3x3().determinant() < 0:
            obj.data.flip_normals()
        obj.parent = None
        obj.matrix_world = Matrix.Identity(4)
        obj.data.update()
    return {"auto_fit": auto_fit, "yaw_degrees": yaw, "scale_multiplier": scale,
            "method": "Uniform height fit, horizontal centering and ground alignment; original rig/reference unchanged"}


def source_png(image, output: Path) -> str:
    original = conversion_original_image_source(image, output)
    if original:
        path = Path(original)
        if path.suffix.lower() == ".png":
            try:
                checked_png(read(path))
                return original
            except (CharacterError, ValueError):
                pass
    # Base-color images are decoded by Blender and written as RGB/RGBA 8-bit PNG.
    # Standard view preserves the sRGB transfer function without an AgX film look.
    scene = bpy.context.scene
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0
    scene.view_settings.gamma = 1
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    token = digest((image.name_full + "|" + original).encode("utf-8"))[:24]
    path = private_output(output.parent / (output.stem + "_textures") / (token + ".png"))
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        checked_png(read(path))
        return str(path)
    image.save_render(str(path), scene=scene)
    checked_png(read(path))
    return str(path)


conversion_original_image_source = conversion.image_source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--skeleton", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--yaw", type=float, default=0)
    parser.add_argument("--scale", type=float, default=1)
    parser.add_argument("--no-auto-fit", action="store_true")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--weights", choices=("surface", "existing"), default="surface")
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    if not math.isfinite(args.yaw) or not math.isfinite(args.scale) or not .001 <= args.scale <= 1000:
        raise CharacterError("Invalid rotation or scale")
    output = private_output(args.output)
    if output.exists():
        raise CharacterError("Import output already exists")
    if args.source.suffix.lower() != ".blend":
        skeleton = read_csf(read(args.skeleton))
        mesh = read_cmf(read(args.reference), skeleton)
        bpy.ops.wm.read_factory_settings(use_empty=True)
        rig, reference = conversion.create_reference(skeleton, mesh, args.skeleton, args.reference)
        objects = conversion.import_source(args.source)
        for obj in objects:
            for modifier in obj.modifiers:
                if modifier.type == "ARMATURE":
                    modifier.show_viewport = modifier.show_render = False
        report = fit(objects, reference, args.yaw, args.scale, not args.no_auto_fit)
        bpy.context.scene["doritos_source_path"] = str(args.source)
        bpy.context.scene["doritos_fit"] = json.dumps(report)
    elif not bpy.data.objects.get(conversion.RIG):
        raise CharacterError("A .blend input must be a fitting scene created by this importer")
    scene = private_output(output.with_suffix(".blend"))
    bpy.ops.wm.save_as_mainfile(filepath=str(scene), check_existing=False)
    if args.prepare_only:
        print(json.dumps({"fitting_scene": str(scene), "prepared_only": True}))
        return
    conversion.image_source = source_png
    conversion.export(SimpleNamespace(output=output, weights=args.weights))


if __name__ == "__main__":
    main()
