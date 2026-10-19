"""Independent image-origin and original Cal3D UV loading contracts."""
from pathlib import Path
import hashlib
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from character_uv import blender_to_title_uv, title_to_blender_uv
from cal3d_character import Mesh, Submesh, Vertex, read_cmf, write_cmf


class CharacterUVTests(unittest.TestCase):
    def test_asymmetric_image_corners_keep_their_authored_colours(self):
        # A top-origin image has red/green on its top row, blue/yellow below.
        # The authored Blender lower-left and upper-right therefore select
        # blue and green. A direct, unconverted CMF export selects red/yellow.
        pixels = (("red", "green"), ("blue", "yellow"))
        for uv, expected in (((0.25, 0.25), "blue"), ((0.75, 0.75), "green")):
            u, v = blender_to_title_uv(uv)
            self.assertEqual(pixels[int(v * 2)][int(u * 2)], expected)
            self.assertEqual(title_to_blender_uv((u, v)), uv)

    def test_binary_uv_sets_preserve_tiling_and_other_vertex_data(self):
        authored = (((-0.25, 1.25), (0.25, 0.25)),
                    ((1.75, -0.5), (0.75, 0.75)), ((0.5, 0.0), (0.5, 1.0)))
        expected = (((-0.25, -0.25), (0.25, 0.75)),
                    ((1.75, 1.5), (0.75, 0.25)), ((0.5, 1.0), (0.5, 0.0)))
        vertices = [Vertex((index, 0, 0), (0, 0, 1), -1, 0,
                           tuple(blender_to_title_uv(uv) for uv in sets), ((0, 1.0),))
                    for index, sets in enumerate(authored)]
        mesh = Mesh([Submesh(-1, 2, vertices, [(0, 1, 2)])])
        decoded = read_cmf(write_cmf(mesh))
        self.assertEqual(decoded, mesh)
        self.assertEqual([vertex.uv for vertex in decoded.submeshes[0].vertices], list(expected))
        self.assertEqual([tuple(title_to_blender_uv(uv) for uv in vertex.uv)
                          for vertex in decoded.submeshes[0].vertices], list(authored))

    @unittest.skipUnless((ROOT / "build_native/game_image.bin").is_file(), "Private original image unavailable")
    def test_original_title_leaves_cmf_v_unchanged_before_hardware_copy(self):
        raw = (ROOT / "build_native/game_image.bin").read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),
                         "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7")
        words = {
            # Original Bone constructor installs mode5, excluding invert flag0x2.
            0x8239DFD4: 0x38600005, 0x8239DFD8: 0x48159959,
            0x824F7930: 0x3D6082AB, 0x824F7934: 0x906BD678,
            # CMF loader's optional invert-V branch reads the same mode and
            # skips its 1-v operation unless the independent flag0x2 is set.
            0x824FBFD8: 0x8177D678, 0x824FBFDC: 0x556707BC,
            0x824FBFE4: 0x419A0010, 0x824FBFEC: 0xEDBF0028,
            # Hardware exporter copies the eight UV bytes without arithmetic.
            0x82501F7C: 0x8D2A0001, 0x82501F80: 0x9D280001,
            0x82501F84: 0x4200FFF8,
        }
        for address, expected in words.items():
            self.assertEqual(struct.unpack_from(">I", raw, address - 0x82000000)[0], expected,
                             f"original word {address:08X}")


if __name__ == "__main__":
    unittest.main()
