"""UV origin conversion at the Blender/title character asset boundary."""
from __future__ import annotations


def blender_to_title_uv(uv) -> tuple[float, float]:
    # Blender images use a bottom origin. The title's Cal3D loading mode5
    # leaves V unchanged, and its original image sampler uses a top origin.
    # Preserve U and tiling outside [0,1]; only change the origin of V.
    return float(uv[0]), 1.0 - float(uv[1])


def title_to_blender_uv(uv) -> tuple[float, float]:
    """Decode title CMF UVs for a Blender preview of the unchanged images."""
    return float(uv[0]), 1.0 - float(uv[1])
